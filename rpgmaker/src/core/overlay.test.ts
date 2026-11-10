/**
 * OverlayCheckout (overlay.ts): the browser checkout as an overlay inside the game's own web page.
 * Twins of tests/unit/test_overlay.py (the same names), with `ui` a fake page and the Solana
 * transaction builds Jobs; then the hardening the Godot twin (godot/tests/test_overlay.gd) added.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { OverlayCheckout, type OverlayUi } from "./overlay.ts";
import { BUILD_TIMEOUT_S } from "./page_messages.ts";
import { Job } from "./worker.ts";

const TX = "0x" + "ab".repeat(32);
const SOL = "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3";

class FakeUi implements OverlayUi {
  calls: unknown[][] = [];
  inbox: unknown = [];

  open(key: string, info: Record<string, unknown>, logoUrl: string | null): void {
    this.calls.push(["open", key, info["title"], logoUrl]);
  }

  show(key: string): void {
    this.calls.push(["show", key]);
  }

  close(key: string): void {
    this.calls.push(["close", key]);
  }

  take(_key: string): unknown[] {
    const out = this.inbox;
    this.inbox = [];
    return out as unknown[];
  }

  setStatus(_key: string, status: Record<string, unknown>): void {
    this.calls.push(["status", status["state"]]);
  }

  reply(_key: string, id: number, status: number, body: Record<string, unknown>): void {
    this.calls.push(["reply", id, status, body]);
  }

  replies(): unknown[][] {
    return this.calls.filter((c) => c[0] === "reply");
  }

  count(kind: string): number {
    return this.calls.filter((c) => c[0] === kind).length;
  }
}

class Clock {
  now = 0;
  call = (): number => this.now;
  advance(s: number): void {
    this.now += s;
  }
}

/** A Job the test finishes itself. */
function job<T>(): Job<T> {
  return new Job<T>();
}

function make(opts: { build_tx?: ((account: string) => any) | null; clock?: Clock } = {}) {
  const ui = new FakeUi();
  const hints: [string, string | null | undefined][] = [];
  const status: Record<string, unknown> = { state: "waiting" };
  const clock = opts.clock ?? new Clock();
  const o = new OverlayCheckout({
    ui, key: "o1", session_info: { title: "Chapter 2" },
    on_tx: (tx, sender) => void hints.push([tx, sender]),
    get_status: () => ({ ...status }), logo_url: "data:image/png;base64,AA",
    build_tx: opts.build_tx ?? null, clock: clock.call,
  });
  return { o, ui, hints, status, clock };
}

test("test_start_opens_the_overlay_and_pushes_the_status", () => {
  const { o, ui } = make();
  assert.equal(o.url, OverlayCheckout.URL);
  assert.equal(OverlayCheckout.URL, "overlay:checkout");
  assert.equal(o.running, false);
  o.start();
  assert.equal(o.url, OverlayCheckout.URL);
  assert.equal(o.running, true);
  assert.deepEqual(ui.calls, [["open", "o1", "Chapter 2", "data:image/png;base64,AA"], ["status", "waiting"]]);
});

test("test_status_is_pushed_only_when_it_changes", () => {
  const { o, ui, status } = make();
  o.start();
  o.pump();
  o.pump();
  status["state"] = "confirming";
  o.pump();
  assert.deepEqual(ui.calls.filter((c) => c[0] === "status"), [["status", "waiting"], ["status", "confirming"]]);
});

test("test_a_tx_message_becomes_a_hint", () => {
  const { o, ui, hints } = make();
  o.start();
  ui.inbox = [{ type: "tx", body: { tx_hash: TX } }];
  o.pump();
  assert.deepEqual(hints, [[TX, null]]);
});

test("test_garbled_messages_are_ignored", () => {
  const { o, ui, hints } = make({ build_tx: () => job() });
  o.start();
  ui.inbox = [
    "x", null, undefined, 5, [], [{ type: "tx", body: { tx_hash: TX } }], { type: "tx" }, { type: "tx", body: "x" },
    { type: "tx", body: { tx_hash: "0x12" } }, { type: "nope" }, { type: "build", id: "7", body: { account: SOL } },
    // ids the page could only have made up: a boolean, a fraction, out of the integers JSON keeps exact
    { type: "build", id: true, body: { account: SOL } }, { type: "build", id: 7.5, body: { account: SOL } },
    { type: "build", id: 2 ** 53, body: { account: SOL } }, { type: "build", id: null, body: { account: SOL } },
    { type: "build", body: { account: SOL } },
  ];
  o.pump();
  assert.deepEqual(hints, []);
  assert.deepEqual(ui.replies(), []);
  // what take() hands back that is not a list of messages is no messages
  for (const inbox of [null, "[]", { type: "tx", body: { tx_hash: TX } }, 7]) {
    ui.inbox = inbox;
    assert.doesNotThrow(() => o.pump(), String(inbox));
  }
  assert.deepEqual(hints, []);
});

test("test_build_replies_when_the_future_is_done", () => {
  const fut = job<Record<string, unknown>>();
  const { o, ui } = make({ build_tx: () => fut });
  o.start();
  ui.inbox = [{ type: "build", id: 1, body: { account: SOL } }];
  o.pump();
  assert.deepEqual(ui.replies(), [], "still building");
  fut._succeed({ transaction: "AAA" });
  o.pump();
  assert.deepEqual(ui.replies(), [["reply", 1, 200, { transaction: "AAA" }]]);
});

test("test_build_failures_and_bad_accounts", () => {
  const failing = job<Record<string, unknown>>();
  failing._fail(new Error("rpc down"));
  const accounts: string[] = [];
  const { o, ui } = make({ build_tx: (account) => (accounts.push(account), failing) });
  o.start();
  ui.inbox = [{ type: "build", id: 1, body: { account: SOL } }, { type: "build", id: 2, body: { account: "0xabc" } }];
  o.pump();
  assert.deepEqual(accounts, [SOL], "only the valid account reaches build_tx");
  assert.ok(ui.replies().some((r) => JSON.stringify(r) === JSON.stringify(["reply", 1, 503, { error: "unavailable" }])), JSON.stringify(ui.replies()));
  assert.ok(ui.replies().some((r) => JSON.stringify(r) === JSON.stringify(["reply", 2, 400, { error: "bad account" }])), JSON.stringify(ui.replies()));
});

test("test_build_without_solana_is_not_found", () => {
  const { o, ui } = make({ build_tx: null });
  o.start();
  ui.inbox = [{ type: "build", id: 3, body: { account: SOL } }];
  o.pump();
  assert.deepEqual(ui.replies(), [["reply", 3, 404, { error: "not_found" }]]);
});

test("test_a_build_that_takes_too_long_is_cancelled", () => {
  const clock = new Clock();
  const never = job<Record<string, unknown>>();
  const { o, ui } = make({ build_tx: () => never, clock });
  o.start();
  ui.inbox = [{ type: "build", id: 1, body: { account: SOL } }];
  o.pump();
  clock.advance(BUILD_TIMEOUT_S - 0.5);
  o.pump();
  assert.deepEqual(ui.replies(), [], "not before the timeout");
  clock.advance(1);
  o.pump();
  assert.equal(never.cancelled(), true);
  assert.deepEqual(ui.replies(), [["reply", 1, 503, { error: "unavailable" }]]);
  o.pump();
  assert.equal(ui.replies().length, 1, "answered once");
});

test("test_reopen_shows_the_same_overlay", () => {
  const { o, ui } = make();
  o.reopen();
  assert.deepEqual(ui.calls, [], "nothing before start");
  o.start();
  o.reopen();
  assert.equal(ui.count("open"), 1);
  assert.ok(ui.calls.some((c) => c[0] === "show" && c[1] === "o1"));
});

test("test_stop_closes_once_and_pump_does_nothing_after", () => {
  const { o, ui, hints } = make();
  o.start();
  o.stop();
  o.stop();
  ui.inbox = [{ type: "tx", body: { tx_hash: TX } }];
  o.pump();
  o.reopen();
  assert.deepEqual(ui.calls.filter((c) => c[0] === "close"), [["close", "o1"]]);
  assert.equal(ui.count("show"), 0, "no reopen after stop");
  assert.deepEqual(hints, []);
  assert.equal(o.running, false);
});

// --- hardening (the Godot twin's decisions) -----------------------------------------------------

test("aReusedRequestIdCancelsTheOlderBuild", () => {
  const jobs: Job<Record<string, unknown>>[] = [];
  const { o, ui } = make({ build_tx: () => (jobs.push(job()), jobs[jobs.length - 1]) });
  o.start();
  ui.inbox = [{ type: "build", id: 4, body: { account: SOL } }, { type: "build", id: 4, body: { account: SOL } }];
  o.pump();
  assert.equal(jobs.length, 2);
  assert.equal(jobs[0]!.cancelled(), true, "the older build is moot");
  jobs[1]!._succeed({ transaction: "BBB" });
  o.pump();
  assert.deepEqual(ui.replies(), [["reply", 4, 200, { transaction: "BBB" }]], "one answer, from the newer build");
});

test("aBuildTxThatThrowsOrGivesNoJobIsUnavailable", () => {
  let n = 0;
  const { o, ui } = make({
    build_tx: () => {
      n++;
      if (n === 1) throw new Error("the controller is gone");
      return n === 2 ? null : { transaction: "not a job" };
    },
  });
  o.start();
  ui.inbox = [1, 2, 3].map((id) => ({ type: "build", id, body: { account: SOL } }));
  o.pump();
  assert.deepEqual(ui.replies(), [1, 2, 3].map((id) => ["reply", id, 503, { error: "unavailable" }]));
  o.pump();
  assert.equal(ui.replies().length, 3, "nothing kept for them");
});

test("aBuildThatSucceedsWithNoObjectIsUnavailable", () => {
  const fut = job<unknown>();
  const { o, ui } = make({ build_tx: () => fut });
  o.start();
  ui.inbox = [{ type: "build", id: 9, body: { account: SOL } }];
  o.pump();
  fut._succeed(["not", "an", "object"]);
  o.pump();
  assert.deepEqual(ui.replies(), [["reply", 9, 503, { error: "unavailable" }]]);
});

test("callbacksAreNotCalledAfterStop", () => {
  // the hint stops the overlay (its controller closed): the rest of that batch is not handled,
  // no status is pushed, and a build finishing later is never answered
  const fut = job<Record<string, unknown>>();
  const statuses: number[] = [];
  const ui = new FakeUi();
  const hints: string[] = [];
  let builds = 0;
  const o: OverlayCheckout = new OverlayCheckout({
    ui, key: "o1", session_info: { title: "T" },
    on_tx: (tx) => {
      hints.push(tx);
      o.stop();
    },
    get_status: () => (statuses.push(1), { state: "waiting" }),
    build_tx: () => (builds++, fut),
  });
  o.start();
  ui.inbox = [{ type: "build", id: 1, body: { account: SOL } }];
  o.pump();
  assert.equal(builds, 1);
  const before = statuses.length;
  ui.inbox = [{ type: "tx", body: { tx_hash: TX } }, { type: "tx", body: { tx_hash: TX } }, { type: "build", id: 2, body: { account: SOL } }];
  o.pump();
  assert.deepEqual(hints, [TX], "the batch stops at the message that stopped it");
  assert.equal(builds, 1, "no build after stop");
  assert.equal(statuses.length, before, "no status asked after stop");
  assert.equal(fut.cancelled(), true, "stop() cancelled the build");
  o.pump();
  assert.deepEqual(ui.replies(), [], "never answered after stop");
  assert.deepEqual(ui.calls.filter((c) => c[0] === "close"), [["close", "o1"]]);
});

test("aBuildTxThatStopsTheOverlayHasItsJobCancelled", () => {
  // build_tx closed the controller (and so stopped this overlay) before handing its Job back:
  // nobody will answer that build, so it must not run on in the Worker
  const fut = job<Record<string, unknown>>();
  const ui = new FakeUi();
  const o: OverlayCheckout = new OverlayCheckout({
    ui, key: "o1", session_info: { title: "T" }, on_tx: () => undefined, get_status: () => ({ state: "waiting" }),
    build_tx: () => {
      o.stop();
      return fut;
    },
  });
  o.start();
  ui.inbox = [{ type: "build", id: 1, body: { account: SOL } }];
  o.pump();
  assert.equal(fut.cancelled(), true, "the Job just received is cancelled");
  o.pump();
  assert.deepEqual(ui.replies(), [], "never answered");
});

test("aStatusThatIsNotAnObjectIsNotPushed", () => {
  const ui = new FakeUi();
  let answer: unknown = null;
  const o = new OverlayCheckout({ ui, key: "o1", session_info: { title: "T" }, on_tx: () => undefined, get_status: () => answer as any });
  o.start();
  answer = ["waiting"];
  o.pump();
  assert.equal(ui.count("status"), 0, "nothing to push");
  answer = { state: "waiting", nested: { a: [1, 2] } };
  o.pump();
  answer = { state: "waiting", nested: { a: [1, 2] } };
  o.pump();
  assert.equal(ui.count("status"), 1, "an equal status (deeply) is not pushed again");
  answer = { state: "waiting", nested: { a: [1, 3] } };
  o.pump();
  assert.equal(ui.count("status"), 2);
});
