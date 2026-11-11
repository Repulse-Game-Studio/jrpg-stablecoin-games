/**
 * An RPG Maker MZ stand-in for tests: the parts of MZ's plugin API the SCFG glue uses, written
 * from the API's documented behaviour (see README.md; no KADOKAWA code). `installShim(target)`
 * puts fresh copies of the MZ globals on `target` (globalThis in Node, window in the browser
 * build), so prototype aliases made by one test never leak into the next; `resetShim()` removes
 * them and their event listeners.
 *
 *   const shim = installShim(globalThis, { options: ["test"], plugins: [{ name: "SCFG", status: true, parameters }] });
 *   // ...load the plugin here: as in MZ, $dataSystem is still null and the save keys are not read...
 *   await bootShim();                          // Scene_Boot: database + save keys, new game, Scene_Map
 *   shim.keyDown(13); shim.frame();            // a keyboard OK, then one frame
 *
 * An uncaught error or an unhandled rejection stops the game as in MZ (SceneManager.onError, see
 * scene_manager.ts): `shim.errors` has it, `frame()` throws from then on, and `resetShim()` throws
 * it (an afterEach then fails the test) unless the test took it with `shim.takeErrors()`.
 */
import type { Ctx, GamepadLike, NavigatorLike, ShimOptions, StorageBackend } from "./context.ts";
import { createDataManager } from "./data_manager.ts";
import { createGameInterpreter } from "./game_interpreter.ts";
import { createGameObjects } from "./game_objects.ts";
import { createGraphics } from "./graphics.ts";
import { createInput } from "./input.ts";
import { createPluginManager } from "./plugin_manager.ts";
import { createSceneManager } from "./scene_manager.ts";
import { createScenes } from "./scenes.ts";
import { MemoryStorage, createJsonEx, createStorageManager } from "./storage_manager.ts";
import { createTouchInput } from "./touch_input.ts";
import { createUtils } from "./utils.ts";
import { createWindows } from "./windows.ts";

export type { GamepadLike, PluginEntry, ShimOptions, StorageBackend } from "./context.ts";
export { DEFAULT_GAME_ID } from "./context.ts";
export type { EventCommand } from "./game_interpreter.ts";
export { MemoryStorage } from "./storage_manager.ts";
export { ENCRYPTION_HEADER, encryptForDeployment } from "./utils.ts";

/** Control handle of an installed shim (not part of MZ). */
export interface Shim {
  /** The object the MZ globals were put on. */
  readonly target: any;
  /** Where Input/TouchInput listen (`document` in a page). */
  readonly document: EventTarget;
  /** Where `blur` is listened for (`window` in a page). */
  readonly window: EventTarget;
  readonly storage: StorageBackend;
  /** What the stub `navigator.getGamepads()` returns (when the target has no real one). */
  readonly gamepads: (GamepadLike | null)[];
  /** Dispatch a `keydown` with this MZ key code on `document`; returns the event (see `defaultPrevented`). */
  keyDown(keyCode: number, init?: { key?: string; code?: string }): Event;
  keyUp(keyCode: number, init?: { key?: string; code?: string }): Event;
  /** Dispatch a mouse event (`mousedown`/`mousemove`/`mouseup`) at page coordinates on `document`. */
  pointer(type: "mousedown" | "mousemove" | "mouseup", pageX: number, pageY: number, button?: number): Event;
  /** Run `SceneManager.updateMain()` `n` times (one game frame each). */
  frame(n?: number): void;
  /**
   * Boot as MZ's main.js does after the plugins load: Scene_Boot (database and save keys load
   * asynchronously), a new game, Scene_Map. Runs frames, yielding to the event loop between them,
   * until Scene_Map has started; rejects after `maxFrames`.
   */
  boot(maxFrames?: number): Promise<void>;
  /** What SceneManager.onError stopped the game on (errors, or rejections' reasons), oldest first. */
  readonly errors: readonly unknown[];
  /** The game loop stopped on an error (SceneManager.onError). */
  readonly stopped: boolean;
  /** The errors so far, forgotten by the shim (a test that expects one); the loop stays stopped. */
  takeErrors(): unknown[];
  /** Make the next storage `op` fail (reject, or throw for NW.js's synchronous remove). */
  failNext(op: "save" | "load" | "remove", error?: Error): void;
  /**
   * Make the next NW.js save die half-way, as a hard crash would: after MZ renamed the save file to
   * `<file>_` and before it wrote the new one. That save's Promise never settles.
   */
  crashNextSave(): void;
}

interface Installed {
  target: any;
  previous: Map<string, PropertyDescriptor | undefined>;
  cleanups: (() => void)[];
}

let current: Installed | null = null;
let currentShim: Shim | null = null;

/** An error or a rejection's reason as one line. */
export function describeError(e: unknown): string {
  return e instanceof Error ? `${e.name}: ${e.message}` : String(e);
}

/** What `frame()` and `resetShim()` throw once the game stopped on `errors`. */
const stoppedError = (errors: readonly unknown[]): Error =>
  new Error(`mz shim: the game stopped on an error (SceneManager.onError): ${errors.map(describeError).join("; ")}`);

const GAME_GLOBALS = ["$gameTemp", "$gameSystem", "$gameMessage", "$gameSwitches", "$gameVariables", "$gameMap", "$gamePlayer"];

function makeEvent(type: string, props: Record<string, unknown>): Event {
  const event = new Event(type, { bubbles: true, cancelable: true });
  for (const [k, v] of Object.entries(props)) Object.defineProperty(event, k, { value: v, enumerable: true });
  return event;
}

/** Install a fresh MZ shim on `target` (an installed one is reset first). */
export function installShim(target: any = globalThis, opts: ShimOptions = {}): Shim {
  if (current !== null) resetShim();
  const dom = opts.document === undefined && typeof target.document?.createElement === "function";
  const gamepads: (GamepadLike | null)[] = [];
  const realNav = target.navigator as NavigatorLike | undefined;
  const nav: NavigatorLike = opts.navigator
    ?? (typeof realNav?.getGamepads === "function" ? realNav : { getGamepads: () => gamepads });
  const doc: EventTarget = opts.document ?? (dom ? target.document : new EventTarget());
  const win: EventTarget = opts.window ?? (dom ? target : new EventTarget());
  const failures = new Map<string, Error[]>();
  let crashes = 0;
  const installed: Installed = { target, previous: new Map(), cleanups: [] };
  const errors: unknown[] = [];
  const ctx: Ctx = {
    g: target, opts, doc, win, nav, dom, errors,
    storage: opts.storage ?? new MemoryStorage(),
    listen(t, type, fn, options) {
      t.addEventListener(type, fn, options);
      installed.cleanups.push(() => t.removeEventListener(type, fn, options));
    },
    failpoint(op) {
      const queued = failures.get(op);
      const e = queued?.shift();
      if (e !== undefined) throw e;
    },
    crashpoint() {
      if (crashes === 0) return false;
      crashes--;
      return true;
    },
  };

  const members: Record<string, unknown> = {
    Utils: createUtils(ctx),
    Graphics: createGraphics(ctx),
    Input: createInput(ctx),
    TouchInput: createTouchInput(ctx),
    JsonEx: createJsonEx(ctx),
    StorageManager: createStorageManager(ctx),
    PluginManager: createPluginManager(ctx),
    DataManager: createDataManager(ctx),
    SceneManager: createSceneManager(ctx, (stop) => installed.cleanups.push(stop)),
    ...createGameObjects(ctx),
    ...createGameInterpreter(ctx),
    ...createScenes(ctx),
    ...createWindows(ctx),
    // MZ declares the database null; DataManager.loadDatabase() (from Scene_Boot.create) fills it.
    $dataSystem: null,
    $dataCommonEvents: null,
  };
  for (const name of GAME_GLOBALS) members[name] = null; // MZ declares them null until createGameObjects
  for (const [name, value] of Object.entries(members)) {
    installed.previous.set(name, Object.getOwnPropertyDescriptor(target, name));
    Object.defineProperty(target, name, { value, writable: true, configurable: true, enumerable: false });
  }
  current = installed;

  // Node stands in for the browser: what the browser fires at `window` for an uncaught exception or
  // a rejection nobody handled ("error", "unhandledrejection"), so SceneManager.onError sees it.
  // (node:test still fails the test on its own; the monitor event leaves Node's crash as it is.)
  // Before boot SceneManager.onReject does not listen yet (MZ: the browser prints the rejection and
  // the page goes on). This listener keeps Node from crashing on it, so it is printed here and the
  // process fails (its exit code 1): a plain Node driver never passes with a lost rejection.
  const proc = (globalThis as any).process;
  if (!dom && proc && typeof proc.on === "function" && typeof proc.removeListener === "function") {
    const onRejection = (reason: unknown, promise: unknown) => {
      win.dispatchEvent(makeEvent("unhandledrejection", { reason, promise }));
      if (target.SceneManager?._shimHandlers) return;
      console.error("mz shim: a Promise rejection nobody handled, before the game booted (no SceneManager.onReject yet):",
        reason instanceof Error ? (reason.stack ?? describeError(reason)) : describeError(reason));
      proc.exitCode = 1;
    };
    const onException = (error: unknown) => {
      win.dispatchEvent(makeEvent("error", { error, message: describeError(error) }));
    };
    proc.on("unhandledRejection", onRejection);
    proc.on("uncaughtExceptionMonitor", onException);
    installed.cleanups.push(() => {
      proc.removeListener("unhandledRejection", onRejection);
      proc.removeListener("uncaughtExceptionMonitor", onException);
    });
  }

  const g = target;
  g.Graphics.initialize();
  g.Input.initialize();
  g.TouchInput.initialize();
  if (opts.plugins) g.PluginManager.setup(opts.plugins);

  const dispatch = (type: string, props: Record<string, unknown>) => {
    const event = makeEvent(type, props);
    doc.dispatchEvent(event);
    return event;
  };
  const shim: Shim = {
    target, document: doc, window: win, storage: ctx.storage, gamepads, errors,
    get stopped() {
      return !!g.Graphics._shimStopped;
    },
    takeErrors() {
      return errors.splice(0);
    },
    keyDown: (keyCode, init = {}) => dispatch("keydown", { keyCode, which: keyCode, key: init.key ?? "", code: init.code ?? "" }),
    keyUp: (keyCode, init = {}) => dispatch("keyup", { keyCode, which: keyCode, key: init.key ?? "", code: init.code ?? "" }),
    pointer: (type, pageX, pageY, button = 0) => dispatch(type, { pageX, pageY, button }),
    frame(n = 1) {
      for (let i = 0; i < n; i++) {
        if (g.Graphics._shimStopped) throw stoppedError(errors);
        g.SceneManager.updateMain();
      }
    },
    crashNextSave() {
      crashes++;
    },
    failNext(op, error = new Error(`injected ${op} failure`)) {
      if (!failures.has(op)) failures.set(op, []);
      failures.get(op)!.push(error);
    },
    async boot(maxFrames = 120) {
      // main.js: SceneManager.run(Scene_Boot), its initialize() first (the error handlers)
      g.SceneManager.initialize();
      g.SceneManager.goto(g.Scene_Boot);
      for (let i = 0; i < maxFrames; i++) {
        shim.frame();
        const scene = g.SceneManager._scene;
        if (scene instanceof g.Scene_Map && scene.isStarted()) return;
        await new Promise((resolve) => setTimeout(resolve, 0));
      }
      throw new Error(`mz shim: boot did not reach Scene_Map in ${maxFrames} frames`);
    },
  };
  currentShim = shim;
  return shim;
}

/** `boot()` of the installed shim (see Shim.boot). */
export function bootShim(maxFrames?: number): Promise<void> {
  if (currentShim === null) return Promise.reject(new Error("mz shim: not installed"));
  return currentShim.boot(maxFrames);
}

/**
 * Remove the installed shim: its globals (restoring what was there before) and its listeners.
 * Then throws when the game stopped on an error nobody took (`shim.takeErrors()`): in an afterEach
 * that fails the test, as the error screen would have ended the game.
 */
export function resetShim(): void {
  const installed = current;
  if (installed === null) return;
  const left = currentShim?.takeErrors() ?? [];
  current = null;
  currentShim = null;
  for (const cleanup of installed.cleanups.splice(0)) cleanup();
  for (const [name, descriptor] of installed.previous) {
    if (descriptor === undefined) delete installed.target[name];
    else Object.defineProperty(installed.target, name, descriptor);
  }
  if (left.length > 0) throw stoppedError(left);
}
