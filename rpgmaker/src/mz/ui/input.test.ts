/**
 * MZ input ownership while a screen is open (ui/input.ts), on the shim: the game's Input and
 * TouchInput queries answer "nothing", the screen reads the real state, other plugins' aliases
 * keep working on both sides of the gate, and the game gets its input back when the last screen
 * closes (buttons still held then stay the screen's until they are released). The DOM half (keys
 * stopped at the shadow host while a field has the focus) is tested in tests/rpgmaker_ui.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { bootShim, installShim, resetShim } from "../../../test/mz_shim/index.ts";
import { InputOwner, nextFocus } from "./input.ts";

const g = globalThis as any;

afterEach(() => resetShim());

const DOWN = 40;
const ENTER = 13;
const ESCAPE = 27;

/** What the game asks of Input about `name` this frame. */
const game = (name: string) => [g.Input.isPressed(name), g.Input.isTriggered(name), g.Input.isRepeated(name), g.Input.isLongPressed(name)];

test("theGameSeesNothingWhileAScreenOwnsTheInput", () => {
  const shim = installShim(g);
  const owner = new InputOwner();
  owner.acquire();
  assert.equal(owner.active, true);
  shim.keyDown(DOWN);
  shim.frame();
  assert.deepEqual(game("down"), [false, false, false, false], "Input's queries answer nothing");
  assert.deepEqual([g.Input.dir4, g.Input.dir8], [0, 0], "no direction");
  assert.equal(owner.triggered("down"), true, "the screen reads the key");
  assert.equal(owner.repeated("down"), true, "triggered counts as repeated (MZ)");
  shim.frame();
  assert.equal(owner.triggered("down"), false, "triggered on one frame only");
  assert.equal(owner.pressed("down"), true);
  shim.frame(23);
  assert.equal(owner.repeated("down"), true, "MZ's key repeat: after keyRepeatWait frames");
  shim.frame();
  assert.equal(owner.repeated("down"), false);
  assert.deepEqual(game("down"), [false, false, false, false]);
});

test("touchInputAnswersNothingWhileOwned", () => {
  const shim = installShim(g);
  const owner = new InputOwner();
  owner.acquire();
  shim.pointer("mousedown", 10, 10);
  shim.frame();
  const T = g.TouchInput;
  assert.deepEqual([T.isPressed(), T.isTriggered(), T.isRepeated(), T.isLongPressed(), T.isClicked()], [false, false, false, false, false]);
  shim.pointer("mouseup", 10, 10);
  shim.frame();
  assert.deepEqual([T.isReleased(), T.isClicked(), T.isCancelled(), T.isMoved(), T.isHovered()], [false, false, false, false, false]);
  assert.deepEqual([T.wheelX, T.wheelY], [0, 0]);
  owner.release();
  shim.pointer("mousedown", 10, 10);
  shim.frame();
  assert.equal(T.isTriggered(), true, "the game's touch again once released");
});

test("cancelReadsEscapeAsMzDoes", () => {
  const shim = installShim(g);
  const owner = new InputOwner();
  owner.acquire();
  shim.keyDown(ESCAPE);
  shim.frame();
  assert.equal(owner.triggered("cancel"), true, "Escape is cancel (Input._isEscapeCompatible)");
  assert.equal(g.Input.isTriggered("cancel"), false, "not for the game");
  assert.equal(g.Input.isTriggered("menu"), false);
});

test("buttonsHeldWhenTheScreenOpensAreNotTheScreens", () => {
  const shim = installShim(g);
  const owner = new InputOwner();
  shim.keyDown(ENTER); // the OK that talked to the NPC that opened the sheet
  shim.frame();
  assert.equal(g.Input.isTriggered("ok"), true, "the game's");
  owner.acquire();
  assert.equal(owner.triggered("ok"), false, "the screen does not take the game's press");
  shim.frame(30);
  assert.deepEqual([owner.pressed("ok"), owner.repeated("ok")], [false, false], "nor its repeats while held");
  shim.keyUp(ENTER);
  shim.frame();
  shim.keyDown(ENTER);
  shim.frame();
  assert.equal(owner.triggered("ok"), true, "a new press is the screen's (even without frame() in between)");
});

test("buttonsHeldWhenTheLastScreenClosesStayHiddenUntilReleased", () => {
  const shim = installShim(g);
  const owner = new InputOwner();
  owner.acquire();
  shim.keyDown(DOWN);
  shim.frame();
  shim.keyDown(ENTER);
  shim.frame();
  assert.equal(owner.triggered("ok"), true);
  owner.release(); // the OK closed the sheet
  assert.equal(owner.active, false);
  shim.frame(30);
  owner.frame();
  assert.deepEqual(game("ok"), [false, false, false, false], "the held OK does not reach the game");
  assert.equal(g.Input.dir4, 0, "nor the held direction");
  shim.keyUp(ENTER);
  shim.keyUp(DOWN);
  shim.frame();
  owner.frame();
  shim.keyDown(ENTER);
  shim.frame();
  assert.equal(g.Input.isTriggered("ok"), true, "a new press reaches the game");
  shim.keyDown(DOWN);
  shim.frame();
  assert.equal(g.Input.dir4, 2);
});

test("nestedScreensKeepTheInputUntilTheLastCloses", () => {
  const shim = installShim(g);
  const owner = new InputOwner();
  owner.acquire();
  owner.acquire();
  owner.release();
  shim.keyDown(ENTER);
  shim.frame();
  assert.equal(owner.active, true);
  assert.equal(g.Input.isTriggered("ok"), false);
  owner.release();
  owner.release(); // one too many: ignored
  assert.equal(owner.active, false);
});

test("otherPluginsAliasesKeepWorking", () => {
  const shim = installShim(g);
  const calls: string[] = [];
  // a plugin loaded before (below the gate) and one that aliases after the screen first opened (above it)
  const before = g.Input.isTriggered;
  g.Input.isTriggered = function (this: unknown, name: string) {
    calls.push(`before:${name}`);
    return before.call(this, name);
  };
  const owner = new InputOwner();
  owner.acquire();
  const gated = g.Input.isTriggered;
  g.Input.isTriggered = function (this: unknown, name: string) {
    calls.push(`after:${name}`);
    return gated.call(this, name);
  };
  const top = g.Input.isTriggered;
  shim.keyDown(ENTER);
  shim.frame();
  calls.length = 0;
  assert.equal(g.Input.isTriggered("ok"), false, "the alias above the gate gets the gate's answer");
  assert.deepEqual(calls, ["after:ok"]);
  calls.length = 0;
  assert.equal(owner.triggered("ok"), true, "the screen reads through the functions below the gate");
  assert.ok(calls.includes("before:ok") && !calls.includes("after:ok"), calls.join());
  owner.release();
  shim.keyUp(ENTER);
  shim.frame();
  owner.frame();
  shim.keyDown(ENTER);
  shim.frame();
  calls.length = 0;
  assert.equal(g.Input.isTriggered("ok"), true, "closed: the whole chain answers again");
  assert.deepEqual(calls, ["after:ok", "before:ok"]);
  assert.equal(g.Input.isTriggered, top, "the later alias is still in place (the gate never unwraps)");
  owner.acquire();
  owner.release();
  assert.equal(g.Input.isTriggered, top, "opening again does not wrap again");
});

test("closedTheGateAnswersExactlyAsMzDoes", () => {
  const shim = installShim(g);
  const owner = new InputOwner();
  owner.acquire();
  owner.release();
  shim.frame();
  owner.frame();
  const names = ["ok", "cancel", "menu", "escape", "up", "down", "left", "right", "shift", "pageup", "pagedown", "tab"];
  for (const [code, frames] of [[ENTER, 1], [DOWN, 1], [DOWN, 30], [ESCAPE, 1], [87, 2]] as const) {
    shim.keyDown(code);
    shim.frame(frames);
    for (const name of names) {
      const got = game(name);
      // MZ's own answer: the Input object's functions without the gate, read on a frozen state
      const raw = (fn: string) => (owner as any)._originals.input[fn].call(g.Input, name);
      assert.deepEqual(got, [raw("isPressed"), raw("isTriggered"), raw("isRepeated"), raw("isLongPressed")], `${code}/${frames} ${name}`);
    }
    assert.deepEqual([g.Input.dir4, g.Input.dir8], [g.Input._dir4, g.Input._dir8]);
    shim.keyUp(code);
    shim.frame();
  }
});

test("aNewShimGetsANewGate", () => {
  let shim = installShim(g);
  const owner = new InputOwner();
  owner.acquire();
  owner.release();
  resetShim();
  shim = installShim(g); // fresh Input and TouchInput objects (a test, or a game that rebuilt them)
  owner.acquire();
  shim.keyDown(ENTER);
  shim.frame();
  assert.equal(g.Input.isTriggered("ok"), false, "gated");
  assert.equal(owner.triggered("ok"), true);
});

test("theMapPlayerDoesNotMoveWhileOwned", async () => {
  const shim = installShim(g);
  await bootShim();
  const owner = new InputOwner();
  const start = [g.$gamePlayer.x, g.$gamePlayer.y];
  owner.acquire();
  shim.keyDown(39);
  shim.frame(5);
  assert.deepEqual([g.$gamePlayer.x, g.$gamePlayer.y], start, "Game_Player reads Input.dir4: 0");
  shim.keyDown(ESCAPE);
  shim.frame(5);
  assert.equal(g.SceneManager._scene.constructor.name, "Scene_Map", "no menu");
  shim.keyUp(39);
  shim.keyUp(ESCAPE);
  owner.release();
  shim.frame();
  owner.frame();
  shim.keyDown(39);
  shim.frame(2);
  assert.ok(g.$gamePlayer.x > start[0], "moves again");
});

test("focusMovesAlongTheOrderAndWithinRows", () => {
  // order: back | a1 a2 (row a) | b | c1 c2 c3 (row c); the ends stay put
  const rows = [null, "a", "a", null, "c", "c", "c"];
  const row = (i: number) => rows[i] ?? null;
  assert.equal(nextFocus(0, 7, row, "up"), 0, "the first stays");
  assert.equal(nextFocus(0, 7, row, "down"), 1);
  assert.equal(nextFocus(2, 7, row, "up"), 1, "up is the previous control, as Godot's focus_neighbor_top");
  assert.equal(nextFocus(6, 7, row, "down"), 6, "the last stays");
  assert.equal(nextFocus(1, 7, row, "right"), 2, "right within the row");
  assert.equal(nextFocus(2, 7, row, "right"), 2, "not out of it");
  assert.equal(nextFocus(4, 7, row, "left"), 4, "nor left out of it");
  assert.equal(nextFocus(5, 7, row, "left"), 4);
  assert.equal(nextFocus(3, 7, row, "left"), 3, "no row: left and right stay");
  assert.equal(nextFocus(6, 7, row, "next"), 0, "Tab wraps");
  assert.equal(nextFocus(0, 7, row, "previous"), 6);
  assert.equal(nextFocus(-1, 7, row, "down"), -1, "nothing focused: the caller focuses the primary");
  assert.equal(nextFocus(0, 0, row, "down"), -1);
});
