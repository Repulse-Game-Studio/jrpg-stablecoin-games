/** Twin of tests/unit/test_reverify.py and the reverify tests of test_backends.py, plus pinned quirks. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import { asUnits } from "./amounts.ts";
import { MAINNET } from "./chains.ts";
import type { TransferLog } from "./evm.ts";
import { Dec } from "./decimal.ts";
import { ValueError } from "./errors.ts";
import { addReceipt, computeOwnership, makeReceipt } from "./receipts.ts";
import { RpcError } from "./rpc.ts";
import { FUTURE_TOLERANCE_BLOCKS, checkEvmReceipt, reverifyReceipts } from "./reverify.ts";

const ARB = MAINNET["arbitrum"]!;
const USDC = ARB.tokens["USDC"]!.address;
const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const PAYER = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359";
const TX = "0x" + "ab".repeat(32);

type Store = Record<string, any>;

function storeWith(o: { value?: number; block?: number; recipient?: string; token?: string; chain?: string; log_index?: number } = {}): Store {
  const store: Store = {};
  const t: TransferLog = {
    token: USDC, sender: PAYER, recipient: o.recipient ?? DEV, value: asUnits(BigInt(o.value ?? 4_993_742)), tx_hash: TX,
    log_index: o.log_index ?? 2, block_number: o.block ?? 900, block_hash: "0x" + "11".repeat(32),
  };
  addReceipt(store, makeReceipt({ chain: o.chain ?? "arbitrum", transfer: t, kind: "unlock", item: "chapter2", token: o.token ?? "USDC", source: "payment" }));
  return store;
}

const run = (
  store: Store, chain: FakeChain, wallets: readonly string[] | Record<string, readonly string[]> = [DEV],
  on_error: (e: unknown) => void = () => undefined, // quiet: removals and failed checks are reported
) => reverifyReceipts(store, { rpc_for: () => chain, chains: MAINNET, wallets, on_error });

const genuine = (chain: FakeChain, kw: { value?: number; block?: number; tx_hash?: string; log_index?: number } = {}) =>
  chain.logs.push(transferLog(USDC, PAYER, DEV, kw.value ?? 4_993_742, { block: kw.block ?? 900, tx_hash: kw.tx_hash ?? TX, log_index: kw.log_index ?? 2 }));

test("test_genuine_receipt_is_kept", async () => {
  const store = storeWith();
  const chain = new FakeChain(1000);
  genuine(chain);
  assert.deepEqual(await run(store, chain), []);
  assert.equal(Object.keys(store).length, 1);
});

test("test_lookup_is_scoped_to_the_receipt_block", async () => {
  const chain = new FakeChain(1000);
  await run(storeWith(), chain);
  const [address, topics, lo, hi] = chain.lastGetLogs!;
  assert.deepEqual([address, lo, hi], [USDC, 900, 900]);
  assert.deepEqual((topics as unknown[])[2], ["0x" + "0".repeat(24) + DEV.slice(2).toLowerCase()]);
});

const MISMATCHES: [string, { value?: number; tx_hash?: string; log_index?: number }][] = [
  ["amount differs", { value: 4_993_743 }],
  ["different log in same tx", { log_index: 3 }],
  ["different tx", { tx_hash: "0x" + "cd".repeat(32) }],
];
for (const [name, kw] of MISMATCHES) {
  test(`test_mismatching_log_revokes[${name}]`, async () => {
    const store = storeWith();
    const chain = new FakeChain(1000);
    genuine(chain, kw);
    assert.equal((await run(store, chain)).length, 1);
    assert.deepEqual(store, {});
  });
}

test("test_invented_receipt_with_no_log_is_revoked", async () => {
  assert.equal((await run(storeWith(), new FakeChain(1000))).length, 1);
});

test("test_outage_keeps_everything", async () => {
  const store = storeWith();
  const chain = new FakeChain(1000);
  chain.down = true;
  assert.deepEqual(await run(store, chain), []);
  assert.equal(Object.keys(store).length, 1);
});

test("test_node_error_keeps_the_receipt", async () => {
  const store = storeWith();
  const chain = new FakeChain(1000);
  chain.getLogs = async () => { throw new RpcError(-32000, "boom"); };
  assert.deepEqual(await run(store, chain), []);
});

test("test_block_slightly_ahead_of_lagging_head_is_kept", async () => {
  const store = storeWith({ block: 1005 });
  assert.deepEqual(await run(store, new FakeChain(1000)), []);
  assert.equal(Object.keys(store).length, 1);
});

test("test_impossible_future_block_is_revoked", async () => {
  const store = storeWith({ block: 1000 + FUTURE_TOLERANCE_BLOCKS + 1 });
  assert.equal((await run(store, new FakeChain(1000))).length, 1);
  const edge = storeWith({ block: 1000 + FUTURE_TOLERANCE_BLOCKS });
  assert.deepEqual(await run(edge, new FakeChain(1000)), []);
});

test("test_previous_wallet_payments_survive_wallet_change", async () => {
  const store = storeWith({ recipient: OLD });
  const chain = new FakeChain(1000);
  chain.logs.push(transferLog(USDC, PAYER, OLD, 4_993_742, { block: 900, tx_hash: TX, log_index: 2 }));
  assert.deepEqual(await run(store, chain, [DEV, OLD]), []);
  assert.notDeepEqual(await run(store, chain, [DEV]), []);
});

test("test_unknown_chain_is_left_alone", async () => {
  const store = storeWith({ chain: "tron" });
  assert.deepEqual(await run(store, new FakeChain(1000)), []);
  assert.equal(Object.keys(store).length, 1);
});

test("test_token_not_on_chain_is_revoked", async () => {
  const store = storeWith({ chain: "base", token: "USDT" });
  assert.equal((await run(store, new FakeChain(1000))).length, 1);
});

const JUNK: [string, Record<string, unknown>][] = [
  ["empty", {}],
  ["chain only", { chain: "arbitrum" }],
  ["bad log_index", { chain: "arbitrum", tx_hash: TX, log_index: "x", block: 1, amount: "1", token: "USDC" }],
];
for (const [name, junk] of JUNK) {
  test(`test_malformed_entries_are_removed[${name}]`, async () => {
    const store = storeWith();
    const chain = new FakeChain(1000);
    genuine(chain);
    store["junk"] = junk;
    assert.deepEqual(await run(store, chain), ["junk"]);
    assert.equal(Object.keys(store).length, 1);
  });
}

// --- the Python accept/reject surface of the malformed-entry net -------------------------------------

const good = (over: Record<string, unknown>) => ({
  chain: "arbitrum", tx_hash: TX, log_index: 2, block: 900, amount: "4.993742", token: "USDC", ...over,
});

test("test_malformed_field_forms_follow_python_int_and_decimal", async () => {
  // dropped: Python raises KeyError / TypeError / ValueError / AttributeError / InvalidOperation
  const dropped: Record<string, unknown>[] = [
    good({ tx_hash: 5 }), good({ tx_hash: null }), good({ log_index: null }), good({ log_index: [1] }),
    good({ block: "0x10" }), good({ amount: "abc" }), good({ amount: "1.0000001" }), good({ amount: null }),
    good({ amount: [1] }), good({ amount: {} }), good({ amount: "NaN" }), good({ amount: "sNaN" }),
    good({ chain: ["arbitrum"] }), good({ chain: {} }),
  ];
  // kept for further judging (unknown chain / no wallet for the family / matched): Python converts them
  const parsed: Record<string, unknown>[] = [
    good({ log_index: "2" }), good({ log_index: " 2 " }), good({ log_index: 2.5 }), good({ log_index: true }),
    good({ block: "900" }), good({ amount: 5 }), good({ amount: 1.5 }), good({ amount: true }), good({ amount: " 4.993742 " }),
  ];
  for (const rec of [...dropped, ...parsed]) {
    const chain = new FakeChain(1000);
    const store: Store = { k: rec };
    const errors: unknown[] = [];
    const removed = await reverifyReceipts(store, {
      rpc_for: () => chain, chains: MAINNET, wallets: { evm: [], solana: [] }, on_error: (e) => errors.push(e),
    });
    assert.deepEqual(removed, dropped.includes(rec) ? ["k"] : [], JSON.stringify(rec));
    assert.equal(errors.length, dropped.includes(rec) ? 1 : 0, "a removal is reported with its reason");
  }
});

test("test_a_hashable_unknown_token_name_is_dropped", async () => {
  assert.deepEqual(await run({ k: good({ token: 5 }) }, new FakeChain(1000)), ["k"]);
});

// Python: a receipt whose own fields are invalid (an out-of-range or infinite amount, a chain, token
// or item that cannot be a key, a non-str tx hash, a non-int log index) is removed with its reason
// reported to on_error, whatever chain it names; the run goes on.
const INVALID: [string, Record<string, unknown>][] = [
  ["infinite amount", { amount: "Infinity" }],
  ["amount past Emax", { amount: "1E+999999" }],
  ["amount near Emax", { amount: "1E+999993" }],
  ["negative amount past Emax", { amount: "-1E+999995" }],
  ["amount literal past 2^53", { amount: "1E+9999999999999999" }],
  ["amount past the range", { amount: "1E+41" }],
  ["unhashable token name", { token: ["USDC"] }],
  ["unhashable item", { item: ["chapter2"] }],
  ["unhashable chain", { chain: ["arbitrum"] }],
  ["tx hash not a str", { tx_hash: 5 }],
  ["block not an int", { block: "0x10" }],
];
for (const [name, corrupt] of INVALID) {
  test(`test_a_receipt_with_invalid_fields_is_removed_and_reported[${name}]`, async () => {
    const chain = new FakeChain(1000);
    genuine(chain);
    const bad = { ...good({}), ...corrupt };
    const store: Store = { "a:corrupt": bad, ...storeWith({ log_index: 2 }), ...storeWith({ log_index: 3 }) };
    const errors: unknown[] = [];
    const start = performance.now();
    assert.deepEqual(await run(store, chain, [DEV], (e) => errors.push(e)), ["a:corrupt", `arbitrum:${TX}:3`]);
    assert.ok(performance.now() - start < 2000, "quickly");
    assert.deepEqual(Object.keys(store), [`arbitrum:${TX}:2`]);
    assert.equal(errors.length, 1);
    assert.ok(errors[0] instanceof ValueError && String((errors[0] as Error).message).includes("a:corrupt"));
  });
}

// tests/unit/test_reverify.py: each field checked against its chain family before any RPC
const MALFORMED: [string, Record<string, unknown>][] = [
  ["arbitrum", { tx_hash: "0x123" }], ["arbitrum", { tx_hash: "0x" + "zz".repeat(32) }], ["arbitrum", { tx_hash: "ab".repeat(32) }],
  ["arbitrum", { tx_hash: TX + " " }], ["arbitrum", { tx_hash: TX + "\x85" }], ["arbitrum", { tx_hash: TX + "\x1c" }],
  ["arbitrum", { tx_hash: "\x1f" + TX }], ["arbitrum", { tx_hash: "0x" + "ab".repeat(33) }], ["arbitrum", { log_index: -1 }],
  ["arbitrum", { log_index: "-1" }], ["arbitrum", { block: -5 }],
  ["arbitrum", { log_index: "9".repeat(4301) }], ["arbitrum", { block: "9".repeat(100_000) }],
  ["solana", { tx_hash: "2".repeat(100_000) }], ["solana", { tx_hash: "1".repeat(64) + "\x85" }],
  ["solana", { tx_hash: TX }], ["solana", { tx_hash: "0OIl".repeat(22) }], ["solana", { tx_hash: "1".repeat(63) }],
];
for (const [chainKey, corrupt] of MALFORMED) {
  test(`test_a_type_valid_but_malformed_receipt_is_removed_before_any_rpc[${chainKey} ${JSON.stringify(corrupt).slice(0, 60)}]`, async () => {
    const node = new FakeChain(1000);
    const store: Store = { bad: { ...good({ chain: chainKey }), ...corrupt } };
    const errors: unknown[] = [];
    assert.deepEqual(await run(store, node, [DEV], (e) => errors.push(e)), ["bad"]);
    assert.deepEqual(store, {});
    assert.equal(errors.length, 1);
    assert.deepEqual(node.calls, [], "judged without the network");
  });
}

test("test_a_malformed_receipt_on_an_unknown_chain_is_kept", async () => {
  const store: Store = { old: good({ chain: "tron", tx_hash: "0x123" }) };
  assert.deepEqual(await run(store, new FakeChain(1000)), []);
  assert.equal(Object.keys(store).length, 1);
});

test("test_a_forged_out_of_range_receipt_no_longer_unlocks", async () => {
  const store: Store = { forged: { ...good({}), kind: "unlock", item: "chapter2", amount: "1E+999999" } };
  const items = new Map([["chapter2", { item_id: "chapter2", code: 37, price: Dec.parse("4.99") as Dec, token: "USDC",
    title: "Chapter 2", min_price: Dec.parse("4.99") as Dec }]]);
  assert.equal(computeOwnership(store, items)[0].size, 0);
  assert.deepEqual(await run(store, new FakeChain(1000), [DEV], () => undefined), ["forged"]);
  assert.deepEqual(store, {});
});

test("test_a_receipt_whose_check_crashes_is_kept_and_reported", async () => {
  // the backend's check failing on node data (a log whose logIndex is not hex): kept, reported
  const store = { ...storeWith({ log_index: 2 }), ...storeWith({ log_index: 3 }) };
  const chain = new FakeChain(1000);
  const bad = transferLog(USDC, PAYER, DEV, 1, { block: 900, tx_hash: TX });
  bad["logIndex"] = "0xzz";
  chain.logs.push(bad);
  const errors: unknown[] = [];
  assert.deepEqual(await run(store, chain, [DEV], (e) => errors.push(e)), []);
  assert.equal(Object.keys(store).length, 2);
  assert.equal(errors.length, 2);
});

test("test_a_report_without_on_error_is_printed", async () => {
  const store = storeWith();
  store["x"] = good({ amount: "Infinity" });
  const printed: unknown[] = [];
  const original = console.error;
  console.error = (...a: unknown[]) => { printed.push(a); };
  try {
    const chain = new FakeChain(1000);
    assert.deepEqual(await reverifyReceipts(store, { rpc_for: () => chain, chains: MAINNET, wallets: [DEV] }), [`arbitrum:${TX}:2`, "x"]);
  } finally {
    console.error = original;
  }
  assert.equal(printed.length, 1);
});

test("test_removed_keys_come_back_in_store_order", async () => {
  const store: Store = {};
  for (const k of ["z", "b", "m", "a"]) store[k] = {};
  const keep = storeWith();
  Object.assign(store, keep);
  const chain = new FakeChain(1000);
  genuine(chain);
  assert.deepEqual(await run(store, chain), ["z", "b", "m", "a"]);
  assert.deepEqual(Object.keys(store), Object.keys(keep));
});

// --- wallets per family (test_backends.py) ---------------------------------------------------------

test("test_reverify_accepts_wallets_per_family", async () => {
  const store = storeWith();
  const chain = new FakeChain(1000);
  genuine(chain);
  const removed = await reverifyReceipts(store, { rpc_for: () => chain, chains: { arbitrum: ARB }, wallets: { evm: [DEV] } });
  assert.deepEqual(removed, []);
  assert.equal(Object.keys(store).length, 1);
});

test("test_reverify_keeps_receipts_of_a_family_without_wallets", async () => {
  // A game that dropped EVM (Solana-only now) can't judge old EVM receipts: keep them.
  const store = storeWith();
  const chain = new FakeChain(1000); // no logs at all: would revoke if checked
  const removed = await reverifyReceipts(store, { rpc_for: () => chain, chains: { arbitrum: ARB }, wallets: { evm: [] } });
  assert.deepEqual(removed, []);
  assert.equal(Object.keys(store).length, 1);
  assert.ok(!chain.calls.includes("get_logs"));
  // a mapping without the family at all behaves the same
  assert.deepEqual(await reverifyReceipts(store, { rpc_for: () => chain, chains: { arbitrum: ARB }, wallets: {} }), []);
});

test("test_check_evm_receipt_stops_parsing_at_the_first_match", async () => {
  // Python any() over a generator: a malformed log after the matching one is never parsed.
  const chain = new FakeChain(1000);
  genuine(chain);
  const bad = transferLog(USDC, PAYER, DEV, 1, { block: 900, tx_hash: TX, log_index: 9 });
  bad["logIndex"] = "0xzz";
  chain.logs.push(bad);
  assert.equal(await checkEvmReceipt(storeWith()[`arbitrum:${TX}:2`], { rpc: chain, chain: ARB, wallets: [DEV] }), "keep");
  const first = new FakeChain(1000);
  first.logs.push(bad);
  genuine(first);
  await assert.rejects(checkEvmReceipt(storeWith()[`arbitrum:${TX}:2`], { rpc: first, chain: ARB, wallets: [DEV] }));
});
