/**
 * Transport types for the payment core (twin of the transport half of scfg_core/coio.py
 * and rpc.UrllibTransport). The core never performs a network request itself: it awaits
 * an injected `Transport`. ES2020 built-ins only; `fetch` and `AbortController` are
 * looked up on `globalThis` at call time so this file needs neither DOM nor Node types.
 */
import { pyTruthy } from "./pystr.ts";

/** Network or HTTP failure talking to one endpoint. */
export class TransportError extends Error {
  readonly status: number | null;

  constructor(message: string, status: number | null = null) {
    super(message);
    this.name = "TransportError";
    this.status = status;
  }
}

export interface Transport {
  postJson(url: string, payload: object, timeout: number): Promise<unknown>;
}

interface MinimalResponse {
  ok: boolean;
  status: number;
  text(): Promise<string>;
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
  setTimeout: (fn: () => void, ms: number) => unknown;
  clearTimeout: (handle: unknown) => void;
}

function messageOf(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

export class FetchTransport implements Transport {
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
      raw = await resp.text();
    } catch (e) {
      throw new TransportError(messageOf(e));
    } finally {
      if (timer !== null) g.clearTimeout(timer);
    }
    if (!ok) {
      // Some providers send JSON-RPC errors (e.g. "range too large") with a 4xx status.
      // Pass those through so the client can react to the message.
      let body: unknown = null;
      try {
        body = JSON.parse(raw);
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
      return JSON.parse(raw);
    } catch {
      throw new TransportError("invalid JSON response");
    }
  }
}
