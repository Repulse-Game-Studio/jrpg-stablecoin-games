import { test } from "node:test";
import assert from "node:assert/strict";
import { asUnits } from "../../rpgmaker/src/core/amounts.ts";
import { TransportError } from "../../rpgmaker/src/core/coio.ts";
import { Dec } from "../../rpgmaker/src/core/decimal.ts";
import { RestoreError } from "../../rpgmaker/src/core/restore.ts";
import { Engine } from "../../rpgmaker/src/core/engine.ts";
import { buildConfig } from "../../rpgmaker/src/core/config.ts";
import {
  ENABLED, ReplayTransport, ScriptedEntropy, candidateFromJson, candidateToJson, encodeReturns, replay, runAction, snapshotJson,
} from "./tier2.ts";
import { loadTier2 } from "./vectors.ts";

const vectors = loadTier2();

test("the vectors directory has every Tier 2 scenario", () => {
  assert.equal(vectors.length, 118);
});

for (const { path, vector } of vectors) {
  test(path, { skip: !ENABLED(vector) }, () => replay(vector));
}

test("test_every_vector_enabled", () => {
  assert.equal(vectors.length, 118);
  assert.deepEqual(vectors.filter((v) => !ENABLED(v.vector)).map((v) => v.path), []);
  assert.equal(vectors.filter((v) => ENABLED(v.vector)).length, 118);
  assert.ok(vectors.some((v) => v.path === "tier2/solana/readiness_crash.json"));
  assert.ok(vectors.some((v) => v.path === "tier2/solana/scan_far_behind.json"));
});

// --- the runner itself ------------------------------------------------------------------

const call = (method: string, params: unknown, result: unknown, url = "https://a") => ({
  url, method, params, response_text: JSON.stringify({ jsonrpc: "2.0", id: 9, result }),
});

test("test_replay_transport_is_a_multiset_with_recorded_order_for_duplicates", async () => {
  const t = new ReplayTransport();
  t.load([call("m", [1], "first"), call("x", [], "x"), call("m", [1], "second")]);
  const post = (method: string, params: unknown) => t.postJson("https://a", { jsonrpc: "2.0", id: 1, method, params }, 10);
  assert.deepEqual(await post("x", []), { jsonrpc: "2.0", id: 9, result: "x" });
  assert.equal(((await post("m", [1])) as any).result, "first");
  assert.equal(((await post("m", [1])) as any).result, "second");
  assert.deepEqual(t.problems, []);
  assert.deepEqual(t.leftover(), []);
  await assert.rejects(post("m", [1]));
  assert.equal(t.problems.length, 1);
  assert.match(t.problems[0]!, /unexpected call https:\/\/a m/);
});

test("test_replay_transport_params_use_json_equality", async () => {
  const t = new ReplayTransport();
  t.load([call("m", [{ b: 1, a: [true, null] }], "ok")]);
  const post = (params: unknown) => t.postJson("https://a", { method: "m", params }, 10);
  await assert.rejects(post([{ a: [1, null], b: 1 }])); // true is not 1
  await post([{ a: [true, null], b: 1 }]); // key order ignored
  assert.deepEqual(t.leftover(), []);
  assert.equal(t.problems.length, 1);
});

test("test_replay_transport_url_must_match_and_leftovers_are_reported", async () => {
  const t = new ReplayTransport();
  t.load([call("m", [], 1, "https://b")]);
  await assert.rejects(t.postJson("https://a", { method: "m", params: [] }, 10));
  assert.equal(t.leftover().length, 1);
  assert.match(t.leftover()[0]!, /expected call not made: https:\/\/b m/);
});

test("test_replay_transport_raises_recorded_transport_errors", async () => {
  const t = new ReplayTransport();
  t.load([{ url: "https://a", method: "m", params: [], transport_error: { message: "HTTP 429", status: 429 } }]);
  await assert.rejects(t.postJson("https://a", { method: "m", params: [] }, 10), (e: unknown) =>
    e instanceof TransportError && e.message === "HTTP 429" && e.status === 429);
});

test("test_scripted_entropy_checks_fn_and_n_in_order", () => {
  const e = new ScriptedEntropy([
    { fn: "randbelow", n: 100, value: 86 },
    { fn: "token_hex", n: 8, value: "622c0ce632fb5390" },
    { fn: "token_bytes", n: 2, value: "abff" },
  ]);
  assert.equal(e.randBelow(100), 86);
  assert.equal(e.tokenHex(8), "622c0ce632fb5390");
  assert.deepEqual([...e.tokenBytes(2)], [0xab, 0xff]);
  assert.deepEqual(e.leftover(), []);
  assert.throws(() => e.tokenHex(8), /entropy exhausted at token_hex\(8\)/);
  assert.equal(e.problems.length, 1);

  const m = new ScriptedEntropy([{ fn: "token_hex", n: 8, value: "00" }]);
  assert.throws(() => m.randBelow(100), /entropy mismatch: drew randbelow\(100\), recorded token_hex\(8\)/);
  const n = new ScriptedEntropy([{ fn: "token_hex", n: 8, value: "00" }]);
  assert.throws(() => n.tokenHex(4), /entropy mismatch/);
  const left = new ScriptedEntropy([{ fn: "randbelow", n: 100, value: 1 }]);
  assert.match(left.leftover()[0]!, /unconsumed entropy/);
});

test("test_encode_returns_rules", () => {
  assert.equal(encodeReturns("set_ui_status", undefined), null);
  assert.deepEqual(encodeReturns("tick", [{ a: Dec.parse("4.990000"), b: 5n, c: null, d: [true, "x"] }]),
    [{ a: "4.990000", b: 5, c: null, d: [true, "x"] }]);
  assert.equal(encodeReturns("tick", 2n ** 53n - 1n), 2 ** 53 - 1);
  assert.throws(() => encodeReturns("tick", 2n ** 53n), /not safe in JSON/);
  assert.throws(() => encodeReturns("tick", { x: 2 ** 53 }), /not safe in JSON/);
  assert.throws(() => encodeReturns("tick", { x: 1.5 }), /non-integral/);
  assert.throws(() => encodeReturns("tick", { x: new Uint8Array(1) }), /bytes/);
  assert.deepEqual(encodeReturns("tick", new Set(["b", "a"])), ["a", "b"]);
});

test("test_nested_undefined_fails_and_only_a_top_level_void_is_null", () => {
  assert.equal(encodeReturns("report_hint", undefined), null);
  assert.throws(() => encodeReturns("tick", { a: undefined }), /undefined at a/);
  assert.throws(() => encodeReturns("tick", [undefined]), /undefined at \[0\]/);
  assert.throws(() => snapshotJson({
    snapshot: () => ({ pending: { p: { x: undefined } }, receipts: {}, owned: new Set(), tip_total: Dec.fromInt(0), unready: new Set() }),
  } as never), /undefined at pending\.p\.x/);
});

const TRANSFER_LOG = {
  token: "0xaf88d065e77c8cC2239327C5EDb3A432268e5831", sender: "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
  recipient: "0x52908400098527886E0F7030069857D2E4169EE7", value: asUnits(4_993_742n), tx_hash: "0x" + "ab".repeat(32),
  log_index: 1, block_number: 50, block_hash: "0x" + "11".repeat(32),
};
const CANDIDATE = {
  chain: "arbitrum", token: "USDC", transfer: TRANSFER_LOG, kind: null, item: null, choices: ["chapter2", "tip"], already_restored: false,
};

test("test_candidate_json_round_trip_tags_the_transfer_class", () => {
  const json = candidateToJson(CANDIDATE);
  assert.deepEqual(Object.keys(json), ["chain", "token", "transfer", "kind", "item", "choices", "already_restored"]);
  assert.deepEqual(json.transfer, { ...TRANSFER_LOG, value: 4_993_742, class: "TransferLog" });
  assert.deepEqual(candidateFromJson(JSON.parse(JSON.stringify(json))), CANDIDATE);
  const { block_hash: _drop, ...plain } = TRANSFER_LOG; // Solana's Transfer has no block_hash
  const t = candidateToJson({ ...CANDIDATE, transfer: plain });
  assert.equal(t.transfer.class, "Transfer");
  assert.deepEqual(candidateFromJson(t).transfer, plain);
});

test("test_candidate_from_json_rejects_a_wrong_class_or_fields", () => {
  const json = candidateToJson(CANDIDATE);
  assert.throws(() => candidateFromJson({ ...json, transfer: { ...json.transfer, class: "Nope" } }), /KeyError/);
  assert.throws(() => candidateFromJson({ ...json, transfer: { ...json.transfer, class: "Transfer" } }), TypeError); // extra block_hash
  const { block_hash: _x, ...noHash } = json.transfer;
  assert.throws(() => candidateFromJson({ ...json, transfer: noHash }), TypeError); // TransferLog needs it
});

test("test_encode_returns_restore_lookup_candidates", () => {
  assert.deepEqual(encodeReturns("restore_lookup", [CANDIDATE]), [candidateToJson(CANDIDATE)]);
});

test("test_run_action_restore_apply_rebuilds_candidates_from_the_recorded_step", async () => {
  const config = buildConfig({ wallet: TRANSFER_LOG.recipient });
  const engine = new Engine(config, {});
  const recorded = [{ returns: [candidateToJson(CANDIDATE)] }];
  const [none, noneRaises] = await runAction(engine, "restore_apply", { from_step: 0, choices: null }, recorded);
  assert.deepEqual([none, noneRaises], [[], null]);
  const [added, raises] = await runAction(engine, "restore_apply", { from_step: 0, choices: { "0": "tip" } }, recorded);
  assert.equal(raises, null);
  assert.deepEqual((added as any[]).map((r) => [r.kind, r.item, r.source, r.amount]), [["tip", null, "restore", "4.993742"]]);
  const [, bad] = await runAction(engine, "restore_apply", { from_step: 0, choices: { "0": "bogus" } }, recorded);
  assert.deepEqual(bad, { type: "other" }); // not an allowed choice: ValueError -> other
  const [, badKey] = await runAction(engine, "restore_apply", { from_step: 0, choices: { x: "tip" } }, recorded);
  assert.deepEqual(badKey, { type: "other" }); // int("x") fails
});

test("test_run_action_recognises_restore_error_by_class_not_by_name", async () => {
  const engine = new Engine(buildConfig({ wallet: TRANSFER_LOG.recipient }), {});
  engine.restoreLookup = async () => { throw new RestoreError("tx_reverted"); };
  assert.deepEqual((await runAction(engine, "restore_lookup", { tx: "0x1" }, []))[1], { type: "RestoreError", code: "tx_reverted" });
  class Impostor extends Error { override name = "RestoreError"; code = "tx_reverted"; }
  engine.restoreLookup = async () => { throw new Impostor("x"); };
  assert.deepEqual((await runAction(engine, "restore_lookup", { tx: "0x1" }, []))[1], { type: "other" });
});

test("test_snapshot_json_sorts_owned_and_unready", () => {
  const fake = {
    snapshot: () => ({
      pending: { p: { n: 1 } }, receipts: {}, owned: new Set(["z", "a"]), tip_total: Dec.parse("1.5") as Dec,
      unready: new Set(["solana\u0000USDT", "base\u0000USDC", "solana\u0000USDC"]),
    }),
  };
  assert.deepEqual(snapshotJson(fake as never), {
    pending: { p: { n: 1 } }, receipts: {}, owned: ["a", "z"], tip_total: "1.5",
    unready: [["base", "USDC"], ["solana", "USDC"], ["solana", "USDT"]],
  });
});

function tampered(path: string, edit: (v: any) => void): any {
  const v = JSON.parse(JSON.stringify(vectors.find((x) => x.path === path)!.vector));
  edit(v);
  return v;
}

test("test_replay_detects_differences", async () => {
  const P = "tier2/smoke/evm_unlock_paid.json";
  await replay(tampered(P, () => {})); // untouched: passes
  await assert.rejects(replay(tampered(P, (v) => { v.steps[0].returns.units += 1; })), /step 0 \(start_session\): returns/);
  await assert.rejects(replay(tampered(P, (v) => { v.steps[0].entropy.pop(); })), /step 0 .*entropy/);
  await assert.rejects(replay(tampered(P, (v) => { v.steps[0].calls.push(v.steps[0].calls[0]); })), /expected call not made/);
  await assert.rejects(replay(tampered(P, (v) => { v.steps[0].snapshot.tip_total = "1"; })), /snapshot/);
  await assert.rejects(replay(tampered(P, (v) => { v.steps[0].internal_errors = 1; })), /internal_errors/);
  await assert.rejects(replay(tampered(P, (v) => { v.steps[0].raises = { type: "other" }; })), /raises/);
  await assert.rejects(replay({ tier: 1 }), /not a Tier 2 vector/);
});

test("test_crash_vector_counts_internal_errors", async () => {
  const v = vectors.find((x) => x.path === "tier2/evm/crash_in_one_session.json")!.vector;
  assert.ok(v.steps.some((s: any) => s.internal_errors > 0));
  await replay(v);
});
