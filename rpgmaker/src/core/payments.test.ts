import { test } from "node:test";
import assert from "node:assert/strict";
import { buildConfig, addItem } from "./config.ts";
import { ConfigError, ValueError } from "./errors.ts";
import { MIN_TIP, TIP_PRESETS, preparePayment } from "./payments.ts";

const W = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
function cfg() {
  const c = buildConfig({ wallet: W });
  addItem(c, "chapter-2", { code: 1, price: "4.99", token: "USDC", title: "Chapter 2", min_price: "2.00" });
  return c;
}
const code = (f: () => unknown) => { try { f(); } catch (e) { return (e as ConfigError).code; } return "no error"; };

test("test_constants", () => {
  assert.equal(String(MIN_TIP), "0.50");
  assert.deepEqual(TIP_PRESETS.map(String), ["1.00", "3.00", "5.00", "10.00"]);
});

test("test_unlock_uses_item_defaults", () => {
  const s = preparePayment(cfg(), "unlock", { item: "chapter-2" });
  assert.deepEqual(Object.keys(s).sort(), ["chains", "code", "item", "kind", "price", "title", "token"]);
  assert.equal(String(s.price), "4.99");
  assert.equal(s.code, 1);
  assert.equal(s.title, "Chapter 2");
  assert.deepEqual(s.chains, ["arbitrum", "base", "arc", "polygon", "ethereum"]);
});

test("test_unlock_pins_codes_and_error_classes", () => {
  const c = cfg();
  assert.equal(code(() => preparePayment(c, "unlock", { item: "nope" })), "item.unknown");
  assert.equal(code(() => preparePayment(c, "unlock", { item: "chapter-2", amount: "1.99" })), "payment.below_min_price");
  assert.equal(code(() => preparePayment(c, "unlock", { item: "chapter-2", token: "DAI" })), "token.not_enabled");
  assert.throws(() => preparePayment(c, "unlock", { item: "chapter-2", amount: 4.99 }), TypeError);
  assert.throws(() => preparePayment(c, "unlock", { item: "chapter-2", amount: "abc" }), ValueError);
});

test("test_tip", () => {
  const c = cfg();
  const s = preparePayment(c, "tip", { amount: "0.50", token: "USDT" });
  assert.equal(s.code, 0);
  assert.equal(s.title, "Tip");
  assert.equal(s.item, null);
  assert.equal(code(() => preparePayment(c, "tip", { token: "USDC" })), "tip.needs_amount_and_token");
  assert.equal(code(() => preparePayment(c, "tip", { amount: "0.49", token: "USDC" })), "tip.below_minimum");
  assert.equal(code(() => preparePayment(c, "gift", {})), "payment.bad_kind");
});
