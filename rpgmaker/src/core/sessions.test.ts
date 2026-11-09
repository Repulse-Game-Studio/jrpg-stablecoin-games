/** Twin of tests/unit/test_sessions.py, plus the Entropy contract. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { Dec } from "./decimal.ts";
import { KeyError, ValueError } from "./errors.ts";
import {
  HINT_TIMEOUT_S, SCAN_HORIZON_S, SESSION_WINDOW_S, UI_STATUSES, clearHint, createSession, defaultEntropy,
  getSession, isPastHorizon, isPastWindow, listSessions, newSessionId, normalizeTxHash, removeSession,
  sessionPrice, setHint, setUiStatus, type CreateSessionArgs, type Entropy,
} from "./sessions.ts";

const TX = "0x" + "AB".repeat(32);

function make(store: Record<string, any>, kw: Partial<CreateSessionArgs> = {}, entropy?: Entropy) {
  const args: CreateSessionArgs = {
    kind: "unlock", item: "chapter2", title: "Chapter 2", token: "USDC", chain: "arbitrum", price: "4.99",
    code: 37, start_block: 1000, now: 500.0, nonce: 42, ...kw,
  };
  return createSession(store, args, entropy);
}

/** Records every draw, answering from a fixed script. */
class RecordingEntropy implements Entropy {
  draws: [string, number][] = [];
  tokenHex(n: number): string { this.draws.push(["token_hex", n]); return "00".repeat(n); }
  randBelow(n: number): number { this.draws.push(["randbelow", n]); return 7; }
  tokenBytes(n: number): Uint8Array { this.draws.push(["token_bytes", n]); return new Uint8Array(n); }
}

test("test_constants", () => {
  assert.equal(SESSION_WINDOW_S, 1800);
  assert.equal(SCAN_HORIZON_S, 86400);
  assert.equal(HINT_TIMEOUT_S, 1800);
  assert.deepEqual([...UI_STATUSES], ["open", "pending", "cancelled", "expired"]);
});

test("test_create_session_record_shape", () => {
  const store: Record<string, any> = {};
  const rec = make(store);
  assert.equal(store[rec.id], rec);
  assert.ok(rec.id.startsWith("ps_"));
  assert.equal(rec.units, 4_993_742);
  assert.equal(rec.price, "4.99");
  assert.equal(rec.cursor, 1000);
  assert.equal(rec.start_block, 1000);
  assert.equal(rec.state, "awaiting");
  assert.equal(rec.ui_status, "open");
  assert.equal(rec.hint_tx, null);
  assert.equal(rec.match, null);
  assert.deepEqual(Object.keys(rec), [
    "id", "kind", "item", "title", "token", "chain", "price", "code", "nonce", "units", "start_block",
    "cursor", "created_at", "state", "ui_status", "hint_tx", "hint_at", "match",
  ]);
});

test("test_record_contains_only_plain_types", () => {
  const rec = make({});
  assert.deepEqual(JSON.parse(JSON.stringify(rec)), rec);
});

test("test_random_nonce_and_unique_ids", () => {
  const store: Record<string, any> = {};
  const recs = Array.from({ length: 50 }, () => make(store, { nonce: null }));
  assert.equal(Object.keys(store).length, 50);
  assert.ok(recs.every((r) => Number.isInteger(r.nonce) && r.nonce >= 0 && r.nonce <= 99));
  assert.ok(new Set(recs.map((r) => r.nonce)).size > 1);
});

test("test_create_rejects_bad_kind_and_price", () => {
  assert.throws(() => make({}, { kind: "refund" }), ValueError);
  assert.throws(() => make({}, { price: "4.999" }), ValueError);
  assert.throws(() => make({}, { price: 4.99 }), TypeError);
});

test("test_create_rejects_reserved_extra_fields", () => {
  const store: Record<string, any> = {};
  assert.throws(() => make(store, { extra: { cursor: 5 } }), ValueError);
  assert.deepEqual(store, {});
  const rec = make(store, { extra: { reference: "R", dest: "D" } });
  assert.equal(rec.reference, "R");
  assert.equal(rec.dest, "D");
});

test("test_tip_session_encodes_code_zero", () => {
  const rec = make({}, { kind: "tip", item: null, code: 0, price: "3", nonce: 7 });
  assert.equal(rec.units, 3_000_007);
  assert.equal(String(sessionPrice(rec)), "3.00");
  assert.ok(sessionPrice(rec) instanceof Dec);
});

test("test_get_remove_list", () => {
  const store: Record<string, any> = {};
  const a = make(store, { now: 10.0 });
  const b = make(store, { now: 5.0 });
  assert.equal(getSession(store, a.id), a);
  assert.equal(getSession(store, "nope"), null);
  assert.deepEqual(listSessions(store).map((r) => r.id), [b.id, a.id]);
  removeSession(store, a.id);
  removeSession(store, a.id); // idempotent
  assert.deepEqual(Object.keys(store), [b.id]);
});

test("test_hints", () => {
  const store: Record<string, any> = {};
  const rec = make(store);
  setHint(store, rec.id, "  " + TX + " ", 600.0);
  assert.equal(rec.hint_tx, TX.toLowerCase());
  assert.equal(rec.hint_at, 600.0);
  clearHint(rec);
  assert.equal(rec.hint_tx, null);
  assert.equal(rec.hint_at, null);
});

for (const bad of ["", "0x1234", "0x" + "zz".repeat(32), "ab".repeat(32), null]) {
  test(`test_bad_tx_hash[${JSON.stringify(bad)}]`, () => {
    assert.throws(() => normalizeTxHash(bad), ValueError);
  });
}

test("test_set_hint_unknown_session", () => {
  assert.throws(() => setHint({}, "ps_missing", TX, 0.0), KeyError);
});

test("test_set_hint_uses_the_given_normalizer", () => {
  const store: Record<string, any> = {};
  const rec = make(store);
  setHint(store, rec.id, "sig", 1, (t) => `<${String(t)}>`);
  assert.equal(rec.hint_tx, "<sig>");
});

test("test_ui_status", () => {
  const store: Record<string, any> = {};
  const rec = make(store);
  setUiStatus(store, rec.id, "cancelled");
  assert.equal(rec.ui_status, "cancelled");
  assert.throws(() => setUiStatus(store, rec.id, "paid"), ValueError);
  assert.throws(() => setUiStatus(store, "ps_missing", "open"), KeyError);
});

test("test_window_and_horizon", () => {
  const rec = make({}, { now: 0.0 });
  assert.ok(!isPastWindow(rec, SESSION_WINDOW_S - 1));
  assert.ok(isPastWindow(rec, SESSION_WINDOW_S));
  assert.ok(!isPastHorizon(rec, SCAN_HORIZON_S - 1));
  assert.ok(isPastHorizon(rec, SCAN_HORIZON_S));
});

// --- entropy: draw order and sizes equal Python's `secrets` calls -------------------------------

test("test_create_session_draws_nonce_then_id", () => {
  const e = new RecordingEntropy();
  const rec = make({}, { nonce: null }, e);
  assert.deepEqual(e.draws, [["randbelow", 100], ["token_hex", 8]]);
  assert.equal(rec.nonce, 7);
  assert.equal(rec.id, "ps_" + "00".repeat(8));
});

test("test_given_nonce_draws_only_the_id", () => {
  const e = new RecordingEntropy();
  make({}, { nonce: 42 }, e);
  assert.deepEqual(e.draws, [["token_hex", 8]]);
});

test("test_new_session_id_format", () => {
  assert.match(newSessionId(), /^ps_[0-9a-f]{16}$/);
  assert.notEqual(newSessionId(), newSessionId());
});

test("test_default_entropy_ranges", () => {
  assert.match(defaultEntropy.tokenHex(8), /^[0-9a-f]{16}$/);
  assert.equal(defaultEntropy.tokenBytes(32).length, 32);
  const seen = new Set<number>();
  for (let i = 0; i < 2000; i++) {
    const n = defaultEntropy.randBelow(100);
    assert.ok(Number.isInteger(n) && n >= 0 && n < 100);
    seen.add(n);
  }
  assert.ok(seen.size > 90);
  assert.equal(defaultEntropy.randBelow(1), 0);
  assert.throws(() => defaultEntropy.randBelow(0), ValueError);
});
