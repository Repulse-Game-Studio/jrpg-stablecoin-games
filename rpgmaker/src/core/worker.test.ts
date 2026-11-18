/** Twin of tests/unit/test_worker.py and the non-frame-pumping semantics of test_pump_worker.py. */
import { test } from "node:test";
import assert from "node:assert/strict";
import type { Engine, Snapshot } from "./engine.ts";
import { Dec } from "./decimal.ts";
import { makeStepOutcome, type StepOutcome } from "./watcher.ts";
import { CancelledError, Job, Worker } from "./worker.ts";
import { Abandoned, TransportError, type Cancel } from "./coio.ts";
import { TESTNET } from "./chains.ts";
import { addItem, buildConfig } from "./config.ts";
import { Engine as RealEngine } from "./engine.ts";
import { FakeTransport } from "../../test/rpc_fakes.ts";

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
  refuseRequests: (() => boolean) | null = null;
  refuseInFlight(): void {} // its jobs make no requests
}

/**
 * Twin of test_pump_worker.py's CoopEngine over its FakeBridge: each job makes one request named
 * after it and records the answer. A request ends only when the test answers it, or when the
 * Worker refuses it (refuseRequests, refuseInFlight), as the real Engine's `requests` does.
 */
class CoopEngine {
  log: string[] = [];
  sent: string[] = [];
  cancelled: string[] = []; // the requests in flight that refuseInFlight ended
  refuseRequests: (() => boolean) | null = null;
  private _held: { name: string; resolve: (v: unknown) => void; reject: (e: unknown) => void }[] = [];

  snapshot(): Snapshot {
    return { pending: {}, receipts: {}, owned: new Set(this.log), tip_total: Dec.parse("0") as Dec, unready: new Set() };
  }

  refuseInFlight(): void {
    if (this.refuseRequests === null || !this.refuseRequests()) return;
    for (const h of this._held.splice(0)) {
      this.cancelled.push(h.name);
      h.reject(new Abandoned(`abandoned while its request to ${h.name} was in flight`));
    }
  }

  last(): string | undefined {
    return this.sent[this.sent.length - 1];
  }

  answer(name: string, result: unknown): void {
    const i = this._held.findIndex((h) => h.name === name);
    assert.ok(i >= 0, `${name} is in flight`);
    this._held.splice(i, 1)[0]!.resolve(result);
  }

  private async _one(name: string): Promise<unknown> {
    if (this.refuseRequests !== null && this.refuseRequests()) throw new Abandoned(`abandoned before its request to ${name}`);
    this.sent.push(name);
    const r = await new Promise((resolve, reject) => this._held.push({ name, resolve, reject }));
    this.log.push(`${name}:${String(r)}`);
    return r;
  }

  async reverify(): Promise<string[]> {
    await this._one("reverify");
    return [];
  }

  async tick(): Promise<StepOutcome[]> {
    await this._one("tick");
    return [makeStepOutcome("ps_1", "paid"), makeStepOutcome("ps_2", "waiting")];
  }

  command(name: string): Promise<unknown> {
    return this._one(name);
  }
}

const asEngine = (e: FakeEngine | CoopEngine): Engine => e as unknown as Engine;

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

test("test_a_command_submitted_by_a_command_runs_before_the_same_tick", async () => {
  const e = new FakeEngine();
  const w = new Worker(asEngine(e));
  const inner: Job<number>[] = [];
  w.submit(() => {
    inner.push(w.submit((eng) => (eng as unknown as FakeEngine).ticks));
  });
  await w.runOnce();
  assert.equal(inner[0]!.result(), 0); // drained in the same pass, as in Python
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

// Testbed U15: the player's command does not wait behind a background tick. The tick's request
// in flight is ended (review T7: its answer is never kept), the command runs next, and the next
// tick comes after it. No error is reported for the yield.
test("test_a_command_submitted_mid_tick_makes_the_tick_yield", async () => {
  const e = new CoopEngine();
  const errors: unknown[] = [];
  const t = fakeTimer();
  const w = new Worker(asEngine(e), { interval: () => 10, on_error: (x) => errors.push(x), schedule: t.schedule, cancel: t.cancel });
  w.start();
  t.fire();
  await settle();
  e.answer("reverify", "ok");
  await settle();
  assert.equal(e.last(), "tick"); // tick in flight
  const job = w.submit((x) => (x as unknown as CoopEngine).command("choose"));
  await settle();
  assert.deepEqual(e.cancelled, ["tick"]);
  assert.equal(e.last(), "choose");
  e.answer("choose", "chosen");
  await settle();
  assert.equal(job.result(), "chosen");
  assert.deepEqual(e.sent, ["reverify", "tick", "choose", "tick"]); // the follow-up tick, at once
  e.answer("tick", 1);
  await settle();
  assert.deepEqual(e.log, ["reverify:ok", "choose:chosen", "tick:1"]);
  assert.deepEqual(w.drainOutcomes().map((o) => o.session_id), ["ps_1"]);
  assert.deepEqual(errors, []);
  assert.equal(t.pending!.ms, 10_000);
  w.stop();
});

// Only Engine.tick yields: the launch reverify (run once) finishes first.
test("test_the_first_run_reverify_does_not_yield", async () => {
  const e = new CoopEngine();
  const w = new Worker(asEngine(e));
  const run = w.runOnce();
  await settle();
  assert.equal(e.last(), "reverify"); // reverify in flight
  const job = w.submit((x) => (x as unknown as CoopEngine).command("choose"));
  await settle();
  assert.ok(e.cancelled.length === 0 && e.last() === "reverify" && !job.done());
  e.answer("reverify", "ok");
  await settle();
  // the tick yields before its request
  assert.ok(e.cancelled.length === 0 && e.last() === "choose", String(e.sent));
  assert.deepEqual(e.log, ["reverify:ok"]);
  e.answer("choose", "chosen");
  await settle();
  e.answer("tick", 1);
  await run; // no failure: yielding is no error
  assert.equal(job.result(), "chosen");
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

// --- ordering and follow-ups -------------------------------------------------------------

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

// test_worker.py's test_an_abandoned_running_job_is_refused_its_next_request, with the real Engine:
// the player picks another network while a start waits on its first endpoint, which fails.
test("test_an_abandoned_running_job_is_refused_its_next_request", async () => {
  const cfg = buildConfig({ wallet: "0x52908400098527886E0F7030069857D2E4169EE7", chains: ["base"], testnet: true });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const urls = TESTNET["base"]!.rpc;
  assert.ok(urls.length >= 2, "Base Sepolia has a second endpoint to fall back to");
  let start: Job<any> | null = null;
  const handlers: Record<string, (m: string) => unknown> = {};
  for (const u of urls) handlers[u] = () => ({ jsonrpc: "2.0", id: 1, result: "0x10" });
  handlers[urls[0]!] = () => {
    start!.abandon(); // while its first request is in flight
    throw new TransportError("timed out");
  };
  const transport = new FakeTransport(handlers as never);
  const engine = new RealEngine(cfg, { transport, clock: () => 1700000000 });
  const w = new Worker(engine);
  const job = w.submit((e) => e.startSession("unlock", "base", { item: "chapter2" }));
  start = job;
  await w.runOnce();
  assert.ok(job.exception() instanceof Abandoned, String(job.exception()));
  assert.deepEqual(transport.calls.filter((c) => c[1] === "eth_blockNumber").map((c) => c[0]), [urls[0]]);
  assert.equal(engine.refuseRequests, null, "cleared after the job");
});

// Testbed U15: a superseded start whose request is in flight (a host that never answers) ends at
// once when abandoned, not at the request's 10 s timeout. The transport is told (Cancel).
test("test_an_abandoned_job_stops_waiting_for_its_request_in_flight", async () => {
  const cfg = buildConfig({ wallet: "0x52908400098527886E0F7030069857D2E4169EE7", chains: ["base"], testnet: true });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const cancels: Cancel[] = [];
  let release!: () => void;
  const released = new Promise<void>((r) => { release = r; });
  const hanging = {
    async postJson(_url: string, _payload: object, timeout: number, cancel?: Cancel): Promise<unknown> {
      if (cancel) cancels.push(cancel);
      let timer: ReturnType<typeof setTimeout> | undefined;
      await Promise.race([released, new Promise((r) => { timer = setTimeout(r, timeout * 1000); })]);
      clearTimeout(timer);
      return { jsonrpc: "2.0", id: 1, result: "0x10" };
    },
  };
  const engine = new RealEngine(cfg, { transport: hanging, clock: () => 1700000000 });
  const w = new Worker(engine);
  const job = w.submit((e) => e.startSession("unlock", "base", { item: "chapter2" }));
  const timer = setTimeout(() => job.abandon(), 200);
  const t = performance.now();
  try {
    await w.runOnce();
    assert.ok(performance.now() - t < 2000, `the job waited ${performance.now() - t} ms`);
    assert.ok(job.exception() instanceof Abandoned, String(job.exception()));
    assert.match(String((job.exception() as Error).message), /in flight/);
    assert.ok(cancels.length === 1 && cancels[0]!.cancelled, "the transport was told to stop the request");
  } finally {
    clearTimeout(timer);
    release();
  }
});

test("test_abandoning_a_queued_job_cancels_it", async () => {
  const w = new Worker(asEngine(new FakeEngine()));
  let ran = false;
  const job = w.submit(async () => { ran = true; });
  job.abandon();
  await w.runOnce();
  assert.ok(job.cancelled() && !ran && !job.abandoned);
});
