/**
 * Owned switches (switches.ts, D6, Review Focus 2): each Items entry's "Owned Switch" is kept
 * equal to `SCFG.owns(id)` after New Game, after every Load, whenever the receipts change, and
 * once when the stored receipts are first read. The plugin owns those switches.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import {
  CHAPTER2, DEV, PAYER, TX, bootGlue, encodeItems, pass, payOnChain, session,
} from "../../test/mz_glue.ts";
import { bootShim, resetShim } from "../../test/mz_shim/index.ts";
import { ConfigError } from "../core/index.ts";
import { addReceipt } from "../core/receipts.ts";

const g = globalThis as any;

afterEach(() => resetShim());

const BONUS = { id: "bonus", code: "38", price: "2.00", token: "USDC", title: "Bonus", minPrice: "", ownedSwitch: "3" };
const NO_SWITCH = { id: "extra", code: "39", price: "1.00", token: "USDC", title: "Extra", minPrice: "", ownedSwitch: "0" };
const PARAMS = { wallet: DEV, items: encodeItems([CHAPTER2, BONUS, NO_SWITCH]) };

const receipt = (item: string, tx = TX) => ({
  chain: "arbitrum", tx_hash: tx, log_index: 0, block: 10, kind: "unlock", item, amount: "4.99", token: "USDC",
  payer: PAYER.toLowerCase(), recipient: DEV.toLowerCase(), source: "payment",
}) as any;

test("ownedSwitchesSurviveNewGameAndLoad", async () => {
  const glue = await bootGlue({ params: PARAMS });
  const { s, storage } = glue;
  assert.deepEqual([g.$gameSwitches.value(1), g.$gameSwitches.value(3)], [false, false], "nothing owned yet");
  assert.equal(await g.DataManager.saveGame(1), 0, "a save made before the purchase");
  s._worker!.submit((e) => addReceipt(e.receipts, receipt("chapter2")));
  await pass(glue);
  assert.equal(g.$gameSwitches.value(1), true, "ON once the receipt is there");
  // a game event turns it OFF: the plugin puts it back at the next sync point
  g.$gameSwitches.setValue(1, false);
  assert.equal(await g.DataManager.loadGame(1), 0);
  assert.equal(g.$gameSwitches.value(1), true, "ON after loading a save made before the purchase");
  assert.equal(g.$gameSwitches.value(3), false, "an item not owned stays OFF");
  g.$gameSwitches.setValue(1, false);
  g.$gameSwitches.setValue(3, true);
  g.DataManager.setupNewGame();
  assert.deepEqual([g.$gameSwitches.value(1), g.$gameSwitches.value(3)], [true, false], "after New Game: each switch = owns()");
  // the next launch: the stored receipt sets the switch on the new game it boots into (checked
  // before the Worker's first pass, which re-verifies receipts against the fresh fake chains)
  await s._mirror.idle();
  resetShim();
  const next = await bootGlue({ params: PARAMS, storage, start: false });
  await bootShim();
  assert.equal(next.s.owns("chapter2"), true);
  assert.deepEqual([g.$gameSwitches.value(1), g.$gameSwitches.value(3)], [true, false], "ON in the next run's new game");
  assert.deepEqual(next.errors, []);
});

test("switchTurnsOnWhenAReceiptArrivesMidMap", async () => {
  const glue = await bootGlue({ params: PARAMS });
  const { s, host } = glue;
  const box = s.pay("unlock", { item: "chapter2" });
  const ctl = await session(glue);
  assert.equal(g.$gameSwitches.value(1), false);
  let refreshes = 0;
  const requestRefresh = g.$gameMap.requestRefresh;
  g.$gameMap.requestRefresh = function (this: unknown) {
    refreshes++;
    return requestRefresh.call(this);
  };
  await payOnChain(glue, ctl);
  assert.equal(g.$gameSwitches.value(1), true, "ON as soon as the payment confirms, with the sheet still open");
  assert.ok(refreshes > 0, "the map re-checks its event pages (Game_Switches.onChange)");
  host.last.close("close");
  assert.equal((await box).status, "paid");
  // clearLocalData() forgets the receipts: the switch follows
  s.clearLocalData();
  assert.equal(g.$gameSwitches.value(1), false, "OFF when the receipt is gone");
  // only changed switches are written
  refreshes = 0;
  s._worker!.submit((e) => addReceipt(e.receipts, receipt("bonus", "0x" + "cd".repeat(32))));
  await pass(glue);
  await pass(glue);
  assert.deepEqual([g.$gameSwitches.value(1), g.$gameSwitches.value(3)], [false, true]);
});

test("sharedSwitchNumberIsABootError", async () => {
  const twin = { ...BONUS, ownedSwitch: "1" };
  const glue = await bootGlue({ params: { wallet: DEV, items: encodeItems([CHAPTER2, twin]) } });
  const { s } = glue;
  assert.ok(s._disabled !== null, "the plugin is off");
  assert.match(s._disabled!, /owned switch 1 is used by both chapter2 and bonus/);
  assert.equal(glue.errors.length, 1, glue.errors.join("\n"));
  assert.match(glue.errors[0]!, /^scfg: the plugin parameters are invalid, payments are off: .*owned switch 1/);
  assert.equal(glue.notices.length, 1, "shown over the game in test play");
  assert.throws(() => s.owns("chapter2"), (e: unknown) => e instanceof ConfigError && e.code === "params.invalid");
  assert.equal(g.$gameSwitches.value(1), false, "the plugin writes no switch");
  // switch 0 (none) may repeat
  resetShim();
  const ok = await bootGlue({ params: { wallet: DEV, items: encodeItems([{ ...CHAPTER2, ownedSwitch: "0" }, NO_SWITCH]) } });
  assert.equal(ok.s._disabled, null);
});

test("ownedSwitchOutsideTheDatabaseIsIgnored", async () => {
  // MZ's Game_Switches ignores ids past $dataSystem.switches (the shim has 20)
  const glue = await bootGlue({ params: { wallet: DEV, items: encodeItems([{ ...CHAPTER2, ownedSwitch: "500" }]) } });
  glue.s._worker!.submit((e) => addReceipt(e.receipts, receipt("chapter2")));
  await pass(glue);
  assert.equal(g.$gameSwitches.value(500), false);
  assert.deepEqual(glue.errors, []);
});
