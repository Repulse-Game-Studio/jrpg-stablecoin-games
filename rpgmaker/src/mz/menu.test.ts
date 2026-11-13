/**
 * The Tip Jar entry of the main menu (menu.ts): with the "Tip Jar Menu" parameter on,
 * Window_MenuCommand.addOriginalCommands adds the "Tip Jar Label" command; it opens the tip sheet
 * and gives the menu back when the sheet closes.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { CHAPTER2, DEV, bootGlue, encodeItems, tick, type Glue } from "../../test/mz_glue.ts";
import { resetShim } from "../../test/mz_shim/index.ts";
import { DEFAULT_TIP_AMOUNT } from "./commands.ts";
import { TIP_JAR_SYMBOL } from "./menu.ts";

const g = globalThis as any;

afterEach(() => resetShim());

/** Open the main menu from the map (as the menu key does) and return its command window. */
function openMenu(glue: Glue): any {
  g.SceneManager.push(g.Scene_Menu);
  glue.shim.frame(2);
  const scene = g.SceneManager._scene;
  assert.ok(scene instanceof g.Scene_Menu && scene.isStarted(), "the menu is open");
  return scene._commandWindow;
}

test("tipJarMenuEntryOpensTheTipSheet", async () => {
  const glue = await bootGlue({ params: { wallet: DEV, items: encodeItems([CHAPTER2]), tipJarMenu: "true", tipJarLabel: "Leave a tip" } });
  const { host } = glue;
  const win = openMenu(glue);
  const symbols = win._list.map((c: any) => c.symbol);
  assert.deepEqual(symbols, ["item", "skill", "equip", "status", "formation", TIP_JAR_SYMBOL, "options", "save", "gameEnd"],
    "added with the original commands");
  const entry = win._list[symbols.indexOf(TIP_JAR_SYMBOL)];
  assert.deepEqual([entry.name, entry.enabled], ["Leave a tip", true]);
  win.selectSymbol(TIP_JAR_SYMBOL);
  win.processOk();
  assert.deepEqual(host.sheets.map((x) => x.kind), ["payment"], "the tip sheet is shown");
  const spec = host.last.ctl.spec;
  assert.deepEqual([spec.kind, String(spec.price), spec.token], ["tip", DEFAULT_TIP_AMOUNT, "USDC"]);
  assert.equal(win.active, false, "the menu waits while the sheet is open");
  glue.shim.frame(3);
  assert.equal(win.active, false);
  host.last.close("cancel");
  await tick();
  assert.equal(win.active, true, "back to the menu");
  assert.ok(g.SceneManager._scene instanceof g.Scene_Menu, "still in the menu");
  assert.equal(win.currentSymbol(), TIP_JAR_SYMBOL, "on the Tip Jar entry");
  assert.deepEqual(glue.errors, []);
});

test("tipJarMenuGivesTheMenuBackWhenTheTipCannotRun", async () => {
  // a disabled plugin: the entry is shown disabled; pressed anyway (a script), it reports and gives the menu back
  const glue = await bootGlue({ params: { wallet: "nope", tipJarMenu: "true" } });
  const win = openMenu(glue);
  const entry = win._list.find((c: any) => c.symbol === TIP_JAR_SYMBOL);
  assert.equal(entry.name, "Tip Jar", "the default label");
  assert.equal(entry.enabled, false, "disabled while payments are off");
  win.selectSymbol(TIP_JAR_SYMBOL);
  win.deactivate();
  g.SceneManager._scene.commandScfgTipJar();
  await tick();
  assert.equal(win.active, true, "back to the menu");
  assert.equal(glue.host.sheets.length, 0);
  assert.ok(glue.errors.some((e) => /^scfg: Tip Jar failed: ConfigError/.test(e)), glue.errors.join("\n"));
});

test("tipJarMenuIsOffByDefault", async () => {
  const glue = await bootGlue();
  const win = openMenu(glue);
  assert.ok(!win._list.some((c: any) => c.symbol === TIP_JAR_SYMBOL), "no entry");
  assert.equal(typeof g.Scene_Menu.prototype.commandScfgTipJar, "undefined", "nothing hooked");
});
