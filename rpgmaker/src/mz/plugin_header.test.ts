/**
 * The MZ plugin header (plugin_header.ts), read back as MZ's Plugin Manager reads it
 * (test/mz_header_reader.ts): every parameter params.ts reads and every command commands.ts
 * registers is declared, with its arguments, and a value of each declared type goes through MZ's
 * encoding into `parseParams` and comes out as the configure() option.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { DEV, bootGlue } from "../../test/mz_glue.ts";
import { mzEncode, readHeader, structOf, type Header } from "../../test/mz_header_reader.ts";
import { resetShim } from "../../test/mz_shim/index.ts";
import { ITEM_FIELD, PARAM, TEXT_FIELD, parseParams } from "./params.ts";
import { ARG, COMMAND, COMMAND_ARGS, PLUGIN_DESC, renderHeader } from "./plugin_header.ts";
import { VERSION } from "./scfg.ts";

const g = globalThis as any;

afterEach(() => resetShim());

const header = (): Header => readHeader(renderHeader());
const OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const MYL2 = {
  name: "My L2", chain_id: 99999, rpc: ["https://rpc.myl2.example"], tokens: { USDC: "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359" },
  confirmations: 5, explorer_tx: "https://explorer.myl2.example/tx/{}",
};
const names = (list: { name: string }[]) => list.map((p) => p.name).sort();

test("headerDeclaresEveryCommandAndParam", async () => {
  const h = header();
  assert.deepEqual([h.meta["target"], h.meta["plugindesc"], h.meta["author"], h.meta["url"]],
    ["MZ", `${PLUGIN_DESC} v${VERSION}`, "Repulse Game Studio", "https://jrpg.com/stablecoinforgames/"]);
  // the parameters are exactly what params.ts reads, with the plan's display names as @text
  assert.deepEqual(names(h.params), Object.values(PARAM).sort());
  const text = Object.fromEntries(h.params.map((p) => [p.name, p.text]));
  assert.deepEqual(text, {
    wallet: "Wallet", solanaWallet: "Solana Wallet", testnet: "Testnet", logo: "Logo", accent: "Accent", items: "Items",
    notify: "Notify", tipJarMenu: "Tip Jar Menu", tipJarLabel: "Tip Jar Label", texts: "Texts", chains: "Chains",
    tokens: "Tokens", methods: "Methods", rpcUrls: "RPC URLs", confirmations: "Confirmations", previousWallets: "Previous Wallets",
    customChains: "Custom Chains",
  });
  const p = Object.fromEntries(h.params.map((x) => [x.name, x]));
  assert.deepEqual([p["testnet"]!.type, p["testnet"]!.default], ["boolean", "false"]);
  assert.deepEqual([p["notify"]!.type, p["notify"]!.default], ["boolean", "true"]);
  assert.deepEqual([p["tipJarMenu"]!.type, p["tipJarMenu"]!.default], ["boolean", "false"]);
  assert.deepEqual([p["tipJarLabel"]!.type, p["tipJarLabel"]!.default], ["string", "Tip Jar"]);
  assert.deepEqual([p["logo"]!.type, p["logo"]!.dir, p["logo"]!.require], ["file", "img/scfg/", "1"]);
  assert.deepEqual([p["items"]!.type, p["items"]!.default], ["struct<Item>[]", "[]"]);
  assert.deepEqual([p["texts"]!.type, p["texts"]!.default], ["struct<Text>[]", "[]"]);
  for (const json of [PARAM.chains, PARAM.tokens, PARAM.methods, PARAM.rpcUrls, PARAM.confirmations, PARAM.previousWallets,
    PARAM.customChains]) {
    assert.equal(p[json]!.type, "multiline_string", `${json}: JSON text, as written (carried item a, Ruling 9)`);
    assert.equal(p[json]!.default, "", `${json}: empty = the defaults`);
  }
  for (const param of h.params) assert.ok(param.desc, `${param.name} has a @desc`);
  // the structs
  assert.deepEqual(Object.keys(h.structs).sort(), ["Item", "Text"]);
  assert.deepEqual(names(h.structs["Item"]!), Object.values(ITEM_FIELD).sort());
  assert.deepEqual(names(h.structs["Text"]!), Object.values(TEXT_FIELD).sort());
  const item = Object.fromEntries(h.structs["Item"]!.map((x) => [x.name, x]));
  assert.deepEqual(Object.fromEntries(Object.entries(item).map(([k, v]) => [k, v.text])), {
    id: "Id", code: "Code", price: "Price", token: "Token", title: "Title", minPrice: "Min Price", ownedSwitch: "Owned Switch",
  });
  assert.deepEqual([item["code"]!.type, item["code"]!.min, item["code"]!.max], ["number", "1", "99"]);
  assert.deepEqual([item["token"]!.type, item["token"]!.options.map((o) => o.value), item["token"]!.default],
    ["select", ["USDC", "USDT"], "USDC"]);
  assert.deepEqual([item["ownedSwitch"]!.type, item["ownedSwitch"]!.default], ["switch", "0"]);
  // the commands: exactly those registered under "SCFG", each with the arguments its handler reads
  await bootGlue({ start: false });
  const registered = Object.keys(g.PluginManager._commands).filter((k) => k.startsWith("SCFG:")).map((k) => k.slice(5)).sort();
  assert.deepEqual(names(h.commands), registered);
  assert.deepEqual(registered, Object.values(COMMAND).sort());
  const cmd = Object.fromEntries(h.commands.map((c) => [c.name, c]));
  assert.deepEqual(Object.fromEntries(h.commands.map((c) => [c.name, c.text])), {
    UnlockItem: "Unlock Item", Tip: "Tip", RestorePurchases: "Restore Purchases", WalletHelp: "Wallet Help",
  });
  for (const [name, args] of Object.entries(COMMAND_ARGS)) {
    assert.deepEqual(cmd[name]!.args.map((a) => a.name), args, `${name}'s @arg list`);
    for (const a of cmd[name]!.args) assert.ok(a.text && a.desc, `${name} ${a.name} has @text and @desc`);
  }
  const statusArg = cmd[COMMAND.unlockItem]!.args.find((a) => a.name === ARG.statusVariable)!;
  assert.deepEqual([statusArg.type, statusArg.default], ["variable", "0"]);
  // the help documents the status codes and that the plugin owns the switches
  for (const needle of ["0 = ", "1 = paid", "2 = already owned", "3 = pending", "7 = failed", "nothing found", "owns those switches",
    "Confirmations {", "Previous Wallets [", "Custom Chains:", "\"chain_id\"", "SCFG.configure({...}) again", "SCFG.addItem(id"]) {
    assert.ok(h.help.includes(needle), `the help mentions ${JSON.stringify(needle)}`);
  }
});

test("headerShowsTheVersion", () => {
  // MZ has no @version tag: the Plugin Manager's description and the help's first line say it
  const h = header();
  assert.equal(h.meta["plugindesc"], `${PLUGIN_DESC} v${VERSION}`);
  assert.equal(h.help.split("\n")[0], `Stablecoin for Games (SCFG) for RPG Maker MZ, v${VERSION}.`);
  assert.equal(h.meta["plugindesc"]!.split("\n").length, 1, "one line");
});

test("headerParamsRoundTripThroughMzEncoding", () => {
  // Carried item (a): what a developer types for each declared type, encoded as MZ stores it,
  // parses into the configure() options; the JSON parameters also when stored JSON-quoted (note).
  const h = header();
  const typed: Record<string, unknown> = {
    wallet: DEV, solanaWallet: "", testnet: "true", logo: "logo", accent: "#0e7c74", notify: "false", tipJarMenu: "true",
    tipJarLabel: "Tips", chains: "[\n  \"base\",\n  \"arbitrum\"\n]", tokens: "[\"USDC\"]",
    methods: "{\"manual\": {\"allow_tx_paste\": false}}", rpcUrls: "{\"base\": [\"https://rpc.example\"]}",
    confirmations: "{\"base\": 12}", previousWallets: `["${OLD}"]`, customChains: `{"myl2": ${JSON.stringify(MYL2)}}`,
    items: [{ id: "chapter2", code: "37", price: "4.99", token: "USDC", title: "Chapter \"2\"", minPrice: "", ownedSwitch: "1" }],
    texts: [{ id: "payment_confirmed", text: "Danke!" }],
  };
  const declared = Object.fromEntries(h.params.map((x) => [x.name, x.type]));
  const raw = Object.fromEntries(Object.entries(typed).map(([k, v]) => [k, mzEncode(h, declared[k], v)]));
  assert.equal(raw["items"], JSON.stringify([JSON.stringify(typed["items"] && (typed["items"] as any[])[0])]), "MZ's double encoding");
  const expected = {
    testnet: true, wallet: DEV, logo: "logo", accent: "#0e7c74", chains: ["base", "arbitrum"], tokens: ["USDC"],
    methods: { manual: { allow_tx_paste: false } }, rpc_urls: { base: ["https://rpc.example"] }, confirmations: { base: 12 },
    previous_wallets: [OLD],
  };
  const parsed = parseParams(raw);
  assert.deepEqual(parsed.configure, expected);
  assert.deepEqual(parsed.customChains, [["myl2", MYL2]]);
  assert.deepEqual(parsed.items, [{ id: "chapter2", code: 37, price: "4.99", token: "USDC", title: "Chapter \"2\"", min_price: null, ownedSwitch: 1 }]);
  assert.deepEqual([parsed.notify, parsed.tipJarMenu, parsed.tipJarLabel, parsed.texts], [false, true, "Tips", { payment_confirmed: "Danke!" }]);
  // an older editor or a hand-edited plugins.js may store multi-line text JSON-quoted (MZ's note type)
  const asNote = { ...raw };
  for (const k of [PARAM.chains, PARAM.tokens, PARAM.methods, PARAM.rpcUrls, PARAM.confirmations, PARAM.previousWallets,
    PARAM.customChains]) asNote[k] = mzEncode(h, "note", typed[k]);
  assert.deepEqual(parseParams(asNote).configure, expected, "JSON-quoted JSON text is read too");
  assert.deepEqual(parseParams(asNote).customChains, [["myl2", MYL2]]);
  // every type the header declares can be encoded (a type the reader doesn't know would be a typo)
  for (const x of [...h.params, ...Object.values(h.structs).flat()]) {
    const known = ["string", "multiline_string", "boolean", "number", "select", "combo", "switch", "variable", "file", "note"];
    assert.ok(known.includes(x.type ?? "") || structOf(x.type) !== null, `${x.name}: @type ${x.type}`);
  }
});
