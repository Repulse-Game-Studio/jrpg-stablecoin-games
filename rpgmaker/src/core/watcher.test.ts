/** Twin of tests/unit/test_watcher_awaiting.py and test_watcher_confirming.py. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import { MAINNET, type Chain } from "./chains.ts";
import { Dec } from "./decimal.ts";
import { TRANSFER_TOPIC, addressTopic } from "./evm.ts";
import type { Item } from "./models.ts";
import { receiptKey } from "./receipts.ts";
import { KeyError, ValueError } from "./errors.ts";
import { HINT_TIMEOUT_S, SCAN_HORIZON_S, createSession, setHint, type CreateSessionArgs } from "./sessions.ts";
import { MAX_SCAN_CALLS, SCAN_STEP_BLOCKS, advance, minAmount, resultFromMatch } from "./watcher.ts";

const CHAIN = MAINNET["arbitrum"]!; // 20 confirmations
const USDC = CHAIN.tokens["USDC"]!.address;
const USDT = CHAIN.tokens["USDT"]!.address;
const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const dec = (s: string): Dec => Dec.parse(s) as Dec;
const ITEMS: ReadonlyMap<string, Item> = new Map([
  ["chapter2", { item_id: "chapter2", code: 37, price: dec("4.99"), token: "USDC", title: "Chapter 2", min_price: dec("4.99") }],
]);
const TX = "0x" + "ab".repeat(32);
const UNITS = 4_993_742;
const hex64 = (n: number): string => n.toString(16).padStart(64, "0");

type Store = Record<string, any>;

function setup(head = 1000, kw: Partial<CreateSessionArgs> = {}) {
  const pending: Store = {};
  const receipts: Store = {};
  const chain = new FakeChain(head);
  const rec = createSession(pending, {
    kind: "unlock", item: "chapter2", title: "Chapter 2", token: "USDC", chain: "arbitrum",
    price: "4.99", code: 37, nonce: 42, start_block: 995, now: 0.0, ...kw,
  });
  return { pending, receipts, chain, rec };
}

function step(s: { pending: Store; receipts: Store; chain: FakeChain; rec: any }, now = 10.0, chain: Chain = CHAIN) {
  return advance(s.pending, s.receipts, s.rec.id, { rpc: s.chain, chain, wallet: DEV, items: ITEMS, now });
}

test("test_missing_session", async () => {
  const out = await advance({}, {}, "ps_nope", { rpc: new FakeChain(), chain: CHAIN, wallet: DEV, items: ITEMS, now: 0 });
  assert.deepEqual(out, {
    session_id: "ps_nope", status: "missing", confirmations: 0, required: 20, result: null, error: null, hint_tx: null,
  });
});

// --- amount scan --------------------------------------------------------------

test("test_scan_without_payment_advances_cursor", async () => {
  const s = setup();
  const out = await step(s);
  assert.equal(out.status, "waiting");
  assert.equal(out.error, null);
  assert.equal(s.rec.cursor, 1001);
  const [address, topics, lo, hi] = s.chain.lastGetLogs!;
  assert.equal(address, USDC);
  assert.deepEqual(topics, [TRANSFER_TOPIC, null, addressTopic(DEV)]);
  assert.deepEqual([lo, hi], [995, 1000]);
});

test("test_scan_does_nothing_when_cursor_ahead_of_head", async () => {
  const s = setup(900); // lagging endpoint
  const out = await step(s);
  assert.equal(out.status, "waiting");
  assert.equal(s.rec.cursor, 995);
  assert.ok(!s.chain.calls.includes("get_logs"));
});

test("test_scan_is_bounded_per_step", async () => {
  const s = setup(995 + 3 * SCAN_STEP_BLOCKS);
  await step(s);
  assert.equal(s.rec.cursor, 995 + SCAN_STEP_BLOCKS);
});

test("test_scan_matches_exact_amount_and_starts_confirming", async () => {
  const s = setup(1000);
  s.chain.logs.push(transferLog(USDC, PAYER, DEV, UNITS, { block: 998, tx_hash: TX, log_index: 2 }));
  const out = await step(s);
  assert.equal(out.status, "confirming");
  assert.equal(out.confirmations, 3);
  assert.equal(out.required, 20);
  assert.equal(s.rec.state, "confirming");
  assert.deepEqual(s.rec.match, {
    tx_hash: TX, log_index: 2, block: 998, block_hash: "0x" + hex64(998), value: UNITS, payer: PAYER, recipient: DEV,
  });
});

test("test_scan_ignores_wrong_amount_token_recipient_and_old_blocks", async () => {
  const s = setup(1000);
  const other = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359";
  s.chain.logs.push(
    transferLog(USDC, PAYER, DEV, UNITS + 1, { block: 998, tx_hash: TX }), // amount off by 1
    transferLog(USDT, PAYER, DEV, UNITS, { block: 998, tx_hash: TX }), // other token
    transferLog(USDC, PAYER, other, UNITS, { block: 998, tx_hash: TX }), // other recipient
    transferLog(USDC, PAYER, DEV, UNITS, { block: 990, tx_hash: TX }), // before session
  );
  assert.equal((await step(s)).status, "waiting");
  assert.equal(s.rec.match, null);
});

test("test_scan_picks_earliest_match", async () => {
  const s = setup(1000);
  s.chain.logs.push(
    transferLog(USDC, PAYER, DEV, UNITS, { block: 999, tx_hash: "0x" + "02".repeat(32) }),
    transferLog(USDC, PAYER, DEV, UNITS, { block: 997, tx_hash: "0x" + "01".repeat(32) }),
  );
  await step(s);
  assert.equal(s.rec.match.tx_hash, "0x" + "01".repeat(32));
});

test("test_scan_breaks_block_ties_by_log_index", async () => {
  const s = setup(1000);
  s.chain.logs.push(
    transferLog(USDC, PAYER, DEV, UNITS, { block: 997, tx_hash: "0x" + "02".repeat(32), log_index: 9 }),
    transferLog(USDC, PAYER, DEV, UNITS, { block: 997, tx_hash: "0x" + "01".repeat(32), log_index: 3 }),
  );
  await step(s);
  assert.equal(s.rec.match.log_index, 3);
});

test("test_scan_skips_transfer_already_used_as_receipt", async () => {
  const s = setup(1000);
  s.chain.logs.push(transferLog(USDC, PAYER, DEV, UNITS, { block: 998, tx_hash: TX, log_index: 2 }));
  s.receipts[receiptKey("arbitrum", TX, 2)] = { kind: "unlock" };
  assert.equal((await step(s)).status, "waiting");
});

test("test_horizon_closes_session", async () => {
  const s = setup();
  const out = await step(s, SCAN_HORIZON_S);
  assert.equal(out.status, "closed");
  assert.equal(out.result, null);
  assert.deepEqual(s.pending, {});
});

test("test_network_outage_is_reported_and_state_kept", async () => {
  const s = setup();
  s.chain.down = true;
  const out = await step(s);
  assert.equal(out.error, "network_unavailable");
  assert.equal(s.rec.cursor, 995);
  assert.equal(s.rec.state, "awaiting");
});

// --- hints (browser page / pasted hash) --------------------------------------

const receiptWith = (logs: Record<string, any>[], status = "0x1") => ({ status, logs });

test("test_hint_not_mined_yet_keeps_waiting_without_scanning", async () => {
  const s = setup();
  setHint(s.pending, s.rec.id, TX, 5.0);
  const out = await step(s, 10.0);
  assert.equal(out.status, "waiting");
  assert.equal(out.error, null);
  assert.equal(s.rec.hint_tx, TX);
  assert.ok(!s.chain.calls.includes("get_logs"));
});

test("test_hint_dropped_after_timeout_then_scan_resumes", async () => {
  const s = setup();
  setHint(s.pending, s.rec.id, TX, 0.0);
  await step(s, HINT_TIMEOUT_S);
  assert.equal(s.rec.hint_tx, null);
  assert.ok(s.chain.calls.includes("get_logs"));
});

test("test_hint_reverted", async () => {
  const s = setup();
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = receiptWith([], "0x0");
  const out = await step(s);
  assert.equal(out.error, "tx_reverted");
  assert.equal(s.rec.hint_tx, null);
  assert.equal(out.hint_tx, TX); // names the hint judged, as on Solana
});

test("test_hint_not_a_payment", async () => {
  const s = setup();
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = receiptWith([transferLog(USDT, PAYER, DEV, UNITS, { block: 998, tx_hash: TX })]);
  const out = await step(s);
  assert.equal(out.error, "tx_not_a_payment");
  assert.equal(s.rec.hint_tx, null);
  assert.equal(out.hint_tx, TX);
});

test("test_hint_from_before_session_is_rejected", async () => {
  const s = setup();
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = receiptWith([transferLog(USDC, PAYER, DEV, UNITS, { block: 900, tx_hash: TX })]);
  assert.equal((await step(s)).error, "tx_not_a_payment");
});

test("test_hint_receipt_without_logs_is_not_a_payment", async () => {
  // Python: receipt.get("logs") or [] -- a missing or null "logs" is an empty list.
  for (const receipt of [{ status: "0x1" }, { status: "0x1", logs: null }, {}]) {
    const s = setup();
    setHint(s.pending, s.rec.id, TX, 0.0);
    s.chain.receipts[TX] = receipt;
    assert.equal((await step(s)).error, "tx_not_a_payment");
  }
});

test("test_hint_malformed_receipt_raises", async () => {
  // Python: a non-dict receipt has no .get (AttributeError); the Engine contains it.
  for (const receipt of ["0x1", [1], 5, true]) {
    const s = setup();
    setHint(s.pending, s.rec.id, TX, 0.0);
    s.chain.receipts[TX] = receipt;
    await assert.rejects(step(s), TypeError);
  }
  const s = setup();
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = { status: "0x1", logs: "abc" }; // iterates characters in Python
  await assert.rejects(step(s), TypeError);
});

test("test_hint_accepts_any_amount_at_or_above_min_price", async () => {
  // exchange payer: could not send the encoded decimals, sent a round 5.00
  const s = setup(1000);
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = receiptWith([transferLog(USDC, PAYER, DEV, 5_000_000, { block: 999, tx_hash: TX })]);
  const out = await step(s);
  assert.equal(out.status, "confirming");
  assert.equal(s.rec.match.value, 5_000_000);
});

test("test_hint_underpaid_unlock_is_final", async () => {
  const s = setup();
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = receiptWith([transferLog(USDC, PAYER, DEV, 4_000_000, { block: 999, tx_hash: TX })]);
  const out = await step(s);
  assert.equal(out.status, "underpaid");
  assert.equal(out.result!.status, "underpaid");
  assert.equal(String(out.result!.amount), "4.000000");
  assert.equal(out.result!.tx_hash, TX);
  assert.equal(out.result!.payer, PAYER);
  assert.deepEqual(s.pending, {});
  assert.deepEqual(s.receipts, {});
});

test("test_hint_tip_accepts_any_positive_amount", async () => {
  const s = setup(1000, { kind: "tip", item: null, code: 0, price: "3.00" });
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = receiptWith([transferLog(USDC, PAYER, DEV, 1_000_000, { block: 999, tx_hash: TX })]);
  assert.equal((await step(s)).status, "confirming");
});

test("test_hint_with_several_transfers_takes_largest_qualifying", async () => {
  const s = setup(1000);
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = receiptWith([
    transferLog(USDC, PAYER, DEV, 1_000_000, { block: 999, tx_hash: TX, log_index: 0 }),
    transferLog(USDC, PAYER, DEV, UNITS, { block: 999, tx_hash: TX, log_index: 1 }),
  ]);
  await step(s);
  assert.equal(s.rec.match.log_index, 1);
});

test("test_hint_largest_tie_keeps_the_first", async () => {
  // Python max() returns the first maximal element.
  const s = setup(1000);
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = receiptWith([
    transferLog(USDC, PAYER, DEV, UNITS, { block: 999, tx_hash: TX, log_index: 4 }),
    transferLog(USDC, PAYER, DEV, UNITS, { block: 999, tx_hash: TX, log_index: 1 }),
  ]);
  await step(s);
  assert.equal(s.rec.match.log_index, 4);
});

test("test_scan_step_is_bounded_by_calls_when_rpc_reports_its_range", async () => {
  const s = setup(995 + 100_000);
  s.chain.logRange = 50; // a stingy public endpoint
  await step(s);
  assert.equal(s.rec.cursor, 995 + 50 * MAX_SCAN_CALLS);
});

test("test_min_amount", () => {
  assert.equal(String(minAmount({ kind: "unlock", item: "chapter2", price: "9.99" }, ITEMS)), "4.99");
  assert.equal(String(minAmount({ kind: "unlock", item: "gone", price: "9.99" }, ITEMS)), "9.99");
  assert.equal(String(minAmount({ kind: "tip", item: null, price: "3.00" }, ITEMS)), "0.000001");
});

test("test_result_from_match", () => {
  const rec = { id: "ps_1", kind: "unlock", item: "chapter2", token: "USDC" };
  const m = { tx_hash: TX, payer: PAYER, value: UNITS };
  assert.deepEqual(resultFromMatch(rec, "paid", "arbitrum", m), {
    status: "paid", kind: "unlock", item: "chapter2", amount: dec("4.993742"), token: "USDC", chain: "arbitrum",
    tx_hash: TX, payer: PAYER, session_id: "ps_1", error: null,
  });
});

test("test_session_missing_a_field_raises_key_error", async () => {
  // Python indexes rec["state"]: a record without it is a KeyError (contained by the Engine), not a guess.
  const s = setup();
  delete s.rec.state;
  await assert.rejects(step(s), KeyError);
  assert.deepEqual(s.chain.calls, []);
});

// --- confirming (test_watcher_confirming.py) ----------------------------------

async function matched(head = 1000, block = 998) {
  const s = setup(head);
  s.chain.logs.push(transferLog(USDC, PAYER, DEV, UNITS, { block, tx_hash: TX, log_index: 2 }));
  const out = await step(s);
  assert.equal(out.status, "confirming");
  return s;
}

test("test_counts_confirmations_until_required", async () => {
  const s = await matched();
  s.chain.head = 1010;
  const out = await step(s);
  assert.deepEqual([out.status, out.confirmations, out.required], ["confirming", 13, 20]);
});

test("test_paid_writes_receipt_and_removes_session", async () => {
  const s = await matched();
  s.chain.head = 998 + 19; // exactly 20 confirmations
  const out = await step(s);
  assert.equal(out.status, "paid");
  const r = out.result!;
  assert.deepEqual([r.status, r.kind, r.item, r.token, r.chain], ["paid", "unlock", "chapter2", "USDC", "arbitrum"]);
  assert.equal(String(r.amount), "4.993742");
  assert.equal(r.tx_hash, TX);
  assert.equal(r.payer, PAYER);
  assert.equal(r.session_id, s.rec.id);
  assert.deepEqual(s.pending, {});
  const stored = s.receipts[receiptKey("arbitrum", TX, 2)];
  assert.equal(stored.source, "payment");
  assert.equal(stored.block, 998);
  assert.equal(stored.amount, "4.993742");
  assert.deepEqual(stored, {
    chain: "arbitrum", tx_hash: TX, log_index: 2, block: 998, kind: "unlock", item: "chapter2", amount: "4.993742",
    token: "USDC", payer: PAYER, recipient: DEV, source: "payment",
  });
});

test("test_confirmations_override_is_respected", async () => {
  const s = await matched();
  const fast = { ...CHAIN, confirmations: 3 };
  const out = await step(s, 1, fast);
  assert.equal(out.status, "paid");
});

test("test_reorg_returns_to_awaiting_and_rewinds_cursor", async () => {
  const s = await matched();
  s.chain.hashes.set(998, "0x" + "ee".repeat(32)); // block 998 replaced
  s.chain.logs = []; // transfer no longer in canonical chain
  s.chain.head = 1005;
  const out = await step(s);
  assert.equal(out.status, "reorged");
  assert.equal(s.rec.state, "awaiting");
  assert.equal(s.rec.match, null);
  assert.equal(s.rec.cursor, 998);
});

test("test_reorg_then_transfer_reappears_in_new_block", async () => {
  const s = await matched();
  s.chain.hashes.set(998, "0x" + "ee".repeat(32));
  s.chain.logs = [transferLog(USDC, PAYER, DEV, UNITS, { block: 1001, tx_hash: TX, log_index: 0 })];
  s.chain.head = 1002;
  assert.equal((await step(s)).status, "reorged");
  assert.equal((await step(s)).status, "confirming");
  assert.equal(s.rec.match.block, 1001);
});

test("test_block_hash_gone_is_a_reorg", async () => {
  const s = await matched();
  s.chain.getBlockHash = async () => null;
  assert.equal((await step(s)).status, "reorged");
});

test("test_lagging_endpoint_below_match_block_is_not_a_reorg", async () => {
  const s = await matched();
  s.chain.head = 990; // another endpoint, behind
  const out = await step(s);
  assert.equal(out.status, "confirming");
  assert.equal(out.confirmations, 0);
  assert.equal(s.rec.state, "confirming");
});

test("test_outage_while_confirming_keeps_match", async () => {
  const s = await matched();
  s.chain.down = true;
  const out = await step(s);
  assert.equal(out.error, "network_unavailable");
  assert.equal(s.rec.state, "confirming");
  assert.notEqual(s.rec.match, null);
});

test("test_cancelled_or_expired_ui_status_does_not_stop_confirmation", async () => {
  const s = await matched();
  s.rec.ui_status = "cancelled";
  s.chain.head = 1100;
  assert.equal((await step(s, 4000.0)).status, "paid");
});

test("test_hinted_value_at_2_pow_53_is_refused_not_rounded", async () => {
  // Ruling 3: match.value is a JSON number, so 2^53 units cannot be stored exactly. The watcher
  // throws (the Engine contains it as internal_error) and leaves the session untouched.
  const s = setup(1000);
  setHint(s.pending, s.rec.id, TX, 0.0);
  s.chain.receipts[TX] = receiptWith([transferLog(USDC, PAYER, DEV, 2n ** 53n, { block: 999, tx_hash: TX })]);
  await assert.rejects(step(s), ValueError);
  assert.equal(s.rec.state, "awaiting");
  assert.equal(s.rec.match, null);
  // the largest storable value still works
  const ok = setup(1000);
  setHint(ok.pending, ok.rec.id, TX, 0.0);
  ok.chain.receipts[TX] = receiptWith([transferLog(USDC, PAYER, DEV, 2n ** 53n - 1n, { block: 999, tx_hash: TX })]);
  assert.equal((await step(ok)).status, "confirming");
  assert.equal(ok.rec.match.value, Number(2n ** 53n - 1n));
});

test("test_scan_reads_units_only_for_a_qualifying_log", async () => {
  // Python evaluates rec["units"] after _qualifies: without it a record scans fine until a log qualifies.
  const noLogs = setup();
  delete noLogs.rec.units;
  assert.equal((await step(noLogs)).status, "waiting");
  const wrongToken = setup();
  delete wrongToken.rec.units;
  wrongToken.chain.logs.push(transferLog(USDT, PAYER, DEV, UNITS, { block: 999, tx_hash: TX }));
  assert.equal((await step(wrongToken)).status, "waiting");
  const qualifying = setup();
  delete qualifying.rec.units;
  qualifying.chain.logs.push(transferLog(USDC, PAYER, DEV, UNITS, { block: 999, tx_hash: TX }));
  await assert.rejects(step(qualifying), KeyError);
});
