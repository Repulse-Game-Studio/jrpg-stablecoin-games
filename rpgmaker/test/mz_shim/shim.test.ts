/**
 * Self-tests of the MZ shim: the behaviour every glue test relies on. The expected values are
 * RPG Maker MZ 1.x's documented plugin API (see README.md), not a run of the real runtime.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { DEFAULT_GAME_ID, MemoryStorage, bootShim, encryptForDeployment, installShim, resetShim, type Shim } from "./index.ts";
import { DirStorage } from "./node_storage.ts";

const g = globalThis as any;

afterEach(() => resetShim());

const KEYS: Record<number, string> = {
  9: "tab", 13: "ok", 16: "shift", 17: "control", 18: "control", 27: "escape", 32: "ok", 33: "pageup",
  34: "pagedown", 37: "left", 38: "up", 39: "right", 40: "down", 45: "escape", 81: "pageup", 87: "pagedown",
  88: "escape", 90: "ok", 96: "escape", 98: "down", 100: "left", 102: "right", 104: "up", 120: "debug",
};
const PADS: Record<number, string> = {
  0: "ok", 1: "cancel", 2: "shift", 3: "menu", 4: "pageup", 5: "pagedown", 12: "up", 13: "down", 14: "left", 15: "right",
};

test("keyMapperAndGamepadMapperAreMzDefaults", () => {
  installShim(g);
  assert.deepEqual({ ...g.Input.keyMapper }, KEYS);
  assert.deepEqual({ ...g.Input.gamepadMapper }, PADS);
  assert.equal(g.Input.keyRepeatWait, 24);
  assert.equal(g.Input.keyRepeatInterval, 6);
});

test("keydownMapsThroughKeyMapperOnTheDocument", () => {
  const shim = installShim(g);
  for (const [code, name] of Object.entries(KEYS)) {
    g.Input.clear();
    shim.keyDown(Number(code));
    g.Input.update();
    assert.ok(g.Input.isPressed(name), `${code} -> ${name}`);
    assert.ok(g.Input.isTriggered(name), `${code} -> ${name}`);
    shim.keyUp(Number(code));
    g.Input.update();
    assert.ok(!g.Input.isPressed(name), `${code} released`);
  }
  shim.keyDown(65); // "a" is not mapped
  g.Input.update();
  assert.equal(g.Input._latestButton, null);
});

test("shouldPreventDefaultForBackspaceTabPagesAndArrowsButNotHomeEnd", () => {
  const shim = installShim(g);
  const prevented = new Set([8, 9, 33, 34, 37, 38, 39, 40]);
  for (let code = 0; code < 256; code++) {
    assert.equal(g.Input._shouldPreventDefault(code), prevented.has(code), String(code));
    assert.equal(shim.keyDown(code).defaultPrevented, prevented.has(code), String(code));
    shim.keyUp(code);
  }
});

test("triggeredRepeatedAndLongPressedFollowKeyRepeatWaitAndInterval", () => {
  const shim = installShim(g);
  shim.keyDown(40);
  const seen: [boolean, boolean, boolean][] = [];
  for (let frame = 0; frame <= 40; frame++) {
    g.Input.update();
    seen.push([g.Input.isTriggered("down"), g.Input.isRepeated("down"), g.Input.isLongPressed("down")]);
  }
  // pressedTime is 0 on the first update after keydown, then counts frames.
  const repeated = seen.map(([, r]) => r).flatMap((r, i) => (r ? [i] : []));
  assert.deepEqual(repeated, [0, 24, 30, 36]);
  assert.deepEqual(seen.map(([t]) => t).flatMap((t, i) => (t ? [i] : [])), [0]);
  assert.deepEqual(seen.map(([, , l]) => l).flatMap((l, i) => (l ? [i] : [])), [...Array(17).keys()].map((i) => i + 24));
  // A second key takes over as the latest button and restarts the count.
  shim.keyDown(38);
  g.Input.update();
  assert.ok(g.Input.isTriggered("up") && !g.Input.isRepeated("down") && g.Input.isPressed("down"));
});

test("escapeAnswersForCancelAndMenu", () => {
  const shim = installShim(g);
  shim.keyDown(27);
  g.Input.update();
  for (const name of ["escape", "cancel", "menu"]) {
    assert.ok(g.Input.isTriggered(name) && g.Input.isPressed(name) && g.Input.isRepeated(name), name);
  }
  assert.ok(!g.Input.isTriggered("ok"));
});

test("dir4AndDir8PreferTheNewerAxis", () => {
  const shim = installShim(g);
  shim.keyDown(37);
  g.Input.update();
  assert.deepEqual([g.Input.dir4, g.Input.dir8], [4, 4]);
  shim.keyDown(38); // up while left is held: dir8 7, dir4 follows the axis pressed last
  g.Input.update();
  assert.deepEqual([g.Input.dir4, g.Input.dir8], [8, 7]);
  shim.keyUp(37);
  shim.keyUp(38);
  g.Input.update();
  assert.deepEqual([g.Input.dir4, g.Input.dir8], [0, 0]);
});

test("gamepadButtonsAndAxesComeFromNavigatorGetGamepads", () => {
  const shim = installShim(g);
  const pad = (pressed: number[], axes = [0, 0]) => ({
    connected: true, index: 0, axes, buttons: [...Array(16).keys()].map((i) => ({ pressed: pressed.includes(i), value: 0 })),
  });
  shim.gamepads[0] = pad([0]);
  g.Input.update();
  assert.ok(g.Input.isTriggered("ok"));
  shim.gamepads[0] = pad([], [0, 0.9]); // stick down
  g.Input.update();
  assert.ok(!g.Input.isPressed("ok") && g.Input.isTriggered("down"));
  assert.equal(g.Input.dir4, 2);
  shim.gamepads[0] = pad([1]);
  g.Input.update();
  assert.ok(g.Input.isTriggered("cancel") && !g.Input.isPressed("down"));
});

test("blurClearsTheKeyboardState", () => {
  const shim = installShim(g);
  shim.keyDown(13);
  g.Input.update();
  shim.window.dispatchEvent(new Event("blur"));
  g.Input.update();
  assert.ok(!g.Input.isPressed("ok"));
});

test("touchInputTriggersOnTheFrameAfterTheEvent", () => {
  const shim = installShim(g);
  shim.pointer("mousedown", 10, 20);
  assert.ok(!g.TouchInput.isTriggered());
  g.TouchInput.update();
  assert.ok(g.TouchInput.isTriggered() && g.TouchInput.isPressed());
  assert.deepEqual([g.TouchInput.x, g.TouchInput.y], [10, 20]);
  shim.pointer("mouseup", 10, 20);
  g.TouchInput.update();
  assert.ok(g.TouchInput.isReleased() && g.TouchInput.isClicked() && !g.TouchInput.isPressed());
  shim.pointer("mousedown", 5, 5, 2); // right button
  g.TouchInput.update();
  assert.ok(g.TouchInput.isCancelled());
});

test("storageManagerRoundTripsAnObjectInMemory", async () => {
  const shim = installShim(g);
  await shim.boot();
  assert.equal(g.StorageManager.exists("scfg"), false);
  await g.StorageManager.saveObject("scfg", { version: 1, receipts: { "base:0xab:1": { amount: "4.993742" } } });
  assert.equal(g.StorageManager.exists("scfg"), true);
  assert.deepEqual(await g.StorageManager.loadObject("scfg"), { version: 1, receipts: { "base:0xab:1": { amount: "4.993742" } } });
  assert.ok(shim.storage.has(`rmmzsave.${DEFAULT_GAME_ID}.scfg`)); // web mode: localforage key
  await g.StorageManager.remove("scfg");
  assert.equal(g.StorageManager.exists("scfg"), false);
  assert.equal(await g.StorageManager.loadObject("scfg"), null);
});

test("storageManagerUsesSaveFilesOnNwjsAndATempDir", async () => {
  const dir = mkdtempSync(join(tmpdir(), "scfg-shim-"));
  try {
    await installShim(g, { nwjs: true, storage: new DirStorage(dir) }).boot();
    assert.equal(g.Utils.isNwjs(), true);
    await g.StorageManager.saveObject("scfg", { version: 1 });
    assert.ok(g.StorageManager.exists("scfg"));
    assert.deepEqual(JSON.parse(readFileSync(join(dir, "save", "scfg.rmmzsave"), "utf8")), { version: 1 });
    assert.deepEqual(await g.StorageManager.loadObject("scfg"), { version: 1 });
    assert.equal(g.StorageManager.remove("scfg"), undefined); // synchronous on NW.js
    assert.ok(!existsSync(join(dir, "save", "scfg.rmmzsave")));
    await assert.rejects(g.StorageManager.loadObject("scfg"));
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test("storageFailuresAndCorruptFilesCanBeInjected", async () => {
  const shim = installShim(g);
  await shim.boot();
  shim.failNext("save");
  await assert.rejects(g.StorageManager.saveObject("scfg", {}));
  await g.StorageManager.saveObject("scfg", { ok: true });
  shim.storage.write(`rmmzsave.${DEFAULT_GAME_ID}.scfg`, "{not json");
  await assert.rejects(g.StorageManager.loadObject("scfg"));
});

test("nwjsSaveKeepsABackupWhileWritingAsMz", async () => {
  // MZ's saveToLocalFile: unlink <file>_, rename <file> to <file>_, write <file>, unlink <file>_;
  // a failed write puts the backup back
  const shim = installShim(g, { nwjs: true });
  await shim.boot();
  await g.StorageManager.saveObject("scfg", { n: 1 });
  assert.deepEqual(shim.storage.keys(), ["save/scfg.rmmzsave"], "no backup after a save");
  shim.failNext("save");
  await assert.rejects(g.StorageManager.saveObject("scfg", { n: 2 }));
  assert.deepEqual([shim.storage.keys(), JSON.parse(shim.storage.read("save/scfg.rmmzsave")!)], [["save/scfg.rmmzsave"], { n: 1 }],
    "a failed write keeps the old file");
  shim.crashNextSave();
  let settled = false;
  void g.StorageManager.saveObject("scfg", { n: 3 }).finally(() => (settled = true));
  for (let i = 0; i < 5; i++) await new Promise((r) => setTimeout(r, 0));
  assert.equal(settled, false, "the game died: the save never settles");
  assert.deepEqual(shim.storage.keys(), ["save/scfg.rmmzsave_"], "only the backup is left");
  assert.deepEqual(JSON.parse(shim.storage.read("save/scfg.rmmzsave_")!), { n: 1 });
  shim.storage.write("save/scfg.rmmzsave", "{}");
  await g.StorageManager.saveObject("scfg", { n: 4 });
  assert.deepEqual(shim.storage.keys(), ["save/scfg.rmmzsave"], "the next save unlinks a stale backup");
  assert.equal(g.StorageManager.fsReadFile("save/nothing"), null, "fsReadFile: null for a missing file");
});

test("pluginParametersAndCommandsAsPluginManager", () => {
  installShim(g, { plugins: [{ name: "SCFG", status: true, description: "", parameters: { Notify: "true" } }] });
  assert.deepEqual(g.PluginManager.parameters("SCFG"), { Notify: "true" });
  assert.deepEqual(g.PluginManager.parameters("scfg"), { Notify: "true" }); // names are case-insensitive
  assert.deepEqual(g.PluginManager.parameters("Other"), {});
  const calls: unknown[] = [];
  g.PluginManager.registerCommand("SCFG", "unlockItem", function (this: unknown, args: unknown) { calls.push([this, args]); });
  const interp = new g.Game_Interpreter();
  interp.setup([
    { code: 357, indent: 0, parameters: ["SCFG", "unlockItem", "Unlock Item", { item: "chapter2", variable: "5" }] },
    { code: 0, indent: 0, parameters: [] },
  ]);
  interp.update();
  assert.deepEqual(calls, [[interp, { item: "chapter2", variable: "5" }]]);
  assert.ok(!interp.isRunning());
});

test("interpreterWaitsOnACustomWaitMode", async () => {
  const shim = installShim(g);
  let open = true;
  const base = g.Game_Interpreter.prototype.updateWaitMode;
  g.Game_Interpreter.prototype.updateWaitMode = function (this: any) {
    if (this._waitMode === "scfg") {
      if (open) return true;
      this._waitMode = "";
      return false;
    }
    return base.call(this);
  };
  const after: number[] = [];
  g.PluginManager.registerCommand("SCFG", "tip", function (this: any) { this.setWaitMode("scfg"); });
  g.PluginManager.registerCommand("SCFG", "after", () => after.push(g.Graphics.frameCount));
  await shim.boot();
  g.$gameMap._interpreter.setup([
    { code: 357, indent: 0, parameters: ["SCFG", "tip", "Tip", {}] },
    { code: 357, indent: 0, parameters: ["SCFG", "after", "After", {}] },
  ]);
  shim.frame(4);
  assert.deepEqual(after, []);
  assert.ok(g.$gameMap.isEventRunning());
  open = false;
  shim.frame(1);
  assert.equal(after.length, 1);
  shim.frame(1);
  assert.ok(!g.$gameMap.isEventRunning());
  g.$gameMap._interpreter.setup([{ code: 230, indent: 0, parameters: [3] }, { code: 357, indent: 0, parameters: ["SCFG", "after", "", {}] }]);
  shim.frame(3);
  assert.equal(after.length, 1);
  shim.frame(1);
  assert.equal(after.length, 2);
});

test("aCommonEventRunsInAChildInterpreterTheParentWaitsFor", async () => {
  // MZ's command117 (Common Event): the event's list runs in a child interpreter (setupChild,
  // depth + 1) that the parent updates first (updateChild) and waits for until it ends; the
  // child's commands get the child as `this`, and its own wait modes hold only the child
  const shim = installShim(g, {
    commonEvents: [{
      name: "Pay", list: [
        { code: 357, indent: 0, parameters: ["SCFG", "tip", "Tip", {}] },
        { code: 357, indent: 0, parameters: ["SCFG", "mark", "", { who: "child" }] },
        { code: 0, indent: 0, parameters: [] },
      ],
    }],
  });
  let open = true;
  const base = g.Game_Interpreter.prototype.updateWaitMode;
  g.Game_Interpreter.prototype.updateWaitMode = function (this: any) {
    if (this._waitMode !== "scfg") return base.call(this);
    if (open) return true;
    this._waitMode = "";
    return false;
  };
  const marks: [string, number][] = [];
  let waiting: any = null;
  g.PluginManager.registerCommand("SCFG", "tip", function (this: any) {
    waiting = this;
    this.setWaitMode("scfg");
  });
  g.PluginManager.registerCommand("SCFG", "mark", (args: { who: string }) => void marks.push([args.who, g.Graphics.frameCount]));
  assert.equal(g.$dataCommonEvents, null, "null until the database loads, as every $data*");
  await shim.boot();
  assert.deepEqual([g.$dataCommonEvents[0], g.$dataCommonEvents[1].id, g.$dataCommonEvents[1].name], [null, 1, "Pay"]);
  const parent = g.$gameMap._interpreter;
  parent.setup([
    { code: 117, indent: 0, parameters: [1] },
    { code: 117, indent: 0, parameters: [9] }, // no such common event: nothing runs
    { code: 357, indent: 0, parameters: ["SCFG", "mark", "", { who: "parent" }] },
    { code: 0, indent: 0, parameters: [] },
  ], 7);
  shim.frame(3);
  assert.ok(waiting !== null && waiting !== parent, "the command ran in the child interpreter");
  assert.ok(waiting instanceof g.Game_Interpreter);
  assert.equal(waiting._depth, parent._depth + 1, "one level deeper");
  assert.equal(waiting._eventId, 7, "the calling event's id (it is on the current map)");
  assert.equal(parent._childInterpreter, waiting);
  assert.equal(parent._waitMode, "", "the child waits, not the parent");
  assert.deepEqual(marks, [], "the parent waits for its child");
  assert.ok(g.$gameMap.isEventRunning());
  open = false;
  shim.frame(1);
  const frame = g.Graphics.frameCount;
  assert.deepEqual(marks, [["child", frame], ["parent", frame]], "the child ends and the parent goes on in the same update");
  assert.equal(parent._childInterpreter, null);
  shim.frame(1);
  assert.ok(!g.$gameMap.isEventRunning());
});

test("saveGameMakesTheContentsBeforeTheNameAsMz", async () => {
  await installShim(g).boot();
  const order: string[] = [];
  const dm = g.DataManager;
  const contents = dm.makeSaveContents;
  const name = dm.makeSavename;
  dm.makeSaveContents = function (this: unknown) {
    order.push("makeSaveContents");
    return contents.call(this);
  };
  dm.makeSavename = function (this: unknown, id: number) {
    order.push("makeSavename");
    return name.call(this, id);
  };
  const saved = dm.saveGame(2);
  assert.deepEqual(order, ["makeSaveContents", "makeSavename"], "MZ 1.x: the contents first, then the save name");
  assert.equal(await saved, 0);
  assert.ok(g.StorageManager.exists("file2"));
});

test("aRejectionBeforeBootIsReportedAndFailsThePlainNodeProcess", () => {
  // Before boot no SceneManager.onReject listens (MZ: the browser prints the rejection and the
  // page goes on). In Node the shim's process listener keeps Node from crashing on it, so it says
  // it on the console and fails the process: a plain Node driver must not pass with it.
  const theirs = process.listeners("unhandledRejection");
  process.removeAllListeners("unhandledRejection");
  const quiet = console.error;
  const lines: string[] = [];
  console.error = (...a: unknown[]) => void lines.push(a.map((x) => (x instanceof Error ? x.stack ?? x.message : String(x))).join(" "));
  const exitCode = process.exitCode;
  try {
    const shim = installShim(g);
    const early = new Error("rejected before boot");
    process.emit("unhandledRejection", early, Promise.resolve());
    assert.equal(lines.length, 1, lines.join("\n"));
    assert.match(lines[0]!, /before the game booted/);
    assert.match(lines[0]!, /rejected before boot/);
    assert.equal(process.exitCode, 1, "the process fails");
    assert.deepEqual([shim.stopped, shim.errors.length], [false, 0], "no game was running to stop");
  } finally {
    process.exitCode = exitCode;
    console.error = quiet;
    for (const fn of theirs) process.on("unhandledRejection", fn as (...a: unknown[]) => void);
  }
});

test("switchesAndVariablesAreBoundedByTheDatabase", async () => {
  await installShim(g, { switches: 3, variables: 2 }).boot();
  g.$gameSwitches.setValue(3, true);
  g.$gameSwitches.setValue(4, true); // beyond $dataSystem.switches: ignored
  g.$gameSwitches.setValue(0, true);
  assert.deepEqual([0, 1, 3, 4].map((i) => g.$gameSwitches.value(i)), [false, false, true, false]);
  assert.ok(g.$gameMap._needsRefresh);
  g.$gameVariables.setValue(2, 7.9);
  g.$gameVariables.setValue(3, 1);
  assert.deepEqual([g.$gameVariables.value(2), g.$gameVariables.value(3), g.$gameVariables.value(1)], [7, 0, 0]);
});

test("newGameSaveAndLoadRebuildTheGameObjects", async () => {
  await installShim(g, { switches: 5 }).boot();
  g.$gameSwitches.setValue(1, true);
  assert.equal(await g.DataManager.saveGame(1), 0);
  g.DataManager.setupNewGame();
  assert.equal(g.$gameSwitches.value(1), false);
  const extracted: unknown[] = [];
  const base = g.DataManager.extractSaveContents;
  g.DataManager.extractSaveContents = function (this: unknown, contents: unknown) {
    base.call(this, contents);
    extracted.push(contents);
  };
  assert.equal(await g.DataManager.loadGame(1), 0);
  assert.equal(extracted.length, 1);
  assert.ok(g.$gameSwitches instanceof g.Game_Switches);
  assert.equal(g.$gameSwitches.value(1), true);
  assert.ok(g.StorageManager.exists("file1"));
  await new Promise((resolve) => setTimeout(resolve, 0)); // the global info is saved without waiting, as MZ
  assert.ok(g.StorageManager.exists("global"));
});

test("sceneManagerUpdateMainReadsInputThenUpdatesTheScene", async () => {
  const shim = installShim(g);
  await shim.boot();
  const order: string[] = [];
  const updateInputData = g.SceneManager.updateInputData;
  g.SceneManager.updateInputData = function (this: unknown) { order.push("input"); updateInputData.call(this); };
  const sceneUpdate = g.Scene_Map.prototype.update;
  g.Scene_Map.prototype.update = function (this: unknown) { order.push("scene"); sceneUpdate.call(this); };
  g.SceneManager.goto(g.Scene_Map);
  shim.frame(1); // the scene is created and started
  shim.frame(1);
  assert.ok(g.SceneManager._scene instanceof g.Scene_Map);
  assert.deepEqual(order, ["input", "input", "scene"]);
});

test("playerMovesOnArrowsUnlessAnEventRuns", async () => {
  const shim = installShim(g);
  await shim.boot();
  const start = [g.$gamePlayer.x, g.$gamePlayer.y];
  shim.keyDown(39);
  shim.frame(1);
  assert.deepEqual([g.$gamePlayer.x, g.$gamePlayer.y], [start[0] + 1, start[1]]);
  g.$gameMap._interpreter.setup([{ code: 230, indent: 0, parameters: [60] }]);
  shim.frame(3);
  assert.ok(!g.$gamePlayer.canMove());
  assert.deepEqual([g.$gamePlayer.x, g.$gamePlayer.y], [start[0] + 1, start[1]]);
});

test("menuCommandsTakeAnOriginalCommandAndItsHandler", async () => {
  const shim = installShim(g);
  g.Window_MenuCommand.prototype.addOriginalCommands = function (this: any) { this.addCommand("Tip Jar", "scfgTipJar", true); };
  const opened: string[] = [];
  const create = g.Scene_Menu.prototype.createCommandWindow;
  g.Scene_Menu.prototype.createCommandWindow = function (this: any) {
    create.call(this);
    this._commandWindow.setHandler("scfgTipJar", () => opened.push("tip"));
  };
  await shim.boot();
  shim.keyDown(27); // escape opens the menu
  shim.frame(1);
  shim.keyUp(27);
  shim.frame(2);
  assert.ok(g.SceneManager._scene instanceof g.Scene_Menu);
  const win = g.SceneManager._scene._commandWindow;
  const symbols = win._list.map((c: { symbol: string }) => c.symbol);
  assert.deepEqual(symbols.slice(symbols.indexOf("formation")), ["formation", "scfgTipJar", "options", "save", "gameEnd"]);
  win.selectSymbol("scfgTipJar");
  shim.keyDown(13);
  shim.frame(1);
  assert.deepEqual(opened, ["tip"]);
});

test("utilsAnswerFromTheOptions", () => {
  installShim(g, { options: ["test"], mobile: true });
  assert.equal(g.Utils.RPGMAKER_NAME, "MZ");
  assert.ok(g.Utils.isOptionValid("test") && !g.Utils.isOptionValid("btest"));
  assert.ok(g.Utils.isMobileDevice() && !g.Utils.isNwjs());
  assert.equal(g.Utils.extractFileName("js/plugins/SCFG"), "SCFG");
});

test("eachInstallIsFreshAndResetRemovesTheGlobals", async () => {
  installShim(g);
  const first = g.Scene_Map;
  g.Scene_Map.prototype.update = () => { throw new Error("aliased"); };
  resetShim();
  assert.equal(g.Input, undefined);
  assert.equal(g.$gameSwitches, undefined);
  const shim: Shim = installShim(g);
  assert.notEqual(g.Scene_Map, first);
  await shim.boot();
  shim.frame(2); // the alias of the first install is gone
  assert.equal(g.Graphics.width, 816);
  assert.equal(g.Graphics.height, 624);
});

test("databaseIsNullWhilePluginsLoadAndForageKeyThrows", async () => {
  // MZ runs the plugin scripts before Scene_Boot.create loads the database.
  installShim(g, { plugins: [{ name: "SCFG", status: true, parameters: {} }] });
  assert.equal(g.$dataSystem, null);
  assert.equal(g.DataManager.isDatabaseLoaded(), false);
  assert.throws(() => g.StorageManager.forageKey("scfg"), TypeError);
  assert.throws(() => g.StorageManager.exists("scfg"), TypeError); // web: forageExists -> forageKey
  assert.equal(g.$gameSwitches, null);
  await bootShim();
  assert.equal(g.DataManager.isDatabaseLoaded(), true);
  assert.equal(g.StorageManager.forageKey("scfg"), `rmmzsave.${DEFAULT_GAME_ID}.scfg`);
  assert.ok(g.SceneManager._scene instanceof g.Scene_Map);
});

test("webExistsAnswersFromForageKeysUpdatedAtBoot", async () => {
  const storage = new MemoryStorage();
  storage.write(`rmmzsave.${DEFAULT_GAME_ID}.scfg`, JSON.stringify({ version: 1 }));
  installShim(g, { storage });
  assert.equal(g.StorageManager.forageKeysUpdated(), false);
  g.DataManager.loadDatabase();
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.ok(g.DataManager.isDatabaseLoaded());
  assert.equal(g.StorageManager.exists("scfg"), false); // the key cache is still empty
  assert.equal(await g.StorageManager.updateForageKeys(), 0);
  assert.ok(g.StorageManager.forageKeysUpdated());
  assert.equal(g.StorageManager.exists("scfg"), true);
});

test("sceneBootWaitsForTheDatabaseAndTheForageKeys", async () => {
  const shim = installShim(g);
  g.SceneManager.goto(g.Scene_Boot);
  shim.frame(3); // no event-loop turn: neither load can have finished
  assert.ok(g.SceneManager._scene instanceof g.Scene_Boot && !g.SceneManager._scene.isStarted());
  assert.equal(g.$dataSystem, null);
  await shim.boot();
  assert.ok(g.StorageManager.forageKeysUpdated());
  assert.deepEqual([g.$gamePlayer.x, g.$gamePlayer.y], [8, 6]);
});

test("jsonExTagsClassesWithAtAndRefusesDeepNesting", () => {
  installShim(g);
  const json = g.JsonEx.stringify({ s: new g.Game_Switches() });
  assert.deepEqual(JSON.parse(json), { s: { "@": "Game_Switches", _data: [] } });
  assert.ok(g.JsonEx.parse(json).s instanceof g.Game_Switches);
  assert.equal(g.JsonEx.maxDepth, 100);
  let deep: unknown = 1;
  for (let i = 0; i < 99; i++) deep = [deep];
  assert.doesNotThrow(() => g.JsonEx.stringify(deep)); // depths 0..99
  assert.throws(() => g.JsonEx.stringify([deep]), /Object too deep/); // depth 100
});

test("commandOkFallsBackThroughWindowSelectablesCallOkHandler", () => {
  installShim(g);
  // a plugin aliases Window_Selectable's callOkHandler: a command window without a handler for
  // its current symbol reaches the alias through the "ok" fallback, as in MZ
  const reached: string[] = [];
  const base = g.Window_Selectable.prototype.callOkHandler;
  g.Window_Selectable.prototype.callOkHandler = function (this: any) {
    reached.push("alias");
    base.call(this);
  };
  const win = new g.Window_Command(new g.Rectangle(0, 0, 200, 200));
  win.addCommand("Tip", "tip", true);
  win.addCommand("Other", "other", true);
  win.select(0);
  const handled: string[] = [];
  win.setHandler("tip", () => handled.push("tip"));
  win.setHandler("ok", () => handled.push("ok"));
  win.callOkHandler();
  assert.deepEqual([reached, handled], [[], ["tip"]], "the current symbol's own handler: no fallback");
  win.select(1);
  win.callOkHandler();
  assert.deepEqual([reached, handled], [["alias"], ["tip", "ok"]], "the ok fallback goes through Window_Selectable.prototype.callOkHandler");
  const bare = new g.Window_Command(new g.Rectangle(0, 0, 200, 200));
  bare.addCommand("Other", "other", true);
  bare.select(0);
  bare.deactivate();
  bare.callOkHandler();
  assert.deepEqual(reached, ["alias"], "with neither handler the window is re-activated, nothing called");
  assert.ok(bare.active);
});

test("interpreterRunsAnyCommandMethodLikeMz", () => {
  installShim(g);
  // a plugin adds an event command by defining Game_Interpreter.prototype["command" + code]
  const ran: unknown[] = [];
  let holds = 2;
  g.Game_Interpreter.prototype.command999 = function (this: any, params: unknown[]) {
    ran.push(params);
    return holds-- <= 0; // false: run again next update
  };
  g.Game_Interpreter.prototype.command101 = function (this: any, params: unknown[]) {
    ran.push(["text", ...params]);
    return true;
  };
  const interp = new g.Game_Interpreter();
  interp.setup([
    { code: 101, indent: 0, parameters: ["", 0, 0, 2, ""] },
    { code: 999, indent: 1, parameters: ["x"] },
    { code: 4242, indent: 0, parameters: [] }, // no such method: skipped, as MZ
    { code: 0, indent: 0, parameters: [] },
  ]);
  interp.update();
  assert.deepEqual(ran, [["text", "", 0, 0, 2, ""], ["x"]]);
  assert.equal(interp._indent, 1, "the running command's indent");
  assert.ok(interp.isRunning(), "a false answer waits");
  interp.update();
  interp.update();
  assert.deepEqual(ran.length, 4);
  assert.ok(!interp.isRunning(), "then the rest runs to the end");
  delete g.Game_Interpreter.prototype.command999;
  delete g.Game_Interpreter.prototype.command101;
});

test("anUnhandledRejectionOrUncaughtErrorStopsTheGameAsMz", async () => {
  // MZ's SceneManager.setupEventHandlers: window "error" -> onError, "unhandledrejection" ->
  // onReject -> onError, which prints the error, stops the game loop and shows the error screen
  const quiet = console.error;
  console.error = () => undefined;
  try {
    let shim = installShim(g);
    await bootShim();
    shim.frame();
    const reason = new Error("nobody caught me");
    shim.window.dispatchEvent(Object.assign(new Event("unhandledrejection"), { reason }));
    assert.equal(shim.stopped, true, "the game loop stopped");
    assert.deepEqual(shim.errors, [reason]);
    assert.deepEqual(g.Graphics._shimPrinted, { name: "Error", message: "Error: nobody caught me" }, "the error screen");
    assert.throws(() => shim.frame(), /stopped on an error.*nobody caught me/, "no frame runs any more");
    assert.throws(() => resetShim(), /stopped on an error.*nobody caught me/, "an afterEach fails the test");

    shim = installShim(g);
    await bootShim();
    const thrown = new TypeError("x is not a function");
    shim.window.dispatchEvent(Object.assign(new Event("error"), { error: thrown, message: thrown.message }));
    assert.equal(shim.stopped, true, "an uncaught exception stops it too");
    assert.deepEqual(shim.takeErrors(), [thrown], "a test that expects it takes it");
    resetShim(); // nothing left: no throw

    // In Node the process's unhandled rejections reach the window as the browser's event would.
    // (node:test's own listener is set aside meanwhile: it would fail this test.)
    const theirs = process.listeners("unhandledRejection");
    process.removeAllListeners("unhandledRejection");
    try {
      shim = installShim(g);
      await bootShim();
      const late = new Error("rejected later");
      process.emit("unhandledRejection", late, Promise.resolve());
      assert.deepEqual(shim.takeErrors(), [late], "Node's unhandledRejection is MZ's onReject");
      assert.equal(shim.stopped, true);
      resetShim();
      assert.equal(process.listenerCount("unhandledRejection"), 0, "resetShim removes the shim's listener");
    } finally {
      for (const fn of theirs) process.on("unhandledRejection", fn as (...a: unknown[]) => void);
    }
  } finally {
    console.error = quiet;
  }
});

test("encryptedImagesAreDecryptedWithTheDatabasesKey", async () => {
  // the editor's "Encrypt image files": Scene_Boot gives the database's settings to Utils
  const key = "0123456789abcdef0123456789abcdef";
  installShim(g, { encryption: { images: true, key } });
  assert.equal(g.Utils.hasEncryptedImages(), false, "not known while the plugins load");
  await bootShim();
  assert.equal(g.Utils.hasEncryptedImages(), true);
  assert.equal(g.Utils.hasEncryptedAudio(), false);
  const png = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52, 1, 2, 3]);
  const shipped = encryptForDeployment(png, key);
  assert.equal(shipped.length, png.length + 16, "a 16-byte header first");
  assert.notDeepEqual(shipped.slice(16), png, "the first bytes are scrambled");
  assert.deepEqual([...shipped.slice(32)], [...png.slice(16)], "the rest is stored as it is");
  assert.deepEqual(new Uint8Array(g.Utils.decryptArrayBuffer(shipped.slice().buffer)), png);
  assert.throws(() => g.Utils.decryptArrayBuffer(png.slice().buffer), /Decryption error/, "not an encrypted file");
  resetShim();
  installShim(g);
  await bootShim();
  assert.equal(g.Utils.hasEncryptedImages(), false, "a normal deployment");
});
