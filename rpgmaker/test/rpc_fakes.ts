/** Shared test doubles for the RPC tests (twin of tests/unit/fakes.py). Not a test file. */
import type { Transport } from "../src/core/coio.ts";
import { TransportError } from "../src/core/coio.ts";

export type Handler = (m: string, p: any) => unknown;
export class FakeClock {
  now = 1000;
  call = (): number => this.now;
  advance(s: number): void { this.now += s; }
}
export class FakeTransport implements Transport {
  calls: Array<[string, string, any]> = [];
  payloads: any[] = [];
  handlers: Record<string, Handler>;
  constructor(handlers: Record<string, Handler>) { this.handlers = handlers; }
  async postJson(url: string, payload: any, _timeout: number): Promise<unknown> {
    this.payloads.push(payload);
    this.calls.push([url, payload.method, payload.params]);
    return this.handlers[url]!(payload.method, payload.params);
  }
}
export const ok = (result: unknown) => ({ jsonrpc: "2.0", id: 1, result });
export const err = (code: number, message: string) => ({ jsonrpc: "2.0", id: 1, error: { code, message } });
export const down: Handler = () => { throw new TransportError("connection refused"); };

