/**
 * The localhost checkout server for desktop browser wallets (NW.js): the twin of
 * scfg_core/local_server.py (CheckoutServer) and of Godot's addons/scfg/local_server.gd, route by
 * route.
 *
 * Browser extensions (MetaMask, Rabby, Phantom...) only talk to web pages, never to a game. So
 * the game serves the bundled checkout page on 127.0.0.1 and opens the browser on it. The page
 * connects the wallet, sends the transfer and posts the tx hash back here.
 *
 * `new CheckoutServer(opts)` takes local_server.CheckoutServer's keyword arguments as keys:
 *   - load_asset(name) -> bytes (Uint8Array) or text (sent as UTF-8): one of ASSETS (a throw, or
 *     anything else, is a 500);
 *   - session_info: the value served as session.json;
 *   - on_tx(tx_hash, sender): the page's hint, already normalised (parseTxMessage);
 *   - get_status() -> object: served as status;
 *   - load_logo() -> [bytes, content type] (optional; no route without it);
 *   - build_tx(account) -> Job (optional, Solana; no route without it);
 *   - host ("127.0.0.1");
 *   - net: Node's `net` module (NW.js: `require("net")`, looked up by the glue at run time; this
 *     file never imports Node);
 *   - crypto: an object with WebCrypto's getRandomValues (default: the page's `crypto`);
 *   - on_error(e): an error of the listening server that does not stop it (a failed accept),
 *     reported while the server keeps serving (optional).
 * Every callback is checked before each call, and released by stop(): one that is gone (null, not
 * a function) or throws answers 503 (500 for an asset, 404 for the logo), never an exception in
 * a socket handler.
 *
 * Node is event-driven: the server answers from Node's event loop, `pump()` does nothing and
 * on_tx runs in the socket's turn (JavaScript has one thread, so it never runs inside a worker
 * command). `start()` returns before Node's listen() completes: `url` is "" until then (Ruling
 * 1; `ready()` resolves once listening, rejects when it cannot listen, and `failed` keeps that
 * error). `stop()` closes everything, cancels a build a page is still waiting for and releases
 * the callbacks; a stopped server does not start again.
 *
 * Security (as local_server.py):
 *   - bound to 127.0.0.1 on a random port;
 *   - every URL holds a 128-bit secret, /c/<secret>/..., otherwise 404;
 *   - the Host header must be exactly 127.0.0.1:<port> (defeats DNS rebinding), otherwise 403;
 *   - no CORS headers; a request body is at most MAX_BODY bytes (413), with a valid
 *     Content-Length (400);
 *   - every answer has Cache-Control: no-store, Referrer-Policy: no-referrer and
 *     X-Content-Type-Options: nosniff (the 501 too, as Godot; Python's lacks them);
 *   - /build returns the *unsigned* transaction only for a valid account, and gives up after
 *     BUILD_TIMEOUT_S (503, the job cancelled);
 *   - a posted tx hash is only a hint: the watcher verifies it on-chain;
 *   - the logo is served only as a raster image (png, jpeg, webp), never SVG.
 * As the Godot server: a connection has DEADLINE_S to deliver its request (and again to take its
 * answer), at most MAX_CONNECTIONS are served at once (the next ones wait, unread, until one
 * closes), and a request head is at most MAX_HEAD bytes in all (431).
 *
 * HTTP is parsed here, on a raw `net` server, not by Node's `http`: its parser trims header
 * values (Python keeps a trailing space, so `Host: 127.0.0.1:<port> ` is a 403), rejects a
 * lowercase method with 400 (Python: 501), refuses Python-valid Content-Lengths (` 5 `, `1_0`),
 * has no per-line cap (Python: 65536 bytes), truncates at maxHeadersCount instead of refusing
 * the 100th line, and waits on an empty request line. As Python's BaseHTTPRequestHandler reads a
 * request: GET and POST only, any other method (HEAD included, which gets no body) is 501; a
 * request line that is empty is closed without an answer, a bad version is 400 and HTTP/2+ is
 * 505; lines may end in LF or CRLF; the header block is read as email.parser (compat32) reads it;
 * a path starting with `//` is reduced to one `/`; the query string is ignored; Content-Length is
 * read as Python's int(). Every answer is HTTP/1.1 with `Connection: close`. Deviations (the
 * Godot server's): a two-word (HTTP/0.9) request line is a 400, an answer always has its status
 * line, the 501's body is plain text, and a client that closes its side before its head is
 * complete gets no answer (Python reads what it has as the whole head).
 *
 * Node APIs used, all in Node 8 or older:
 * net.createServer({allowHalfOpen, pauseOnConnect}), server.listen(port, host) / address() /
 * close(), socket events data/end/close/error, write/end/destroy/pause/resume, and Uint8Array
 * writes. The page globals TextEncoder/TextDecoder (Node 11+ as globals), setTimeout and
 * crypto.getRandomValues come from Chromium on NW.js.
 */
import { BUILD_TIMEOUT_S, type Job, parseBuildMessage, parseTxMessage } from "../core/index.ts";
import { LOGO_TYPES } from "../core/config.ts";
import { pyInt } from "../core/pystr.ts";

export const MAX_BODY = 4096;
export const ASSETS = ["index.html", "app.js", "evm.js", "solana.js"] as const;
/** Seconds a connection has to send its whole request, and again to take its answer. */
export const DEADLINE_S = 10;
/**
 * One line of the head, in bytes with its line end: http.server reads the request line with
 * readline(65537) and http.client each header line with readline(_MAXLINE + 1), so a longer
 * request line is a 414 and a longer header line a 431 ("Line too long"). A browser may send
 * 127.0.0.1 a Cookie header this large.
 */
export const MAX_LINE = 65536;
/**
 * The request line plus headers in all, in bytes; more is a 431 (as the Godot server: the first
 * MAX_HEAD + 1 bytes must hold the whole head; Python has no total cap, only MAX_LINE per line
 * and MAX_HEADERS lines).
 */
export const MAX_HEAD = 262144;
/**
 * http.client's _MAXHEADERS. It counts the blank line that ends the head as one of them, so a
 * request has at most MAX_HEADERS - 1 header lines; one more is a 431.
 */
export const MAX_HEADERS = 100;
export const MAX_CONNECTIONS = 32;
/** Connections waiting for one of the MAX_CONNECTIONS slots; more are closed at once (a listen backlog). */
export const MAX_WAITING = 128;
/**
 * After an answer given before the whole request was read (a body never read, a head cut short):
 * how long the rest is drained before closing, and how much of it. Closing on unread input resets
 * the connection, and the client could lose the answer. An answer to a complete request closes as
 * soon as it is written.
 */
export const LINGER_S = 1;
export const LINGER_MAX_BYTES = 65536;
/** The content types the logo is served as: the ones the core accepts for a logo file. */
export const RASTER_TYPES: readonly string[] = [...new Set(Object.values(LOGO_TYPES))];

const CTYPES: Record<string, string> = {
  "index.html": "text/html; charset=utf-8",
  "app.js": "application/javascript; charset=utf-8",
  "evm.js": "application/javascript; charset=utf-8",
  "solana.js": "application/javascript; charset=utf-8",
};
const TEXT = "text/plain; charset=utf-8";
const REASONS: Record<number, string> = {
  200: "OK", 202: "Accepted", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
  413: "Request Entity Too Large", 414: "Request-URI Too Long", 431: "Request Header Fields Too Large",
  500: "Internal Server Error", 501: "Not Implemented", 503: "Service Unavailable",
  505: "HTTP Version Not Supported",
};

// --- the Node API this file uses (Node's own types are not part of the glue's build) ----------

export interface NetSocket {
  on(event: string, listener: (...args: any[]) => void): unknown;
  write(data: Uint8Array): unknown;
  end(data?: Uint8Array): unknown;
  destroy(): unknown;
  pause(): unknown;
  resume(): unknown;
}

export interface NetServer {
  on(event: string, listener: (...args: any[]) => void): unknown;
  listen(port: number, host: string): unknown;
  address(): unknown;
  close(): unknown;
}

/** Node's `net` module, as far as the server uses it. */
export interface NodeNet {
  createServer(options: { allowHalfOpen?: boolean; pauseOnConnect?: boolean }, listener: (socket: any) => void): NetServer;
}

export interface RandomSource {
  getRandomValues(bytes: Uint8Array): Uint8Array;
}

type Bytes = Uint8Array;
type Timer = unknown;

export interface ServerOptions {
  load_asset?: ((name: string) => Bytes | string) | null;
  session_info?: unknown;
  on_tx?: ((tx_hash: string, sender: string | null) => void) | null;
  get_status?: (() => unknown) | null;
  load_logo?: (() => [Bytes, string]) | null;
  build_tx?: ((account: string) => Job<unknown>) | null;
  host?: string;
  net: NodeNet;
  crypto?: RandomSource;
  /** Errors of a listening server that do not stop it (a failed accept). Default: none. */
  on_error?: ((e: unknown) => void) | null;
}

// --- bytes and Python's string rules --------------------------------------------------------

const ENCODER = new TextEncoder();

const ascii = (s: string): Bytes => {
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0xff;
  return out;
};

/** Bytes as Python's str(b, "iso-8859-1"). */
function latin1(b: Bytes, start: number, end: number): string {
  let s = "";
  for (let i = start; i < end; i += 0x2000) {
    s += String.fromCharCode(...b.subarray(i, Math.min(end, i + 0x2000)));
  }
  return s;
}

const B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/** base64url without padding: Python's secrets.token_urlsafe(n) of these bytes. */
export function base64url(b: Bytes): string {
  let out = "";
  for (let i = 0; i < b.length; i += 3) {
    const n = (b[i]! << 16) | ((b[i + 1] ?? 0) << 8) | (b[i + 2] ?? 0);
    const chars = Math.min(4, Math.ceil(((b.length - i) * 8) / 6));
    for (let j = 0; j < chars; j++) out += B64URL[(n >> (18 - 6 * j)) & 63];
  }
  return out;
}

/** Python's whitespace for str.split() on a Latin-1 string. */
const isPySpace = (c: number): boolean => (c >= 9 && c <= 13) || (c >= 0x1c && c <= 0x20) || c === 0x85 || c === 0xa0;

/** Python's str.split() with no argument. */
function pySplit(s: string): string[] {
  const words: string[] = [];
  let start = -1;
  for (let i = 0; i < s.length; i++) {
    if (isPySpace(s.charCodeAt(i))) {
      if (start >= 0) words.push(s.slice(start, i));
      start = -1;
    } else if (start < 0) {
      start = i;
    }
  }
  if (start >= 0) words.push(s.slice(start));
  return words;
}

const isAsciiDigits = (s: string): boolean => /^[0-9]+$/.test(s);

// Everything below runs on bytes a client sent, before any check of it: no regular expression
// that can backtrack (an unanchored `[\r\n]+$` on a run of 64 KiB of CRs takes seconds of the
// game's main thread). Line ends are stripped and lines split with index loops.

/** `s` without its trailing \r and \n characters (Python's rstrip("\r\n")), in linear time. */
export function stripLineEnds(s: string): string {
  let end = s.length;
  while (end > 0 && (s.charCodeAt(end - 1) === 13 || s.charCodeAt(end - 1) === 10)) end--;
  return end === s.length ? s : s.slice(0, end);
}

/** `s` split after each \r\n, \r or \n, line ends kept (feedparser's universal newlines). */
function splitLines(s: string): string[] {
  const lines: string[] = [];
  let start = 0;
  for (let i = 0; i < s.length; i++) {
    const ch = s.charCodeAt(i);
    if (ch === 13 && s.charCodeAt(i + 1) === 10) i++;
    if (ch === 13 || ch === 10) {
      lines.push(s.slice(start, i + 1));
      start = i + 1;
    }
  }
  if (start < s.length) lines.push(s.slice(start));
  return lines;
}

/** What int() skips around a number (CPython: ASCII whitespace, and \x85 / \xa0 mapped to spaces). */
const isIntSpace = (c: number): boolean => (c >= 9 && c <= 13) || c === 0x20 || c === 0x85 || c === 0xa0;

/**
 * A Content-Length as Python's int() reads it, or null where int() raises. Surrounding whitespace
 * is removed here in linear time, and whitespace inside is refused at once, so the core's pyInt
 * never meets a whitespace run (its strip pattern backtracks on one).
 */
function contentLength(raw: string): bigint | null {
  let start = 0;
  let end = raw.length;
  while (start < end && isIntSpace(raw.charCodeAt(start))) start++;
  while (end > start && isIntSpace(raw.charCodeAt(end - 1))) end--;
  for (let i = start; i < end; i++) if (isIntSpace(raw.charCodeAt(i))) return null;
  try {
    return pyInt(raw.slice(start, end));
  } catch {
    return null;
  }
}

/**
 * True for a Uint8Array of any realm (a Node Buffer included): NW.js's Node may run in a separate
 * context, where its Buffers fail `instanceof Uint8Array` of the page.
 */
export function isBytes(v: unknown): v is Uint8Array {
  return ArrayBuffer.isView(v) && Object.prototype.toString.call(v) === "[object Uint8Array]";
}

// email.feedparser: a header line starts with "From ", a name of printable characters but ":"
// followed by ":", or a space/tab (a continuation).
const HEADER_LINE = /^(From |[\x21-\x39\x3b-\x7e]*:|[\t ])/;

/**
 * The header block (Latin-1, line ends kept) as http.client.parse_headers reads it with
 * email.parser (compat32): {lowercase name: first value}.
 *   - the block is split at \r\n, \r or \n (feedparser's universal newlines);
 *   - the first line that is not a header line ends the block;
 *   - a continuation line (leading space or tab) is appended to the header before it, line ends
 *     and all (so a folded value matches nothing); one with no header before it is skipped;
 *   - a "From " line and a line with an empty name are skipped;
 *   - a value loses its leading spaces and tabs and its trailing \r and \n, not its trailing
 *     spaces; the first of a repeated name counts.
 */
export function pyHeaders(block: string): Map<string, string> {
  const lines = splitLines(block);
  const out = new Map<string, string>();
  let name = "";
  let parts: string[] = [];
  const finish = () => {
    if (name === "") return;
    const first = parts[0]!;
    let at = first.indexOf(":") + 1;
    while (first[at] === " " || first[at] === "\t") at++;
    const value = stripLineEnds(first.slice(at) + parts.slice(1).join(""));
    const key = name.toLowerCase();
    if (!out.has(key)) out.set(key, value);
    name = "";
    parts = [];
  };
  for (const line of lines) {
    if (!HEADER_LINE.test(line)) break;
    if (line[0] === " " || line[0] === "\t") {
      if (name !== "") parts.push(line);
      continue;
    }
    finish();
    if (line.startsWith("From ")) continue;
    const colon = line.indexOf(":");
    if (colon === 0) continue;
    name = line.slice(0, colon);
    parts = [line];
  }
  finish();
  return out;
}

/** The body as Python's json.loads(raw.decode("utf-8")) takes it: strict UTF-8, a BOM refused. */
function parseBody(raw: Bytes): { ok: boolean; value?: unknown } {
  let text: string;
  try {
    text = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(raw);
  } catch {
    return { ok: false };
  }
  try {
    return { ok: true, value: JSON.parse(text) };
  } catch {
    return { ok: false };
  }
}

const isPlainObject = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

function isJob(v: unknown): v is Job<unknown> {
  const j = v as Job<unknown> | null;
  return typeof j === "object" && j !== null && typeof j.done === "function" && typeof j.cancel === "function"
    && typeof j.exception === "function" && typeof j.result === "function"
    && typeof j.promise === "object" && j.promise !== null && typeof j.promise.then === "function";
}

// --- one connection -------------------------------------------------------------------------

type State = "head" | "body" | "building" | "writing" | "linger" | "closed";

class Conn {
  state: State = "head";
  buf = new Uint8Array(4096);
  len = 0;
  lineStart = 0; // where the line being read starts in buf
  scanned = 0; // where the search for that line's end goes on
  lines: string[] = []; // the request line, then the header lines with their line ends (Latin-1)
  method = "";
  path = "";
  sub = ""; // the path after /c/<secret>/
  need = 0; // body bytes to read
  job: Job<unknown> | null = null;
  timer: Timer = null;
  complete = false; // the whole request was read, and nothing after it
  ended = false; // the client closed its side
  drained = 0;
  readonly socket: NetSocket;
  constructor(socket: NetSocket) {
    this.socket = socket;
  }

  append(chunk: Bytes): void {
    if (this.len + chunk.length > this.buf.length) {
      let size = this.buf.length * 2;
      while (size < this.len + chunk.length) size *= 2;
      const next = new Uint8Array(size);
      next.set(this.buf.subarray(0, this.len));
      this.buf = next;
    }
    this.buf.set(chunk, this.len);
    this.len += chunk.length;
  }
}

export class CheckoutServer {
  load_asset: ServerOptions["load_asset"];
  session_info: unknown;
  on_tx: ServerOptions["on_tx"];
  get_status: ServerOptions["get_status"];
  load_logo: ServerOptions["load_logo"];
  build_tx: ServerOptions["build_tx"];
  readonly host: string;
  /** 16 random bytes, base64url without padding: Python's secrets.token_urlsafe(16). */
  readonly secret: string;
  /** Seconds a connection has for its request (and again for its answer). Tests lower it. */
  deadline_s = DEADLINE_S;
  /** How long /build waits for its job. Python's module constant BUILD_TIMEOUT_S; tests lower it. */
  build_timeout_s = BUILD_TIMEOUT_S;
  /** Why listening failed (Node's listen error), or null. */
  failed: unknown = null;
  /** Where errors of a listening server go (it keeps serving). */
  on_error: ((e: unknown) => void) | null;

  private readonly _net: NodeNet;
  private _server: NetServer | null = null;
  private _port = 0;
  private _listening = false;
  private _stopped = false;
  private _ready: Promise<void> | null = null;
  private _settle: { resolve: () => void; reject: (e: unknown) => void } | null = null;
  private readonly _conns = new Set<Conn>();
  private readonly _waiting: Conn[] = [];

  constructor(opts: ServerOptions) {
    this.load_asset = opts.load_asset ?? null;
    this.session_info = opts.session_info ?? {};
    this.on_tx = opts.on_tx ?? null;
    this.get_status = opts.get_status ?? null;
    this.load_logo = opts.load_logo ?? null;
    this.build_tx = opts.build_tx ?? null;
    this.host = String(opts.host ?? "127.0.0.1");
    this.on_error = opts.on_error ?? null;
    if (!opts.net || typeof opts.net.createServer !== "function") throw new TypeError("CheckoutServer needs Node's net module");
    this._net = opts.net;
    const random = opts.crypto ?? (globalThis as any).crypto as RandomSource | undefined;
    if (!random || typeof random.getRandomValues !== "function") throw new TypeError("CheckoutServer needs crypto.getRandomValues");
    this.secret = base64url(random.getRandomValues(new Uint8Array(16)));
  }

  /** The port bound once listening (0 before). */
  get port(): number {
    return this._port;
  }

  /** The page's URL once listening; "" until then (Ruling 1: the glue opens the browser only on a URL). */
  get url(): string {
    return this._listening || (this._stopped && this._port !== 0) ? `http://${this.host}:${this._port}/c/${this.secret}/` : "";
  }

  /** From start() until stop() or a failed listen. */
  get running(): boolean {
    return this._server !== null && this.failed === null;
  }

  /**
   * Bind `host` on a random free port. Returns at once: Node's listen() completes on a later turn
   * of the event loop (`url` is "" until then; see `ready()`). A second start() while running does
   * nothing; a stopped server cannot start again (its callbacks are released).
   */
  start(): void {
    if (this._stopped) throw new Error("this CheckoutServer was stopped; make a new one");
    if (this._server !== null) return;
    this._ready = new Promise<void>((resolve, reject) => (this._settle = { resolve, reject }));
    this._ready.catch(() => undefined); // a caller that only reads `failed` never sees the rejection
    const server = this._net.createServer({ allowHalfOpen: true, pauseOnConnect: true }, (socket: NetSocket) => this._accept(socket));
    this._server = server;
    server.on("listening", () => {
      if (this._server !== server) {
        server.close(); // stop() came first: the listen() in flight completes, then closes
        return;
      }
      const address = server.address() as { port?: unknown } | null;
      this._port = typeof address === "object" && address !== null ? Number(address.port) : 0;
      this._listening = true;
      this._settle?.resolve();
    });
    server.on("error", (e: unknown) => {
      if (this._server !== server) return;
      const error = e instanceof Error ? e : new Error(String(e));
      if (this._listening) {
        // after listen() completed (an accept that failed, EMFILE...): reported, still serving
        this._report(error);
        return;
      }
      this.failed = error;
      this._settle?.reject(this.failed);
    });
    try {
      server.listen(0, this.host);
    } catch (e) {
      this.failed = e instanceof Error ? e : new Error(String(e));
      this._settle?.reject(this.failed);
    }
  }

  /** Resolves once listening; rejects when the server cannot listen, or is stopped first. */
  ready(): Promise<void> {
    return this._ready ?? Promise.reject(new Error("the server was not started"));
  }

  /** Close the listener and every connection, cancel a build a page still waits for, release the callbacks. */
  stop(): void {
    if (this._stopped) return;
    this._stopped = true;
    const server = this._server;
    this._server = null;
    this._listening = false;
    if (server !== null) {
      try {
        server.close();
      } catch {
        // not listening yet (or failed): the "listening" handler closes it
      }
    }
    this._settle?.reject(new Error("the server was stopped"));
    for (const c of [...this._conns, ...this._waiting]) {
      if (c.job !== null && !c.job.done()) c.job.cancel(); // don't build it later for a page that is gone
      this._close(c);
    }
    this._conns.clear();
    this._waiting.length = 0;
    this.load_asset = null;
    this.session_info = null;
    this.on_tx = null;
    this.get_status = null;
    this.load_logo = null;
    this.build_tx = null;
    this.on_error = null;
  }

  private _report(e: unknown): void {
    try {
      if (typeof this.on_error === "function") this.on_error(e);
    } catch {
      // a reporter that fails must not reach Node's event loop
    }
  }

  /** The page lives in the player's browser: there is nothing to show again. */
  reopen(): void {}

  /** Nothing: Node answers from its event loop. */
  pump(): void {}

  // --- connections ------------------------------------------------------------------------

  private _accept(socket: NetSocket): void {
    const c = new Conn(socket);
    socket.on("error", () => this._close(c));
    socket.on("close", () => this._close(c));
    if (this._server === null) {
      this._close(c);
      return;
    }
    if (this._conns.size >= MAX_CONNECTIONS) {
      if (this._waiting.length >= MAX_WAITING) {
        this._close(c);
        return;
      }
      this._waiting.push(c); // paused (pauseOnConnect): read once a slot is free
      return;
    }
    this._serve(c);
  }

  private _serve(c: Conn): void {
    this._conns.add(c);
    c.socket.on("data", (chunk: Bytes) => this._data(c, chunk));
    c.socket.on("end", () => this._end(c));
    this._arm(c, this.deadline_s, () => this._close(c));
    c.socket.resume();
  }

  /** (Re)start the connection's one timer. */
  private _arm(c: Conn, seconds: number, fire: () => void): void {
    if (c.timer !== null) clearTimeout(c.timer as any);
    c.timer = setTimeout(fire, Math.max(0, seconds * 1000));
  }

  private _close(c: Conn): void {
    if (c.state === "closed") return;
    const served = this._conns.delete(c);
    const waiting = this._waiting.indexOf(c);
    if (waiting >= 0) this._waiting.splice(waiting, 1);
    c.state = "closed";
    if (c.timer !== null) clearTimeout(c.timer as any);
    c.timer = null;
    if (c.job !== null && !c.job.done()) c.job.cancel(); // the page stopped waiting
    c.job = null;
    try {
      c.socket.destroy();
    } catch {
      // already gone
    }
    if (served && this._server !== null) {
      const next = this._waiting.shift();
      if (next !== undefined) this._serve(next);
    }
  }

  private _data(c: Conn, chunk: Bytes): void {
    switch (c.state) {
      case "head":
        c.append(chunk);
        this._readHead(c);
        if ((c.state as State) === "body" && c.len >= c.need) this._onBody(c); // (_readHead moved it on)
        return;
      case "body":
        c.append(chunk);
        if (c.len >= c.need) this._onBody(c);
        return;
      case "building":
      case "writing":
      case "linger":
        // more than the request: drained (and counted) until the connection closes
        c.complete = false;
        c.drained += chunk.length;
        if (c.state === "linger" && c.drained >= LINGER_MAX_BYTES) this._close(c);
        return;
      default:
        return;
    }
  }

  /** The client closed its side: before a whole request, closed without an answer; after, it may still read. */
  private _end(c: Conn): void {
    c.ended = true;
    if (c.state === "head" || c.state === "body" || c.state === "linger") this._close(c);
  }

  /**
   * Splits off the complete lines received so far: the request line, then headers up to the
   * blank line. Only the first MAX_HEAD + 1 bytes may hold the head. Answers a bad request line at
   * once, as http.server does before reading the headers.
   */
  private _readHead(c: Conn): void {
    while (c.state === "head") {
      const limit = Math.min(c.len, MAX_HEAD + 1);
      const nl = c.buf.subarray(0, limit).indexOf(10, c.scanned);
      c.scanned = nl >= 0 ? nl + 1 : limit;
      const lineSize = (nl >= 0 ? nl + 1 : limit) - c.lineStart; // with its line end
      if (lineSize > MAX_LINE) {
        if (c.lines.length === 0) this._send(c, 414, "request line too long");
        else this._send(c, 431, "line too long");
        return;
      }
      if (nl < 0) {
        if (limit > MAX_HEAD) this._send(c, 431, "headers too large");
        return;
      }
      const start = c.lineStart;
      c.lineStart = nl + 1;
      if (c.lines.length === 0) {
        this._requestLine(c, stripLineEnds(latin1(c.buf, start, nl + 1)));
      } else if (c.lines.length > MAX_HEADERS) {
        // MAX_HEADERS header lines are in: this line, a header or the blank end, is one more
        this._send(c, 431, "too many headers");
      } else if (nl === start || (nl === start + 1 && c.buf[start] === 13)) {
        const rest = c.buf.slice(c.lineStart, c.len);
        c.buf = rest.length > 0 ? rest : new Uint8Array(1024);
        c.len = rest.length;
        this._onHead(c);
      } else {
        c.lines.push(latin1(c.buf, start, nl + 1));
      }
    }
  }

  /** BaseHTTPRequestHandler.parse_request's checks of the request line. */
  private _requestLine(c: Conn, line: string): void {
    const words = pySplit(line);
    if (words.length === 0) {
      this._close(c); // http.server closes without an answer
      return;
    }
    if (words.length >= 3) {
      const version = words[words.length - 1]!;
      const numbers = version.startsWith("HTTP/") ? version.slice(5).split(".") : [];
      const valid = numbers.length === 2 && numbers.every((n) => n.length > 0 && n.length <= 10 && isAsciiDigits(n));
      if (!valid) {
        this._send(c, 400, "bad request version");
        return;
      }
      if (Number(numbers[0]) >= 2) {
        this._send(c, 505, "invalid http version");
        return;
      }
    }
    if (words.length !== 3) {
      this._send(c, 400, "bad request syntax");
      return;
    }
    c.method = words[0]!;
    c.path = words[1]!;
    if (c.path.startsWith("//")) { // http.server's gh-87389 fix: one leading "/"
      let at = 0;
      while (c.path[at] === "/") at++;
      c.path = "/" + c.path.slice(at);
    }
    c.lines.push(line);
  }

  private _onHead(c: Conn): void {
    const headers = pyHeaders(c.lines.slice(1).join(""));
    c.complete = !(headers.has("content-length") || headers.has("transfer-encoding")) && c.len === 0;
    if (c.method !== "GET" && c.method !== "POST") {
      this._send(c, 501, "unsupported method", TEXT, c.method === "HEAD");
      return;
    }
    if (headers.get("host") !== `${this.host}:${this._port}`) {
      this._send(c, 403, "forbidden");
      return;
    }
    const prefix = `/c/${this.secret}/`;
    const path = c.path.split("?", 1)[0]!;
    if (!path.startsWith(prefix)) {
      this._send(c, 404, "not found");
      return;
    }
    c.sub = path.slice(prefix.length);
    if (c.method === "GET") {
      this._serveGet(c);
      return;
    }
    if ((c.sub === "build" && (this.build_tx === null || this.build_tx === undefined)) || (c.sub !== "build" && c.sub !== "tx")) {
      this._send(c, 404, "not found");
      return;
    }
    const n = contentLength(headers.get("content-length") ?? "0");
    const length = n === null ? -1 : n > BigInt(MAX_BODY) ? MAX_BODY + 1 : Number(n); // not a number: a 400
    if (length <= 0 || length > MAX_BODY) {
      this._send(c, length > MAX_BODY ? 413 : 400, "bad body");
      return;
    }
    c.need = length;
    c.state = "body";
    if (c.len >= c.need) this._onBody(c);
  }

  private _serveGet(c: Conn): void {
    const sub = c.sub === "" ? "index.html" : c.sub;
    if ((ASSETS as readonly string[]).includes(sub)) {
      const body = this._asset(sub);
      if (body === null) this._send(c, 500, "asset missing");
      else this._send(c, 200, body, CTYPES[sub]);
    } else if (sub === "session.json") {
      this._json(c, 200, this.session_info);
    } else if (sub === "status") {
      const get = this.get_status;
      let status: unknown;
      try {
        status = typeof get === "function" ? get() : undefined;
      } catch {
        status = undefined;
      }
      if (status === undefined) this._json(c, 503, { error: "unavailable" });
      else this._json(c, 200, status);
    } else if (sub === "logo" && this.load_logo !== null && this.load_logo !== undefined) {
      const logo = this._logo();
      if (logo === null) this._send(c, 404, "not found");
      else this._send(c, 200, logo[0], logo[1]);
    } else {
      this._send(c, 404, "not found");
    }
  }

  private _asset(name: string): Bytes | null {
    const load = this.load_asset;
    if (typeof load !== "function") return null;
    try {
      const body = load(name);
      if (typeof body === "string") return ENCODER.encode(body);
      return isBytes(body) ? body : null;
    } catch {
      return null;
    }
  }

  private _logo(): [Bytes, string] | null {
    const load = this.load_logo;
    if (typeof load !== "function") return null;
    try {
      const logo = load();
      if (Array.isArray(logo) && logo.length === 2 && isBytes(logo[0]) && typeof logo[1] === "string"
          && RASTER_TYPES.includes(logo[1])) {
        return [logo[0], logo[1]];
      }
    } catch {
      // unreadable: 404, as Python
    }
    return null;
  }

  private _onBody(c: Conn): void {
    const raw = c.buf.slice(0, c.need);
    c.complete = c.len === c.need;
    const parsed = parseBody(raw);
    if (!parsed.ok || !isPlainObject(parsed.value)) {
      this._send(c, 400, "bad body");
      return;
    }
    const data = parsed.value;
    if (c.sub === "build") {
      let account: string;
      try {
        account = parseBuildMessage(data);
      } catch {
        this._send(c, 400, "bad account");
        return;
      }
      const build = this.build_tx;
      let job: unknown = null;
      try {
        job = typeof build === "function" ? build(account) : null;
      } catch {
        job = null; // build_tx failed (worker busy...): the page offers a retry
      }
      if (c.state === "closed") return; // (build_tx may have stopped the server)
      if (!isJob(job)) {
        this._json(c, 503, { error: "unavailable" });
        return;
      }
      this._hold(c, job);
      return;
    }
    let hint: [string, string | null];
    try {
      hint = parseTxMessage(data);
    } catch {
      this._send(c, 400, "bad tx hash");
      return;
    }
    const onTx = this.on_tx;
    if (typeof onTx !== "function") {
      this._json(c, 503, { error: "unavailable" });
      return;
    }
    // ServerArgs.on_tx (core/checkout.ts) asks for the game loop's turn; this is the socket's turn
    // of Node's event loop instead (pump() does nothing). It is safe: JavaScript runs one thing at
    // a time, so on_tx never runs inside a worker command, and what it submits is queued
    // (Worker._pass takes the queue it finds) — as Python's server thread calls on_tx.
    try {
      onTx(hint[0], hint[1]);
    } catch {
      this._json(c, 503, { error: "unavailable" });
      return;
    }
    this._json(c, 202, { ok: true }); // (nothing when on_tx stopped the server)
  }

  /**
   * The held /build connection: answered once its job is done, 503 (and the job cancelled) after
   * build_timeout_s. A failed or cancelled job, or a result that is not an object, is a 503.
   */
  private _hold(c: Conn, job: Job<unknown>): void {
    c.job = job;
    c.state = "building";
    c.socket.pause(); // as the Godot server, nothing more is read while the job runs
    const answer = () => {
      if (c.state !== "building" || c.job !== job) return;
      c.job = null;
      const failure = job.exception();
      let value: unknown = null;
      if (failure === null) {
        try {
          value = job.result();
        } catch {
          value = null;
        }
      }
      if (isPlainObject(value)) this._json(c, 200, value);
      else this._json(c, 503, { error: "unavailable" });
    };
    this._arm(c, this.build_timeout_s, () => {
      if (c.state !== "building" || c.job !== job) return;
      c.job = null;
      job.cancel(); // don't build it later for a page that stopped waiting
      this._json(c, 503, { error: "unavailable" });
    });
    job.promise.then(answer, answer).catch((e: unknown) => this._report(e)); // never an unhandled rejection (MZ's onReject)
  }

  private _json(c: Conn, code: number, value: unknown): void {
    let text: string | undefined;
    try {
      text = JSON.stringify(value);
    } catch {
      text = undefined;
    }
    if (text === undefined) {
      code = 503;
      text = JSON.stringify({ error: "unavailable" });
    }
    this._send(c, code, ENCODER.encode(text), "application/json");
  }

  private _send(c: Conn, code: number, body: Bytes | string, ctype = TEXT, headOnly = false): void {
    if (c.state === "closed") return;
    const bytes = typeof body === "string" ? ENCODER.encode(body) : body;
    const head = ascii(`HTTP/1.1 ${code} ${REASONS[code] ?? "Unknown"}\r\nContent-Type: ${ctype}\r\n`
      + `Content-Length: ${bytes.length}\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n`
      + "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n");
    const out = new Uint8Array(head.length + (headOnly ? 0 : bytes.length));
    out.set(head);
    if (!headOnly) out.set(bytes, head.length);
    if (c.state === "head" || c.state === "body") c.drained = 0;
    c.buf = new Uint8Array(0);
    c.len = 0;
    c.state = "writing";
    c.socket.resume(); // (paused while building) drain what the client still sends
    const socket = c.socket as NetSocket & { once?: (e: string, f: () => void) => unknown };
    const deadline = Date.now() + this.deadline_s * 1000;
    this._arm(c, this.deadline_s, () => this._close(c));
    // "finish": everything is handed to the system and the FIN is queued
    const finished = () => {
      if (c.state !== "writing") return;
      if (c.complete || c.ended) {
        this._close(c);
        return;
      }
      // the client may still be sending what was never read: drain it before closing, so the
      // close does not reset the connection under the answer
      c.state = "linger";
      this._arm(c, Math.min((deadline - Date.now()) / 1000, LINGER_S), () => this._close(c));
      if (c.drained >= LINGER_MAX_BYTES) this._close(c);
    };
    if (typeof socket.once === "function") socket.once("finish", finished);
    else socket.on("finish", finished);
    socket.end(out);
  }
}
