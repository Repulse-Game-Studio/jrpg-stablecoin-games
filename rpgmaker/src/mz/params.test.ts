/** The plugin parameters as MZ's Plugin Manager encodes them (every value a string, structs as JSON text). */
import { test } from "node:test";
import assert from "node:assert/strict";
import { ConfigError } from "../core/index.ts";
import { DEFAULT_TIP_JAR_LABEL, parseParams } from "./params.ts";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";

const isParamsError = (e: unknown) => e instanceof ConfigError && e.code === "params.invalid";

test("paramsStructListParses", () => {
  // MZ stores a struct<Item>[] as the JSON text of an array whose entries are JSON texts too
  const chapter2 = { id: "chapter2", code: "37", price: "4.99", token: "USDC", title: "Chapter 2", minPrice: "", ownedSwitch: "1" };
  const bonus = { id: "bonus", code: "38", price: "2", token: "USDT", title: "Bonus \"quoted\"", minPrice: "1.50", ownedSwitch: "0" };
  const raw = {
    wallet: DEV, solanaWallet: "", testnet: "true", logo: "logo", accent: "#123456",
    items: JSON.stringify([JSON.stringify(chapter2), JSON.stringify(bonus)]),
    texts: JSON.stringify([JSON.stringify({ id: "sheet.cancel", text: "Abbrechen" })]),
    notify: "false", tipJarMenu: "true", tipJarLabel: "",
    chains: "[\"base\", \"arbitrum\"]", tokens: "", methods: "", rpcUrls: "{\"base\": [\"https://rpc.example\"]}",
  };
  const p = parseParams(raw);
  assert.deepEqual(p.configure, {
    testnet: true, wallet: DEV, logo: "logo", accent: "#123456", chains: ["base", "arbitrum"],
    rpc_urls: { base: ["https://rpc.example"] },
  });
  assert.deepEqual(p.items, [
    { id: "chapter2", code: 37, price: "4.99", token: "USDC", title: "Chapter 2", min_price: null, ownedSwitch: 1 },
    { id: "bonus", code: 38, price: "2", token: "USDT", title: "Bonus \"quoted\"", min_price: "1.50", ownedSwitch: 0 },
  ]);
  assert.deepEqual(p.texts, { "sheet.cancel": "Abbrechen" });
  assert.equal(p.notify, false);
  assert.equal(p.tipJarMenu, true);
  assert.equal(p.tipJarLabel, DEFAULT_TIP_JAR_LABEL, "an empty label is the default");
});

test("paramsDefaultsWhenUnset", () => {
  for (const raw of [{}, null, undefined, { wallet: "", items: "", texts: "[]" }]) {
    const p = parseParams(raw);
    assert.equal(p.configure, null, "no wallet: the game may configure from a script");
    assert.deepEqual([p.items, p.texts, p.notify, p.tipJarMenu, p.tipJarLabel], [[], {}, true, false, "Tip Jar"]);
  }
  assert.deepEqual(parseParams({ solanaWallet: "So1ana" }).configure, { testnet: false, solana_wallet: "So1ana" });
});

test("paramsRejectWhatTheyCannotRead", () => {
  assert.throws(() => parseParams({ wallet: DEV, rpcUrls: "{base: 1}" }), isParamsError, "invalid JSON");
  assert.throws(() => parseParams({ wallet: DEV, testnet: "yes" }), isParamsError, "a boolean that is not true/false");
  assert.throws(() => parseParams({ wallet: DEV, items: "{}" }), isParamsError, "a struct list that is not a list");
  assert.throws(() => parseParams({ wallet: DEV, items: "[\"{oops\"]" }), isParamsError, "an entry that is not JSON");
  assert.throws(() => parseParams({ wallet: DEV, items: JSON.stringify([JSON.stringify({ id: "a", ownedSwitch: "x" })]) }),
    isParamsError, "an owned switch that is not a number");
  assert.throws(() => parseParams({ items: JSON.stringify([JSON.stringify({ id: "a", code: "1" })]) }), isParamsError,
    "items without a wallet");
  assert.throws(() => parseParams({ texts: JSON.stringify([JSON.stringify({ text: "x" })]) }), isParamsError, "a text without an id");
  assert.throws(() => parseParams({ wallet: DEV, chains: JSON.stringify("[base") }), isParamsError, "JSON-quoted text that is not JSON");
  assert.throws(() => parseParams({ wallet: DEV, items: JSON.stringify([JSON.stringify({ id: "a", ownedSwitch: "4" }),
    JSON.stringify({ id: "b", ownedSwitch: "4" })]) }), (e: unknown) => isParamsError(e) && /owned switch 4 is used by both a and b/
    .test((e as Error).message), "two items sharing an owned switch");
  // what the core validates is passed on as written, for the core's own error
  assert.equal(parseParams({ wallet: DEV, items: JSON.stringify([JSON.stringify({ id: "a", code: "1.5" })]) }).items[0]!.code, "1.5");
});

// Ruling 9: Ren'Py's confirmations, previous_wallets and add_chain, as advanced parameters
const OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const MYL2 = {
  name: "My L2", chain_id: 99999, rpc: ["https://rpc.myl2.example"], tokens: { USDC: "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359" },
  confirmations: 5, explorer_tx: "https://explorer.myl2.example/tx/{}",
};

test("paramsAdvancedConfirmationsPreviousWalletsAndCustomChainsParse", () => {
  const p = parseParams({
    wallet: DEV, confirmations: "{\"base\": 12, \"arbitrum\": 2}", previousWallets: `["${OLD}"]`,
    customChains: JSON.stringify({ myl2: MYL2, other: { ...MYL2, chain_id: 99998 } }),
  });
  assert.deepEqual(p.configure, { testnet: false, wallet: DEV, confirmations: { base: 12, arbitrum: 2 }, previous_wallets: [OLD] });
  assert.deepEqual(p.customChains, [["myl2", MYL2], ["other", { ...MYL2, chain_id: 99998 }]], "in the order written");
  const empty = parseParams({ wallet: DEV, confirmations: "", previousWallets: "", customChains: "" });
  assert.deepEqual([empty.configure, empty.customChains], [{ testnet: false, wallet: DEV }, []], "empty: the defaults");
  // a script-configured game (no wallet parameter) still gets its custom chains, for its own configure
  assert.deepEqual(parseParams({ customChains: JSON.stringify({ myl2: MYL2 }) }).customChains, [["myl2", MYL2]]);
});

test("paramsAdvancedBadJsonNamesItsParameter", () => {
  for (const param of ["confirmations", "previousWallets", "customChains"]) {
    assert.throws(() => parseParams({ wallet: DEV, [param]: "{base: 12" }),
      (e: unknown) => isParamsError(e) && (e as Error).message.startsWith(`plugin parameter ${param} is not valid JSON`), param);
  }
  assert.throws(() => parseParams({ wallet: DEV, customChains: "[1]" }),
    (e: unknown) => isParamsError(e) && /plugin parameter customChains must be a JSON object/.test((e as Error).message));
  assert.throws(() => parseParams({ wallet: DEV, customChains: "{\"myl2\": 5}" }),
    (e: unknown) => isParamsError(e) && /customChains entry "myl2" is not an object/.test((e as Error).message));
});
