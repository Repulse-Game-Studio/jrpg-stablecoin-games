import { test } from "node:test";
import assert from "node:assert/strict";
import { addReceipt, computeOwnership, makeReceipt, receiptKey, removeReceipt, type ReceiptJson } from "./receipts.ts";
import { asUnits } from "./amounts.ts";
import { Dec } from "./decimal.ts";
import { ValueError } from "./errors.ts";
import type { Item } from "./models.ts";
import type { Transfer } from "./transfers.ts";

const TX = "0x" + "ab".repeat(32);
const P = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const R = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359";
const dec = (s: string): Dec => Dec.parse(s) as Dec;
const transfer = (over: Partial<Transfer> = {}): Transfer => ({
  token: "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48", sender: P, recipient: R, value: asUnits(4_990_000n),
  tx_hash: TX, log_index: 7, block_number: 1_234_567, ...over,
});
const mk = (over: Partial<Parameters<typeof makeReceipt>[0]> = {}) =>
  makeReceipt({ chain: "ethereum", transfer: transfer(), kind: "unlock", item: "chapter2", token: "USDC", source: "payment", ...over });

test("test_receipt_key_lowercases_evm_but_not_solana", () => {
  assert.equal(receiptKey("ethereum", "0xABCDEF", 1), "ethereum:0xabcdef:1");
  assert.equal(receiptKey("solana", "AbCd", 3), "solana:AbCd:3");
  assert.equal(receiptKey("base", "", 0), "base::0");
  assert.equal(receiptKey("ethereum", "0X" + "AB", 0), "ethereum:0X" + "AB:0"); // only a lowercase 0x prefix counts
});

test("test_make_receipt_fields", () => {
  assert.deepEqual(mk(), {
    chain: "ethereum", tx_hash: TX, log_index: 7, block: 1_234_567, kind: "unlock", item: "chapter2",
    amount: "4.990000", token: "USDC", payer: P, recipient: R, source: "payment",
  });
  assert.equal(mk({ transfer: transfer({ value: asUnits(0n) }) }).amount, "0.000000");
  assert.equal(mk({ kind: "tip", item: null, source: "restore" }).item, null);
  // Python stores None when a Solana transfer names no payer (Transfer.sender is str | None there)
  assert.equal(mk({ transfer: transfer({ sender: null }) }).payer, null);
});

test("test_make_receipt_validates_kind_and_source", () => {
  for (const kind of ["refund", "", "Unlock", null]) assert.throws(() => mk({ kind: kind as any }), ValueError);
  for (const source of ["manual", null]) assert.throws(() => mk({ source: source as any }), ValueError);
});

test("test_add_and_remove_receipt", () => {
  const store: Record<string, ReceiptJson> = {};
  const r = mk();
  assert.equal(addReceipt(store, r), true);
  assert.equal(addReceipt(store, { ...r, tx_hash: TX.toUpperCase().replace("0X", "0x") }), false); // same key
  assert.equal(addReceipt(store, { ...r, log_index: 8 }), true);
  assert.equal(Object.keys(store).length, 2);
  removeReceipt(store, receiptKey("ethereum", TX, 7));
  removeReceipt(store, "nothing");
  assert.deepEqual(Object.keys(store), [receiptKey("ethereum", TX, 8)]);
  // keys that exist on every object are not "already there"
  assert.equal(addReceipt({}, { ...r, chain: "constructor", tx_hash: "", log_index: 0 }), true);
});

const item = (id: string, price: string, min: string): Item => ({
  item_id: id, code: 1, price: dec(price), token: "USDC", title: id, min_price: dec(min),
});
const items = { chapter2: item("chapter2", "4.99", "4.99"), pwyw: item("pwyw", "10.00", "2.00") };
const rec = (over: Record<string, unknown>) => ({ kind: "unlock", item: "chapter2", amount: "4.990000", ...over });

test("test_compute_ownership_unlocks_and_tips", () => {
  const [owned, tips] = computeOwnership({
    a: rec({}), b: rec({ item: "pwyw", amount: "2.000000" }), c: rec({ item: "ghost" }),
    d: rec({ kind: "tip", item: null, amount: "0.500000" }), e: rec({ kind: "tip", item: null, amount: "3.000000" }),
  }, items);
  assert.deepEqual([...owned].sort(), ["chapter2", "pwyw"]);
  assert.equal(String(tips), "3.500000");
  assert.equal(String(computeOwnership({ a: rec({ amount: "4.980000" }), z: rec({ item: "pwyw", amount: "1.99" }) }, items)[1]), "0");
});

test("test_tip_total_follows_decimal_addition", () => {
  const tips = (...a: unknown[]) => String(computeOwnership(Object.fromEntries(a.map((x, i) => [`k${i}`, rec({ kind: "tip", amount: x })])), items)[1]);
  assert.equal(tips("1.5", "0"), "1.5");
  assert.equal(tips("1.000000", "0.5"), "1.500000");
  assert.equal(tips(), "0");
  assert.equal(tips(4, "0.25"), "4.25");
  assert.equal(tips(true), "1");
  assert.equal(tips(0.5), "0.5");
});

test("test_malformed_entries_are_skipped", () => {
  const store: Record<string, unknown> = {
    a: null, b: "text", c: [], d: 5, e: {}, f: { kind: "unlock" }, g: { kind: "tip" },
    h: { kind: "tip", amount: "abc" }, i: { kind: "tip", amount: null }, j: { kind: "tip", amount: "NaN" },
    k: { kind: "tip", amount: "Infinity" }, l: { kind: "unlock", item: "chapter2", amount: "-Infinity" },
    m: { kind: "refund", amount: "5.00" }, n: { amount: "5.00" }, o: { kind: "unlock", item: "chapter2", amount: "sNaN" },
    p: { kind: "tip", amount: "2.000000" }, s: { kind: "tip", amount: "" }, t: { kind: ["tip"], amount: "1" },
    u: { kind: "tip", amount: {} },
  };
  const [owned, tips] = computeOwnership(store, items);
  assert.equal(owned.size, 0);
  assert.equal(String(tips), "2.000000");
});

test("test_item_lookup_ignores_prototype_names", () => {
  const [owned] = computeOwnership({ a: rec({ item: "constructor" }), b: rec({ item: "__proto__" }) }, items);
  assert.equal(owned.size, 0);
});
