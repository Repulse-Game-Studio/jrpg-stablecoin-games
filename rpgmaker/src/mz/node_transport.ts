/**
 * The desktop (NW.js) RPC transport: JSON-RPC over Node's `https` (and `http`, for a local node
 * such as Anvil), the twin of scfg_core.rpc.UrllibTransport. NW.js's `fetch` runs
 * from the game page's origin and depends on each RPC's CORS headers, which urllib never needed.
 *
 *   new NodeHttpsTransport({ https: require("https"), http: require("http") })
 *
 * The modules are passed in (the glue looks them up at run time on NW.js, platform.ts
 * requireNode); this file never imports Node. As UrllibTransport:
 *   - a POST with `Content-Type: application/json` and `User-Agent: scfg/0.1`;
 *   - `timeout` (seconds) bounds every wait on the socket, the connection and each read of the
 *     answer, not the whole exchange (urllib's socket timeout): a silent server is a
 *     TransportError("timed out");
 *   - a 2xx answer is decoded as UTF-8 JSON, else TransportError("invalid JSON response") (JSON as
 *     the core's Python reads it, pystr.ts jsonLoads: an integer literal of more than 4300 digits
 *     is not JSON);
 *   - any other status is TransportError("HTTP <status>", status), except a JSON object with a
 *     truthy "error" (a JSON-RPC error some providers send with a 4xx), which is returned;
 *   - a network failure (refused, DNS, TLS, cut off) is TransportError(<its message>).
 * An answer of more than MAX_RESPONSE_BYTES (coio.ts; urllib has no cap) is
 * TransportError("response too large"): refused on its Content-Length, else when the bytes
 * received pass the limit, and the request is destroyed either way.
 * One deviation: a redirect is not followed (urllib turns a 301-303 to a POST into a GET, which
 * no JSON-RPC endpoint answers); it is TransportError("HTTP 30x"). A failure's Node error code
 * (`ECONNREFUSED`, `CERT_HAS_EXPIRED`, ...) is kept as the TransportError's `code`.
 *
 * THE FALLBACK. NW.js's Node trusts only the CA bundle frozen into its build (no OS certificate
 * store: a corporate or antivirus root, a CA newer than the build) and ignores the OS proxy;
 * Chromium's `fetch` in the same window does both. With `fallback` (the glue passes the core's
 * FetchTransport), a request Node fails on TLS trust (TRUST_CODES), a proxy-class failure
 * (PROXY_CODES) or HTTP 407 goes once more through it, and the fallback's answer or failure is
 * the request's. That failure's message ends with Node's code ("Failed to fetch (Node's https
 * failed first: CERT_HAS_EXPIRED)") and its `cause` is the fallback's own error. Our own read
 * timeout ("timed out") is not retried: it would double the wait on a server that is only slow.
 *
 * Node APIs used are all in Node 8 or older (`request(options, callback)`, req.on("error"),
 * setTimeout, destroy, end(string), res.statusCode, res.on("data" | "end" | "error" | "aborted"));
 * URL, TextEncoder and TextDecoder come from NW.js's Chromium.
 */
import { TransportError, type Transport } from "../core/index.ts";
import { MAX_RESPONSE_BYTES, RESPONSE_TOO_LARGE, declaredLength } from "../core/coio.ts";
import { jsonLoads, pyTruthy } from "../core/pystr.ts";

/** Node's `http` / `https` module, as far as the transport uses it. */
export interface NodeHttpModule {
  request(options: Record<string, unknown>, callback: (res: any) => void): any;
}

export interface NodeTransportOptions {
  https: NodeHttpModule;
  /** For http: URLs (a local node); without it they fail. */
  http?: NodeHttpModule | null;
  user_agent?: string;
  /** Where a request goes once more when Node fails it on trust or a proxy (see the header). */
  fallback?: Transport | null;
  /** The most bytes of an answer that are read. Default: MAX_RESPONSE_BYTES. */
  max_response_bytes?: number;
}

/** Node's certificate / TLS-trust failures (the frozen CA bundle). */
export const TRUST_CODES: ReadonlySet<string> = new Set([
  "CERT_HAS_EXPIRED", "CERT_NOT_YET_VALID", "CERT_UNTRUSTED", "UNABLE_TO_GET_ISSUER_CERT", "UNABLE_TO_GET_ISSUER_CERT_LOCALLY",
  "UNABLE_TO_VERIFY_LEAF_SIGNATURE", "SELF_SIGNED_CERT_IN_CHAIN", "DEPTH_ZERO_SELF_SIGNED_CERT", "ERR_TLS_CERT_ALTNAME_INVALID",
]);

/** Node's failures that a direct connection gets where only the OS proxy goes through. */
export const PROXY_CODES: ReadonlySet<string> = new Set([
  "ENOTFOUND", "EAI_AGAIN", "ECONNREFUSED", "ECONNRESET", "ETIMEDOUT", "EHOSTUNREACH", "ENETUNREACH",
]);

/** HTTP 407 Proxy Authentication Required. */
const PROXY_AUTH = 407;

/**
 * The fallback's failure `f` after Node's `first`: a TransportError (fetch's say only "Failed to
 * fetch") gets Node's code (its message for a 407) appended and `f` as its `cause`; anything else
 * (a bug, not a transport failure) is passed on as it is.
 */
function afterNode(f: unknown, first: TransportError): unknown {
  if (!(f instanceof TransportError)) return f;
  const message = `${f.message} (Node's https failed first: ${codeOf(first) ?? first.message})`;
  const err = withCode(new TransportError(message, f.status), codeOf(f));
  // ES2022's Error cause, set by hand (the bundle targets ES2020; NW.js's older Chromium lacks the option)
  Object.defineProperty(err, "cause", { value: f, enumerable: false, writable: true, configurable: true });
  return err;
}

/** True for a failure the fallback may get through (see the header). */
export function trustOrProxyFailure(e: unknown): boolean {
  if (!(e instanceof TransportError)) return false;
  if (e.status === PROXY_AUTH) return true;
  const code = (e as TransportError & { code?: unknown }).code;
  return typeof code === "string" && (TRUST_CODES.has(code) || PROXY_CODES.has(code));
}

const messageOf = (e: unknown): string => (e instanceof Error ? e.message : String(e));
const codeOf = (e: unknown): string | null => {
  const code = typeof e === "object" && e !== null ? (e as { code?: unknown }).code : undefined;
  return typeof code === "string" && code !== "" ? code : null;
};
/** `err` with Node's error `code` (when there is one) as its own `code`. */
const withCode = (err: TransportError, code: string | null): TransportError => {
  if (code !== null) Object.defineProperty(err, "code", { value: code, enumerable: true });
  return err;
};

export class NodeHttpsTransport implements Transport {
  private readonly _https: NodeHttpModule;
  private readonly _http: NodeHttpModule | null;
  private readonly _ua: string;
  private readonly _maxBytes: number;
  /** The transport a trust or proxy failure is retried through, or null (see the header). */
  readonly fallback: Transport | null;

  constructor(opts: NodeTransportOptions) {
    if (!opts || !opts.https || typeof opts.https.request !== "function") {
      throw new TypeError("NodeHttpsTransport needs Node's https module");
    }
    this._https = opts.https;
    this._http = opts.http && typeof opts.http.request === "function" ? opts.http : null;
    this._ua = opts.user_agent ?? "scfg/0.1";
    this._maxBytes = opts.max_response_bytes ?? MAX_RESPONSE_BYTES;
    this.fallback = opts.fallback && typeof opts.fallback.postJson === "function" ? opts.fallback : null;
  }

  postJson(url: string, payload: object, timeout: number): Promise<unknown> {
    const direct = this._exchange(url, payload, timeout).then(([status, raw]) => decode(status, raw));
    const fallback = this.fallback;
    if (fallback === null) return direct;
    return direct.catch((e: unknown) => {
      if (!trustOrProxyFailure(e)) throw e;
      // once; its answer or failure is the request's, a failure naming why Node's went first
      return fallback.postJson(url, payload, timeout).catch((f: unknown) => {
        throw afterNode(f, e as TransportError);
      });
    });
  }

  /** [status, body bytes] of one POST, or a TransportError. */
  private _exchange(url: string, payload: object, timeout: number): Promise<[number, Uint8Array]> {
    return new Promise((resolve, reject) => {
      let target: URL;
      try {
        target = new URL(url);
      } catch (e) {
        reject(new TransportError(`unknown url type: ${messageOf(e)}`));
        return;
      }
      const module = target.protocol === "https:" ? this._https : target.protocol === "http:" ? this._http : null;
      if (module === null) {
        reject(new TransportError(target.protocol === "http:"
          ? "http: URLs need Node's http module" : `unknown url type: ${target.protocol.replace(/:$/, "")}`));
        return;
      }
      // A string body: Node 14 and older (early MZ's NW.js) take no Uint8Array in end().
      const text = JSON.stringify(payload);
      const length = new TextEncoder().encode(text).length;
      let settled = false;
      const fail = (message: string, code: string | null = null) => {
        if (settled) return;
        settled = true;
        reject(withCode(new TransportError(message), code));
      };
      let req: any = null;
      const destroyQuietly = () => {
        try {
          if (req !== null && typeof req.destroy === "function") req.destroy();
        } catch {
          // already gone
        }
      };
      // Everything that touches the module is inside the try: a throw (a refused option, an
      // older Node's TypeError) is a TransportError like any other failure, never a raw error.
      try {
        req = module.request({
          protocol: target.protocol,
          hostname: target.hostname.replace(/^\[(.*)\]$/, "$1"), // [::1] -> ::1
          port: target.port === "" ? undefined : Number(target.port),
          path: (target.pathname || "/") + target.search,
          method: "POST",
          headers: { "Content-Type": "application/json", "Content-Length": String(length), "User-Agent": this._ua },
        }, (res: any) => {
          try {
            const limit = this._maxBytes;
            const tooLarge = () => {
              fail(RESPONSE_TOO_LARGE);
              destroyQuietly();
            };
            const parts: Uint8Array[] = [];
            let size = 0;
            res.on("data", (chunk: Uint8Array) => {
              if (settled) return;
              size += chunk.length;
              if (size > limit) {
                parts.length = 0;
                tooLarge();
                return;
              }
              parts.push(chunk);
            });
            res.on("error", (e: unknown) => fail(messageOf(e), codeOf(e)));
            res.on("aborted", () => fail("the connection was closed before the answer was complete"));
            res.on("end", () => {
              if (settled) return;
              settled = true;
              const raw = new Uint8Array(size);
              let at = 0;
              for (const p of parts) {
                raw.set(p, at);
                at += p.length;
              }
              resolve([Number(res.statusCode), raw]);
            });
            // (after the listeners: destroying the request may still emit an error on `res`)
            const declared = declaredLength(res.headers ? res.headers["content-length"] : undefined);
            if (declared !== null && declared > limit) tooLarge();
          } catch (e) {
            fail(messageOf(e));
            destroyQuietly();
          }
        });
        req.on("error", (e: unknown) => fail(messageOf(e), codeOf(e)));
        // urllib's socket timeout: every wait on the socket (connect, each read) has `timeout`
        req.setTimeout(Math.max(1, timeout * 1000), () => {
          fail("timed out");
          req.destroy();
        });
        req.end(text);
      } catch (e) {
        fail(messageOf(e));
        destroyQuietly();
      }
    });
  }
}

// strict UTF-8 that keeps a BOM, as Python's raw.decode("utf-8") (json.loads then refuses it)
const UTF8 = () => new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });

/** The answer as UrllibTransport.post_json returns it, or its TransportError. */
function decode(status: number, raw: Uint8Array): unknown {
  if (!(status >= 200 && status < 300)) {
    // some providers send JSON-RPC errors ("range too large") with a 4xx: passed through
    let body: unknown = null;
    try {
      body = jsonLoads(UTF8().decode(raw));
    } catch {
      body = null;
    }
    if (typeof body === "object" && body !== null && !Array.isArray(body)
        && pyTruthy((body as Record<string, unknown>)["error"])) {
      return body;
    }
    throw new TransportError(`HTTP ${status}`, status);
  }
  try {
    return jsonLoads(UTF8().decode(raw));
  } catch {
    throw new TransportError("invalid JSON response");
  }
}
