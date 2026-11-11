/**
 * The game's side of an RPG Maker MZ web deployment: every call into the shared checkout page's
 * bridge (plugin/game/scfg/web/bridge.js, `window.scfgBridge`) behind a small method. The twin
 * of scfg_core/web_page.py and Godot's addons/scfg/web_page.gd.
 *
 * There is no eval boundary here (Ren'Py and Godot hand the page JavaScript source): the plugin
 * runs in the game's own page, so it calls the bridge's functions directly with JavaScript values
 * (strings, numbers, objects). Game data is never turned into code; the only scripts added to the
 * page are the four embedded asset files, as `<script>` elements whose text is the file's text.
 * What the bridge answers as JSON text (take, takePaste) is parsed in a try/catch: anything that is
 * not the expected JSON is "no messages" ([]) or "the browser refused" (""), never an exception.
 * A bridge call that throws (a replaced or broken bridge) answers the same and is reported once
 * per entry point through `warn` (developer advice: test play only in the glue).
 *
 * WebPage is the core OverlayCheckout's `ui` (open, show, close, take, setStatus, reply) and the
 * clipboard, links and wallet detection of a web deployment. Every call is synchronous: the page
 * answers a paste or a transaction build later, and the glue polls for it each frame. (Ren'Py's
 * post/result request bridge has no twin: the core's FetchTransport already uses fetch.)
 *
 * The overlay's host element (what bridge.js adds to the body for an opened checkout, tagged
 * `data-scfg-overlay="<key>"`) gets `guardOverlay` (ui/input.ts): its keys and pointer events are the overlay's, so MZ's document
 * listeners neither preventDefault them (Tab, the arrows, the wheel) nor turn them into the
 * game's or the sheet's input. Opening or showing an overlay gives the focus to its first control,
 * so the keyboard drives the overlay rather than the sheet hidden under it.
 */
import type { OverlayUi } from "../core/index.ts";
import { pageAsset, type PageFile } from "./assets.ts";
import { guardOverlay, OVERLAY_ATTR } from "./ui/input.ts";

/** The page's scripts, in order: bridge.js first (it marks the page as embedded for app.js). */
export const SCRIPTS: readonly PageFile[] = ["bridge.js", "evm.js", "solana.js", "app.js"];

export interface WebPageOptions {
  /** The page's window: its `document`, and `scfgBridge` once bridge.js ran. */
  window: any;
  /** Text of a shared page file. Default: the embedded copy (assets.ts). */
  asset?: (name: PageFile) => string;
  /** Developer advice (the bridge could not be installed, a bridge call failed). */
  warn?: (message: string) => void;
}

/** Where the chain is paid (a core Chain has both). */
export interface WalletChain {
  family: string;
  wallet_chain: string;
}

const NO_PAGE = "there is no page document to add the checkout page's scripts to, so the browser wallet checkout is off "
  + "(players can still pay by phone wallet or manual transfer).";
const NOT_RUN = "the checkout page's scripts did not run in this page (does a Content-Security-Policy block inline scripts?), "
  + "so the browser wallet checkout is off (players can still pay by phone wallet or manual transfer).";

const NO_HOST = "the checkout overlay has no host element tagged data-scfg-overlay (is another, older copy of the "
  + "checkout page's bridge in this page?), so its keys and clicks also reach the game.";

const NO_ANSWER = Symbol("no answer");

/** The controls a keyboard can reach, as the overlay's first one is looked up. */
const FOCUSABLE = "button:not([disabled]), a[href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), "
  + "[tabindex]:not([tabindex='-1'])";

/** JSON text as a value; NO_ANSWER for anything that is not JSON text. */
function parseJson(text: unknown): unknown {
  if (typeof text !== "string") return NO_ANSWER;
  try {
    return JSON.parse(text);
  } catch {
    return NO_ANSWER;
  }
}

/** A number or boolean the bridge answered (1 / 0). */
function truthy(answer: unknown): boolean {
  if (typeof answer === "boolean") return answer;
  return typeof answer === "number" && answer !== 0 && !Number.isNaN(answer);
}

export class WebPage {
  static readonly SCRIPTS = SCRIPTS;
  /** install() ran (whether or not the bridge came up: see `available`). */
  installed = false;
  private readonly _win: any;
  private readonly _asset: (name: PageFile) => string;
  private readonly _warnFn: ((message: string) => void) | null;
  private readonly _warned = new Set<string>();
  private readonly _guards = new Map<string, () => void>();
  /** The overlays' host elements (by key), to give them the focus. */
  private readonly _hosts = new Map<string, any>();

  constructor(opts: WebPageOptions) {
    this._win = opts.window;
    this._asset = opts.asset ?? pageAsset;
    this._warnFn = opts.warn ?? null;
  }

  /** The bridge is in the page: its calls do something. */
  get available(): boolean {
    return this._bridge() !== null;
  }

  /**
   * Add bridge.js and the checkout page's scripts to the page, then hand the bridge the page's
   * markup; once. A bridge already there (another copy of the plugin) is kept as it is.
   */
  install(): void {
    if (this.installed) return;
    this.installed = true;
    if (this._bridge() !== null) return;
    const doc = this._win?.document;
    const parent = doc ? doc.head ?? doc.body ?? doc.documentElement : null;
    if (!doc || typeof doc.createElement !== "function" || !parent) {
      this._warn(NO_PAGE);
      return;
    }
    try {
      for (const name of SCRIPTS) {
        const el = doc.createElement("script");
        el.setAttribute("data-scfg", name);
        el.textContent = this._asset(name); // an embedded file's text: never game data
        parent.appendChild(el);
      }
    } catch (e) {
      this._warn(`${NOT_RUN} (${e instanceof Error ? e.message : String(e)})`);
      return;
    }
    if (this._bridge() === null) {
      this._warn(NOT_RUN);
      return;
    }
    this._call("setPage", [this._asset("index.html")], undefined);
  }

  // --- wallets -----------------------------------------------------------------------------------

  /** Is there a wallet in the page that can pay on this chain? */
  hasWallet(chain: WalletChain): boolean {
    return truthy(this._call("hasWallet", [chain.family, chain.wallet_chain], 0));
  }

  // --- checkout overlay (core OverlayCheckout's ui) ------------------------------------------------

  open(key: string, sessionInfo: Record<string, unknown>, logoUrl: string | null): void {
    this._call("openCheckout", [key, sessionInfo, logoUrl], undefined);
    const host = this._overlayHost(key);
    if (host !== null && !this._guards.has(key) && typeof host.addEventListener === "function") {
      this._guards.set(key, guardOverlay(host));
      this._hosts.set(key, host);
    } else if (host === null && !this._guards.has(key) && this.available && !this._warned.has("overlayHost")) {
      this._warned.add("overlayHost");
      this._warn(NO_HOST);
    }
    this._focus(key);
  }

  show(key: string): void {
    this._call("show", [key], undefined);
    this._focus(key);
  }

  close(key: string): void {
    this._call("closeCheckout", [key], undefined);
    this._hosts.delete(key);
    const unguard = this._guards.get(key);
    if (unguard !== undefined) {
      this._guards.delete(key);
      unguard();
    }
  }

  /** The messages the overlay's page posted since the last call ([] for anything that is not a JSON list). */
  take(key: string): unknown[] {
    const parsed = parseJson(this._call("take", [key], "[]"));
    return Array.isArray(parsed) ? parsed : [];
  }

  setStatus(key: string, status: Record<string, unknown>): void {
    this._call("setStatus", [key, status], undefined);
  }

  reply(key: string, id: number, status: number, body: Record<string, unknown>): void {
    this._call("reply", [key, id, status, body], undefined);
  }

  // --- clipboard and links -------------------------------------------------------------------------

  copy(text: string): void {
    this._call("copy", [text], undefined);
  }

  requestPaste(): void {
    this._call("requestPaste", [], undefined);
  }

  /** null until the browser answers; "" if it refused (or answered anything but text); else the clipboard text. */
  takePaste(): string | null {
    const answer = this._call("takePaste", [], undefined);
    if (typeof answer !== "string") return null; // the page gave no answer at all
    const parsed = parseJson(answer);
    if (parsed === null) return null; // "null": not answered yet
    return typeof parsed === "string" ? parsed : "";
  }

  /** True once after a copy() the browser refused; false otherwise. */
  takeCopyFailed(): boolean {
    return truthy(this._call("takeCopyFailed", [], 0));
  }

  openUrl(url: string): void {
    this._call("openUrl", [url], undefined);
  }

  // --- internals -----------------------------------------------------------------------------------

  /**
   * Move the keyboard focus to the shown overlay's first control (its "Back to the game" button):
   * a keyboard-only player's keys then reach the overlay, not the sheet under it (Task 7 carry).
   */
  private _focus(key: string): void {
    const root = this._hosts.get(key)?.shadowRoot;
    if (!root || typeof root.querySelector !== "function") return;
    try {
      const first = root.querySelector(FOCUSABLE);
      if (first && typeof first.focus === "function") first.focus({ preventScroll: true });
    } catch {
      // the overlay still works with the mouse
    }
  }

  /**
   * The overlay's host: the element bridge.js tagged `data-scfg-overlay="<key>"` (compared as a
   * value, so no key needs escaping in a selector), with its shadow root; else null.
   */
  private _overlayHost(key: string): any {
    const doc = this._win?.document;
    if (!doc || typeof doc.querySelectorAll !== "function") return null;
    try {
      for (const el of Array.from(doc.querySelectorAll(`[${OVERLAY_ATTR}]`) as ArrayLike<any>)) {
        if (el && el.shadowRoot && el.getAttribute(OVERLAY_ATTR) === key) return el;
      }
    } catch {
      // no overlay found: it still works, unguarded
    }
    return null;
  }

  private _bridge(): Record<string, unknown> | null {
    let b: unknown;
    try {
      b = this._win?.scfgBridge;
    } catch {
      return null;
    }
    return typeof b === "object" && b !== null ? (b as Record<string, unknown>) : null;
  }

  /** `scfgBridge[name](...args)`, or `fallback` when there is no such function or it throws (reported once per name). */
  private _call(name: string, args: unknown[], fallback: unknown): unknown {
    const bridge = this._bridge();
    const fn = bridge?.[name];
    if (typeof fn !== "function") return fallback;
    try {
      return fn.apply(bridge, args);
    } catch (e) {
      if (!this._warned.has(name)) {
        this._warned.add(name);
        this._warn(`scfgBridge.${name} failed: ${e instanceof Error ? e.message : String(e)}`);
      }
      return fallback;
    }
  }

  private _warn(message: string): void {
    if (this._warnFn === null) return;
    try {
      this._warnFn(message);
    } catch {
      // advice only
    }
  }
}

/**
 * The overlay's page while the logo is still being read (web; Scfg._platformServer): the
 * overlay's open waits for the read, at most until `deadline` (on `clock`), then opens with the
 * logo, or without it once the deadline passed (a later logo is not used). The overlay polls
 * (take) every frame, which is when the wait is checked. Meanwhile its latest status is kept for
 * the open, show is moot (the open shows it), take answers nothing, and a close cancels the open.
 */
export class LogoWaitUi implements OverlayUi {
  private readonly _page: OverlayUi;
  private readonly _logo: () => string | null | undefined;
  private readonly _deadline: number;
  private readonly _clock: () => number;
  private _pending: { key: string; info: Record<string, unknown> } | null = null;
  private _status: Record<string, unknown> | null = null;
  private _opened = false;

  /** `logo()`: the read logo's data: URL, null (none, or unreadable), undefined while it is read. */
  constructor(page: OverlayUi, logo: () => string | null | undefined, deadline: number, clock: () => number) {
    this._page = page;
    this._logo = logo;
    this._deadline = deadline;
    this._clock = clock;
  }

  open(key: string, sessionInfo: Record<string, unknown>, _logoUrl: string | null): void {
    this._pending = { key, info: sessionInfo };
    this._status = null;
    this._opened = false;
    this._settle();
  }

  /** Open the waiting overlay once the logo is read or the deadline passed. */
  private _settle(): void {
    const waiting = this._pending;
    if (waiting === null) return;
    const logo = this._logo();
    if (logo === undefined && this._clock() < this._deadline) return;
    this._pending = null;
    this._opened = true;
    this._page.open(waiting.key, waiting.info, logo ?? null);
    const status = this._status;
    this._status = null;
    if (status !== null) this._page.setStatus(waiting.key, status);
  }

  show(key: string): void {
    this._settle();
    if (this._opened) this._page.show(key);
  }

  close(key: string): void {
    if (this._pending !== null) {
      this._pending = null; // never opened: nothing to close
      this._status = null;
      return;
    }
    this._opened = false;
    this._page.close(key);
  }

  take(key: string): unknown[] {
    this._settle();
    return this._opened ? this._page.take(key) : [];
  }

  setStatus(key: string, status: Record<string, unknown>): void {
    if (this._pending !== null) {
      this._status = status;
      this._settle();
      return;
    }
    this._page.setStatus(key, status);
  }

  reply(key: string, id: number, status: number, body: Record<string, unknown>): void {
    this._page.reply(key, id, status, body); // only for a message take() handed over: opened
  }
}
