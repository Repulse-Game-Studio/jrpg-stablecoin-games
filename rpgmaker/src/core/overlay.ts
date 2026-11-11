/**
 * The browser checkout as an overlay inside the game's own web page (an RPG Maker MZ web
 * deployment). Mirrors scfg_core/overlay.py.
 *
 * On the web the game already runs in a browser, and wallet extensions inject into its page, so
 * the checkout page (app.js, evm.js, solana.js) is shown over the game instead of being served on
 * localhost. OverlayCheckout has CheckoutServer's interface (start, stop, url, running, reopen,
 * pump), so CheckoutController drives both alike. The page's messages are read by pump(), which
 * the controller calls every frame; they are validated like the local server's requests
 * (page_messages.ts), and a tx hash stays a hint the watcher verifies on-chain.
 *
 * `ui` is the page (the glue's WebPage over bridge.js); everything it hands back comes from the
 * page and is checked here: a take() that is not a list is no messages, a request id must be an
 * integer JSON keeps exact (not a boolean, a fraction or a string), a reused id cancels the older
 * build. The callbacks are the controller's: once stop() ran none of them is called again, also
 * not for the rest of a batch of messages whose handling stopped the overlay, and a build that
 * finishes after stop() is never answered (a Job handed back by a build_tx that stopped the overlay
 * is cancelled at once). A build_tx that throws or gives no Job, or a Job that
 * ends without an object, is answered 503. Every method is synchronous and never awaits a Job.
 */
import { BUILD_TIMEOUT_S, parseBuildMessage, parseTxMessage } from "./page_messages.ts";
import type { CheckoutServer } from "./checkout.ts";
import { ValueError } from "./errors.ts";
import { hasOwn } from "./pystr.ts";
import { Job } from "./worker.ts";

/** The page the overlay is shown in (the twin of web_page.py's overlay methods). */
export interface OverlayUi {
  open(key: string, sessionInfo: Record<string, unknown>, logoUrl: string | null): void;
  show(key: string): void;
  close(key: string): void;
  /** The messages the overlay's page posted since the last call. */
  take(key: string): unknown[];
  setStatus(key: string, status: Record<string, unknown>): void;
  reply(key: string, id: number, status: number, body: Record<string, unknown>): void;
}

/** overlay.py's keyword arguments. */
export interface OverlayOptions {
  ui: OverlayUi;
  /** Which overlay of the page this is (the glue's "o<n>"). */
  key: string;
  session_info: Record<string, unknown>;
  /** A hint from the page, already through parseTxMessage. */
  on_tx: (tx_hash: string, sender: string | null) => void;
  get_status: () => Record<string, unknown>;
  /** The logo as a data: URL, or null. */
  logo_url?: string | null;
  /** Solana only: the unsigned transfer for the player's account, as a Job the overlay polls. */
  build_tx?: ((account: string) => Job<any>) | null;
  /** Seconds, monotonic (the build timeout). Default: performance.now(), else Date.now(). */
  clock?: () => number;
}

const isObject = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

/** Python's `==` for JSON values (dicts by keys, lists in order). */
function jsonEqual(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  if (Array.isArray(a)) return Array.isArray(b) && a.length === b.length && a.every((x, i) => jsonEqual(x, b[i]));
  if (!isObject(a) || !isObject(b)) return false;
  const ka = Object.keys(a);
  if (ka.length !== Object.keys(b).length) return false;
  return ka.every((k) => hasOwn(b, k) && jsonEqual(a[k], b[k]));
}

/** A copy of a JSON value, so a later change to the controller's status object is still seen. */
function jsonCopy<T>(v: T): T {
  if (Array.isArray(v)) return v.map(jsonCopy) as unknown as T;
  if (!isObject(v)) return v;
  const out: Record<string, unknown> = {};
  for (const k of Object.keys(v)) out[k] = jsonCopy(v[k]);
  return out as T;
}

function defaultClock(): number {
  const perf = (globalThis as any).performance;
  return perf && typeof perf.now === "function" ? perf.now() / 1000 : Date.now() / 1000;
}

const UNAVAILABLE = { error: "unavailable" };

export class OverlayCheckout implements CheckoutServer {
  /** What `url` is: there is no page to open. */
  static readonly URL = "overlay:checkout";

  readonly ui: OverlayUi;
  readonly key: string;
  readonly session_info: Record<string, unknown>;
  readonly on_tx: OverlayOptions["on_tx"];
  readonly get_status: OverlayOptions["get_status"];
  readonly logo_url: string | null;
  readonly build_tx: ((account: string) => Job<any>) | null;
  readonly clock: () => number;
  url: string = OverlayCheckout.URL;
  private readonly _builds = new Map<number, [Job<any>, number]>(); // page request id -> [job, deadline]
  private _status: Record<string, unknown> | null = null;
  private _open = false;

  constructor(opts: OverlayOptions) {
    this.ui = opts.ui;
    this.key = opts.key;
    this.session_info = opts.session_info;
    this.on_tx = opts.on_tx;
    this.get_status = opts.get_status;
    this.logo_url = opts.logo_url ?? null;
    this.build_tx = opts.build_tx ?? null;
    this.clock = opts.clock ?? defaultClock;
  }

  get running(): boolean {
    return this._open;
  }

  start(): void {
    this.ui.open(this.key, this.session_info, this.logo_url);
    this._open = true;
    this._pushStatus();
  }

  reopen(): void {
    if (this._open) this.ui.show(this.key);
  }

  stop(): void {
    if (!this._open) return;
    this._open = false;
    for (const [job] of this._builds.values()) job.cancel();
    this._builds.clear();
    this.ui.close(this.key);
  }

  pump(): void {
    if (!this._open) return;
    const messages = this.ui.take(this.key);
    for (const message of Array.isArray(messages) ? messages : []) {
      this._handle(message);
      if (!this._open) return; // a callback stopped the overlay: the rest is moot
    }
    this._finishBuilds();
    if (this._open) this._pushStatus();
  }

  private _handle(message: unknown): void {
    if (!isObject(message)) return;
    const kind = message["type"];
    if (kind === "tx") {
      let parsed: [string, string | null];
      try {
        parsed = parseTxMessage(message["body"]);
      } catch (e) {
        if (e instanceof ValueError || e instanceof TypeError) return; // the page only posts what a wallet returned: nothing to answer
        throw e;
      }
      this.on_tx(parsed[0], parsed[1]);
      return;
    }
    const rid = message["id"];
    if (kind !== "build" || typeof rid !== "number" || !Number.isSafeInteger(rid)) return;
    if (this.build_tx === null) {
      this.ui.reply(this.key, rid, 404, { error: "not_found" });
      return;
    }
    let account: string;
    try {
      account = parseBuildMessage(message["body"]);
    } catch (e) {
      if (!(e instanceof ValueError || e instanceof TypeError)) throw e;
      this.ui.reply(this.key, rid, 400, { error: "bad account" });
      return;
    }
    let job: unknown;
    try {
      job = this.build_tx(account);
    } catch {
      job = null; // (the controller is gone, or its Worker stopped)
    }
    if (!this._open) {
      // build_tx stopped the overlay (its controller closed): nobody answers this build, so it must not run
      if (job instanceof Job) job.cancel();
      return;
    }
    if (!(job instanceof Job)) {
      this.ui.reply(this.key, rid, 503, UNAVAILABLE);
      return;
    }
    this._builds.get(rid)?.[0].cancel(); // the page reused an id: the older build is moot
    this._builds.set(rid, [job, this.clock() + BUILD_TIMEOUT_S]);
  }

  private _finishBuilds(): void {
    for (const [rid, [job, deadline]] of [...this._builds]) {
      if (job.done()) {
        this._builds.delete(rid);
        const value: unknown = job.exception() === null ? job.result() : null;
        if (isObject(value)) this.ui.reply(this.key, rid, 200, value);
        else this.ui.reply(this.key, rid, 503, UNAVAILABLE);
      } else if (this.clock() >= deadline) {
        this._builds.delete(rid);
        job.cancel(); // don't build it later for a page that stopped waiting
        this.ui.reply(this.key, rid, 503, UNAVAILABLE);
      }
    }
  }

  private _pushStatus(): void {
    const status: unknown = this.get_status();
    if (!isObject(status)) return;
    if (this._status === null || !jsonEqual(status, this._status)) {
      this._status = jsonCopy(status);
      this.ui.setStatus(this.key, status);
    }
  }
}
