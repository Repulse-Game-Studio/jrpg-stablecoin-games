/**
 * Where the game runs, asked of RPG Maker MZ: desktop (NW.js: Chromium plus Node) or a browser,
 * on a phone or not, in test play or not. Node APIs are reached only through `requireNode`, which
 * looks `require` up at call time and only on NW.js, so nothing here touches Node in a browser.
 *
 * The glue takes these as a `Platform` (see `defaultPlatform`), so tests can stand in for NW.js
 * (`require("fs")`, the game folder, the `nw` object) and the page (`window`, `navigator`)
 * without real ones. What the glue does with them is in actions.ts.
 */

/** The MZ globals the glue reads (`Utils`, `StorageManager`, ...), on the page's global object. */
export function mz(): any {
  return globalThis as any;
}

/** `Utils[name](...args)` as a boolean; false when MZ's Utils or that method is missing. */
function askUtils(name: string, ...args: unknown[]): boolean {
  const utils = mz().Utils;
  return !!(utils && typeof utils[name] === "function" && utils[name](...args));
}

/** True on desktop: the game runs in NW.js (`Utils.isNwjs()`). */
export function isNwjs(): boolean {
  return askUtils("isNwjs");
}

/** True in a browser deployment (anything that is not NW.js). */
export function isWeb(): boolean {
  return !isNwjs();
}

/** True on a phone or tablet (`Utils.isMobileDevice()`, Ren'Py's renpy.mobile). */
export function isMobile(): boolean {
  return askUtils("isMobileDevice");
}

/** True in test play (`Utils.isOptionValid("test")`): the editor's Playtest. Developer warnings show only then. */
export function isTestPlay(): boolean {
  return askUtils("isOptionValid", "test");
}

/** A Node module on NW.js (`require(name)`); an Error anywhere else. */
export function requireNode(name: string): any {
  const req = mz().require;
  if (!isNwjs() || typeof req !== "function") throw new Error(`scfg: Node's ${name} is only available on desktop (NW.js)`);
  return req(name);
}

/**
 * The game folder on NW.js, where `img/` and `save/` live: the folder of the page NW.js started,
 * as MZ's `StorageManager.fileDirectoryPath()` finds it. Null in a browser.
 */
export function gameDir(): string | null {
  if (!isNwjs()) return null;
  const path = requireNode("path");
  const main = mz().process?.mainModule;
  return main && typeof main.filename === "string" ? path.dirname(main.filename) : null;
}

/** NW.js's `nw` object (`nw.Shell`, `nw.Clipboard`) on desktop; null anywhere else or when it is missing. */
export function nwObject(): any {
  if (!isNwjs()) return null;
  try {
    const nw = mz().nw;
    return typeof nw === "object" && nw !== null ? nw : null;
  } catch {
    return null;
  }
}

/** The page's `window` (window.open, its unload events), or null without one (Node). */
export function pageWindow(): any {
  const w = mz().window;
  return typeof w === "object" && w !== null ? w : null;
}

/** The page's `navigator` (navigator.clipboard), or null. */
export function pageNavigator(): any {
  const n = mz().navigator;
  return typeof n === "object" && n !== null ? n : null;
}

/**
 * A browser: can MZ's storage keep the save data? True once MZ's `localforage` started (IndexedDB,
 * or its localStorage fallback); false without localforage or when no storage driver works there
 * (blocked site data, a third-party iframe). Never rejects.
 */
export async function webStoragePersists(): Promise<boolean> {
  const lf = mz().localforage;
  if (!lf || typeof lf.ready !== "function") return false;
  try {
    await lf.ready();
    return true;
  } catch {
    return false;
  }
}

/** The platform answers the glue uses; tests replace single members. */
export interface Platform {
  isNwjs(): boolean;
  isWeb(): boolean;
  isMobile(): boolean;
  isTestPlay(): boolean;
  requireNode(name: string): any;
  gameDir(): string | null;
  /** Show a developer message over the game (test play only; see `showDevNotice`). */
  devNotice(text: string): void;
  /** NW.js's `nw` object, or null (a browser, or NW.js without it). Never throws. */
  nw(): any;
  /** The page's `window`, or null. */
  window(): any;
  /** The page's `navigator`, or null. */
  navigator(): any;
  /** A browser: whether MZ's storage can keep the receipts (see `webStoragePersists`). */
  webStoragePersists(): Promise<boolean>;
}

/**
 * A developer message over the game, in test play only: a fixed box in the page's corner, one line
 * per message, closed by clicking it. MZ's own error screen stops the game, which a setup mistake
 * in this plugin must not do. Nothing happens without a page (`document.body`).
 */
export function showDevNotice(text: string): void {
  if (!isTestPlay()) return;
  const doc = (globalThis as any).document as Document | undefined;
  if (!doc || !doc.body || typeof doc.createElement !== "function") return;
  let box = doc.getElementById("scfg-dev-notice");
  if (box === null) {
    box = doc.createElement("div");
    box.id = "scfg-dev-notice";
    box.title = "Stablecoin for Games: click to close";
    box.style.cssText = "position:fixed;left:8px;top:8px;z-index:2147483647;max-width:60%;padding:8px 12px;"
      + "background:#7a1020;color:#fff;font:13px/1.4 sans-serif;border-radius:6px;white-space:pre-wrap;cursor:pointer";
    box.addEventListener("click", () => box!.remove());
    doc.body.appendChild(box);
  }
  const line = doc.createElement("div");
  line.textContent = "scfg: " + text;
  box.appendChild(line);
}

export const defaultPlatform: Platform = {
  isNwjs, isWeb, isMobile, isTestPlay, requireNode, gameDir, devNotice: showDevNotice,
  nw: nwObject, window: pageWindow, navigator: pageNavigator, webStoragePersists,
};
