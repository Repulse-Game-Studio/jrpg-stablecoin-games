/**
 * Boot from the plugin parameters (boot.ts), for the advanced parameters: the custom
 * chains are added before configure, and with the confirmations and previous wallets they reach
 * the configuration; what the core refuses is a boot error. A plugin below SCFG may
 * configure again before the game boots; the Items parameter's owned switches follow the items it
 * re-adds.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { CHAPTER2, DEV, PAYER, TX, bootGlue, encodeItems, pass } from "../../test/mz_glue.ts";
import { bootShim, resetShim } from "../../test/mz_shim/index.ts";
import { addReceipt } from "../core/receipts.ts";
import { registry } from "../core/config.ts";

const g = globalThis as any;

afterEach(() => resetShim());

const OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const MYL2 = {
  name: "My L2", chain_id: 99999, rpc: ["https://rpc.myl2.example"], tokens: { USDC: "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359" },
  confirmations: 5, explorer_tx: "https://explorer.myl2.example/tx/{}",
};

test("customChainFromParametersIsUsableInChainsRpcUrlsAndConfirmations", async () => {
  const glue = await bootGlue({
    params: {
      wallet: DEV, testnet: "false", items: encodeItems([CHAPTER2]),
      customChains: JSON.stringify({ myl2: MYL2 }), chains: "[\"myl2\", \"arbitrum\"]",
      rpcUrls: "{\"myl2\": [\"https://my.own.rpc\"]}", confirmations: "{\"myl2\": 7, \"arbitrum\": 2}",
      previousWallets: `["${OLD}"]`,
    },
  });
  const { s, errors } = glue;
  assert.deepEqual(errors, []);
  assert.equal(s._disabled, null);
  const cfg = s._config!;
  assert.deepEqual(cfg.chains, ["myl2", "arbitrum"]);
  assert.equal(registry(cfg).get("myl2")!.name, "My L2", "the custom chain is in the game's registry");
  assert.deepEqual(cfg.rpc_urls, { myl2: ["https://my.own.rpc"] });
  assert.deepEqual(cfg.confirmations, { myl2: 7, arbitrum: 2 });
  assert.deepEqual(cfg.previous_wallets, [OLD], "the previous wallets reach the config");
  // the Worker runs on it: the sheet offers the custom chain
  const box = s.pay("unlock", { item: "chapter2" });
  await pass(glue);
  assert.deepEqual(glue.host.last.ctl.chainOptions().map((o: any) => o[0]), ["myl2", "arbitrum"]);
  glue.host.last.close("cancel");
  assert.equal((await box).status, "cancelled");
});

test("customChainsWithoutAWalletParameterAreThereForTheScriptsConfigure", async () => {
  const glue = await bootGlue({ params: { customChains: JSON.stringify({ myl2: MYL2 }) }, start: false });
  const { s, errors } = glue;
  assert.equal(s._config, null, "nothing configured by the parameters");
  s.configure({ wallet: DEV, chains: ["myl2"] });
  assert.deepEqual(s._config!.chains, ["myl2"]);
  assert.deepEqual(errors, []);
});

test("advancedParametersTheCoreRefusesAreABootError", async () => {
  for (const [params, needle] of [
    [{ solanaWallet: "So11111111111111111111111111111111111111112", chains: "[\"solana\"]", confirmations: "{\"solana\": 5}" },
      /confirmations can't be set for 'solana'/],
    [{ wallet: DEV, customChains: JSON.stringify({ arbitrum: MYL2 }) }, /built in/],
    [{ wallet: DEV, previousWallets: "[\"not a wallet\"]" }, /previous_wallets/],
    [{ wallet: DEV, confirmations: "{base: 1" }, /plugin parameter confirmations is not valid JSON/],
  ] as const) {
    const glue = await bootGlue({ params: params as Record<string, string>, start: false });
    assert.notEqual(glue.s._disabled, null, `disabled: ${JSON.stringify(params)}`);
    assert.equal(glue.errors.length, 1, glue.errors.join("\n"));
    assert.match(glue.errors[0]!, /the plugin parameters are invalid, payments are off/);
    assert.match(glue.errors[0]!, needle);
    resetShim();
  }
});

test("aScriptConfigureBeforeBootKeepsTheItemsParametersOwnedSwitches", async () => {
  // a plugin below SCFG configures again from its script (before the game boots) and re-adds the item
  const glue = await bootGlue({ params: { wallet: DEV, testnet: "false", items: encodeItems([CHAPTER2]) }, start: false });
  const { s, errors } = glue;
  s.configure({ wallet: DEV, chains: ["arbitrum"], confirmations: { arbitrum: 2 } });
  assert.throws(() => s.owns("chapter2"), /unknown item/, "configure replaced the Items");
  s.addItem("chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  await bootShim();
  await s._mirror.idle();
  glue.shim.frame();
  assert.ok(s._worker !== null, "the game boots on the script's configuration");
  assert.deepEqual(s._config!.confirmations, { arbitrum: 2 });
  assert.equal(g.$gameSwitches.value(1), false);
  await pass(glue); // (the first pass re-verifies stored receipts: as bootGlue, before adding one)
  s._worker.submit((e) => addReceipt(e.receipts, {
    chain: "arbitrum", tx_hash: TX, log_index: 0, block: 10, kind: "unlock", item: "chapter2", amount: "4.99", token: "USDC",
    payer: PAYER.toLowerCase(), recipient: DEV.toLowerCase(), source: "payment",
  } as any));
  await pass(glue);
  assert.equal(g.$gameSwitches.value(1), true, "the Items entry's Owned Switch follows the re-added item");
  assert.deepEqual(errors, []);
});

test("anItemsEntryAScriptConfigureDidNotReAddLeavesItsSwitchAlone", async () => {
  // a plugin below SCFG configures again and re-adds no item: the Items entry's Owned Switch
  // belongs to no item any more, so it is never set (the game's own value stays); the reason is
  // reported once, not on every refresh, and payments work on the script's configuration
  const glue = await bootGlue({ params: { wallet: DEV, testnet: "false", items: encodeItems([CHAPTER2]) }, start: false });
  const { s, errors } = glue;
  s.configure({ wallet: DEV, chains: ["arbitrum"] });
  s.addItem("chapter3", { code: 38, price: "2.99", token: "USDC", title: "Chapter 3" });
  await bootShim();
  await s._mirror.idle();
  glue.shim.frame();
  assert.ok(s._worker !== null, "the game boots on the script's configuration");
  assert.throws(() => s.owns("chapter2"), /unknown item/, "chapter2 was not re-added");
  g.$gameSwitches.setValue(1, true); // the game's own use of that switch
  await pass(glue);
  s._worker.submit((e) => addReceipt(e.receipts, {
    chain: "arbitrum", tx_hash: TX, log_index: 0, block: 10, kind: "unlock", item: "chapter3", amount: "2.99", token: "USDC",
    payer: PAYER.toLowerCase(), recipient: DEV.toLowerCase(), source: "payment",
  } as any));
  await pass(glue);
  assert.equal(s.owns("chapter3"), true, "a re-added item is owned as usual");
  assert.equal(g.$gameSwitches.value(1), true, "the refresh on that receipt left the switch alone");
  s._worker.submit((e) => addReceipt(e.receipts, {
    chain: "arbitrum", tx_hash: "0x" + "ef".repeat(32), log_index: 0, block: 11, kind: "tip", item: null, amount: "1.00", token: "USDC",
    payer: PAYER.toLowerCase(), recipient: DEV.toLowerCase(), source: "payment",
  } as any));
  await pass(glue); // the receipts changed: another refresh
  assert.equal(g.$gameSwitches.value(1), true, "still as the game set it");
  const reports = errors.filter((e) => e.includes("cannot set item chapter2's owned switch"));
  assert.equal(reports.length, 1, errors.join("\n"));
  assert.match(reports[0]!, /unknown item/);
  assert.deepEqual(errors.filter((e) => !reports.includes(e)), [], "nothing else reported");
});
