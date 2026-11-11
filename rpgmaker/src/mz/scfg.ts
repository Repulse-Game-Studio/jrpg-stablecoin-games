/**
 * Stablecoin for Games (SCFG) for RPG Maker MZ: USDC/USDT tips and unlock purchases paid straight
 * to the developer's wallet. No server, no accounts. One `Scfg` lives at `window.SCFG` (boot.ts
 * makes it from the plugin parameters). It is the twin of plugin/game/scfg/scfg_api.rpy and of
 * Godot's addons/scfg/scfg.gd: when this file and the plan are silent, the Ren'Py glue decides.
 *
 *     SCFG.addChain("myl2", {...});                           // optional: your own EVM chain, before configure
 *     SCFG.configure({ wallet: "0x...", testnet: true });     // or the plugin parameters
 *     SCFG.addItem("chapter2", { code: 1, price: "4.99", token: "USDC", title: "Chapter 2" });
 *     if (SCFG.owns("chapter2")) { ... }
 *
 * DEVELOPER ERRORS throw, as Ren'Py raises (Ruling M2-3): `configure`, `addChain`, `addItem` and
 * the queries throw a ConfigError (with a `code`) or a TypeError; a misused `pay`/`restore`
 * rejects. What is not the script's mistake (payments cannot run now, no screen to show) resolves
 * a "failed" result with the code instead (Ruling 10, as Godot): MZ's SceneManager.onReject stops
 * the game on a rejection nobody handled. Errors of the running plugin are printed with
 * `console.error("scfg: ...")`; setup advice only in test play (`console.warn`).
 *
 * STATE. Receipts and pending payments are kept by persist.ts / store.ts in MZ's storage
 * (`StorageManager`, key "scfg"), global to the install and never inside a save slot. They are
 * read once MZ's database and save keys are ready (boot.ts; Scene_Boot waits for them). The
 * Engine and the Worker start on the first frame after that once configure() succeeded
 * (`_ensureStarted`, from the pump), with a monotonic clock for the RPC clients.
 *
 * THE PUMP (`pump()`, after every `SceneManager.updateMain`), in Ren'Py's `_periodic` order: drain
 * the Worker's outcomes, give each to every open and background checkout controller and deliver
 * its result; then `poll()` every controller and register its callback; open the browser on a
 * desktop page that started listening since "Open checkout in browser" was pressed (Ruling 1);
 * close background controllers whose session is final or older than the browser timeout; poll
 * the restore controllers and deliver a finished one; warn once about unready routes; then mirror
 * a new snapshot to storage (persist.ts).
 *
 * FLOWS (Ren'Py's _open_checkout / _close_checkout / _open_restore / _close_restore, as Godot's
 * Scfg.pay / Scfg.restore). `pay()` and `restore()` build a core controller, show it through
 * `sheetHost` (sheet_host.ts) and wait for its handle's `closed`. Closing a payment sheet closes
 * its controller first (cancel / finish stop its browser page), and only then the screen;
 * "keep_playing" moves the controller to `_background`, where its page keeps answering until the
 * session is final or the browser timeout passes. Misuse rejects (Ruling M2-3); payments off or
 * no screen resolve "failed" (Ruling 10). The sheet's
 * buttons that leave the game call `openBrowser`, `openWalletApp`, `openExplorer`, `copy` and
 * `pasteInto` (actions.ts has the per-platform parts). Notifications go through `text()`.
 *
 * WEB (not NW.js; Ren'Py's WEB branches): the browser checkout is the in-page overlay
 * (core/overlay.ts) over the shared page's bridge (web_page.ts), installed on the first payment
 * sheet; the browser wallet method is offered only when the page has a wallet for the chain
 * (`browser_wallet`); copy, paste and links go through the page (its copy falls back to
 * execCommand where the clipboard API is missing, and a refused copy or paste is told). When the
 * bridge cannot run in the page (reported in test play) these fall back to the browser's own APIs
 * and there is no browser wallet method. In test play a web game warns once when its storage
 * (localforage) cannot keep the receipts.
 *
 * CLOSING (Ren'Py's `_shutdown`; boot.ts installs the hooks): flows waiting on a screen resume at
 * once and deliver nothing, every page is stopped, the Worker stops, and what is not stored yet
 * (scan progress inside the save interval) is written before the window closes.
 */
import {
  CheckoutController, ConfigError, makePaymentResult, makeRestoreResult, ScfgError, Dec, Engine, FetchTransport, RestoreController, Worker, addItem, backendFor, buildChain,
  buildConfig, computeOwnership, logoContentType, OverlayCheckout, parseUnreadyKey,
  preparePayment,
  type Chain, type CheckoutServer, type Config, type EngineOptions, type Job, type PaymentResult, type RestoreResult,
  type ServerArgs, type Snapshot, type Transport, type WorkerOptions,
} from "../core/index.ts";
import { cleanPaste, copyText, openExternal, readBrowserClipboard, readDesktopClipboard } from "./actions.ts";
import { STATUS } from "./commands.ts";
import { pageAsset } from "./assets.ts";
import { CheckoutServer as LocalServer, isBytes, type RandomSource } from "./local_server.ts";
import { NodeHttpsTransport } from "./node_transport.ts";
import { StateMirror } from "./persist.ts";
import { defaultPlatform, mz, type Platform } from "./platform.ts";
import { isSheetHandle, type SheetHandle, type SheetHost } from "./sheet_host.ts";
import { DefaultSheetHost } from "./ui/default_sheet_host.ts";
import { resolveText } from "./texts.ts";
import { ScfgStore, messageOf, plainCopy, type StateDoc } from "./store.ts";
import { LogoWaitUi, WebPage } from "./web_page.ts";
import { VERSION } from "./version.ts";

export { VERSION } from "./version.ts";
/** Web: how long (seconds) Open wallet waits for a logo still being read before it opens the overlay without it. */
export const LOGO_WAIT_S = 1;
/** exportReceipts() on desktop: this file in MZ's save folder. */
export const EXPORT_NAME = "scfg-receipts.json";
/** The logo option names a file in this folder of the game (`img/scfg/logo.png` for "logo"). */
export const LOGO_DIR = "img/scfg/";
/** How long the game's close waits for the last save (ms). */
export const QUIT_FLUSH_MS = 2000;
/** Web: how long after a Copy the page is asked whether the browser refused it (Ren'Py's copy_watch_until), seconds. */
export const COPY_WATCH_S = 5;

/** pay()'s options. */
export interface PayOptions {
  item?: string | null;
  amount?: string | null;
  token?: string | null;
  /** Gets this call's own already_owned or cancelled result, and every later result of the payment. */
  callback?: ((result: PaymentResult) => void) | null;
}

/** restore()'s options. */
export interface RestoreOptions {
  /** Gets the result once a restore finishes (while the screen is still open). */
  callback?: ((result: RestoreResult) => void) | null;
}

const PAY_OPTIONS = ["item", "amount", "token", "callback"];
const RESTORE_OPTIONS = ["callback"];

/** The options object of pay() / restore(), checked as Python checks keyword arguments. */
function optionsOf(fn: string, opts: unknown, known: string[]): Record<string, any> {
  if (opts === undefined || opts === null) return {};
  if (typeof opts !== "object" || Array.isArray(opts)) throw new TypeError(`${fn}() takes an options object`);
  for (const k of Object.keys(opts)) {
    if (!known.includes(k)) throw new TypeError(`${fn}() got an unexpected option ${JSON.stringify(k)}`);
  }
  const cb = (opts as Record<string, unknown>)["callback"];
  if (cb !== undefined && cb !== null && typeof cb !== "function") {
    throw new TypeError(`${fn}(): callback must be a function, not ${typeof cb}`);
  }
  return opts as Record<string, any>;
}

/** Payments cannot run now (the state is unreadable or still being read, the Engine failed to start, the game is closing). */
function unavailable(why: string): ScfgError {
  return Object.assign(new ScfgError(`payments are off: ${why}`), { code: "payments.unavailable" });
}

/** Why a flow cannot run that is not the script's mistake (Ruling 10): it resolves "failed" with this code. */
interface Unavailable {
  code: "payments.unavailable" | "sheet.unavailable";
  message: string;
  /** Already reported (the Engine's start failure, the unreadable state): not again per call. */
  reported: boolean;
}

/** A payment sheet's controller: the core's, plus its id and the pay() callback. */
export class ScreenCheckout extends CheckoutController {
  cid = "";
  callback: ((result: PaymentResult) => void) | null = null;
  /** the session id _registerCallback registered `callback` for */
  callbackSid: string | null = null;
}

/** A restore screen's controller, with its id. */
export class ScreenRestore extends RestoreController {
  rid = "";
}

/** TEST HOOKS, not part of the API: defaults replaced before first use. */
export interface ScfgHooks {
  /** Seconds, monotonic: the save interval and the RPC clients' cooldowns. Default `performance.now() / 1000`. */
  clock?: () => number;
  /** The Engine's transport. Default: NodeHttpsTransport on NW.js (Ruling M2-1), FetchTransport in a browser. */
  transport?: Transport;
  /** Extra Engine options (rpc_factory, clock, entropy). */
  engine_opts?: Partial<EngineOptions>;
  /** Extra Worker options (schedule / cancel: the timer source). */
  worker_opts?: Partial<WorkerOptions>;
  store?: ScfgStore;
  platform?: Partial<Platform>;
  /** The checkout controllers' page (the browser checkout), instead of the platform's. */
  server_factory?: (args: ServerArgs) => CheckoutServer;
  /** Where reports go. Default `console`. */
  log?: { error(...args: unknown[]): void; warn(...args: unknown[]): void };
}

const monotonic = (): number => (globalThis as any).performance.now() / 1000;
const isObject = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);
const ZERO = Dec.fromInt(0);

/** Python's `json.dumps(value, indent=2, sort_keys=True)` (ensure_ascii included). */
export function pyJsonDump(value: unknown): string {
  const sorted = (v: unknown): unknown => {
    if (Array.isArray(v)) return v.map(sorted);
    if (!isObject(v)) return v;
    const out: Record<string, unknown> = {};
    for (const k of Object.keys(v).sort()) {
      Object.defineProperty(out, k, { value: sorted(v[k]), enumerable: true, writable: true, configurable: true });
    }
    return out;
  };
  return JSON.stringify(sorted(value), null, 2)
    .replace(/[\u007f-￿]/g, (c) => "\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"));
}

/** Bytes as base64 (the page's btoa; NW.js and every browser have it). */
function base64Of(bytes: Uint8Array): string {
  let bin = "";
  for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return (globalThis as any).btoa(bin);
}

/** A logo option as a game file: under img/scfg/, ".png" when no extension is given. */
export function logoFile(name: unknown): unknown {
  if (typeof name !== "string" || name === "") return name; // the core refuses it
  const path = name.startsWith(LOGO_DIR) ? name : LOGO_DIR + name;
  const last = path.slice(path.lastIndexOf("/") + 1);
  return last.includes(".") ? path : path + ".png";
}

/**
 * WebCrypto's getRandomValues from Node's crypto module (NW.js's require("crypto")): its own
 * getRandomValues (Node 17+), else webcrypto's (Node 15+), else randomFillSync (Node 9+);
 * undefined when the module has none (the server then uses the page's crypto).
 */
function randomSource(crypto: any): RandomSource | undefined {
  if (crypto && typeof crypto.getRandomValues === "function") return { getRandomValues: (b) => crypto.getRandomValues(b) };
  const web = crypto?.webcrypto;
  if (web && typeof web.getRandomValues === "function") return { getRandomValues: (b) => web.getRandomValues(b) };
  if (crypto && typeof crypto.randomFillSync === "function") return { getRandomValues: (b) => crypto.randomFillSync(b) };
  return undefined;
}

export class Scfg {
  readonly version = VERSION;
  /** The status codes the plugin commands store in their variable (Ruling M2-4): `SCFG.STATUS.paid === 1`. */
  readonly STATUS = STATUS;
  /** Show a short notification when a payment confirms or purchases are restored (parameter "notify"). */
  notify = true;
  /** A final result of a payment that has no pay() callback (Ren'Py's `on_result`). */
  onResult: ((result: PaymentResult) => void) | null = null;
  /** The plugin's own sheet host (ui/default_sheet_host.ts): the HTML payment sheet and restore screen over the game. */
  readonly _defaultHost: DefaultSheetHost = new DefaultSheetHost(this);
  /** How the payment sheet and the restore screen are shown (sheet_host.ts); replace it to show them your own way. */
  sheetHost: SheetHost = this._defaultHost;
  /** A stylesheet appended to the default sheet's own (the class names are in ui/margin.css). */
  css = "";
  /** Overrides of the player-facing texts by id (`SCFG.text`); they win over the "Texts" plugin parameter. */
  texts: Record<string, string> = {};

  // Everything below is internal (tests read it, as Godot's tests read the autoload's fields).
  _config: Config | null = null;
  _customChains: Chain[] = [];
  /** Why the plugin is disabled (its parameters are invalid), or null. */
  _disabled: string | null = null;
  _engine: Engine | null = null;
  _worker: Worker | null = null;
  /** The Engine could not start from this config and state (or the state is unreadable). */
  _startError: unknown = null;
  _checkouts = new Map<string, ScreenCheckout>();
  /** "Keep playing" controllers, whose browser page still answers until the session is final. */
  _background = new Map<string, ScreenCheckout>();
  _restores = new Map<string, ScreenRestore>();
  /** session id -> the pay() callback, until the session is over (see _pruneCallbacks) */
  _callbacks = new Map<string, (result: PaymentResult) => void>();
  /** the callbacks' session ids the last snapshot listed as pending */
  _seenPending = new Set<string>();
  _restoreCallbacks = new Map<string, (result: RestoreResult) => void>();
  _lastSnapshot: Snapshot | null = null;
  _warnedUnready = "";
  _warnedEarly = false;
  /** clearLocalData() in progress: the snapshots made before the Engine was wiped are skipped. */
  _wipe: { job: Job<null> | null; done: boolean; before: Snapshot | null } | null = null;
  /** Bumped whenever the receipts change (screens compare it instead of copying receipts). */
  _receiptsRev = 0;
  /** The "Texts" plugin parameter (`SCFG.texts` wins over it). */
  _paramTexts: Record<string, string> = {};
  _nextId = 0;
  /** The game is closing (`_shutdown` ran): no more frames, flows or payments. */
  _closing = false;
  /** Releases a flow waiting on its screen (`_shutdown`). */
  private readonly _waits = new Set<() => void>();
  private _pasteSeq = 0;
  /**
   * Desktop: "Open checkout in browser" pressed while the controller's page was still starting
   * (Node's listen() completes on a later turn: its url is "" until then, Ruling 1). The browser
   * opens on the first frame the page listens; a page that cannot listen fails like a start().
   */
  readonly _pendingOpens = new Set<ScreenCheckout>();
  /** Set while a failed page's open is replayed through the controller (its start() then throws this). */
  private _refuseServer: unknown = null;
  readonly _mirror: StateMirror;
  readonly _platform: Platform;
  readonly _hooks: ScfgHooks;
  readonly _clock: () => number;
  private readonly _log: NonNullable<ScfgHooks["log"]>;
  private readonly _listeners = new Set<() => void>();
  private _logoUrl: Promise<string | null> | null = null;
  /** The logo's data: URL once read (undefined until then): the overlay takes it as it is when it opens. */
  private _logoRead: string | null | undefined = undefined;
  /** Web: the page's bridge, built on the first payment sheet (`_webPage()`). */
  _page: WebPage | null = null;
  /** Web: until when (`_clock`) the page is asked whether a Copy was refused. */
  private _copyWatchUntil = -Infinity;
  /** Web: the Paste press waiting for the page's clipboard answer. */
  private _pasteTarget: { ctl: ScreenCheckout | ScreenRestore; seq: number } | null = null;
  private _warnedStorage = false;

  constructor(hooks: ScfgHooks = {}) {
    this._hooks = hooks;
    this._clock = hooks.clock ?? monotonic;
    this._log = hooks.log ?? console;
    this._platform = { ...defaultPlatform, ...hooks.platform };
    this._mirror = new StateMirror(hooks.store ?? new ScfgStore(), {
      clock: this._clock,
      reportError: (heading, e) => this._reportError(heading, e),
      alert: (message) => this._alert(message),
      receiptsChanged: () => this._emitReceiptsChanged(),
    });
  }

  // --- reports -------------------------------------------------------------------------------

  /** Setup advice for the developer (Ren'Py's `_warn`): test play only. */
  _warn(message: string): void {
    if (this._platform.isTestPlay()) this._log.warn("scfg: " + message);
  }

  /** An error of the running plugin, for the developer (Ren'Py's `_report_error`). */
  _reportError(heading: string, e?: unknown): void {
    if (e === undefined) this._log.error("scfg: " + heading);
    else this._log.error("scfg: " + heading, e);
  }

  /** A setup or storage problem: reported, and shown over the game in test play (Ruling M2-3). */
  _alert(message: string): void {
    this._log.error("scfg: " + message);
    try {
      this._platform.devNotice(message);
    } catch {
      // the console has it
    }
  }

  private readonly _logError = (e: unknown): void => this._reportError("worker error:", e);

  // --- configuration -------------------------------------------------------------------------

  /** Boot found invalid plugin parameters: everything that needs a configuration throws. */
  _disable(why: string): void {
    this._disabled = why;
    this._config = null;
    this._customChains = [];
  }

  private _checkEnabled(): void {
    if (this._disabled !== null) {
      throw new ConfigError(`SCFG is disabled because its plugin parameters are invalid: ${this._disabled}`,
        { code: "params.invalid" });
    }
  }

  /**
   * Add an EVM chain that isn't built in (Optimism, Avalanche, your own L2), before configure().
   * `opts`: name, chain_id, rpc, tokens ({"USDC": "0x..."}, 6-decimal tokens only), confirmations,
   * explorer_tx (an https url with `{}` where the hash goes), and optionally native_symbol,
   * fee_hint, testnet and max_log_range.
   */
  addChain(key: unknown, opts: Record<string, unknown> = {}): void {
    this._checkEnabled();
    if (this._config !== null) {
      throw new ConfigError("call SCFG.addChain() before SCFG.configure()", { code: "configure.after_add_chain" });
    }
    this._customChains.push(buildChain(key, opts as any));
  }

  /**
   * Set the payout wallet and options (Ren'Py's keyword arguments, by their Python names):
   * wallet and/or solana_wallet, testnet, chains, tokens, methods, rpc_urls, confirmations,
   * previous_wallets, logo and accent. `logo` names an image in img/scfg/ ("logo" is
   * img/scfg/logo.png). It can be called again until payments have started (the frame after the
   * state is read).
   *
   * The logo (Ruling 11): in test play on desktop a logo that is neither in the game folder nor
   * shipped encrypted (`<file>_`) is a ConfigError "logo.file_missing" (the boot reports it and
   * the plugin stays disabled). A released game never stops taking payments for its logo: one
   * that cannot be read is a console warning and the screens show none.
   */
  configure(opts: Record<string, unknown> = {}): void {
    this._checkEnabled();
    if (this._worker !== null || this._startError !== null) {
      throw new ConfigError("SCFG.configure() can't change the configuration once payments have started",
        { code: "configure.after_start" });
    }
    if (!isObject(opts)) throw new TypeError("configure() takes an options object");
    if (Object.prototype.hasOwnProperty.call(opts, "custom_chains")) {
      throw new TypeError("configure() got an unexpected keyword argument 'custom_chains' (use SCFG.addChain())");
    }
    const o: Record<string, unknown> = { ...opts, custom_chains: this._customChains };
    if (o["logo"] !== undefined && o["logo"] !== null) o["logo"] = logoFile(o["logo"]);
    const cfg = buildConfig(o as any);
    if (cfg.logo !== null && this._platform.isNwjs() && this._platform.isTestPlay() && !this._imagesEncrypted()) {
      const dir = this._platform.gameDir();
      if (dir !== null) {
        const path = this._platform.requireNode("path");
        const fs = this._platform.requireNode("fs");
        if (!fs.existsSync(path.join(dir, cfg.logo)) && !fs.existsSync(path.join(dir, cfg.logo + "_"))) {
          throw new ConfigError(`logo file not found in the game folder: ${JSON.stringify(cfg.logo)}`, { code: "logo.file_missing" });
        }
      }
    }
    this._config = cfg;
    this._logoUrl = null;
    this._logoRead = undefined;
  }

  /** Declare a paid unlock. `opts`: code (1-99, never change it after release), price ("4.99"), token, title, min_price. */
  addItem(itemId: unknown, opts: Record<string, unknown>): void {
    addItem(this._requireConfig(), itemId, opts as any);
  }

  _requireConfig(): Config {
    this._checkEnabled();
    if (this._config === null) throw new ConfigError("call SCFG.configure(...) first", { code: "configure.not_called" });
    return this._config;
  }

  /** True where payments can run: every platform MZ deploys to. Kept for parity with Ren'Py. */
  supported(): true {
    return true;
  }

  /**
   * The configured logo as a data: URL for the checkout page, or null (none configured, or it
   * cannot be read: reported). Read once per configuration.
   */
  logoDataUrl(): Promise<string | null> {
    if (this._logoUrl === null) {
      const reading = this._readLogo();
      this._logoUrl = reading;
      void reading.then((url) => {
        if (this._logoUrl === reading) this._logoRead = url;
      });
    }
    return this._logoUrl;
  }

  private async _readLogo(): Promise<string | null> {
    const file = this._config?.logo ?? null;
    if (file === null) return null;
    try {
      const type = logoContentType(file);
      const bytes = this._platform.isNwjs() ? this._logoFileBytes(file) : await this._fetchLogo(file);
      return `data:${type};base64,${base64Of(bytes)}`;
    } catch (e) {
      this._logoUnreadable(file, e);
      return null;
    }
  }

  /** MZ's "Encrypt image files" deployment: images ship as `<file>_` (`Utils.hasEncryptedImages()`, known once the database loaded). */
  _imagesEncrypted(): boolean {
    try {
      const utils = mz().Utils;
      return !!(utils && typeof utils.hasEncryptedImages === "function" && utils.hasEncryptedImages());
    } catch {
      return false;
    }
  }

  /** A shipped `<file>_` decrypted the MZ way (`Utils.decryptArrayBuffer`, the routine MZ's own image loader uses). */
  _decryptImage(shipped: Uint8Array): Uint8Array {
    const utils = mz().Utils;
    if (!utils || typeof utils.decryptArrayBuffer !== "function") throw new Error("this RPG Maker has no Utils.decryptArrayBuffer");
    return new Uint8Array(utils.decryptArrayBuffer(new Uint8Array(shipped).buffer)); // (a copy: never a Node Buffer's pool)
  }

  /**
   * Desktop: the logo's bytes from the game folder. In an encrypted deployment its `<file>_`,
   * decrypted, when there is one (MZ encrypts the png images only, so another type stays as it is).
   */
  _logoFileBytes(file: string): Uint8Array {
    const dir = this._platform.gameDir();
    const path = this._platform.requireNode("path");
    const fs = this._platform.requireNode("fs");
    const at = (f: string) => (dir === null ? f : path.join(dir, f));
    if (this._imagesEncrypted() && fs.existsSync(at(file + "_"))) return this._decryptImage(fs.readFileSync(at(file + "_")));
    const read: unknown = fs.readFileSync(at(file));
    if (!isBytes(read)) throw new TypeError("not a file's bytes"); // (a Buffer of NW.js's Node context: any realm)
    return read;
  }

  /** Web: the logo fetched beside index.html; in an encrypted deployment its `<file>_` first, decrypted. */
  async _fetchLogo(file: string): Promise<Uint8Array> {
    const get = async (url: string): Promise<Uint8Array | null> => { // null: not there
      const resp = await (globalThis as any).fetch(url);
      if (resp.ok) return new Uint8Array(await resp.arrayBuffer());
      if (resp.status === 404) return null;
      throw new Error(`HTTP ${resp.status}`);
    };
    if (this._imagesEncrypted()) {
      const shipped = await get(file + "_");
      if (shipped !== null) return this._decryptImage(shipped);
    }
    const plain = await get(file);
    if (plain === null) throw new Error("HTTP 404");
    return plain;
  }

  /**
   * A logo that cannot be read (Ruling 11): in test play reported and shown over the game; in a
   * released game a console warning. Either way the screens show no logo; payments go on.
   */
  _logoUnreadable(file: string, e: unknown): void {
    const why = `cannot read the logo ${file}: ${e instanceof Error ? e.message : String(e)}`;
    if (this._platform.isTestPlay()) this._alert(why);
    else this._log.warn(`scfg: ${why}; the payment sheet and the checkout page show no logo`);
  }

  // --- worker lifecycle ----------------------------------------------------------------------

  _interval(): number {
    return this._checkouts.size > 0 || this._restores.size > 0 ? 3.0 : 15.0;
  }

  /** Read the stored state once MZ can: its database (the web save key needs the game id) and save keys are loaded. */
  _maybeLoad(): void {
    if (this._mirror.loadStarted) return;
    const g = mz();
    const sm = g.StorageManager;
    if (!g.$dataSystem || !sm || (typeof sm.forageKeysUpdated === "function" && !sm.forageKeysUpdated())) return;
    void this._mirror.load();
  }

  /** The stored state has been read (Scene_Boot waits for it). */
  stateLoaded(): boolean {
    return this._mirror.loaded;
  }

  /**
   * The Engine's transport: Node's https on NW.js (Ruling M2-1: the page's fetch would depend on
   * every RPC's CORS headers), retrying through fetch a request Node fails on certificate trust or
   * a proxy (Ruling 12: NW.js's Node has a frozen CA bundle and ignores the OS proxy); fetch in a
   * browser. NW.js without Node's https (reported) falls back to fetch.
   */
  _defaultTransport(): Transport {
    if (this._platform.isNwjs()) {
      try {
        // Ruling 12: a request Node fails on its frozen CA bundle or the OS proxy goes once more through fetch
        return new NodeHttpsTransport({
          https: this._platform.requireNode("https"), http: this._platform.requireNode("http"), fallback: new FetchTransport(),
        });
      } catch (e) {
        this._reportError("cannot use Node's https for the RPC, using fetch:", e);
      }
    }
    return new FetchTransport();
  }

  /** The running Worker, started on first use; null when not configured, not read yet, or it cannot start (reported once). */
  _ensureStarted(): Worker | null {
    if (this._worker !== null || this._startError !== null || this._config === null) return this._worker;
    const m = this._mirror;
    if (!m.loaded || m.doc === null) return null;
    if (m.storeError !== null) {
      // Not corrupt, only unreadable for now: starting empty would save over the player's
      // receipts (reported when it was read). The next launch retries.
      this._startError = new Error(`cannot read the stored receipts: ${m.storeError}`);
      return null;
    }
    const transport = this._hooks.transport ?? this._defaultTransport();
    const rpcClock = this._clock;
    let engine: Engine;
    let worker: Worker;
    try {
      engine = new Engine(this._config, {
        transport, pending: m.doc.pending, receipts: m.doc.receipts, on_error: this._logError,
        // RPC cooldowns must not jump with the wall clock
        rpc_factory: (chain) => backendFor(chain).makeRpc(chain, transport, rpcClock),
        ...this._hooks.engine_opts,
      });
      worker = new Worker(engine, { interval: () => this._interval(), on_error: this._logError, ...this._hooks.worker_opts });
    } catch (e) {
      if (Object.keys(m.doc.pending).length > 0 || Object.keys(m.doc.receipts).length > 0) {
        // The core refuses the stored state (anything it cannot use when the Engine is built from
        // it): kept aside as corrupt; a later frame starts from the empty state.
        void m.setAside(e);
        return null;
      }
      this._startError = e;
      this._reportError("cannot start the payment engine:", e);
      return null;
    }
    worker.start();
    this._engine = engine;
    this._worker = worker;
    this._checkWebStorage();
    return worker;
  }

  /**
   * Web, test play: warn once when the browser cannot keep the receipts (MZ's localforage did not
   * start: blocked site data, a third-party iframe, private browsing). `navigator.storage.persisted()`
   * is not asked: it is false for almost every site (storage is "best effort" until the browser
   * grants more), so it would warn always.
   */
  _checkWebStorage(): void {
    if (this._warnedStorage || this._platform.isNwjs() || !this._platform.isTestPlay()) return;
    this._warnedStorage = true;
    let check: Promise<boolean>;
    try {
      check = Promise.resolve(this._platform.webStoragePersists());
    } catch {
      check = Promise.resolve(false);
    }
    void check.then((ok) => ok, () => false).then((ok) => {
      if (!ok) {
        this._warn("the browser storage is not persistent here (localforage could not start: blocked site data, a game "
          + "embedded in a third-party iframe, or private browsing). Players' receipts are then lost when the tab closes; "
          + "Restore Purchases brings them back.");
      }
    });
  }

  // --- the pump ------------------------------------------------------------------------------

  /** One frame (see the header). */
  pump(): void {
    if (this._closing) return;
    this._maybeLoad();
    if (this._config === null) return;
    const worker = this._ensureStarted();
    if (worker === null) return;
    this._pumpPage();
    // Read before draining: every outcome of the pass that made this snapshot is queued.
    const outcomesSnap = worker.snapshot;
    const controllers = [...this._checkouts.values(), ...this._background.values()];
    for (const outcome of worker.drainOutcomes()) {
      for (const c of controllers) c.onOutcome(outcome);
      if (outcome.result !== null) this._deliver(outcome.result);
    }
    for (const c of controllers) {
      c.poll();
      this._registerCallback(c);
    }
    this._pruneCallbacks(outcomesSnap);
    this._openWhenListening();
    const timeout = Number(this._config.methods["browser"]?.["timeout_min"]) * 60;
    const now = this._engine!.clock();
    for (const [cid, c] of this._background) {
      const s = c.session;
      const expired = s === null || now - Number(s["created_at"]) > timeout;
      if (expired || ["paid", "underpaid", "expired", "choose"].includes(c.state)) {
        c.close();
        this._background.delete(cid);
      }
    }
    for (const r of [...this._restores.values()]) {
      const before = r.state;
      r.poll();
      if (r.state !== before && r.state === "done") this._deliverRestore(r);
    }
    const snap = worker.snapshot;
    const unready = [...snap.unready].sort();
    if (unready.length > 0 && unready.join("\n") !== this._warnedUnready) {
      this._warnedUnready = unready.join("\n");
      for (const key of unready) {
        const [chain, token] = parseUnreadyKey(key);
        this._warn(`your wallet can't receive ${token} on ${chain} yet (no token account). `
          + `Receive any amount of ${token} there once; until then players aren't offered it.`);
      }
    }
    this._syncSnapshot(worker);
    this._mirror.flushIfDue();
  }

  /** Mirror the Worker's snapshot when it is new (the ones made before clearLocalData()'s wipe are skipped). */
  _syncSnapshot(worker: Worker): void {
    const snap = worker.snapshot;
    const wipe = this._wipe;
    if (wipe !== null && (wipe.job?.cancelled() || (wipe.done && snap !== wipe.before))) this._wipe = null;
    if (snap !== this._lastSnapshot) {
      this._lastSnapshot = snap;
      if (this._wipe === null) this._mirror.sync(snap);
    }
  }

  /**
   * Web, once a frame (the first part of Ren'Py's WEB `_periodic`): the browser's answer to a
   * Paste press, and whether a Copy was refused (told to the player once, whatever `notify` is).
   */
  _pumpPage(): void {
    const page = this._page;
    if (page === null || !page.available) return;
    const target = this._pasteTarget;
    if (target !== null) {
      const text = page.takePaste();
      if (text !== null) {
        this._pasteTarget = null;
        this._applyPaste(target.ctl, target.seq, text, true);
      }
    }
    if (this._clock() < this._copyWatchUntil && page.takeCopyFailed()) {
      this._copyWatchUntil = -Infinity;
      this._notify("copy_failed");
    }
  }

  /**
   * Remember the pay() callback of the controller's session, once per session: then _deliver (its
   * final result) or _pruneCallbacks (it left the Engine) drops it for good. Registering again each
   * pump would bring back the callback of a sheet left open on an expired session past its scan
   * horizon, which no snapshot lists to prune any more. Not at all once the session's final result
   * (paid / underpaid) was delivered.
   */
  _registerCallback(c: ScreenCheckout): void {
    const sid = c.sessionId;
    if (sid && typeof c.callback === "function" && sid !== c.callbackSid
      && c.state !== "paid" && c.state !== "underpaid") {
      this._callbacks.set(sid, c.callback);
      c.callbackSid = sid;
    }
  }

  /**
   * Drop the callbacks of sessions that left the Engine since a snapshot listed them (closed at
   * their scan horizon, or discarded): no result can reach them any more. Results still can while
   * a session is pending, also after it expired ("keep playing"). `snap` was read before this pump
   * drained the outcomes, so a session that left because it was paid had its result delivered
   * first. A session id no snapshot listed yet (just started) is kept.
   */
  _pruneCallbacks(snap: Snapshot): void {
    const pending = snap.pending;
    const seen = new Set<string>();
    for (const sid of [...this._callbacks.keys()]) {
      if (Object.prototype.hasOwnProperty.call(pending, sid)) seen.add(sid);
      else if (this._seenPending.has(sid)) this._callbacks.delete(sid);
    }
    this._seenPending = seen;
  }

  /** A session's result: to its pay() callback, else to `onResult`. A paid one is announced when `notify` is on. */
  _deliver(result: PaymentResult): void {
    const sid = result.session_id;
    const cb = (sid !== null ? this._callbacks.get(sid) : undefined) ?? this.onResult;
    if (sid !== null && (result.status === "paid" || result.status === "underpaid")) this._callbacks.delete(sid);
    if (result.status === "paid" && this.notify) {
      const item = result.item !== null ? this._config?.items.get(result.item) : undefined;
      if (item !== undefined) this._notify("payment_confirmed_item", item.title);
      else this._notify("payment_confirmed");
    }
    this._call(cb, result);
  }

  /** A finished restore: to its restore() callback (once), announced when `notify` is on. */
  _deliverRestore(r: ScreenRestore): void {
    const cb = this._restoreCallbacks.get(r.rid) ?? null;
    this._restoreCallbacks.delete(r.rid);
    const result = r.result();
    if (result.restored.length > 0 && this.notify) this._notify("purchases_restored");
    this._call(cb, result);
  }

  /**
   * Once a frame, after the pump: the sheet host's `update()` (the default host reads MZ's input
   * for its screens and rebuilds them). The default host also runs when the game replaced
   * `sheetHost` while one of its screens was open, so that screen still works and lets go of the input.
   */
  _updateScreens(): void {
    for (const host of new Set<SheetHost>([this.sheetHost, this._defaultHost])) {
      if (typeof host.update !== "function") continue;
      try {
        host.update();
      } catch (e) {
        this._reportError("the sheet host's update() failed:", e);
      }
    }
  }

  /** A notification over the game (Ren'Py's renpy.notify), the text `id` in the player's language; shown by the sheet host. */
  _notify(id: string, ...args: unknown[]): void {
    let text: string;
    try {
      text = this.text(id, ...args);
    } catch (e) {
      // an unknown id throws in test play; it must not stop the pump (or the result's delivery)
      this._reportError(`text ${JSON.stringify(id)}:`, e);
      text = id;
    }
    try {
      this.sheetHost.notify(text);
    } catch (e) {
      this._reportError("the sheet host's notify() failed:", e);
    }
  }

  /**
   * A player-facing text: `SCFG.texts[id]`, else the "Texts" plugin parameter's, else the
   * built-in English (texts.ts); `%s` takes the arguments in order. An unknown id shows itself (throws in test play).
   */
  text(id: string, ...args: unknown[]): string {
    return resolveText({ script: this.texts, params: this._paramTexts }, id, args, this._platform.isTestPlay());
  }

  _call<T>(fn: ((v: T) => void) | null | undefined, value: T): void {
    if (typeof fn !== "function") return;
    try {
      fn(value);
    } catch (e) {
      this._reportError("exception in developer callback", e);
    }
  }

  // --- payment and restore flows (Ren'Py's _open_checkout / _close_checkout / _open_restore) --

  /**
   * Show the payment sheet and wait until the player closes it; resolves the outcome:
   * `const result = await SCFG.pay("unlock", { item: "chapter2" })`.
   *
   * `kind` is "unlock" (`item` required; `amount` and `token` default to the item's) or "tip"
   * (`amount`, the suggested amount the player may change, and `token` required). Amounts are
   * strings such as "4.99", never floats.
   *
   * The result's status: "already_owned" (no sheet is shown), "cancelled", "pending" (the player
   * chose to keep playing while the payment confirms), "paid", "underpaid" or "expired".
   *
   * `callback` gets this call's own already_owned or cancelled result and every later result of
   * the payment, such as the paid result of a payment the player kept playing through. Without a
   * callback, a cancel and the later results reach `SCFG.onResult` (Ren'Py's `on_result`), also
   * when this call resolves the same result: a payment that confirms while the sheet is open is
   * given to onResult then, and resolved when the player closes the sheet.
   *
   * A misused call (before configure(), an unknown item, a bad option...) rejects with a
   * ConfigError (with a `code`) or a TypeError, as Ren'Py raises (Ruling M2-3). What is not the
   * script's mistake resolves a "failed" result whose `error` says why (Ruling 10, as Godot):
   * "payments.unavailable" (the stored receipts cannot be read or are still being read, the
   * payment engine could not start, the game is closing) or "sheet.unavailable" (the sheet host
   * showed nothing); it is reported with console.error and given to `callback` too. An unhandled
   * rejection stops an MZ game (SceneManager.onReject), so always `.catch` a pay() call.
   */
  async pay(kind: unknown, opts: PayOptions = {}): Promise<PaymentResult> {
    const o = optionsOf("pay", opts, PAY_OPTIONS);
    const config = this._requireConfig();
    const spec = preparePayment(config, kind, { item: o["item"], amount: o["amount"], token: o["token"] });
    const callback: ((result: PaymentResult) => void) | null = o["callback"] ?? null;
    if (spec.kind === "unlock" && spec.item !== null && this._ownership(config)[0].has(spec.item)) {
      const owned = makePaymentResult({ status: "already_owned", kind: spec.kind, item: spec.item, token: spec.token });
      this._call(callback, owned);
      return owned;
    }
    const worker = this._workerOrWhy();
    if (!(worker instanceof Worker)) return this._failedPayment(spec, callback, worker);
    if (!this._platform.isNwjs() && config.logo !== null) void this.logoDataUrl(); // for the overlay, read by the time it opens
    const ctl = new ScreenCheckout({
      config, spec, submit: (fn) => worker.submit(fn), mobile: this._platform.isMobile(),
      server_factory: (args) => this._serverFactory(args), unready: () => this._unready(),
      game_name: this._gameName(), browser_wallet: this._browserWallet(),
    });
    ctl.cid = this._newId("c");
    ctl.callback = callback;
    this._checkouts.set(ctl.cid, ctl);
    const shown = this._show("openPayment", ctl, () => {
      this._checkouts.delete(ctl.cid);
      ctl.cancel(); // discards a session start already submitted, as a cancel does
    });
    if (!Array.isArray(shown)) return this._failedPayment(spec, callback, shown);
    const how = await this._waitClosed(shown[1]);
    return this._closeCheckout(ctl, shown, how);
  }

  /** Ruling 10 (Godot's _failed_payment): a "failed" result with the code; reported, and given to the callback. */
  _failedPayment(spec: { kind: string; item: string | null; token: string | null }, callback: ((r: PaymentResult) => void) | null,
    why: Unavailable): PaymentResult {
    if (!why.reported) this._reportError(`pay() failed (${why.code}): ${why.message}`);
    const r = makePaymentResult({ status: "failed", kind: spec.kind, item: spec.item, token: spec.token, error: why.code });
    this._call(callback, r);
    return r;
  }

  /** The same for restore(). */
  _failedRestore(callback: ((r: RestoreResult) => void) | null, why: Unavailable): RestoreResult {
    if (!why.reported) this._reportError(`restore() failed (${why.code}): ${why.message}`);
    const r = makeRestoreResult({ status: "failed", error: why.code });
    this._call(callback, r);
    return r;
  }

  /**
   * Ren'Py's _close_checkout: close the controller (cancel / keep playing / finish) and only then
   * the sheet. A cancel goes to the callback or `onResult`. At shutdown (`how` null) the
   * controller is only closed and nothing is delivered.
   */
  _closeCheckout(ctl: ScreenCheckout, shown: [SheetHost, SheetHandle], how: string | null): PaymentResult {
    this._checkouts.delete(ctl.cid);
    if (how === null) {
      // Ren'Py's _shutdown: the page is closed and nothing delivered; the session stays watched
      const result = ctl.finish();
      this._closeScreen(shown);
      return result;
    }
    this._registerCallback(ctl);
    let result: PaymentResult;
    if (how === "cancel") {
      result = ctl.cancel();
    } else if (how === "keep_playing" && ctl.sessionId) {
      result = ctl.keepPlaying();
      this._background.set(ctl.cid, ctl); // keeps the browser page answering until final
    } else {
      result = ctl.finish();
    }
    this._closeScreen(shown);
    if (how === "cancel") this._call(ctl.callback ?? this.onResult, result);
    return result;
  }

  /**
   * Show the restore screen (the player pastes a transaction hash) and wait until it is closed;
   * resolves a RestoreResult: "restored", "nothing_found", "cancelled" (closed before a restore
   * finished) or "failed" (payments cannot run now, or no screen: `error` says which, as pay()).
   * `callback` gets the result once a restore finishes, while the screen is still open (and a
   * failed one). Rejects as pay() does for a misused call: always `.catch` it.
   */
  async restore(opts: RestoreOptions = {}): Promise<RestoreResult> {
    const o = optionsOf("restore", opts, RESTORE_OPTIONS);
    this._requireConfig();
    const callback: ((r: RestoreResult) => void) | null = typeof o["callback"] === "function" ? o["callback"] : null;
    const worker = this._workerOrWhy();
    if (!(worker instanceof Worker)) return this._failedRestore(callback, worker);
    const ctl = new ScreenRestore({ submit: (fn) => worker.submit(fn) });
    ctl.rid = this._newId("r");
    this._restores.set(ctl.rid, ctl);
    if (callback !== null) this._restoreCallbacks.set(ctl.rid, callback);
    const shown = this._show("openRestore", ctl, () => {
      this._restores.delete(ctl.rid);
      this._restoreCallbacks.delete(ctl.rid);
    });
    if (!Array.isArray(shown)) return this._failedRestore(callback, shown);
    await this._waitClosed(shown[1]);
    this._restores.delete(ctl.rid);
    this._restoreCallbacks.delete(ctl.rid);
    this._closeScreen(shown);
    return ctl.result();
  }

  /** The running Worker for a flow, or why payments cannot run now (Ruling 10: a "failed" result). */
  _workerOrWhy(): Worker | Unavailable {
    const off = (why: string, reported: boolean): Unavailable => ({ code: "payments.unavailable", message: unavailable(why).message, reported });
    if (this._closing) return off("the game is closing", false);
    const worker = this._ensureStarted();
    if (worker !== null) return worker;
    if (this._startError !== null) return off(messageOf(this._startError), true); // reported when it happened
    return off("the stored receipts are still being read (they are read while Scene_Boot loads the database)", false);
  }

  /**
   * Show a screen through the sheet host; its handle. When the host shows nothing usable,
   * `discard()` drops the controller first, then a non-handle the host returned is given back to
   * `host.close()` (controller first, screen second, as everywhere), and "sheet.unavailable" is
   * returned (the flow resolves "failed", Ruling 10).
   */
  _show(method: "openPayment" | "openRestore", ctl: ScreenCheckout | ScreenRestore, discard: () => void): [SheetHost, SheetHandle] | Unavailable {
    const host = this.sheetHost;
    let handle: unknown = null;
    let why = "returned nothing";
    try {
      handle = method === "openPayment" ? host.openPayment(ctl as ScreenCheckout) : host.openRestore(ctl as ScreenRestore);
    } catch (e) {
      handle = null;
      why = `threw ${e instanceof Error ? `${e.name}: ${e.message}` : String(e)}`;
    }
    if (isSheetHandle(handle)) return [host, handle];
    discard();
    if (handle !== null && handle !== undefined) {
      this._closeScreen([host, handle as SheetHandle]);
      why = "returned no handle with a `closed` Promise";
    }
    return { code: "sheet.unavailable", message: `no screen to show: SCFG.sheetHost.${method}() ${why}`, reported: false };
  }

  _closeScreen([host, handle]: [SheetHost, SheetHandle]): void {
    try {
      host.close(handle);
    } catch (e) {
      this._reportError("the sheet host's close() failed:", e);
    }
  }

  /**
   * Resolves how the screen was closed (`closed`'s value; a rejection or no value is "cancel"),
   * or null at once when the game is closing (`_shutdown`).
   */
  _waitClosed(handle: SheetHandle): Promise<string | null> {
    return new Promise((resolve) => {
      let done = false;
      const end = (how: string | null) => {
        if (done) return;
        done = true;
        this._waits.delete(release);
        resolve(how);
      };
      const release = () => end(null);
      this._waits.add(release);
      Promise.resolve(handle.closed).then(
        (how) => end(how === undefined || how === null ? "cancel" : String(how)),
        () => end("cancel"),
      );
    });
  }

  _newId(prefix: string): string {
    this._nextId++;
    return `${prefix}${this._nextId}`;
  }

  /** The checkout controllers' `unready`: the routes the Worker's snapshot marks unusable. */
  _unready(): ReadonlySet<string> {
    return this._worker !== null ? this._worker.snapshot.unready : new Set<string>();
  }

  /** The game's title for wallet apps (Solana Pay's label): the database's, else the page's. */
  _gameName(): string {
    const title = mz().$dataSystem?.gameTitle;
    if (typeof title === "string" && title !== "") return title;
    const page = (globalThis as any).document?.title;
    return typeof page === "string" ? page : "";
  }

  /**
   * Web only: can the page's wallet pay on a chain (the browser checkout is offered only then).
   * The page's scripts are installed here, on the first payment sheet (Ren'Py's `_web_page()`).
   */
  _browserWallet(): ((chain: Chain) => boolean) | null {
    if (this._platform.isNwjs()) return null;
    const page = this._webPage();
    return (chain) => page !== null && page.hasWallet(chain);
  }

  /** Web: the page's bridge (web_page.ts), installed on first use; null on desktop or without a page. */
  _webPage(): WebPage | null {
    if (this._platform.isNwjs()) return null;
    if (this._page === null) {
      const win = this._platform.window();
      if (win === null) return null;
      const page = new WebPage({ window: win, warn: (m) => this._warn(m) });
      page.install();
      this._page = page;
    }
    return this._page;
  }

  /** The checkout controllers' `server_factory`: the page a browser wallet pays on. */
  _serverFactory(args: ServerArgs): CheckoutServer {
    if (this._refuseServer !== null) throw this._refuseServer; // (_openWhenListening's replay of a failed page)
    const factory = this._hooks.server_factory;
    if (factory !== undefined) return factory(args);
    return this._platformServer(args);
  }

  /**
   * Desktop: the localhost checkout page on Node's net (local_server.ts; Ren'Py's _server_factory).
   * `build_tx` stays undefined on EVM, so the page has no /build route there, as in Python. The
   * logo is read here, once (an unreadable one is reported and the page has no logo).
   * Web: the in-page overlay over the page's bridge (Ren'Py's WEB `_server_factory`), under a new
   * key, with the logo as a data: URL, and the glue's clock for the build timeout. pay() started
   * reading the logo; one still being read is waited for, at most LOGO_WAIT_S (LogoWaitUi), and
   * the overlay opens without it after that.
   */
  _platformServer(args: ServerArgs): CheckoutServer {
    if (!this._platform.isNwjs()) {
      const page = this._webPage();
      if (page === null || !page.available) throw new Error("the page's checkout bridge is not available here");
      const reading = this._logoRead === undefined && this._logoUrl !== null;
      const ui = reading ? new LogoWaitUi(page, () => this._logoRead, this._clock() + LOGO_WAIT_S, this._clock) : page;
      return new OverlayCheckout({
        ui, key: this._newId("o"), session_info: args.session_info,
        on_tx: (tx, sender) => args.on_tx(tx, sender), get_status: args.get_status,
        logo_url: this._logoRead ?? null, build_tx: args.build_tx ?? null, clock: this._clock,
      });
    }
    const net = this._platform.requireNode("net");
    const crypto = this._platform.requireNode("crypto");
    return new LocalServer({
      net, crypto: randomSource(crypto),
      load_asset: (name) => pageAsset(name as Parameters<typeof pageAsset>[0]),
      session_info: args.session_info, on_tx: args.on_tx, get_status: args.get_status,
      load_logo: this._logoForPage(), build_tx: args.build_tx ?? null,
      on_error: (e) => this._reportError("the browser checkout page:", e),
    });
  }

  /** load_logo for the desktop page: the configured logo's bytes and type, read now; null without one. */
  _logoForPage(): (() => [Uint8Array, string]) | null {
    const file = this._config?.logo ?? null;
    if (file === null) return null;
    try {
      const logo: [Uint8Array, string] = [this._logoFileBytes(file), logoContentType(file)];
      return () => logo;
    } catch (e) {
      this._logoUnreadable(file, e);
      return null;
    }
  }

  /**
   * Desktop (Ruling 1): open the browser for each "Open checkout in browser" whose page was still
   * starting, once it listens; a page that cannot listen is dropped and the press replayed, so the
   * controller fails the browser method as when its page's start() throws. The press is forgotten
   * when the sheet closed or its page went away.
   */
  _openWhenListening(): void {
    for (const ctl of [...this._pendingOpens]) {
      const page = ctl._server as (CheckoutServer & { failed?: unknown }) | null;
      if (this._openController(ctl) !== ctl || page === null) {
        this._pendingOpens.delete(ctl);
        continue;
      }
      const failed = page.failed ?? null;
      if (failed !== null) {
        this._pendingOpens.delete(ctl);
        this._reportError("cannot start the browser checkout page:", failed);
        ctl._server = null;
        try {
          page.stop();
        } catch (e) {
          this._reportError("error while stopping the browser checkout page:", e);
        }
        this._refuseServer = failed;
        try {
          ctl.openBrowser();
        } finally {
          this._refuseServer = null;
        }
        continue;
      }
      if (!page.url) continue;
      this._pendingOpens.delete(ctl);
      this.openBrowser(ctl);
    }
  }

  // --- the sheet's actions -------------------------------------------------------------------

  /** `ctl` when it is the controller of an open payment sheet or restore screen, else null. */
  _openController(ctl: unknown): ScreenCheckout | ScreenRestore | null {
    if (ctl instanceof ScreenCheckout && this._checkouts.get(ctl.cid) === ctl) return ctl;
    if (ctl instanceof ScreenRestore && this._restores.get(ctl.rid) === ctl) return ctl;
    return null;
  }

  /**
   * The sheet's "Open checkout in browser" (Ren'Py's _open_browser): start the controller's page
   * and open the player's browser on it. On the web the overlay is already showing.
   */
  openBrowser(ctl: CheckoutController): void {
    const open = this._openController(ctl);
    if (!(open instanceof ScreenCheckout)) return;
    const url = open.openBrowser();
    if (this._platform.isWeb()) return;
    if (url) this._openUrl(url);
    else if (open._server !== null) this._pendingOpens.add(open); // still starting: opened once it listens
  }

  /** The sheet's "Open wallet app" (phones): the payment request link (EIP-681 or Solana Pay), for the wallet app that handles it. */
  openWalletApp(ctl: CheckoutController): void {
    const open = this._openController(ctl);
    if (!(open instanceof ScreenCheckout)) return;
    let uri: string | null;
    try {
      uri = open.paymentUri();
    } catch (e) {
      this._reportError("cannot build the payment link:", e);
      return;
    }
    if (uri) this._openUrl(uri);
  }

  /** The sheet's "View on explorer": open `url` (nothing for null or ""). */
  openExplorer(url: unknown): void {
    if (typeof url === "string" && url !== "") this._openUrl(url);
  }

  /** Open `url` outside the game: NW.js's shell on desktop; on the web the page's window.open (noopener). */
  _openUrl(url: string): void {
    const page = this._webPage();
    if (page !== null && page.available) {
      page.openUrl(url);
      return;
    }
    if (!openExternal(this._platform, url)) this._reportError(`cannot open ${url}: there is no browser to open it in`);
  }

  /** The sheet's Copy buttons: `text` to the clipboard. A browser that refuses it is told to the player, whatever `notify` is. */
  copy(text: unknown): void {
    // not gated on `notify`: it answers the player's own Copy press (as Ren'Py and Godot)
    const page = this._webPage();
    if (page !== null && page.available) {
      this._copyWatchUntil = this._clock() + COPY_WATCH_S;
      page.copy(String(text)); // the browser may refuse: _pumpPage() tells the player
      return;
    }
    void copyText(this._platform, String(text)).catch(() => this._notify("copy_failed"));
  }

  /**
   * The Paste button of a payment sheet's or restore screen's hash field: the clipboard's first
   * word, hash characters only (Ren'Py's _clean_paste), replaces the field. On desktop an empty
   * clipboard pastes nothing; a browser that refuses (or answers nothing) sets the controller's
   * error "paste_blocked". A restore screen that moved past its input by the time the clipboard
   * answers drops the paste; so does a screen that was closed, or an older Paste press.
   */
  pasteInto(ctl: CheckoutController | RestoreController): void {
    const open = this._openController(ctl);
    if (open === null) return;
    const seq = ++this._pasteSeq;
    const desktop = this._platform.isNwjs() ? readDesktopClipboard(this._platform) : null;
    if (desktop !== null) {
      this._applyPaste(open, seq, desktop, false);
      return;
    }
    const page = this._webPage();
    if (page !== null && page.available) {
      this._pasteTarget = { ctl: open, seq }; // _pumpPage() hands it the page's answer
      page.requestPaste();
      return;
    }
    readBrowserClipboard(this._platform)
      .then((text) => this._applyPaste(open, seq, text, true), () => this._applyPaste(open, seq, "", true))
      .catch((e: unknown) => this._reportError("cannot paste:", e)); // never an unhandled rejection (MZ's onReject)
  }

  /** Ren'Py's _take_paste: `text` into `open`'s hash field, unless a newer press, a closed screen or a pressed Check made it moot. */
  _applyPaste(open: ScreenCheckout | ScreenRestore, seq: number, text: string, fromBrowser: boolean): void {
    if (seq !== this._pasteSeq || this._openController(open) !== open) return;
    if (open instanceof ScreenRestore && open.state !== "input") return; // Check was pressed: drop it
    const cleaned = cleanPaste(text);
    if (cleaned !== "") {
      open.tx_input = cleaned;
      open.error = null;
    } else if (fromBrowser) {
      open.error = "paste_blocked";
    }
  }

  // --- closing (Ren'Py's _shutdown) ------------------------------------------------------------

  /**
   * The game is closing: every flow waiting on a screen resumes now and closes without delivering
   * (pay() resolves "cancelled" or the final result its controller has, restore() its result so
   * far; no callback, no onResult, no notification, and the session is not marked cancelled, so
   * the Engine keeps watching it on the next launch); every controller's page is stopped; the
   * Worker stops; what is not stored yet is written. Once only; the pump stops for good.
   */
  _shutdown(): void {
    if (this._closing) return;
    this._closing = true;
    for (const release of [...this._waits]) release();
    this._waits.clear();
    this._pendingOpens.clear();
    this._pasteTarget = null;
    for (const c of [...this._checkouts.values(), ...this._background.values()]) {
      try {
        c.close();
      } catch (e) {
        this._reportError("error while closing a payment:", e);
      }
    }
    this._checkouts.clear();
    this._background.clear();
    this._restores.clear();
    this._restoreCallbacks.clear();
    const worker = this._worker;
    if (worker !== null) {
      worker.stop();
      this._syncSnapshot(worker);
    }
    if (this._mirror.dirty) this._mirror.save();
  }

  /** Write what is not stored yet (scan progress inside the save interval), without shutting down. */
  _flush(): void {
    if (!this._closing && this._worker !== null) this._syncSnapshot(this._worker);
    if (this._mirror.dirty) this._mirror.save();
  }

  /** `_shutdown()`, then wait for the last save (at most QUIT_FLUSH_MS). Never rejects. */
  async closeForQuit(): Promise<void> {
    try {
      this._shutdown();
    } catch (e) {
      this._reportError("error while closing:", e);
    }
    let timer: ReturnType<typeof setTimeout> | undefined;
    const gaveUp = new Promise<void>((resolve) => {
      timer = setTimeout(resolve, QUIT_FLUSH_MS);
    });
    try {
      await Promise.race([this._mirror.idle(), gaveUp]);
    } catch (e) {
      this._reportError("cannot save the payment state:", e);
    } finally {
      clearTimeout(timer);
    }
  }

  // --- receipts changed ----------------------------------------------------------------------

  /**
   * Call `fn` whenever the stored receipts change (a payment confirmed, a restore,
   * clearLocalData()), and once when they are first read. Returns the unsubscribe function.
   */
  onReceiptsChanged(fn: () => void): () => void {
    if (typeof fn !== "function") throw new TypeError("onReceiptsChanged() takes a function");
    const entry = () => fn();
    this._listeners.add(entry);
    return () => {
      this._listeners.delete(entry);
    };
  }

  _emitReceiptsChanged(): void {
    this._receiptsRev++;
    for (const fn of [...this._listeners]) {
      try {
        fn();
      } catch (e) {
        this._reportError("exception in an onReceiptsChanged listener", e);
      }
    }
  }

  // --- queries -------------------------------------------------------------------------------

  /** What readers see: the Worker's snapshot, or the stored state before it starts and during clearLocalData(). */
  _source(): StateDoc {
    if (this._worker !== null && this._wipe === null) return this._worker.snapshot;
    return this._mirror.doc ?? { pending: {}, receipts: {} };
  }

  _ownership(config: Config): [Set<string>, Dec] {
    if (this._worker !== null && this._wipe === null) return [this._worker.snapshot.owned, this._worker.snapshot.tip_total];
    if (this._mirror.doc === null && !this._warnedEarly) {
      this._warnedEarly = true;
      this._warn("a purchase was asked about before the stored receipts were read (they are read while Scene_Boot "
        + "loads the database); until then nothing counts as owned.");
    }
    try {
      return computeOwnership(this._source().receipts, config.items);
    } catch (e) {
      this._reportError("cannot read the stored receipts:", e);
      return [new Set(), ZERO];
    }
  }

  /** True if a verified receipt unlocks `itemId`. Instant and offline. An unknown item is a ConfigError. */
  owns(itemId: unknown): boolean {
    const cfg = this._requireConfig();
    if (typeof itemId !== "string" || !cfg.items.has(itemId)) {
      throw new ConfigError(`unknown item ${JSON.stringify(itemId)}; declare it with SCFG.addItem()`, { code: "item.unknown" });
    }
    return this._ownership(cfg)[0].has(itemId);
  }

  /** True once the player has tipped any amount (Ren'Py: `tip_total() > 0`). */
  hasTipped(): boolean {
    return this._ownership(this._requireConfig())[1].cmp(ZERO) > 0;
  }

  /** The sum of the player's tips as a decimal string ("0", "2.50"), Python's str(Decimal). */
  tipTotal(): string {
    return String(this._ownership(this._requireConfig())[1]);
  }

  /** The stored receipts (chain, tx_hash, log_index, block, kind, item, amount, token, payer, recipient, source), as copies. */
  receipts(): Record<string, any>[] {
    return Object.values(plainCopy(this._source().receipts));
  }

  /** The payments still being watched for, as copies. */
  pending(): Record<string, any>[] {
    return Object.values(plainCopy(this._source().pending));
  }

  /**
   * Testing helper: forget every receipt and pending payment on THIS device, to see your paywall
   * again after a test purchase. Real purchases are not lost: the payments are still on the
   * blockchain and Restore brings them back. Called before the stored receipts are read (while
   * the plugins load), it takes effect once they are.
   */
  clearLocalData(): void {
    const hadReceipts = Object.keys(this._source().receipts).length > 0;
    const worker = this._worker;
    if (worker !== null) {
      const wipe: NonNullable<Scfg["_wipe"]> = { job: null, done: false, before: null };
      wipe.job = worker.submit((engine) => {
        for (const k of Object.keys(engine.receipts)) delete engine.receipts[k];
        for (const k of Object.keys(engine.pending)) delete engine.pending[k];
        wipe.before = worker.snapshot; // the last snapshot made before the wipe
        wipe.done = true;
        return null;
      });
      this._wipe = wipe;
    }
    this._mirror.replace({ pending: {}, receipts: {} });
    if (hadReceipts) this._emitReceiptsChanged();
  }

  /**
   * Support helper: the receipts as readable JSON (a list, sorted keys, indented, as Ren'Py
   * writes it). On desktop it is written to `save/scfg-receipts.json` and the file's path is
   * returned; in a browser the JSON text itself is returned.
   */
  exportReceipts(): string {
    const text = pyJsonDump(this.receipts());
    if (!this._platform.isNwjs()) return text;
    const fs = this._platform.requireNode("fs");
    const dir = String(mz().StorageManager.fileDirectoryPath());
    fs.mkdirSync(dir, { recursive: true });
    const path = dir + EXPORT_NAME;
    fs.writeFileSync(path, text);
    return path;
  }
}
