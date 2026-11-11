/**
 * The plugin commands on the MZ shim (commands.ts): Unlock Item, Tip, Restore Purchases and Wallet
 * Help, run by event interpreters as MZ runs them (code 357 -> PluginManager.callCommand with the
 * interpreter as `this`). Each sets its status variable to 0, holds its own interpreter in the
 * "scfg" wait mode until the Promise settles, then stores the Ruling M2-4 code (SCFG.STATUS).
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import {
  CHAPTER2, DEV, PAYER, TX, USDC_ARB, bootGlue, encodeItems, pass, payOnChain, pluginCommand, run, session, tick,
} from "../../test/mz_glue.ts";
import { resetShim } from "../../test/mz_shim/index.ts";
import { addReceipt } from "../core/receipts.ts";
import { DEFAULT_TIP_AMOUNT, STATUS, WAIT_MODE, WALLET_HELP_URL } from "./commands.ts";

const g = globalThis as any;

afterEach(() => resetShim());

const unlock = (statusVariable: string, item = "chapter2") => pluginCommand("UnlockItem", { item, statusVariable }, "Unlock Item");

test("statusCodesAreTheRulingM24Table", async () => {
  const { s } = await bootGlue();
  assert.equal((s as any).STATUS, STATUS, "exported as SCFG.STATUS");
  assert.deepEqual({ ...STATUS }, {
    none: 0, paid: 1, already_owned: 2, pending: 3, underpaid: 4, expired: 5, cancelled: 6, failed: 7,
    restored: 1, nothing_found: 2,
  });
  assert.ok(Object.isFrozen(STATUS), "a game cannot change the table");
});

test("unlockCommandWaitsAndStoresStatus", async () => {
  const glue = await bootGlue();
  const { s, host } = glue;
  // a cancel: 6
  const cancelled = run(unlock("6"));
  host.last.close("cancel");
  await tick();
  cancelled.update();
  assert.deepEqual([g.$gameVariables.value(6), cancelled.isRunning()], [STATUS.cancelled, false], "cancelled = 6");
  g.$gameVariables.setValue(5, 99);
  const i = run(unlock("5"));
  assert.equal(g.$gameVariables.value(5), STATUS.none, "the variable is 0 at once");
  assert.equal(i._waitMode, WAIT_MODE, "the interpreter waits in the scfg mode");
  assert.deepEqual(host.sheets.map((x) => x.kind), ["payment", "payment"], "the payment sheet is shown");
  assert.equal(host.last.ctl.spec.item, "chapter2");
  const ctl = await session(glue);
  i.update();
  assert.equal(i.isRunning(), true, "the event waits while the sheet is open");
  await payOnChain(glue, ctl);
  assert.equal(s.owns("chapter2"), true, "paid");
  i.update();
  assert.equal(i.isRunning(), true, "still waiting: the player has not closed the sheet");
  assert.equal(g.$gameVariables.value(5), 0, "nothing stored while the sheet is open");
  host.last.close("close");
  await tick();
  assert.equal(g.$gameVariables.value(5), STATUS.paid, "paid = 1");
  i.update();
  assert.equal(i.isRunning(), false, "the event goes on, and ends");
  assert.equal(i._waitMode, "", "the wait mode is cleared");
  // owned now: already_owned (2), without a sheet
  const again = run(unlock("5"));
  assert.equal(g.$gameVariables.value(5), 0);
  await tick();
  again.update();
  assert.equal(g.$gameVariables.value(5), STATUS.already_owned, "already_owned = 2");
  assert.equal(again.isRunning(), false);
  assert.equal(host.sheets.length, 2, "no sheet for an owned item");
  assert.deepEqual(glue.errors, []);
});

test("unlockCommandWithoutAVariableStillWaits", async () => {
  const glue = await bootGlue();
  const before = JSON.stringify(g.$gameVariables._data);
  const i = run(unlock("0"));
  assert.equal(i._waitMode, WAIT_MODE);
  i.update();
  assert.equal(i.isRunning(), true, "waits for the sheet");
  glue.host.last.close("cancel");
  await tick();
  i.update();
  assert.equal(i.isRunning(), false);
  assert.equal(JSON.stringify(g.$gameVariables._data), before, "no variable written");
});

test("tipCommandStoresPending3OnContinuePlaying", async () => {
  const glue = await bootGlue();
  const { host } = glue;
  const i = run(pluginCommand("Tip", { amount: "3.00", token: "USDC", statusVariable: "2" }));
  const ctl = host.last.ctl;
  assert.deepEqual([ctl.spec.kind, String(ctl.spec.price), ctl.spec.token], ["tip", "3.00", "USDC"], "the suggested tip");
  assert.equal(ctl.nextStep(), true, "past the amount step");
  await session(glue);
  host.last.close("keep_playing");
  await tick();
  assert.equal(g.$gameVariables.value(2), STATUS.pending, "Continue playing = pending = 3");
  i.update();
  assert.equal(i.isRunning(), false);
  // amount and token are optional: the default suggestion and the first enabled token
  const j = run(pluginCommand("Tip", { amount: "", token: "", statusVariable: "2" }));
  assert.deepEqual([String(host.last.ctl.spec.price), host.last.ctl.spec.token], [DEFAULT_TIP_AMOUNT, "USDC"]);
  assert.equal(DEFAULT_TIP_AMOUNT, "1.00", "as the Ren'Py and Godot demos");
  host.last.close("cancel");
  await tick();
  j.update();
  assert.equal(g.$gameVariables.value(2), STATUS.cancelled);
});

test("restoreCommandStoresNothingFound2", async () => {
  const glue = await bootGlue();
  const { s, host } = glue;
  // the purchase is already on this device: restoring its transaction finds nothing new
  const job = s._worker!.submit((e) => addReceipt(e.receipts, {
    chain: "arbitrum", tx_hash: TX, log_index: 0, block: 10, kind: "unlock", item: "chapter2", amount: "4.99",
    token: "USDC", payer: PAYER.toLowerCase(), recipient: DEV.toLowerCase(), source: "payment",
  } as any));
  await pass(glue);
  assert.equal(job.result(), true);
  const chain = (glue.chains["arbitrum"] ??= new FakeChain(1000));
  chain.receipts[TX] = { status: "0x1", logs: [transferLog(USDC_ARB, PAYER, DEV, 4_993_700, { block: 10, tx_hash: TX })] };
  g.$gameVariables.setValue(3, 42);
  const i = run(pluginCommand("RestorePurchases", { statusVariable: "3" }, "Restore Purchases"));
  assert.equal(g.$gameVariables.value(3), 0);
  assert.deepEqual(host.sheets.map((x) => x.kind), ["restore"], "the restore screen");
  const ctl = host.last.ctl;
  assert.equal(ctl.check(TX), true);
  await pass(glue);
  await pass(glue);
  assert.equal(ctl.result().status, "nothing_found", ctl.state);
  i.update();
  assert.equal(i.isRunning(), true, "waits until the screen is closed");
  host.last.close("close");
  await tick();
  assert.equal(g.$gameVariables.value(3), STATUS.nothing_found, "nothing_found = 2");
  i.update();
  assert.equal(i.isRunning(), false);
  // closed before anything was restored: cancelled = 6
  const j = run(pluginCommand("RestorePurchases", { statusVariable: "3" }));
  host.last.close("close");
  await tick();
  j.update();
  assert.equal(g.$gameVariables.value(3), STATUS.cancelled);
});

test("aCommandThatCannotRunStoresFailed7", async () => {
  const glue = await bootGlue();
  const i = run(unlock("4", "no_such_item"));
  await tick();
  i.update();
  assert.deepEqual([g.$gameVariables.value(4), i.isRunning()], [STATUS.failed, false], "failed = 7, and the event goes on");
  assert.equal(glue.host.sheets.length, 0);
  assert.equal(glue.errors.length, 1, glue.errors.join("\n"));
  assert.match(glue.errors[0]!, /^scfg: Unlock Item failed: ConfigError: unknown item "no_such_item"/);
  const tip = run(pluginCommand("Tip", { amount: "0.01", token: "USDC", statusVariable: "4" }));
  await tick();
  tip.update();
  assert.equal(g.$gameVariables.value(4), STATUS.failed, "a tip below the minimum");
  assert.match(glue.errors[1]!, /^scfg: Tip failed: ConfigError: tips must be at least/);
});

test("commandsOfADisabledPluginStoreFailed7", async () => {
  // invalid parameters: boot reports them, the plugin is off, every command answers failed
  const glue = await bootGlue({ params: { wallet: "not-an-address", items: encodeItems([CHAPTER2]) } });
  assert.ok(glue.s._disabled !== null);
  for (const [command, args] of [["UnlockItem", { item: "chapter2" }], ["Tip", {}], ["RestorePurchases", {}]] as const) {
    const i = run(pluginCommand(command, { ...args, statusVariable: "7" }));
    await tick();
    i.update();
    assert.deepEqual([g.$gameVariables.value(7), i.isRunning()], [STATUS.failed, false], command);
  }
});

test("eachCommandWaitsOnItsOwnInterpreter", async () => {
  // a parallel event and the map's event each run a command: each waits for its own Promise
  const glue = await bootGlue();
  const { host } = glue;
  const parallel = run(pluginCommand("Tip", { amount: "2.00", token: "USDC", statusVariable: "8" }));
  const tipSheet = host.last;
  const mapEvent = run(unlock("9"));
  const unlockSheet = host.last;
  assert.notEqual(tipSheet, unlockSheet);
  unlockSheet.close("cancel");
  await tick();
  parallel.update();
  mapEvent.update();
  assert.deepEqual([parallel.isRunning(), mapEvent.isRunning()], [true, false], "only the unlock's event goes on");
  assert.deepEqual([g.$gameVariables.value(8), g.$gameVariables.value(9)], [0, STATUS.cancelled]);
  // a new command on the same interpreter replaces its earlier run: the older Promise writes nothing
  const reused = run(pluginCommand("Tip", { amount: "2.00", token: "USDC", statusVariable: "10" }));
  const older = host.last;
  reused.setup(pluginCommand("Tip", { amount: "2.00", token: "USDC", statusVariable: "11" }));
  reused.update();
  const newer = host.last;
  older.close("cancel");
  await tick();
  reused.update();
  assert.deepEqual([g.$gameVariables.value(10), reused.isRunning()], [0, true], "the older run is moot");
  newer.close("cancel");
  tipSheet.close("cancel");
  await tick();
  reused.update();
  parallel.update();
  assert.deepEqual([g.$gameVariables.value(11), reused.isRunning()], [STATUS.cancelled, false]);
  assert.deepEqual([g.$gameVariables.value(8), parallel.isRunning()], [STATUS.cancelled, false]);
});

test("aCommandInACommonEventWaitsOnTheChildInterpreter", async () => {
  // an event calls a common event (MZ's command 117) that runs Unlock Item: the command's `this`
  // is the common event's child interpreter, which waits in the "scfg" mode; the calling event
  // waits for its child and goes on once the command stored its code
  const after: number[] = [];
  const glue = await bootGlue({
    commonEvents: [{ name: "Buy Chapter 2", list: unlock("5").slice(0, 1).concat(pluginCommand("Mark", {}).slice(0, 1), [{ code: 0, indent: 0, parameters: [] }]) }],
  });
  g.PluginManager.registerCommand("SCFG", "Mark", () => void after.push(g.$gameVariables.value(5)));
  const { host } = glue;
  const parent = run([{ code: 117, indent: 0, parameters: [1] }, { code: 0, indent: 0, parameters: [] }]);
  const child = parent._childInterpreter;
  assert.ok(child instanceof g.Game_Interpreter && child !== parent, "the common event runs in a child interpreter");
  assert.equal(child._waitMode, WAIT_MODE, "the child waits in the scfg mode");
  assert.equal(parent._waitMode, "", "not the calling event's own interpreter");
  assert.deepEqual(host.sheets.map((x) => x.kind), ["payment"], "the sheet is shown");
  parent.update();
  parent.update();
  assert.deepEqual([parent.isRunning(), after], [true, []], "the calling event waits for its child");
  host.last.close("cancel");
  await tick();
  assert.equal(g.$gameVariables.value(5), STATUS.cancelled, "the code is stored");
  parent.update();
  assert.deepEqual(after, [STATUS.cancelled], "the common event goes on after the command");
  assert.deepEqual([parent._childInterpreter, parent.isRunning()], [null, false], "and the calling event ends with it");
  assert.deepEqual(glue.errors, []);
});

test("aSaveMadeWhileWaitingLoadsWithoutWaitingForever", async () => {
  // The decision: a run's Promise lives in this session only. A save made while an event waits
  // stores the variable as 0 and the interpreter in the scfg wait mode; after loading it, that
  // interpreter has no live Promise, so it stops waiting at once and goes on with the variable
  // at 0, and the earlier session's result is never written into the loaded game.
  const glue = await bootGlue();
  const { host } = glue;
  g.$gameMap._interpreter.setup(unlock("5"));
  g.$gameMap._interpreter.update();
  assert.equal(g.$gameMap._interpreter._waitMode, WAIT_MODE);
  assert.equal(await g.DataManager.saveGame(1), 0);
  const saved = JSON.parse(glue.storage.read("save/file1.rmmzsave")!);
  assert.equal(saved.map._interpreter._waitMode, WAIT_MODE, "the save holds the waiting interpreter");
  assert.deepEqual(Object.keys(saved.map._interpreter).filter((k) => /scfg/i.test(k)), [], "and nothing of the plugin's own");
  assert.equal(await g.DataManager.loadGame(1), 0);
  const loaded = g.$gameMap._interpreter;
  assert.equal(loaded._waitMode, WAIT_MODE, "loaded in the scfg wait mode");
  loaded.update();
  assert.equal(loaded.isRunning(), false, "no live Promise: it stops waiting and the event goes on");
  assert.equal(g.$gameVariables.value(5), 0, "the variable stays 0 (not done)");
  host.last.close("cancel"); // the earlier session's sheet
  await tick();
  assert.equal(g.$gameVariables.value(5), 0, "the earlier session's result is not written into the loaded game");
  // the same after a New Game
  const i = run(unlock("5"));
  g.DataManager.setupNewGame();
  host.last.close("cancel");
  await tick();
  i.update();
  assert.equal(g.$gameVariables.value(5), 0, "nor into a new game");
});

test("walletHelpOpensThePlayerWalletPage", async () => {
  const glue = await bootGlue();
  assert.equal(WALLET_HELP_URL, "https://jrpg.com/stablecoinforgames/docs/players/wallets");
  const i = run(pluginCommand("WalletHelp", {}, "Wallet Help"));
  assert.deepEqual(glue.opened, [WALLET_HELP_URL], "opened in the system browser (NW.js)");
  assert.equal(i.isRunning(), false, "nothing to wait for");
  assert.equal(glue.host.sheets.length, 0);
});

test("commandArgumentsArriveAsStrings", async () => {
  // MZ hands the args as the editor stored them; a bad variable number is no variable
  const glue = await bootGlue();
  const i = run(unlock(" 12 "));
  assert.equal(i._waitMode, WAIT_MODE);
  glue.host.last.close("cancel");
  await tick();
  assert.equal(g.$gameVariables.value(12), STATUS.cancelled, "spaces around the number are ignored");
  const j = run(unlock("x"));
  glue.host.last.close("cancel");
  await tick();
  j.update();
  assert.equal(j.isRunning(), false, "still runs and goes on");
});
