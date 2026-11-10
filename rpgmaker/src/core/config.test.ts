import { test } from "node:test";
import assert from "node:assert/strict";
import { ConfigError } from "./errors.ts";
import {
  DEFAULT_ACCENT, INK, acceptedWallets, acceptedWalletsFor, addItem, buildChain, buildConfig, chainsForToken,
  enabledChains, itemByCode, logoContentType, registry, textOn, walletFor,
} from "./config.ts";
import { preparePayment } from "./payments.ts";
import { classify } from "./restore.ts";
import { asUnits } from "./amounts.ts";

const W = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const AVAX = {
  name: "Avalanche", chain_id: 43114, rpc: "https://api.avax.network/ext/bc/C/rpc",
  tokens: { USDC: "0xb97ef9ef8734c71904d8002f8b6bc66dd9c48a6e" }, confirmations: 5,
  explorer_tx: "https://snowtrace.io/tx/{}",
};
const code = (f: () => unknown) => { try { f(); } catch (e) { assert.ok(e instanceof ConfigError, String(e)); return e.code; } return "no error"; };

test("test_defaults", () => {
  const c = buildConfig({ wallet: W.toLowerCase() });
  assert.equal(c.wallet, W);
  assert.deepEqual(c.chains, ["arbitrum", "base", "arc", "polygon", "ethereum"]);
  assert.deepEqual(c.tokens, ["USDC", "USDT"]);
  assert.equal(c.accent, DEFAULT_ACCENT);
  assert.equal(c.logo, null);
  assert.deepEqual(Object.keys(c).sort(), [
    "accent", "chains", "confirmations", "custom_chains", "items", "logo", "methods", "previous_solana_wallets",
    "previous_wallets", "rpc_urls", "solana_wallet", "testnet", "tokens", "wallet"]);
  assert.deepEqual(c.methods["qr"], { enabled: true, wallet_button: true, chains: null });
  assert.deepEqual(acceptedWallets(c), [W]);
  assert.deepEqual(acceptedWalletsFor(c, "solana"), []);
});

test("test_config_error_codes", () => {
  assert.equal(code(() => buildConfig({})), "wallet.missing");
  assert.equal(code(() => buildConfig({ wallet: "0x123" })), "address.invalid");
  assert.equal(code(() => buildConfig({ wallet: W, chains: [] })), "chains.empty");
  assert.equal(code(() => buildConfig({ wallet: W, chains: ["solana"] })), "chain.needs_solana_wallet");
  assert.equal(code(() => buildConfig({ wallet: W, chains: ["base", "base"] })), "chains.duplicate");
  assert.equal(code(() => buildConfig({ wallet: W, tokens: ["DAI"] })), "tokens.unknown");
  assert.equal(code(() => buildConfig({ wallet: W, rpc_urls: { base: "http://example.org" } })), "rpc.not_https");
  assert.equal(code(() => buildConfig({ wallet: W, confirmations: { base: true } as never })), "confirmations.invalid");
  assert.equal(code(() => buildConfig({ wallet: W, accent: "red" })), "accent.invalid");
  assert.equal(code(() => buildConfig({ wallet: W, logo: "a.svg" })), "logo.bad_type");
  assert.equal(code(() => buildConfig({ wallet: W, methods: { qr: { colour: 1 } } })), "methods.unknown_option");
});

test("test_unknown_option_is_a_type_error", () => {
  assert.throws(() => buildConfig({ wallet: W, bogus: 1 } as never), TypeError);
});

test("test_accent_and_logo_and_text_on", () => {
  assert.equal(buildConfig({ wallet: W, accent: "#ABC" }).accent, "#aabbcc");
  assert.equal(buildConfig({ wallet: W, accent: "#AABBCCDD" }).accent, "#aabbcc");
  assert.equal(logoContentType("x/Logo.JPG"), "image/jpeg");
  assert.equal(textOn("#ffffff"), INK);
  assert.equal(textOn("#000000"), "#ffffff");
});

test("test_custom_chain_in_default_order", () => {
  const chain = buildChain("avalanche", AVAX);
  assert.equal(chain.tokens["USDC"]!.address, "0xB97EF9Ef8734C71904D8002F8b6Bc66Dd9c48a6E");
  assert.equal(chain.family, "evm");
  assert.equal(chain.max_log_range, 1000);
  const c = buildConfig({ wallet: W, custom_chains: [chain] });
  assert.deepEqual(c.chains, ["arbitrum", "base", "arc", "polygon", "ethereum", "avalanche"]);
  assert.equal(registry(c).get("avalanche"), chain);
  assert.equal(walletFor(c, chain), W);
});

test("test_build_chain_errors", () => {
  assert.equal(code(() => buildChain("Avalanche", AVAX)), "chain.invalid_key");
  assert.equal(code(() => buildChain("a", { ...AVAX, explorer_tx: "https://x.io/{x}/{}" })), "chain.bad_explorer_tx_braces");
  assert.equal(code(() => buildChain("a", { ...AVAX, explorer_tx: "https://x.io/{}/{}" })), "chain.bad_explorer_tx");
  assert.equal(code(() => buildChain("a", { ...AVAX, max_log_range: 9 })), "int.below_minimum");
  assert.equal(code(() => buildChain("a", { ...AVAX, chain_id: true } as never)), "int.below_minimum");
  const { name: _n, ...noName } = AVAX;
  assert.throws(() => buildChain("a", noName as never), TypeError);
});

test("test_enabled_chains_prepend_developer_rpc", () => {
  const c = buildConfig({ wallet: W, chains: ["base"], tokens: ["USDT"].concat(["USDC"]), rpc_urls: { base: "https://mine.example.org" }, confirmations: { base: 2 } });
  const e = enabledChains(c);
  assert.deepEqual(e.get("base")!.rpc[0], "https://mine.example.org");
  assert.equal(e.get("base")!.confirmations, 2);
  assert.deepEqual(Object.keys(e.get("base")!.tokens), ["USDC"]);
  assert.deepEqual(chainsForToken(c, "USDC"), ["base"]);
  assert.deepEqual(chainsForToken(c, "USDT"), []);
});

test("test_add_item", () => {
  const c = buildConfig({ wallet: W });
  const it = addItem(c, "chapter-2", { code: 1, price: "4.99", token: "USDC", title: "Chapter 2" });
  assert.equal(String(it.min_price), "4.99");
  assert.equal(itemByCode(c, 1), it);
  assert.equal(itemByCode(c, 2), null);
  assert.equal(code(() => addItem(c, "chapter-2", { code: 2, price: "1", token: "USDC", title: "x" })), "item.duplicate_id");
  assert.equal(code(() => addItem(c, "b", { code: 1, price: "1", token: "USDC", title: "x" })), "item.duplicate_code");
  assert.equal(code(() => addItem(c, "b", { code: 2, price: "abc", token: "USDC", title: "x" })), "item.invalid_price");
  assert.equal(code(() => addItem(c, "b", { code: 2, price: 1.5, token: "USDC", title: "x" })), "item.invalid_price");
  assert.equal(code(() => addItem(c, "b", { code: 2, price: "1", min_price: "2", token: "USDC", title: "x" })), "item.min_price_above_price");
  assert.equal(code(() => addItem(c, "b", { code: 2, price: "1", token: "DAI", title: "x" })), "token.unavailable");
  assert.equal(code(() => addItem(c, "b", { code: 2, price: "1", token: "USDC", title: " " })), "item.invalid_title");
  assert.throws(() => addItem(c, "b", { code: 2, token: "USDC", title: "x" } as never), TypeError);
});

test("test_qr_chains_entry_that_is_a_list_is_a_disabled_chain_not_a_type_error", () => {
  // Python's `c not in chains` is tuple membership: it never hashes c.
  assert.equal(code(() => buildConfig({ wallet: W, methods: { qr: { chains: [["base"]] } } })), "methods.qr_disabled_chain");
});

// M1 Ruling 9, carried: item ids and chain keys may be integer-like ("10", "0"). Python dicts keep
// insertion order; a JS object would list integer-like keys first. Expected orders are CPython's.
test("test_integer_like_keys_keep_insertion_order", () => {
  const custom = (key: string, chain_id: number) => buildChain(key, { ...AVAX, name: key.toUpperCase(), chain_id });
  const c = buildConfig({ wallet: W, custom_chains: [custom("zeta", 9001), custom("10", 9002), custom("2", 9003)] });
  for (const [id, n] of [["b", 1], ["10", 2], ["a", 3], ["0", 4]] as const) {
    addItem(c, id, { code: n, price: "1.00", token: "USDC", title: id });
  }
  const evm = ["arbitrum", "base", "arc", "polygon", "ethereum", "zeta", "10", "2"];
  assert.deepEqual(c.chains, evm);
  assert.deepEqual(preparePayment(c, "unlock", { item: "10" }).chains, evm);
  assert.deepEqual(chainsForToken(c, "USDC"), evm);
  assert.deepEqual(classify(c, asUnits(5_000_000n)), [null, null, ["b", "10", "a", "0", "tip"]]);
  assert.deepEqual([...c.items.keys()], ["b", "10", "a", "0"]);
  assert.deepEqual([...c.custom_chains.keys()], ["zeta", "10", "2"]);
  assert.deepEqual([...registry(c).keys()], ["ethereum", "arbitrum", "polygon", "base", "arc", "solana", "zeta", "10", "2"]);
  assert.deepEqual([...enabledChains(c).keys()], evm);
  assert.equal(itemByCode(c, 2)!.item_id, "10");
});
