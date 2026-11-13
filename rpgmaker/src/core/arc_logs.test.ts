/** Twin of tests/unit/test_arc_logs.py: parse_transfer, the watcher, restore and re-verification. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import { asUnits } from "./amounts.ts";
import { ARC_SYSTEM_EMITTER, ARC_USDC, MAINNET, parseTransfer } from "./chains.ts";
import { Dec } from "./decimal.ts";
import type { Item } from "./models.ts";
import { addReceipt, makeReceipt, receiptKey } from "./receipts.ts";
import { addItem, buildConfig } from "./config.ts";
import type { TransferLog } from "./evm.ts";
import { lookupEvm } from "./restore.ts";
import { checkEvmReceipt } from "./reverify.ts";
import { createSession, setHint } from "./sessions.ts";
import { advance } from "./watcher.ts";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const TX = "0x" + "ab".repeat(32);
const ARC = MAINNET["arc"]!;
const NATIVE = 10n ** 12n; // native (18-decimal) wei per 6-decimal unit
const dec = (s: string): Dec => Dec.parse(s) as Dec;

function emitterLog(value: bigint, kw: { block?: number; tx_hash?: string; log_index?: number } = {}) {
  return transferLog(ARC_SYSTEM_EMITTER, PAYER, DEV, value, {
    block: kw.block ?? 100, tx_hash: kw.tx_hash ?? TX, log_index: kw.log_index ?? 0,
  });
}

test("test_parse_transfer_floors_native_value", () => {
  const t = parseTransfer(ARC, emitterLog(1_230_017_900_000_000_000n, { log_index: 4 }))!;
  assert.equal(t.value, 1_230_017n);
  assert.equal(t.token, ARC_USDC);
  assert.deepEqual([t.sender, t.recipient, t.log_index], [PAYER, DEV, 4]);
});

test("test_arc_floor_keeps_bigint_precision", () => {
  // 19993754 * 10^12 is above 2^64; Number arithmetic would round the dusty value up a unit.
  const raw = 19_993_754_000_000_000_000n;
  assert.ok(raw > 2n ** 64n);
  assert.equal(BigInt(Number(raw + NATIVE - 1n)) / NATIVE, 19_993_755n); // the corruption bigint avoids
  const t = parseTransfer(ARC, emitterLog(raw))!;
  assert.equal(t.value, 19_993_754n);
  const dust = parseTransfer(ARC, emitterLog(raw + NATIVE - 1n))!;
  assert.equal(dust.value, 19_993_754n);
  const huge = 2n ** 255n + 123n * NATIVE + 7n;
  assert.equal(parseTransfer(ARC, emitterLog(huge))!.value, huge / NATIVE);
});

test("test_parse_transfer_ignores_sub_micro_value", () => {
  assert.equal(parseTransfer(ARC, emitterLog(NATIVE - 1n)), null);
});

test("test_parse_transfer_ignores_interface_log_on_arc", () => {
  const log = transferLog(ARC_USDC, PAYER, DEV, 1_230_017, { block: 100, tx_hash: TX });
  assert.equal(parseTransfer(ARC, log), null);
});

test("test_parse_transfer_ignores_unknown_contract", () => {
  const other = "0x1111111111111111111111111111111111111111";
  const log = transferLog(other, PAYER, DEV, 5, { block: 100, tx_hash: TX });
  assert.equal(parseTransfer(ARC, log), null);
  assert.equal(parseTransfer(MAINNET["base"]!, log), null);
});

test("test_parse_transfer_plain_chain_unchanged", () => {
  const usdc = MAINNET["base"]!.tokens["USDC"]!.address;
  const t = parseTransfer(MAINNET["base"]!, transferLog(usdc, PAYER, DEV, 4_993_742, { block: 7, tx_hash: TX, log_index: 2 }))!;
  assert.deepEqual([t.token, t.value, t.log_index, t.block_number], [usdc, 4_993_742n, 2, 7]);
});

test("test_parse_transfer_skips_non_transfer_logs", () => {
  const log = emitterLog(5n * NATIVE);
  log["topics"] = log["topics"].slice(0, 2);
  assert.equal(parseTransfer(ARC, log), null);
});

// --- the watcher on Arc -----------------------------------------------------------------

const ITEMS: ReadonlyMap<string, Item> = new Map([
  ["chapter2", { item_id: "chapter2", code: 37, price: dec("4.99"), token: "USDC", title: "Chapter 2", min_price: dec("4.99") }],
]);
const UNITS = 4_993_742n; // 4.99 + code 37 + nonce 42

/** The two logs one ERC-20 transfer() on Arc produces: emitter first, then 0x3600. */
function arcTransferTx(units: bigint, kw: { block?: number; log_index?: number; native?: bigint } = {}) {
  const block = kw.block ?? 998;
  const logIndex = kw.log_index ?? 5;
  return [
    emitterLog(kw.native ?? units * NATIVE, { block, log_index: logIndex }),
    transferLog(ARC_USDC, PAYER, DEV, units, { block, tx_hash: TX, log_index: logIndex + 1 }),
  ];
}

function session(head = 1000) {
  const pending: Record<string, any> = {};
  const receipts: Record<string, any> = {};
  const rpc = new FakeChain(head);
  const rec = createSession(pending, {
    kind: "unlock", item: "chapter2", title: "Chapter 2", token: "USDC", chain: "arc", price: "4.99", code: 37,
    nonce: 42, start_block: 995, now: 0.0,
  });
  assert.equal(rec.units, Number(UNITS));
  return { pending, receipts, rpc, rec };
}

const step = (s: ReturnType<typeof session>, now = 10.0) =>
  advance(s.pending, s.receipts, s.rec.id, { rpc: s.rpc, chain: ARC, wallet: DEV, items: ITEMS, now });

test("test_arc_native_send_detected_by_scan", async () => {
  const s = session();
  s.rpc.logs.push(emitterLog(UNITS * NATIVE, { block: 998, log_index: 3 })); // wallet "Send"
  const out = await step(s);
  assert.equal(s.rpc.lastGetLogs![0], ARC_SYSTEM_EMITTER);
  assert.equal(out.status, "paid"); // one confirmation is final on Arc
  assert.equal(String(out.result!.amount), "4.993742");
  assert.deepEqual(Object.keys(s.receipts), [receiptKey("arc", TX, 3)]);
});

test("test_arc_native_send_with_dust_matches_session", async () => {
  const s = session();
  s.rpc.logs.push(emitterLog(UNITS * NATIVE + NATIVE - 1n, { block: 998 }));
  assert.equal((await step(s)).status, "paid");
});

test("test_arc_transfer_counts_once_in_scan", async () => {
  const s = session();
  s.rpc.logs.push(...arcTransferTx(UNITS));
  assert.equal((await step(s)).status, "paid");
  assert.deepEqual(Object.keys(s.receipts), [receiptKey("arc", TX, 5)]);
});

test("test_arc_interface_log_alone_is_not_seen_by_scan", async () => {
  const s = session();
  s.rpc.logs.push(arcTransferTx(UNITS)[1]!);
  assert.equal((await step(s)).status, "waiting");
});

function hinted(logs: Record<string, any>[]) {
  const s = session();
  s.rpc.receipts[TX] = { status: "0x1", logs };
  setHint(s.pending, s.rec.id, TX, 1.0);
  return s;
}

test("test_arc_hint_verifies_from_emitter_log", async () => {
  const s = hinted(arcTransferTx(UNITS + 1000n)); // exchange-style amount
  const out = await step(s);
  assert.equal(out.status, "paid");
  assert.equal(String(out.result!.amount), "4.994742");
  assert.deepEqual(Object.keys(s.receipts), [receiptKey("arc", TX, 5)]);
});

test("test_arc_hint_underpaid_uses_floored_value", async () => {
  const s = hinted(arcTransferTx(0n, { native: 4_989_999n * NATIVE + NATIVE - 1n }));
  const out = await step(s);
  assert.equal(out.status, "underpaid");
  assert.equal(String(out.result!.amount), "4.989999");
});

test("test_arc_hint_with_only_interface_log_is_not_a_payment", async () => {
  const s = hinted(arcTransferTx(UNITS).slice(1));
  assert.equal((await step(s)).error, "tx_not_a_payment");
});

test("test_arc_large_hint_value_floors_exactly_into_the_receipt", async () => {
  // 19993754 units = 19.993754 USDC; raw 18-decimal value > 2^64 plus sub-unit dust.
  const s = hinted([emitterLog(19_993_754n * NATIVE + 999n, { block: 998, log_index: 5 })]);
  const out = await step(s);
  assert.equal(out.status, "paid");
  assert.equal(String(out.result!.amount), "19.993754");
  assert.equal(s.receipts[receiptKey("arc", TX, 5)].amount, "19.993754");
});

// --- restore and re-verification on Arc ------------------------------------------------------------

test("test_arc_restore_returns_one_candidate", async () => {
  const config = buildConfig({ wallet: DEV, chains: ["arc"] });
  addItem(config, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const rpc = new FakeChain();
  rpc.receipts[TX] = { status: "0x1", logs: arcTransferTx(UNITS) };
  const out = (await lookupEvm(TX, { config, chain: ARC, rpc, receipts: {} }))!;
  assert.equal(out.length, 1);
  const c = out[0]!;
  assert.deepEqual([c.token, c.transfer.value, c.transfer.log_index, c.kind, c.item], ["USDC", UNITS, 5, "unlock", "chapter2"]);
});

function arcReceipt(o: { value?: bigint; log_index?: number; block?: number } = {}) {
  const store: Record<string, any> = {};
  const t: TransferLog = {
    token: ARC_USDC, sender: PAYER, recipient: DEV, value: asUnits(o.value ?? UNITS), tx_hash: TX, log_index: o.log_index ?? 5,
    block_number: o.block ?? 998, block_hash: "0x" + "11".repeat(32),
  };
  addReceipt(store, makeReceipt({ chain: "arc", transfer: t, kind: "unlock", item: "chapter2", token: "USDC", source: "payment" }));
  return Object.values(store)[0];
}

const checkArc = (r: unknown, rpc: FakeChain) => checkEvmReceipt(r as never, { rpc, chain: ARC, wallets: [DEV] });

test("test_reverify_keeps_arc_receipt", async () => {
  const rpc = new FakeChain(1000);
  rpc.logs.push(...arcTransferTx(UNITS, { native: UNITS * NATIVE + 123n }));
  assert.equal(await checkArc(arcReceipt(), rpc), "keep");
  assert.equal(rpc.lastGetLogs![0], ARC_SYSTEM_EMITTER);
});

test("test_reverify_drops_arc_receipt_with_wrong_amount", async () => {
  const rpc = new FakeChain(1000);
  rpc.logs.push(...arcTransferTx(UNITS));
  assert.equal(await checkArc(arcReceipt({ value: UNITS + 1n }), rpc), "drop");
});

test("test_reverify_keeps_legacy_custom_arc_receipt", async () => {
  // A game that added Arc itself with add_chain("arc", ...) before it was built in stored receipts
  // at the 0x3600 log (index emitter + 1). They must survive the update.
  const rpc = new FakeChain(1000);
  rpc.logs.push(...arcTransferTx(UNITS));
  assert.equal(await checkArc(arcReceipt({ log_index: 6 }), rpc), "keep");
  assert.equal(rpc.lastGetLogs![0], ARC_USDC); // the second, token-contract lookup
});

test("test_reverify_drops_legacy_arc_receipt_with_wrong_amount", async () => {
  const rpc = new FakeChain(1000);
  rpc.logs.push(...arcTransferTx(UNITS));
  assert.equal(await checkArc(arcReceipt({ value: UNITS + 1n, log_index: 6 }), rpc), "drop");
});

test("test_reverify_drops_arc_receipt_with_no_log_at_all", async () => {
  assert.equal(await checkArc(arcReceipt(), new FakeChain(1000)), "drop");
});

test("test_arc_large_receipt_reverifies_exactly", async () => {
  // 19993754 * 10^12 native units is above 2^64: the floor must stay exact for the value to match.
  const rpc = new FakeChain(1000);
  rpc.logs.push(emitterLog(19_993_754n * NATIVE + 7n, { block: 998, log_index: 5 }));
  assert.equal(await checkArc(arcReceipt({ value: 19_993_754n }), rpc), "keep");
});
