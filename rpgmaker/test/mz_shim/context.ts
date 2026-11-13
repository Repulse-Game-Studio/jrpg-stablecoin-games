/**
 * What every shim module shares: the global object it is installed on, where input events come
 * from, the save storage and the options. Not part of MZ.
 */

/** Where StorageManager keeps its saves: one text per key (a save file path or a localforage key). */
export interface StorageBackend {
  /** Every key held (localforage.keys() on the web). */
  keys(): string[];
  has(key: string): boolean;
  read(key: string): string | null;
  write(key: string, text: string): void;
  remove(key: string): void;
}

/** The parts of the Gamepad API MZ's Input reads. */
export interface GamepadLike {
  connected: boolean;
  index: number;
  buttons: readonly { pressed: boolean }[];
  axes: readonly number[];
}

/** One entry of MZ's `$plugins` (js/plugins.js). */
export interface PluginEntry {
  name: string;
  status: boolean;
  description?: string;
  parameters: Record<string, string>;
}

export interface NavigatorLike {
  getGamepads?(): ArrayLike<GamepadLike | null> | null;
}

/** `$dataSystem.advanced.gameId` when the options name none (`ShimOptions.gameId`). */
export const DEFAULT_GAME_ID = 12345678;

export interface ShimOptions {
  /** `Utils.isNwjs()` and `StorageManager.isLocalMode()`: desktop (NW.js) instead of a browser. Default false. */
  nwjs?: boolean;
  /** `Utils.isMobileDevice()`. Default false. */
  mobile?: boolean;
  /**
   * `Utils.isOptionValid(name)` is true for these (["test"] = test play). Default: the page's
   * `location.search` split on "&" (`?test`), as MZ in a browser; none in Node.
   */
  options?: string[];
  /** Switches / variables in the database (`$dataSystem.switches.length - 1`). Default 20 each. */
  switches?: number;
  variables?: number;
  /** `$dataSystem.advanced.gameId`, part of the localforage key on the web. Default DEFAULT_GAME_ID (12345678). */
  gameId?: number;
  /**
   * The database's common events (`$dataCommonEvents`), common event 1 first: each one's command
   * list (and name), which `command117` (Common Event) runs in a child interpreter. Default: none.
   */
  commonEvents?: { name?: string; list: { code: number; indent: number; parameters: any[] }[] }[];
  /** `$dataSystem.gameTitle`. Default "Shim Game". */
  gameTitle?: string;
  /** Where StorageManager keeps saves. Default: a fresh in-memory `MemoryStorage`. */
  storage?: StorageBackend;
  /** `$plugins` entries whose parameters are registered as `PluginManager.setup` does (no script is loaded). */
  plugins?: PluginEntry[];
  /** Where Input/TouchInput listen. Default: the target's `document` and the target, else private EventTargets. */
  document?: EventTarget;
  window?: EventTarget;
  /** Default: the target's `navigator` if it has `getGamepads`, else a stub returning `shim.gamepads`. */
  navigator?: NavigatorLike;
  /** `Utils.RPGMAKER_VERSION`. Default "1.9.0" (any 1.x; not checked against a real build). */
  version?: string;
  /**
   * `$dataSystem.hasEncryptedImages` / `hasEncryptedAudio` / `encryptionKey` (a deployment made with
   * "Encrypt image files"): what `Utils.setEncryptionInfo` gets once the database loads. Default:
   * no encryption. `key` is 32 hex digits, as MZ's System.json stores it.
   */
  encryption?: { images?: boolean; audio?: boolean; key: string };
  /** `Graphics.width` / `height`. Default 816 x 624, MZ's default screen size. */
  width?: number;
  height?: number;
}

export interface Ctx {
  /** The global object (`globalThis` / `window`): MZ code reads every other class through it. */
  readonly g: any;
  readonly opts: ShimOptions;
  readonly doc: EventTarget;
  readonly win: EventTarget;
  readonly nav: NavigatorLike;
  readonly storage: StorageBackend;
  /** True when the shim runs in a page with a real DOM (the browser build). */
  readonly dom: boolean;
  /** Add a listener that `resetShim()` removes. */
  listen(target: EventTarget, type: string, fn: (e: any) => void, options?: AddEventListenerOptions | boolean): void;
  /** Throw the failure queued for `op` by `shim.failNext(op)`, if any. */
  failpoint(op: "save" | "load" | "remove"): void;
  /** True once for the NW.js save after `shim.crashNextSave()`: the game dies in the middle of it. */
  crashpoint(): boolean;
  /** What `SceneManager.onError` stopped the game on (an error, or a rejection's reason), oldest first. */
  readonly errors: unknown[];
}
