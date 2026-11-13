/**
 * Transport types for the payment core (twin of the transport half of scfg_core/coio.py
 * and rpc.UrllibTransport). The core never performs a network request itself: it awaits
 * an injected `Transport`. ES2020 built-ins only; `fetch` and `AbortController` are
 * looked up on `globalThis` at call time so this file needs neither DOM nor Node types.
 */
import { jsonLoads, pyTruthy } from "./pystr.ts";

/** Network or HTTP failure talking to one endpoint. */
export class TransportError extends Error {
  readonly status: number | null;

  constructor(message: string, status: number | null = null) {
    super(message);
    this.name = "TransportError";
    this.status = status;
  }
}

/**
 * The most bytes of one HTTP answer a transport reads; a longer one is
 * TransportError("response too large") (the message Godot's transport gives for its body size
 * limit; scfg_core's UrllibTransport reads without a cap). An `eth_getLogs` page filtered to one
 * recipient is a few kilobytes, and public RPC providers cap their own answers near 10 MB, so
 * 16 MiB only refuses an endpoint that is misbehaving or hostile.
 */
export const MAX_RESPONSE_BYTES = 16 * 1024 * 1024;

export const RESPONSE_TOO_LARGE = "response too large";

/** The byte count a Content-Length header declares, or null (absent, or not a plain number). */
export function declaredLength(value: unknown): number | null {
  return typeof value === "string" && /^[0-9]+$/.test(value.trim()) ? Number(value.trim()) : null;
}

export interface Transport {
  postJson(url: string, payload: object, timeout: number): Promise<unknown>;
}

interface MinimalReader {
  read(): Promise<{ done: boolean; value?: Uint8Array }>;
  cancel(): Promise<unknown>;
}

interface MinimalResponse {
  ok: boolean;
  status: number;
  text(): Promise<string>;
  headers?: { get(name: string): string | null } | null;
  body?: { getReader(): MinimalReader } | null;
}

interface MinimalTextDecoder {
  decode(input?: Uint8Array, options?: { stream?: boolean }): string;
}

interface MinimalAbortController {
  signal: unknown;
  abort(): void;
}

interface FetchGlobals {
  fetch?: (url: string, init: {
    method: string;
    headers: Record<string, string>;
    body: string;
    signal: unknown;
  }) => Promise<MinimalResponse>;
  AbortController?: new () => MinimalAbortController;
  TextDecoder?: new (label?: string) => MinimalTextDecoder;
  setTimeout: (fn: () => void, ms: number) => unknown;
  clearTimeout: (handle: unknown) => void;
}

function messageOf(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

/**
 * The body of `resp` as `resp.text()` gives it (UTF-8, a BOM dropped, bad bytes replaced), or
 * TransportError("response too large") past `limit` bytes. Where the platform streams the body
 * (`resp.body.getReader()`, with TextDecoder) the read stops at the limit and `abort` ends the
 * request; elsewhere a declared Content-Length is refused before reading, and the text is
 * measured after (a string is never longer than its UTF-8 bytes).
 */
async function readCapped(g: FetchGlobals, resp: MinimalResponse, limit: number, abort: () => void): Promise<string> {
  const tooLarge = (): TransportError => {
    abort();
    return new TransportError(RESPONSE_TOO_LARGE);
  };
  const declared = resp.headers && typeof resp.headers.get === "function"
    ? declaredLength(resp.headers.get("content-length")) : null;
  if (declared !== null && declared > limit) throw tooLarge();
  const body = resp.body;
  if (!body || typeof body.getReader !== "function" || typeof g.TextDecoder !== "function") {
    const text = await resp.text();
    if (text.length > limit) throw tooLarge();
    return text;
  }
  const reader = body.getReader();
  const decoder = new g.TextDecoder("utf-8");
  let text = "";
  let size = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    if (value === undefined) continue;
    size += value.length;
    if (size > limit) {
      // the rest is not wanted; a cancel that fails changes nothing
      reader.cancel().catch(() => undefined);
      throw tooLarge();
    }
    text += decoder.decode(value, { stream: true });
  }
  return text + decoder.decode();
}

export class FetchTransport implements Transport {
  /** The most bytes of an answer that are read (MAX_RESPONSE_BYTES). */
  readonly maxResponseBytes: number;

  constructor(maxResponseBytes: number = MAX_RESPONSE_BYTES) {
    this.maxResponseBytes = maxResponseBytes;
  }

  async postJson(url: string, payload: object, timeout: number): Promise<unknown> {
    const g = globalThis as unknown as FetchGlobals;
    if (typeof g.fetch !== "function") throw new TransportError("fetch is not available");
    const controller = typeof g.AbortController === "function" ? new g.AbortController() : null;
    const timer = controller ? g.setTimeout(() => controller.abort(), timeout * 1000) : null;
    let status: number;
    let ok: boolean;
    let raw: string;
    try {
      const resp = await g.fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
        signal: controller ? controller.signal : undefined,
      });
      status = resp.status;
      ok = resp.ok;
      raw = await readCapped(g, resp, this.maxResponseBytes, () => {
        if (controller) controller.abort();
      });
    } catch (e) {
      if (e instanceof TransportError) throw e;
      throw new TransportError(messageOf(e));
    } finally {
      if (timer !== null) g.clearTimeout(timer);
    }
    if (!ok) {
      // Some providers send JSON-RPC errors (e.g. "range too large") with a 4xx status.
      // Pass those through so the client can react to the message.
      let body: unknown = null;
      try {
        body = jsonLoads(raw);
      } catch {
        body = null;
      }
      if (typeof body === "object" && body !== null && !Array.isArray(body)
          && pyTruthy((body as Record<string, unknown>).error)) { // body.get("error")
        return body;
      }
      throw new TransportError(`HTTP ${status}`, status);
    }
    try {
      return jsonLoads(raw);
    } catch {
      throw new TransportError("invalid JSON response");
    }
  }
}
