/**
 * JSON-RPC client (twin of scfg_core/rpc.py).
 *
 * Public RPC endpoints are free but unreliable: they rate-limit, go down, and cap
 * eth_getLogs block ranges. The client tries endpoints in order on every call, skipping
 * ones in cooldown (30 s doubling to 10 min), and remembers per endpoint the largest
 * eth_getLogs range it accepts. Network access is an injected `Transport`; methods are
 * async where Python's are cooperative generators.
 */
import type { Transport } from "./coio.ts";
import { TransportError } from "./coio.ts";
import { ValueError } from "./errors.ts";
import { PY_DIGIT, digitRunValue, hasOwn, pyGet, pyTruthy } from "./pystr.ts";

export const COOLDOWN_START = 30.0;
export const COOLDOWN_MAX = 600.0;
/** The smallest eth_getLogs block range a chain may be configured with. */
export const MIN_LOG_RANGE = 10;
const RATE_HINTS = ["rate limit", "too many requests", "rate-limit"];
// Phrases providers use for "your eth_getLogs block range is too wide". Some say "archive".
const RANGE_HINTS = ["range", "too many", "limit", "exceed", "max results", "response size", "too large", "archive"];

/** A JSON-RPC error object returned by the node. `message` is the raw node message (Python `.message`). */
export class RpcError extends Error {
  readonly code: number;
  url?: string;

  constructor(code: number, message: string) {
    super(message);
    this.name = "RpcError";
    this.code = code;
  }

  /** Python `str(RpcError)` is "code: message". */
  override toString(): string {
    return `${this.code}: ${this.message}`;
  }
}

/** Every endpoint failed or is cooling down. */
export class RpcUnavailable extends Error {
  constructor(message?: string) {
    super(message);
    this.name = "RpcUnavailable";
  }
}

const HEX_INT = /^\s*([+-]?)(?:0[xX]_?)?([0-9a-fA-F]+(?:_[0-9a-fA-F]+)*)\s*$/;

/** Python `int(x, 16)`: TypeError unless a string, ValueError unless a valid hex literal. */
export function pyHexInt(x: unknown): number {
  if (typeof x !== "string") throw new TypeError("int() can't convert non-string with explicit base");
  const m = HEX_INT.exec(x);
  if (m === null) throw new ValueError(`invalid literal for int() with base 16: ${JSON.stringify(x)}`);
  const n = parseInt(m[2]!.replace(/_/g, ""), 16);
  return m[1] === "-" ? -n : n;
}

const lower = (s: string): string => s.toLowerCase();

export function isRangeError(err: RpcError): boolean {
  const m = lower(err.message);
  return RANGE_HINTS.some((h) => m.includes(h));
}

/** Python's `re.findall(r"\d+")`: every run of CPython 3.9's decimal digits (PY_DIGIT). */
const PY_DIGITS = new RegExp(`${PY_DIGIT}+`, "gu");

/**
 * Next eth_getLogs range to try after `err` rejected a `tried`-block request. Uses the limit
 * when the provider states one ("limited to 0 - 50 blocks range"), otherwise halves.
 */
export function suggestedRange(err: RpcError, tried: number): number {
  // A run beyond DIGIT_RUN_LIMIT significant digits is no number (digitRunValue gives null).
  const stated = (err.message.match(PY_DIGITS) ?? []).map(digitRunValue);
  const candidates = stated.filter((n): n is number => n !== null && MIN_LOG_RANGE <= n && n < tried);
  return candidates.length > 0 ? Math.max(...candidates) : Math.max(MIN_LOG_RANGE, Math.floor(tried / 2));
}

/** Python `str(x)` for the odd `message` values a provider may send (only reached for non-dict errors / non-string messages). */
function pyStr(v: unknown): string {
  if (v === null || v === undefined) return "None";
  if (v === true) return "True";
  if (v === false) return "False";
  if (typeof v === "string") return v;
  if (typeof v === "number" || typeof v === "bigint") return String(v);
  return JSON.stringify(v);
}

/** Python `int(x)` falling back to 0 on TypeError/ValueError, for a JSON-like `code`. */
function intOrZero(code: unknown): number {
  if (typeof code === "number") return Number.isFinite(code) ? Math.trunc(code) : 0;
  if (typeof code === "boolean") return code ? 1 : 0;
  if (typeof code === "string" && /^\s*[+-]?\d+(_\d+)*\s*$/.test(code)) return Number(code.replace(/_/g, ""));
  return 0;
}

/**
 * Providers are inconsistent: the error may be an object, a bare string, or carry a
 * non-numeric code. Never let a malformed error crash the game.
 */
function parseError(e: unknown): RpcError {
  let code: unknown;
  let message: unknown;
  if (typeof e === "object" && e !== null && !Array.isArray(e)) {
    const d = e as Record<string, unknown>;
    code = hasOwn(d, "code") ? d["code"] : 0;
    message = hasOwn(d, "message") ? d["message"] : "";
  } else {
    code = 0;
    message = e;
  }
  return new RpcError(intOrZero(code), pyStr(message));
}

function isRateError(err: RpcError): boolean {
  const m = lower(err.message);
  return RATE_HINTS.some((h) => m.includes(h));
}

interface EndpointState {
  delay: number;
  until: number;
}

type Attempt = ["ok", unknown] | ["error", RpcError] | null;

export class RpcClient {
  readonly urls: readonly string[];
  clock: () => number;
  timeout: number;
  private readonly transport: Transport;
  private readonly _ranges: Map<string, number>;
  private readonly _state: Map<string, EndpointState>;
  private _nextId = 1;

  constructor(urls: readonly string[], opts: { transport: Transport; log_range?: number; clock?: () => number; timeout?: number }) {
    if (!urls || urls.length === 0) throw new ValueError("at least one RPC url required");
    this.urls = [...urls];
    const logRange = opts.log_range ?? 1000;
    this._ranges = new Map(this.urls.map((u) => [u, logRange]));
    this.clock = opts.clock ?? (() => Date.now() / 1000);
    this.timeout = opts.timeout ?? 10.0;
    this.transport = opts.transport;
    this._state = new Map(this.urls.map((u) => [u, { delay: 0.0, until: 0.0 }]));
  }

  private _fail(url: string, now: number): void {
    const st = this._state.get(url)!;
    st.delay = st.delay === 0 ? COOLDOWN_START : Math.min(COOLDOWN_MAX, st.delay * 2);
    st.until = now + st.delay;
  }

  private _ok(url: string): void {
    const st = this._state.get(url)!;
    st.delay = 0.0;
    st.until = 0.0;
  }

  /** The endpoint the next call will try first. */
  private _preferred(): string {
    const now = this.clock();
    for (const url of this.urls) {
      if (this._state.get(url)!.until <= now) return url;
    }
    return this.urls[0]!;
  }

  /** eth_getLogs block range accepted by the currently preferred endpoint. */
  get logRange(): number {
    return this._ranges.get(this._preferred())!;
  }

  /** Endpoints not cooling down right now, in preference order. */
  liveUrls(): string[] {
    const now = this.clock();
    return this.urls.filter((u) => this._state.get(u)!.until <= now);
  }

  async call(method: string, params: unknown[]): Promise<any> {
    return (await this._call(method, params))[0];
  }

  /** ["ok", result] | ["error", RpcError] | null when the endpoint failed (cooled down). */
  private async _attempt(url: string, method: string, params: unknown[], now: number): Promise<Attempt> {
    const payload = { jsonrpc: "2.0", id: this._nextId++, method, params };
    let resp: unknown;
    try {
      resp = await this.transport.postJson(url, payload, this.timeout);
    } catch (e) {
      if (e instanceof TransportError) {
        this._fail(url, now);
        return null;
      }
      throw e;
    }
    const has = (k: string): boolean => hasOwn(resp as object, k);
    if (typeof resp !== "object" || resp === null || Array.isArray(resp) || (!has("result") && !has("error"))) {
      this._fail(url, now);
      return null;
    }
    const body = resp as Record<string, unknown>;
    if (body["error"] !== null && body["error"] !== undefined) {
      const err = parseError(body["error"]);
      if (isRateError(err)) {
        this._fail(url, now);
        return null;
      }
      this._ok(url);
      err.url = url;
      return ["error", err];
    }
    this._ok(url);
    // Python's resp["result"]: a body like {"error": null} has no result, which is a KeyError
    // (not a TransportError), so it propagates rather than cooling the endpoint down.
    return ["ok", pyGet(body, "result")];
  }

  /** Like call(), but also returns which endpoint answered. */
  private async _call(method: string, params: unknown[]): Promise<[any, string]> {
    const now = this.clock();
    for (const url of this.urls) {
      if (this._state.get(url)!.until > now) continue;
      const res = await this._attempt(url, method, params, now);
      if (res === null) continue;
      const [kind, value] = res;
      if (kind === "error") throw value;
      return [value, url];
    }
    throw new RpcUnavailable(`all endpoints failed for ${method}`);
  }

  /** Ask one endpoint only. RpcUnavailable if it fails, RpcError for a node error. */
  async callAt(url: string, method: string, params: unknown[]): Promise<any> {
    const res = await this._attempt(url, method, params, this.clock());
    if (res === null) throw new RpcUnavailable(`${url} failed for ${method}`);
    const [kind, value] = res;
    if (kind === "error") throw value;
    return value;
  }

  /** Ask one endpoint: ["ok", result] | ["error", RpcError] | null if it failed. */
  async tryAt(url: string, method: string, params: unknown[]): Promise<Attempt> {
    return this._attempt(url, method, params, this.clock());
  }

  async blockNumber(): Promise<number> {
    return pyHexInt(await this.call("eth_blockNumber", []));
  }

  async getTransactionReceipt(txHash: string): Promise<any> {
    return this.call("eth_getTransactionReceipt", [txHash]);
  }

  async getBlockHash(number: number): Promise<string | null> {
    const block = await this.call("eth_getBlockByNumber", ["0x" + number.toString(16), false]);
    if (!pyTruthy(block)) return null;
    const hash = pyGet(block, "hash");
    if (typeof hash !== "string") throw new TypeError("block hash is not a string");
    return hash.toLowerCase();
  }

  async getLogs(address: string, topics: unknown[], fromBlock: number, toBlock: number): Promise<any[]> {
    const out: any[] = [];
    let start = fromBlock;
    while (start <= toBlock) {
      const end = Math.min(toBlock, start + this.logRange - 1);
      let logs: any;
      try {
        [logs] = await this._call("eth_getLogs", [{
          address, topics, fromBlock: "0x" + start.toString(16), toBlock: "0x" + end.toString(16),
        }]);
      } catch (e) {
        if (!(e instanceof RpcError)) throw e;
        // Python getattr(e, "url", self._preferred()): the fallback is only evaluated when url is unset.
        const url = e.url ?? this._preferred();
        const tried = Math.min(this._ranges.get(url)!, end - start + 1);
        if (isRangeError(e) && tried > MIN_LOG_RANGE) {
          this._ranges.set(url, suggestedRange(e, tried));
          continue;
        }
        throw e;
      }
      if (logs) for (const log of logs) out.push(log);
      start = end + 1;
    }
    return out;
  }
}
