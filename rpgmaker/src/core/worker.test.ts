/** Twin of tests/unit/test_worker.py and the non-frame-pumping semantics of test_pump_worker.py. */
import { test } from "node:test";
import assert from "node:assert/strict";
import type { Engine, Snapshot } from "./engine.ts";
import { Dec } from "./decimal.ts";
import { makeStepOutcome, type StepOutcome } from "./watcher.ts";
import { CancelledError, Job, Worker } from "./worker.ts";

const tick = (): Promise<void> => new Promise((r) => setImmediate(r));
const settle = async (): Promise<void> => {
  for (let i = 0; i < 5; i++) await tick();
};

class FakeEngine {
  ticks = 0;
  reverified = 0;
  statuses: string[] = ["waiting"];
  tickImpl: (() => Promise<StepOutcome[]> | StepOutcome[]) | null = null;
  log: string[] = [];

  async tick(): Promise<StepOutcome[]> {
    this.ticks++;
    this.log.push("tick");
    if (this.tickImpl) return this.tickImpl();
    return this.statuses.map((s) => makeStepOutcome("ps_1", s));
  }
  async reverify(): Promise<string[]> {
    this.reverified++;
    this.log.push("reverify");
    return [];
  }
  snapshot(): Snapshot {
    return { pending: {}, receipts: {}, owned: new Set([`tick${this.ticks}`]), tip_total: Dec.parse("0") as Dec, unready: new Set() };
  }
}

const asEngine = (e: FakeEngine): Engine => e as unknown as Engine;

/** A manual timer: schedule() records, fire() runs the pending callback. */
function fakeTimer() {
  const t = {
    pending: null as null | { fn: () => void; ms: number },
    scheduled: [] as number[],
    cancelled: 0,
    schedule(fn: () => void, ms: number) {
      t.scheduled.push(ms);
      t.pending = { fn, ms };
      return t.pending;
    },
    cancel(h: unknown) {
      t.cancelled++;
      if (t.pending === h) t.pending = null;
    },
    fire() {
      const p = t.pending;
      t.pending = null;
      p?.fn();
    },
  };
  return t;
}

function deferred<T = void>() {
  let resolve!: (v: T) => void;
  let reject!: (e: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

// --- test_worker.py ---------------------------------------------------------------------

test("test_initial_snapshot_taken_at_construction", () => {
  const w = new Worker(asEngine(new FakeEngine()));
  assert.deepEqual([...w.snapshot.owned], ["tick0"]);
});

test("test_run_once_reverifies_only_first_time_and_publishes_snapshot", async () => {
  const e = new FakeEngine();
  const w = new Worker(asEngine(e));
  await w.runOnce();
  await w.runOnce();
  assert.equal(e.reverified, 1);
  assert.equal(e.ticks, 2);
  assert.deepEqual([...w.snapshot.owned], ["tick2"]);
});

test("test_waiting_outcomes_without_error_are_not_published", async () => {
  const e = new FakeEngine();
  const w = new Worker(asEngine(e));
  e.statuses = ["waiting", "confirming", "paid"];
  await w.runOnce();
  assert.deepEqual(w.drainOutcomes().map((o) => o.status), ["confirming", "paid"]);
  assert.deepEqual(w.drainOutcomes(), []);
});

test("test_waiting_outcome_with_error_is_published", async () => {
  const e = new FakeEngine();
  e.tickImpl = () => [makeStepOutcome("ps_1", "waiting", { error: "network_unavailable" })];
  const w = new Worker(asEngine(e));
  await w.runOnce();
  assert.equal(w.drainOutcomes()[0]!.error, "network_unavailable");
});

test("test_submitted_commands_run_before_tick_and_resolve_jobs", async () => {
  const e = new FakeEngine();
  const w = new Worker(asEngine(e));
  const ok = w.submit((eng) => (eng as unknown as FakeEngine).ticks);
  const bad = w.submit((): number => {
    throw new RangeError("boom");
  });
  assert.equal(ok.done(), false);
  await w.runOnce();
  assert.equal(ok.result(), 0); // ran before the tick
  assert.equal(bad.done(), true);
  assert.throws(() => bad.result(), RangeError);
});

test("test_cancelled_job_is_skipped", async () => {
  const w = new Worker(asEngine(new FakeEngine()));
  const ran: number[] = [];
  const job = w.submit(() => ran.push(1));
  assert.equal(job.cancel(), true);
  await w.runOnce();
  assert.deepEqual(ran, []);
  assert.throws(() => job.result(), CancelledError);
});

test("test_timer_loop_wake_and_stop", async () => {
  const e = new FakeEngine();
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { interval: () => 60, schedule: t.schedule, cancel: t.cancel });
  w.start();
  assert.ok(w.running);
  assert.equal(t.pending!.ms, 0); // first run at once
  t.fire();
  await settle();
  assert.equal(e.ticks, 1);
  assert.equal(t.pending!.ms, 60_000); // long interval: only wake() can trigger tick 2
  const job = w.submit(() => "done"); // submit wakes the loop
  assert.equal(t.pending!.ms, 0);
  t.fire();
  await settle();
  assert.equal(await job.promise, "done");
  assert.equal(e.ticks, 2);
  assert.ok(w.running);
  w.stop();
  assert.ok(!w.running);
  assert.equal(t.pending, null);
});

test("test_loop_survives_exceptions", async () => {
  const e = new FakeEngine();
  const errors: unknown[] = [];
  let n = 0;
  e.tickImpl = () => {
    n++;
    if (n === 1) throw new Error("boom");
    return [];
  };
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { interval: () => 0.01, on_error: (x) => errors.push(x), schedule: t.schedule, cancel: t.cancel });
  w.start();
  t.fire();
  await settle();
  assert.equal(errors.length, 1);
  assert.match((errors[0] as Error).message, /boom/);
  t.fire(); // the loop rescheduled itself
  await settle();
  assert.equal(n, 2);
  w.stop();
});

test("test_async_commands_are_awaited", async () => {
  const e = new FakeEngine();
  const w = new Worker(asEngine(e));
  const job = w.submit(async () => {
    await tick();
    return 42;
  });
  await w.runOnce();
  assert.equal(job.result(), 42);
});

// --- test_pump_worker.py: the semantics that are not frame pumping -----------------------

test("pump: a command submitted mid-tick waits for the tick", async () => {
  const e = new FakeEngine();
  const gate = deferred();
  e.tickImpl = async () => {
    await gate.promise;
    return [];
  };
  const w = new Worker(asEngine(e));
  const run = w.runOnce();
  await settle();
  const job = w.submit(() => {
    e.log.push("choose");
    return "chosen";
  });
  await settle();
  assert.equal(job.done(), false);
  gate.resolve();
  e.tickImpl = null;
  await run;
  assert.equal(job.result(), "chosen");
  assert.deepEqual(e.log, ["reverify", "tick", "choose", "tick"]);
});

test("pump: commands run before a due tick and plain results resolve", async () => {
  const e = new FakeEngine();
  const w = new Worker(asEngine(e));
  const plain = w.submit(() => "now");
  w.submit(() => e.log.push("first"));
  await w.runOnce();
  assert.equal(plain.result(), "now");
  assert.deepEqual(e.log, ["first", "reverify", "tick"]);
});

test("pump: a submit landing mid-run gets its follow-up run at once, not an interval later", async () => {
  const e = new FakeEngine();
  const gate = deferred();
  e.tickImpl = async () => {
    await gate.promise;
    return [];
  };
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { interval: () => 10, schedule: t.schedule, cancel: t.cancel });
  w.start();
  t.fire();
  await settle();
  const job = w.submit(() => "hint");
  e.tickImpl = null;
  gate.resolve();
  await settle();
  assert.equal(job.result(), "hint");
  assert.equal(e.ticks, 2); // the follow-up tick it asked for ran immediately
  assert.equal(t.pending!.ms, 10_000); // then the loop waits the interval
});

test("pump: a run without a mid-run wake waits the interval", async () => {
  const e = new FakeEngine();
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { interval: () => 10, schedule: t.schedule, cancel: t.cancel });
  w.wake(); // not started: nothing to wake
  assert.equal(t.pending, null);
  w.start();
  t.fire();
  await settle();
  assert.equal(e.ticks, 1);
  assert.deepEqual(t.scheduled, [0, 10_000]);
  w.stop();
});

test("pump: a failing tick reports and schedules the next; reverify is not retried", async () => {
  const e = new FakeEngine();
  const errors: unknown[] = [];
  let first = true;
  e.tickImpl = () => {
    if (first) {
      first = false;
      throw new Error("down");
    }
    return [];
  };
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { interval: () => 10, on_error: (x) => errors.push(x), schedule: t.schedule, cancel: t.cancel });
  w.start();
  t.fire();
  await settle();
  assert.equal(errors.length, 1);
  t.fire();
  await settle();
  assert.equal(e.reverified, 1);
  assert.equal(e.ticks, 2);
  w.stop();
});

test("pump: stop cancels queued commands and runs nothing more", async () => {
  const e = new FakeEngine();
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { schedule: t.schedule, cancel: t.cancel });
  w.start();
  const queued = w.submit(() => "later");
  w.stop();
  assert.ok(queued.cancelled());
  assert.ok(!w.running);
  assert.equal(t.pending, null);
  assert.equal(e.ticks, 0);
});

// --- the brief's named tests ------------------------------------------------------------

test("test_commands_never_interleave_with_a_tick", async () => {
  const e = new FakeEngine();
  const gate = deferred();
  let inTick = false;
  let ticks = 0;
  e.tickImpl = async () => {
    ticks++;
    if (ticks === 1) {
      inTick = true;
      await gate.promise;
      inTick = false;
    }
    return [];
  };
  const w = new Worker(asEngine(e));
  const run = w.runOnce();
  await settle();
  assert.equal(inTick, true);
  let ranDuringTick: boolean | null = null;
  const job = w.submit(() => {
    ranDuringTick = inTick;
    e.log.push("cmd");
    return 1;
  });
  await settle();
  assert.equal(job.done(), false); // not executed while the tick awaits the network
  gate.resolve();
  await run;
  assert.equal(job.result(), 1);
  assert.equal(ranDuringTick, false);
  // the command ran in the NEXT run: tick(1st run) ... cmd, then tick(2nd run)
  assert.deepEqual(e.log, ["reverify", "tick", "cmd", "tick"]);
  assert.equal(e.reverified, 1);
});

test("test_wake_during_a_run_schedules_exactly_one_follow_up", async () => {
  const e = new FakeEngine();
  const gate = deferred();
  e.tickImpl = async () => {
    if (e.ticks === 1) await gate.promise;
    return [];
  };
  const w = new Worker(asEngine(e));
  const run = w.runOnce();
  await settle();
  w.wake();
  w.wake();
  void w.runOnce();
  gate.resolve();
  await run;
  assert.equal(e.ticks, 2);
});

test("test_first_run_reverifies_once", async () => {
  const e = new FakeEngine();
  const w = new Worker(asEngine(e));
  await w.runOnce();
  assert.deepEqual(e.log, ["reverify", "tick"]); // reverify before the first tick
  await w.runOnce();
  await w.runOnce();
  assert.equal(e.reverified, 1);
});

test("test_job_reports_exception", async () => {
  const w = new Worker(asEngine(new FakeEngine()));
  const err = new TypeError("bad");
  const bad = w.submit((): void => {
    throw err;
  });
  const good = w.submit(() => 1);
  assert.throws(() => bad.result(), /not done/);
  await w.runOnce();
  assert.equal(bad.exception(), err);
  assert.equal(good.exception(), null);
  assert.throws(() => bad.result(), (x) => x === err);
  await assert.rejects(bad.promise, (x) => x === err);
});

test("test_job_unobserved_failure_raises_no_unhandled_rejection", async () => {
  const seen: unknown[] = [];
  const on = (r: unknown) => seen.push(r);
  process.on("unhandledRejection", on);
  try {
    const w = new Worker(asEngine(new FakeEngine()));
    w.submit(() => {
      throw new Error("never observed");
    });
    const c = w.submit(() => 1);
    c.cancel();
    await w.runOnce();
    await settle();
  } finally {
    process.off("unhandledRejection", on);
  }
  assert.deepEqual(seen, []);
});

test("test_stop_cancels_the_timer", async () => {
  const e = new FakeEngine();
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { schedule: t.schedule, cancel: t.cancel });
  w.start();
  assert.ok(t.pending);
  w.stop();
  assert.equal(t.cancelled, 1);
  assert.equal(t.pending, null);
  assert.ok(!w.running);
});

test("test_no_run_after_stop_even_if_a_run_is_in_flight", async () => {
  const e = new FakeEngine();
  const gate = deferred();
  e.tickImpl = async () => {
    await gate.promise;
    return [];
  };
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { schedule: t.schedule, cancel: t.cancel });
  w.start();
  t.fire();
  await settle();
  w.wake(); // would normally add a follow-up
  w.stop();
  gate.resolve();
  await settle();
  assert.equal(e.ticks, 1);
  assert.equal(t.pending, null);
});

test("Job: result() throws while pending", () => {
  const j = new Job<number>();
  assert.throws(() => j.result(), /not done/);
  assert.equal(j.exception(), null);
});

test("test_reentrant_runOnce_from_a_command_never_overlaps_ticks", async () => {
  const e = new FakeEngine();
  let live = 0;
  let maxLive = 0;
  e.tickImpl = async () => {
    live++;
    maxLive = Math.max(maxLive, live);
    await tick();
    live--;
    return [];
  };
  const w = new Worker(asEngine(e));
  w.submit(() => {
    void w.runOnce();
  });
  await w.runOnce();
  assert.equal(maxLive, 1);
  assert.equal(e.ticks, 2); // exactly one follow-up
});

test("test_wake_and_submit_from_a_command_give_one_follow_up", async () => {
  const e = new FakeEngine();
  let live = 0;
  let maxLive = 0;
  e.tickImpl = async () => {
    live++;
    maxLive = Math.max(maxLive, live);
    await tick();
    live--;
    return [];
  };
  const w = new Worker(asEngine(e));
  let inner: Job<string> | null = null;
  w.submit(() => {
    w.wake();
    inner = w.submit(() => "inner");
  });
  await w.runOnce();
  assert.equal(maxLive, 1);
  assert.equal(e.ticks, 2);
  assert.equal(inner!.result(), "inner");
  assert.deepEqual(e.log.filter((x) => x === "tick").length, 2);
});

test("test_cancelled_job_exception_is_the_cancelled_error", () => {
  const j = new Job<number>();
  j.cancel();
  assert.ok(j.exception() instanceof CancelledError);
  assert.throws(() => j.result(), (x) => x === j.exception());
});

test("test_submit_on_a_stopped_worker_is_cancelled_but_before_start_it_queues", async () => {
  const e = new FakeEngine();
  const w = new Worker(asEngine(e));
  const early = w.submit(() => "queued"); // never started: queues
  assert.equal(early.done(), false);
  w.stop();
  assert.ok(early.cancelled());
  const late = w.submit(() => "never");
  assert.ok(late.cancelled());
  assert.ok(late.exception() instanceof CancelledError);
  w.start(); // a restart accepts work again
  const again = w.submit(() => "ok");
  assert.equal(again.done(), false);
  w.stop();
});

test("test_timer_firing_during_a_wake_triggered_run_yields_one_follow_up", async () => {
  const e = new FakeEngine();
  const gate = deferred();
  let live = 0;
  let maxLive = 0;
  e.tickImpl = async () => {
    live++;
    maxLive = Math.max(maxLive, live);
    if (e.ticks === 1) await gate.promise;
    live--;
    return [];
  };
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { interval: () => 10, schedule: t.schedule, cancel: t.cancel });
  w.start(); // timer armed at 0
  const run = w.runOnce(); // a run in flight, started outside the timer
  await settle();
  w.wake(); // asks for a follow-up
  t.fire(); // the timer fires mid-run: joins the same chain
  gate.resolve();
  await run;
  await settle();
  assert.equal(maxLive, 1);
  assert.equal(e.ticks, 2);
  w.stop();
});
