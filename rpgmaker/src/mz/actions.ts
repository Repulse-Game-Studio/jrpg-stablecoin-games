/**
 * What the sheet's buttons do outside the game, per platform (Ren'Py's `_open_url`, `Copy` and the
 * paste of `_take_paste`), and when the game is closing. Everything goes through the `Platform`
 * seams (platform.ts), so the tests run the desktop, web and mobile paths with fakes. Nothing here
 * throws when NW.js's `nw` object or a browser API is missing.
 *
 * - Links: desktop (NW.js) `nw.Shell.openExternal(url)`, the player's own browser or wallet app;
 *   in a browser (web, phones) `window.open(url, "_blank", "noopener")`. NW.js without `nw` falls
 *   back to `window.open`.
 * - Copy: NW.js `nw.Clipboard.get().set(text, "text")`; a browser `navigator.clipboard.writeText`,
 *   which the browser may refuse (the glue then tells the player).
 * - Paste: NW.js `nw.Clipboard.get().get("text")`, synchronous and never refused; a browser
 *   `navigator.clipboard.readText()`, asynchronous, which the browser may refuse.
 * - Closing: see `watchClosing` (page events only; no NW.js window listener).
 */
import type { Platform } from "./platform.ts";

/** Characters that can appear in a transaction hash (hex) or a Solana signature (base58): Ren'Py's TX_CHARS. */
export const TX_CHARS = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
/** The longest hash field (Ren'Py's `input ... length 90`). */
export const PASTE_MAX = 90;

/** Python's `str.isspace()` for one UTF-16 code unit (what `str.split()` splits at). */
function isPySpace(c: number): boolean {
  if (c <= 0x20) return c === 0x20 || (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f);
  if (c < 0x85) return false;
  return c === 0x85 || c === 0xa0 || c === 0x1680 || (c >= 0x2000 && c <= 0x200a) || c === 0x2028 || c === 0x2029
    || c === 0x202f || c === 0x205f || c === 0x3000;
}

/**
 * What a pasted clipboard becomes in a hash field, like typing it there: Ren'Py's `_clean_paste`,
 * `"".join(ch for ch in (text or "").split()[0] if ch in TX_CHARS)[:90]`. The scan stops at the
 * 90th kept character, so a huge clipboard costs nothing (see `scanPaste`).
 */
export function cleanPaste(text: unknown): string {
  const s = typeof text === "string" ? text : "";
  return scanPaste((i) => s.charCodeAt(i), s.length);
}

/** `cleanPaste` over `length` UTF-16 code units read one at a time by `at(i)`, each at most once. */
export function scanPaste(at: (i: number) => number, length: number): string {
  let i = 0;
  let c = 0;
  while (i < length && isPySpace((c = at(i)))) i++;
  let out = "";
  while (i < length && out.length < PASTE_MAX) {
    if (isPySpace(c)) break;
    if (c < 0x80 && TX_CHARS.includes(String.fromCharCode(c))) out += String.fromCharCode(c);
    i++;
    if (i < length && out.length < PASTE_MAX) c = at(i);
  }
  return out;
}

/** NW.js's clipboard (`nw.Clipboard.get()`), or null. */
function nwClipboard(p: Platform): any {
  if (!p.isNwjs()) return null;
  try {
    const clip = p.nw()?.Clipboard?.get?.();
    return clip && typeof clip.get === "function" && typeof clip.set === "function" ? clip : null;
  } catch {
    return null;
  }
}

/** Open `url` outside the game (see the header). False when there is nothing to open it with. */
export function openExternal(p: Platform, url: string): boolean {
  if (p.isNwjs()) {
    try {
      const shell = p.nw()?.Shell;
      if (shell && typeof shell.openExternal === "function") {
        shell.openExternal(url);
        return true;
      }
    } catch {
      // the page's window.open below
    }
  }
  try {
    const w = p.window();
    if (w && typeof w.open === "function") {
      w.open(url, "_blank", "noopener");
      return true;
    }
  } catch {
    // a browser that refuses it outright
  }
  return false;
}

/** Copy `text` to the clipboard: resolves once copied, rejects when it was refused or there is no clipboard. */
export function copyText(p: Platform, text: string): Promise<void> {
  const clip = nwClipboard(p);
  if (clip !== null) {
    try {
      clip.set(text, "text");
      return Promise.resolve();
    } catch {
      // the browser's clipboard below
    }
  }
  try {
    const cb = p.navigator()?.clipboard;
    if (!cb || typeof cb.writeText !== "function") return Promise.reject(new Error("no clipboard"));
    return Promise.resolve(cb.writeText(text)).then(() => undefined);
  } catch (e) {
    return Promise.reject(e);
  }
}

/** NW.js: the clipboard's text, read at once (never refused); null where there is no `nw` clipboard. */
export function readDesktopClipboard(p: Platform): string | null {
  const clip = nwClipboard(p);
  if (clip === null) return null;
  try {
    const text = clip.get("text");
    return typeof text === "string" ? text : "";
  } catch {
    return null;
  }
}

/** A browser: the clipboard's text; rejects when the browser refuses or has no clipboard. */
export function readBrowserClipboard(p: Platform): Promise<string> {
  try {
    const cb = p.navigator()?.clipboard;
    if (!cb || typeof cb.readText !== "function") return Promise.reject(new Error("no clipboard"));
    return Promise.resolve(cb.readText()).then((t: unknown) => (typeof t === "string" ? t : ""));
  } catch (e) {
    return Promise.reject(e);
  }
}

/** What the game does when it is closing. */
export interface ClosingHandlers {
  /** Shut down (Ren'Py's `_shutdown`) and flush the save; resolves when done or given up. Never rejects. */
  quit(): Promise<void>;
  /** Write what is not stored yet, without shutting down (the page may stay). */
  flush(): void;
}

/**
 * Watch for the game closing:
 * - Everywhere: `pagehide` quits, unless the page goes into the back/forward cache (`persisted`)
 *   and may come back, which only flushes; `visibilitychange` to hidden flushes (a phone's browser
 *   may end a background tab without any other event).
 * - NW.js: `beforeunload` quits (the window is closing or reloading). No `nw.Window` "close"
 *   listener is registered: a close listener keeps NW.js from closing the window, so it would have
 *   to force the close itself and override other plugins' own close handling (a "really quit?"
 *   prompt, their own save), and NW.js gives no way to see whether others listen.
 * - A browser: `beforeunload` only flushes, since another script may still cancel the unload.
 * MZ's own exit (`SceneManager.terminate`) is hooked in boot.ts.
 *
 * Trade-off: the quit-time flush is best effort. Receipts and session changes (a session added or
 * removed, its state or tx hint) are saved at once anyway; what a window close can lose is the
 * scan progress since the last save (at most PENDING_SAVE_INTERVAL, 10 s), which the next launch
 * scans again.
 */
export function watchClosing(p: Platform, h: ClosingHandlers): void {
  const desktop = p.isNwjs();
  const w = p.window();
  if (!w || typeof w.addEventListener !== "function") return;
  w.addEventListener("beforeunload", () => {
    if (desktop) void h.quit();
    else h.flush();
  });
  w.addEventListener("pagehide", (e: { persisted?: boolean } | undefined) => {
    if (e && e.persisted) h.flush();
    else void h.quit();
  });
  // the event is fired at the document; the page's state is document.visibilityState
  const doc = w.document && typeof w.document.addEventListener === "function" ? w.document : null;
  (doc ?? w).addEventListener("visibilitychange", () => {
    if ((doc ?? w).visibilityState === "hidden") h.flush();
  });
}
