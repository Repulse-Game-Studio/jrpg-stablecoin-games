/** Twin of tests/unit/test_restore.py, plus the JS-only edges (error identity, huge values, choices keys). */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import { MAINNET, type Chain } from "./chains.ts";
import { addItem, buildConfig } from "./config.ts";
import { asUnits } from "./amounts.ts";
import { Dec } from "./decimal.ts";
import { ValueError } from "./errors.ts";
import { receiptKey } from "./receipts.ts";
import { RpcError } from "./rpc.ts";
import { RestoreError, TIP_CHOICE, apply, classify, lookup } from "./restore.ts";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const PAYER = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359";
const ARB_USDC = MAINNET["arbitrum"]!.tokens["USDC"]!.address;
const BASE_USDC = MAINNET["base"]!.tokens["USDC"]!.address;
const TX = "0x" + "ab".repeat(32);

function cfg() {
  const c = buildConfig({ wallet: DEV, previous_wallets: [OLD] });
  addItem(c, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  addItem(c, "ost", { code: 2, price: "2.00", token: "USDT", title: "OST" });
  return c;
}

// --- classify -------------------------------------------------------------------

test("test_classify_item_code", () => {
  assert.deepEqual(classify(cfg(), asUnits(4_993_742n)), ["unlock", "chapter2", []]);
});

test("test_classify_tip_code", () => {
  assert.deepEqual(classify(cfg(), asUnits(3_000_017n)), ["tip", null, []]);
});

test("test_classify_round_amount_asks_player", () => {
  const [kind, item, choices] = classify(cfg(), asUnits(5_000_000n));
  assert.equal(kind, null);
  assert.equal(item, null);
  assert.deepEqual(choices, ["chapter2", "ost", TIP_CHOICE]);
});

test("test_classify_unknown_code_offers_affordable_items", () => {
  const [kind, , choices] = classify(cfg(), asUnits(2_509_942n)); // 2.50 | 99 | 42, code 99 unknown
  assert.equal(kind, null);
  assert.deepEqual(choices, ["ost", TIP_CHOICE]);
});

test("test_classify_known_code_but_below_min_price_asks", () => {
  const [kind, , choices] = classify(cfg(), asUnits(1_003_742n)); // code 37 but only ~1.00 paid
  assert.equal(kind, null);
  assert.deepEqual(choices, [TIP_CHOICE]);
});

test("test_classify_amount_exactly_at_min_price_counts", () => {
  // Task 7 review gap: both comparisons are inclusive (Python `amount >= min_price`, `min_price <= amount`).
  const c = cfg();
  c.items["chapter2"]!.min_price = Dec.parse("4.993742") as Dec;
  assert.deepEqual(classify(c, asUnits(4_993_742n)), ["unlock", "chapter2", []]); // code 37, paid exactly min_price
  c.items["chapter2"]!.min_price = Dec.parse("4.993743") as Dec;
  assert.deepEqual(classify(c, asUnits(4_993_742n)), [null, null, ["ost", TIP_CHOICE]]); // one unit short
  // affordability: a round amount equal to an item's min_price offers it, one unit less does not
  const r = cfg();
  r.items["chapter2"]!.min_price = Dec.parse("5.00") as Dec;
  assert.deepEqual(classify(r, asUnits(5_000_000n))[2], ["chapter2", "ost", TIP_CHOICE]);
  r.items["chapter2"]!.min_price = Dec.parse("5.000001") as Dec;
  assert.deepEqual(classify(r, asUnits(5_000_000n))[2], ["ost", TIP_CHOICE]);
  r.items["ost"]!.min_price = Dec.parse("2.509942") as Dec; // unknown code 99: affordable list, inclusive
  assert.deepEqual(classify(r, asUnits(2_509_942n))[2], ["ost", TIP_CHOICE]);
  r.items["ost"]!.min_price = Dec.parse("2.509943") as Dec;
  assert.deepEqual(classify(r, asUnits(2_509_942n))[2], [TIP_CHOICE]);
});

test("test_classify_refuses_values_decode_cannot_hold", () => {
  // Ruling 2: Python's decode overflows its 28-digit context from 10**32 units; the JS port throws too.
  assert.throws(() => classify(cfg(), asUnits(10n ** 32n)), ValueError);
  assert.deepEqual(classify(cfg(), asUnits(10n ** 32n - 10_000n * 100n))[0], null); // just below: whole-cent amount
});

// --- lookup ---------------------------------------------------------------------

function makeChains(receiptsByChain: Record<string, any> = {}) {
  const chains: Record<string, Chain> = { arbitrum: MAINNET["arbitrum"]!, base: MAINNET["base"]! };
  const fakes: Record<string, FakeChain> = { arbitrum: new FakeChain(), base: new FakeChain() };
  for (const [k, r] of Object.entries(receiptsByChain)) fakes[k]!.receipts[TX] = r;
  return { chains, fakes };
}

function runLookup(
  chains: Record<string, Chain>, fakes: Record<string, FakeChain>, receipts: Record<string, unknown> = {}, tx = TX,
) {
  return lookup(tx, { config: cfg(), chains, rpc_for: (k) => fakes[k], receipts });
}

const restoreCode = (code: string) => (e: unknown) => e instanceof RestoreError && e.code === code;

test("test_lookup_finds_chain_and_classifies", async () => {
  const { chains, fakes } = makeChains({
    base: { status: "0x1", logs: [transferLog(BASE_USDC, PAYER, DEV, 4_993_742, { block: 50, tx_hash: TX, log_index: 1 })] },
  });
  const [c] = await runLookup(chains, fakes);
  assert.deepEqual([c!.chain, c!.token, c!.kind, c!.item, c!.already_restored], ["base", "USDC", "unlock", "chapter2", false]);
  assert.deepEqual(fakes["arbitrum"]!.calls, ["get_transaction_receipt"]); // tried first, not found
});

test("test_lookup_accepts_previous_wallet", async () => {
  const { chains, fakes } = makeChains({
    arbitrum: { status: "0x1", logs: [transferLog(ARB_USDC, PAYER, OLD, 3_000_005, { block: 50, tx_hash: TX })] },
  });
  assert.equal((await runLookup(chains, fakes))[0]!.kind, "tip");
});

test("test_lookup_marks_already_restored", async () => {
  const { chains, fakes } = makeChains({
    arbitrum: { status: "0x1", logs: [transferLog(ARB_USDC, PAYER, DEV, 4_993_742, { block: 50, tx_hash: TX, log_index: 3 })] },
  });
  const receipts = { [receiptKey("arbitrum", TX, 3)]: {} };
  assert.equal((await runLookup(chains, fakes, receipts))[0]!.already_restored, true);
});

const LOOKUP_ERRORS: [string, any, string][] = [
  ["reverted", { status: "0x0", logs: [] }, "tx_reverted"],
  ["not to the developer", { status: "0x1", logs: [transferLog(ARB_USDC, PAYER, PAYER, 5_000_000, { block: 1, tx_hash: TX })] },
    "tx_not_a_payment"],
  ["no logs", { status: "0x1", logs: [] }, "tx_not_a_payment"],
  ["logs missing", { status: "0x1" }, "tx_not_a_payment"],
  ["zero value", { status: "0x1", logs: [transferLog(ARB_USDC, PAYER, DEV, 0, { block: 1, tx_hash: TX })] }, "tx_not_a_payment"],
];
for (const [name, receipt, code] of LOOKUP_ERRORS) {
  test(`test_lookup_errors[${name}]`, async () => {
    const { chains, fakes } = makeChains({ arbitrum: receipt });
    await assert.rejects(runLookup(chains, fakes), restoreCode(code));
  });
}

test("test_lookup_not_found_anywhere", async () => {
  const { chains, fakes } = makeChains();
  await assert.rejects(runLookup(chains, fakes), restoreCode("tx_not_found"));
});

test("test_lookup_network_trouble_is_reported_when_not_found", async () => {
  const { chains, fakes } = makeChains();
  fakes["base"]!.down = true;
  await assert.rejects(runLookup(chains, fakes), restoreCode("network_unavailable"));
});

test("test_lookup_a_found_tx_beats_network_trouble_on_an_earlier_chain", async () => {
  const { chains, fakes } = makeChains({
    base: { status: "0x1", logs: [transferLog(BASE_USDC, PAYER, DEV, 4_993_742, { block: 50, tx_hash: TX })] },
  });
  fakes["arbitrum"]!.down = true;
  assert.equal((await runLookup(chains, fakes))[0]!.chain, "base");
});

test("test_lookup_node_error_counts_as_network_trouble", async () => {
  const { chains, fakes } = makeChains();
  fakes["arbitrum"]!.getTransactionReceipt = async () => { throw new RpcError(-32000, "boom"); };
  await assert.rejects(runLookup(chains, fakes), restoreCode("network_unavailable"));
});

test("test_lookup_invalid_hash", async () => {
  const { chains, fakes } = makeChains();
  await assert.rejects(runLookup(chains, fakes, {}, "0x123"), restoreCode("invalid_hash"));
  assert.deepEqual(fakes["arbitrum"]!.calls, []);
});

test("test_lookup_rules_out_solana_for_an_evm_hash_without_touching_it", async () => {
  // The registry holds Solana; its backend only has to refuse the 0x hash, never reach the network.
  const { chains, fakes } = makeChains();
  chains["solana"] = MAINNET["solana"]!;
  await assert.rejects(runLookup(chains, fakes), restoreCode("tx_not_found"));
});

test("test_lookup_malformed_log_rejects_the_whole_lookup", async () => {
  // Only RpcUnavailable / RpcError are swallowed (Python): a bad log is a plain error, not a RestoreError.
  const bad = transferLog(ARB_USDC, PAYER, DEV, 5_000_000, { block: 1, tx_hash: TX });
  bad["data"] = "0xzz";
  const { chains, fakes } = makeChains({ arbitrum: { status: "0x1", logs: [bad] } });
  await assert.rejects(runLookup(chains, fakes), (e: unknown) => e instanceof ValueError && !(e instanceof RestoreError));
});

test("test_lookup_value_decode_cannot_hold_rejects_the_lookup", async () => {
  const huge = transferLog(ARB_USDC, PAYER, DEV, 10n ** 32n, { block: 1, tx_hash: TX });
  const { chains, fakes } = makeChains({ arbitrum: { status: "0x1", logs: [huge] } });
  await assert.rejects(runLookup(chains, fakes), ValueError);
});

test("test_lookup_non_dict_receipt_is_a_type_error", async () => {
  const { chains, fakes } = makeChains({ arbitrum: "0x1" });
  await assert.rejects(runLookup(chains, fakes), TypeError);
});

test("test_restore_error_is_an_error_with_a_code", () => {
  const e = new RestoreError("tx_reverted");
  assert.ok(e instanceof Error);
  assert.deepEqual([e.code, e.name, e.message], ["tx_reverted", "RestoreError", "tx_reverted"]);
});

// --- apply ----------------------------------------------------------------------

test("test_apply_auto_and_chosen_candidates", async () => {
  const { chains, fakes } = makeChains({
    arbitrum: {
      status: "0x1", logs: [
        transferLog(ARB_USDC, PAYER, DEV, 4_993_742, { block: 50, tx_hash: TX, log_index: 0 }),
        transferLog(ARB_USDC, PAYER, DEV, 5_000_000, { block: 50, tx_hash: TX, log_index: 1 }),
      ],
    },
  });
  const cands = await runLookup(chains, fakes);
  const store: Record<string, any> = {};
  const added = apply(store, cands, { 1: "ost" });
  assert.deepEqual(added.map((r) => [r.kind, r.item, r.source]), [["unlock", "chapter2", "restore"], ["unlock", "ost", "restore"]]);
  assert.deepEqual(apply(store, cands, { 1: "ost" }), []); // idempotent
  assert.equal(added[1]!.amount, "5.000000");
});

test("test_apply_skips_unchosen_and_rejects_invalid_choice", async () => {
  const { chains, fakes } = makeChains({
    arbitrum: { status: "0x1", logs: [transferLog(ARB_USDC, PAYER, DEV, 1_000_000, { block: 50, tx_hash: TX })] },
  });
  const cands = await runLookup(chains, fakes);
  assert.deepEqual(apply({}, cands), []);
  assert.throws(() => apply({}, cands, { 0: "chapter2" }), ValueError); // 1.00 can't buy chapter2
  assert.equal(apply({}, cands, { 0: TIP_CHOICE })[0]!.kind, "tip");
});

test("test_apply_choice_keys_are_candidate_indexes_and_null_means_unchosen", async () => {
  const { chains, fakes } = makeChains({
    arbitrum: { status: "0x1", logs: [transferLog(ARB_USDC, PAYER, DEV, 1_000_000, { block: 50, tx_hash: TX })] },
  });
  const cands = await runLookup(chains, fakes);
  assert.deepEqual(apply({}, cands, { 0: null }), []);
  assert.deepEqual(apply({}, cands, { 5: TIP_CHOICE }), []); // no such candidate: ignored
  assert.equal(apply({}, cands, { "0": TIP_CHOICE }).length, 1); // string key, as a JSON object has it
});
