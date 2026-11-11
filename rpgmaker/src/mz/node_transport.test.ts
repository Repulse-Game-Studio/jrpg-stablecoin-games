/**
 * The desktop RPC transport (node_transport.ts) against local Node HTTP servers:
 * the twin of scfg_core.rpc.UrllibTransport's timeouts and error mapping. The Anvil run is
 * tests/integration/test_js_core.py::test_node_transport_against_anvil.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import https from "node:https";
import type { AddressInfo } from "node:net";
import { TransportError } from "../core/index.ts";
import { NodeHttpsTransport } from "./node_transport.ts";

interface Seen { method: string; url: string; headers: http.IncomingHttpHeaders; body: string }

let servers: http.Server[] = [];

afterEach(async () => {
  for (const s of servers) {
    s.closeAllConnections();
    await new Promise<void>((r) => s.close(() => r()));
  }
  servers = [];
});

/** A local HTTP server answering with `handler`; resolves its URL and what it was sent. */
async function serve(handler: (req: http.IncomingMessage, res: http.ServerResponse, body: string) => void) {
  const seen: Seen[] = [];
  const server = http.createServer((req, res) => {
    const parts: Buffer[] = [];
    req.on("data", (b: Buffer) => parts.push(b));
    req.on("end", () => {
      const body = Buffer.concat(parts).toString("utf8");
      seen.push({ method: req.method!, url: req.url!, headers: req.headers, body });
      handler(req, res, body);
    });
  });
  servers.push(server);
  await new Promise<void>((r) => server.listen(0, "127.0.0.1", () => r()));
  const { port } = server.address() as AddressInfo;
  return { url: `http://127.0.0.1:${port}/rpc?key=1`, port, seen };
}

const transport = () => new NodeHttpsTransport({ https, http });

/** `p`, or a rejection after `ms` (a transport that never gives up fails the test instead of hanging it). */
function within<T>(p: Promise<T>, ms: number): Promise<T> {
  let timer: ReturnType<typeof setTimeout>;
  const limit = new Promise<never>((_, reject) => (timer = setTimeout(() => reject(new Error(`still waiting after ${ms} ms`)), ms)));
  return Promise.race([p, limit]).finally(() => clearTimeout(timer));
}

const isTransportError = (status: number | null, message?: RegExp) => (e: unknown) => {
  assert.ok(e instanceof TransportError, `a TransportError, got ${String(e)}`);
  assert.equal(e.status, status);
  if (message) assert.match(e.message, message);
  return true;
};

test("postsJsonAndReturnsTheDecodedAnswer", async () => {
  const { url, seen } = await serve((_req, res) => {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ jsonrpc: "2.0", id: 1, result: "0x10" }));
  });
  const out = await transport().postJson(url, { jsonrpc: "2.0", id: 1, method: "eth_blockNumber", params: [] }, 5);
  assert.deepEqual(out, { jsonrpc: "2.0", id: 1, result: "0x10" });
  assert.equal(seen.length, 1);
  assert.equal(seen[0]!.method, "POST");
  assert.equal(seen[0]!.url, "/rpc?key=1", "path and query string");
  assert.equal(seen[0]!.headers["content-type"], "application/json");
  assert.equal(seen[0]!.headers["user-agent"], "scfg/0.1", "UrllibTransport's User-Agent");
  assert.equal(seen[0]!.headers["content-length"], String(Buffer.byteLength(seen[0]!.body)));
  assert.deepEqual(JSON.parse(seen[0]!.body), { jsonrpc: "2.0", id: 1, method: "eth_blockNumber", params: [] });
});

test("timeoutIsATransportError", async () => {
  const { url } = await serve(() => undefined); // never answers
  const t0 = performance.now();
  await assert.rejects(within(transport().postJson(url, {}, 0.2), 3000), isTransportError(null, /timed out/));
  const ms = performance.now() - t0;
  assert.ok(ms >= 150 && ms < 3000, `after the timeout (${ms} ms)`);
  // an answer whose body stalls is a timeout too (urllib: each read has the timeout)
  const stalled = await serve((_req, res) => {
    res.writeHead(200, { "Content-Type": "application/json", "Content-Length": "100" });
    res.write("{\"result\":");
  });
  await assert.rejects(within(transport().postJson(stalled.url, {}, 0.2), 3000), isTransportError(null, /timed out/));
});

test("non200IsATransportError", async () => {
  let reply: [number, string] = [500, "oops"];
  const { url } = await serve((_req, res) => {
    res.writeHead(reply[0], { "Content-Type": "application/json" });
    res.end(reply[1]);
  });
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(500, /^HTTP 500$/));
  reply = [429, JSON.stringify({ jsonrpc: "2.0", id: 1, error: { code: -32005, message: "rate limit" } })];
  assert.deepEqual(await transport().postJson(url, {}, 5), JSON.parse(reply[1]),
    "a JSON-RPC error sent with a 4xx status is passed through (the client reacts to its message)");
  reply = [503, JSON.stringify({ error: null })];
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(503), "a falsy error is not passed through");
  reply = [400, JSON.stringify(["error"])];
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(400));
  reply = [302, ""];
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(302), "a redirect is not followed");
  reply = [204, ""];
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(null, /invalid JSON response/),
    "any 2xx is a success, and an empty one is not JSON");
});

test("invalidJsonIsATransportError", async () => {
  let body: Buffer = Buffer.from("not json");
  const { url } = await serve((_req, res) => {
    res.writeHead(200);
    res.end(body);
  });
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(null, /^invalid JSON response$/));
  body = Buffer.from([0x7b, 0xff, 0x7d]); // not UTF-8
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(null, /^invalid JSON response$/));
  body = Buffer.from("\ufeff{\"result\":1}"); // json.loads refuses the BOM raw.decode("utf-8") keeps
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(null, /^invalid JSON response$/));
});

test("anOverlongJsonIntegerIsInvalidJson", async () => {
  // tests/unit/test_urllib_transport.py::test_an_overlong_json_integer_is_invalid_json
  let reply: [number, string] = [200, `{"jsonrpc": "2.0", "id": 1, "result": ${"9".repeat(4301)}}`];
  const { url } = await serve((_req, res) => {
    res.writeHead(reply[0], { "Content-Type": "application/json" });
    res.end(reply[1]);
  });
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(null, /^invalid JSON response$/));
  reply = [400, `{"error": {"code": ${"9".repeat(4301)}, "message": "x"}}`];
  await assert.rejects(transport().postJson(url, {}, 5), isTransportError(400, /^HTTP 400$/));
  reply = [200, `{"result": ${"9".repeat(4300)}}`];
  assert.deepEqual(await transport().postJson(url, {}, 5), { result: Infinity }, "4300 digits are read");
});

test("networkFailuresAreTransportErrors", async () => {
  const { port } = await serve(() => undefined);
  const s = servers.pop()!;
  await new Promise<void>((r) => s.close(() => r()));
  await assert.rejects(transport().postJson(`http://127.0.0.1:${port}/`, {}, 5), isTransportError(null, /ECONNREFUSED/));
  await assert.rejects(transport().postJson("ftp://example.com/", {}, 5), isTransportError(null, /unknown url type/));
  await assert.rejects(transport().postJson("not a url", {}, 5), isTransportError(null));
  // the server hangs up mid-answer
  const cut = await serve((req, res) => {
    res.writeHead(200, { "Content-Length": "50" });
    res.write("{");
    setTimeout(() => req.socket.destroy(), 20);
  });
  await assert.rejects(transport().postJson(cut.url, {}, 5), isTransportError(null));
});

test("eachUrlGoesThroughItsSchemesModule", async () => {
  const used: { module: string; options: any }[] = [];
  const fake = (module: string) => ({
    request(options: any, cb: (res: any) => void) {
      used.push({ module, options });
      const handlers = new Map<string, (...a: any[]) => void>();
      const req = {
        on(event: string, fn: (...a: any[]) => void) { handlers.set(event, fn); return req; },
        setTimeout() { return req; },
        destroy() { return req; },
        end(_body: unknown) {
          const res = {
            statusCode: 200,
            on(event: string, fn: (...a: any[]) => void) {
              if (event === "data") queueMicrotask(() => fn(new TextEncoder().encode("{\"result\":1}")));
              if (event === "end") setTimeout(() => fn(), 0);
              return res;
            },
            setTimeout() { return res; },
            resume() { return res; },
          };
          queueMicrotask(() => cb(res));
          return req;
        },
      };
      return req;
    },
  });
  const t = new NodeHttpsTransport({ https: fake("https"), http: fake("http") });
  assert.deepEqual(await t.postJson("https://rpc.example:8443/v1/key?x=1", {}, 7), { result: 1 });
  assert.deepEqual(await t.postJson("http://[::1]:8545", {}, 7), { result: 1 });
  assert.deepEqual(used.map((u) => [u.module, u.options.hostname, u.options.port, u.options.path, u.options.method]), [
    ["https", "rpc.example", 8443, "/v1/key?x=1", "POST"],
    ["http", "::1", 8545, "/", "POST"],
  ]);
  const noHttp = new NodeHttpsTransport({ https: fake("https") });
  await assert.rejects(noHttp.postJson("http://127.0.0.1:8545", {}, 5), isTransportError(null));
  assert.throws(() => new NodeHttpsTransport({ https: {} as any }), TypeError, "a module without request() is refused at once");
});

// --- Node module quirks: a string body for Node 14, a module that throws --------------------

/** A fake http/https module: `end` checks its body as `endAccepts` says; `answer` is sent back. */
function fakeModule(opts: { endAccepts?: (body: unknown) => boolean; throwIn?: "request" | "on" | "setTimeout" | "end" } = {}) {
  const sent: { options: any; body: unknown }[] = [];
  const module = {
    request(options: any, cb: (res: any) => void) {
      if (opts.throwIn === "request") throw new TypeError("request refused");
      const entry = { options, body: undefined as unknown };
      sent.push(entry);
      const req = {
        on(_event: string, _fn: unknown) {
          if (opts.throwIn === "on") throw new TypeError("on refused");
          return req;
        },
        setTimeout() {
          if (opts.throwIn === "setTimeout") throw new TypeError("setTimeout refused");
          return req;
        },
        destroy() { return req; },
        end(body: unknown) {
          if (opts.throwIn === "end") throw new TypeError("end refused");
          if (opts.endAccepts && !opts.endAccepts(body)) {
            throw new TypeError('The "chunk" argument must be of type string or an instance of Buffer');
          }
          entry.body = body;
          const res = {
            statusCode: 200,
            on(event: string, fn: (...a: any[]) => void) {
              if (event === "data") queueMicrotask(() => fn(Buffer.from("{\"result\":\"ok\"}")));
              if (event === "end") setTimeout(() => fn(), 0);
              return res;
            },
          };
          queueMicrotask(() => cb(res));
          return req;
        },
      };
      return req;
    },
  };
  return { module, sent };
}

test("theBodyIsAStringForNode14", async () => {
  // Node 14 and older (early MZ releases' NW.js): end() takes a string or a Buffer only
  const { module, sent } = fakeModule({ endAccepts: (b) => typeof b === "string" || Buffer.isBuffer(b) });
  const t = new NodeHttpsTransport({ https: module });
  const payload = { method: "eth_call", params: ["é€"] };
  assert.deepEqual(await t.postJson("https://rpc.example/", payload, 5), { result: "ok" });
  assert.equal(typeof sent[0]!.body, "string");
  assert.equal(sent[0]!.body, JSON.stringify(payload));
  assert.equal(sent[0]!.options.headers["Content-Length"], String(Buffer.byteLength(JSON.stringify(payload))),
    "Content-Length is the UTF-8 size, not the string length");
});

test("aModuleThatThrowsIsATransportError", async () => {
  for (const where of ["request", "on", "setTimeout", "end"] as const) {
    const t = new NodeHttpsTransport({ https: fakeModule({ throwIn: where }).module });
    await assert.rejects(t.postJson("https://rpc.example/", {}, 5), isTransportError(null, /refused/), where);
  }
});

// --- the fetch fallback for NW.js's frozen CA bundle and OS proxies ---------------------------

/** A module whose request fails like Node does: an "error" with this `code` (or this HTTP status). */
function failingModule(failure: { code?: string; status?: number }) {
  let requests = 0;
  const module = {
    request(_options: any, cb: (res: any) => void) {
      requests++;
      const handlers = new Map<string, (...a: any[]) => void>();
      const req = {
        on(event: string, fn: (...a: any[]) => void) {
          handlers.set(event, fn);
          return req;
        },
        setTimeout() { return req; },
        destroy() { return req; },
        end() {
          if (failure.status !== undefined) {
            const res = {
              statusCode: failure.status,
              on(event: string, fn: (...a: any[]) => void) {
                if (event === "end") setTimeout(() => fn(), 0);
                return res;
              },
            };
            queueMicrotask(() => cb(res));
          } else {
            const e = Object.assign(new Error(`${failure.code}: simulated`), { code: failure.code });
            queueMicrotask(() => handlers.get("error")?.(e));
          }
          return req;
        },
      };
      return req;
    },
  };
  return { module, requests: () => requests };
}

/** The fallback (FetchTransport in the glue): records its calls, answers `answer` or throws it. */
function fallbackTransport(answer: unknown) {
  const calls: unknown[][] = [];
  return {
    calls,
    async postJson(url: string, payload: object, timeout: number) {
      calls.push([url, payload, timeout]);
      if (answer instanceof Error) throw answer;
      return answer;
    },
  };
}

test("aTlsTrustOrProxyFailureIsRetriedOnceThroughTheFallback", async () => {
  // NW.js's Node trusts only its own frozen CA bundle and ignores the OS proxy; Chromium's fetch
  // uses the OS's certificates and proxy settings: that request goes once more through it
  const codes = [
    "CERT_HAS_EXPIRED", "UNABLE_TO_GET_ISSUER_CERT", "UNABLE_TO_GET_ISSUER_CERT_LOCALLY", "UNABLE_TO_VERIFY_LEAF_SIGNATURE",
    "SELF_SIGNED_CERT_IN_CHAIN", "DEPTH_ZERO_SELF_SIGNED_CERT", "ERR_TLS_CERT_ALTNAME_INVALID", "CERT_UNTRUSTED", "CERT_NOT_YET_VALID",
    "ENOTFOUND", "EAI_AGAIN", "ECONNREFUSED", "ECONNRESET", "ETIMEDOUT", "EHOSTUNREACH", "ENETUNREACH",
  ];
  for (const code of codes) {
    const { module, requests } = failingModule({ code });
    const fallback = fallbackTransport({ result: "0x1" });
    const t = new NodeHttpsTransport({ https: module, fallback });
    const payload = { jsonrpc: "2.0", id: 1, method: "eth_blockNumber", params: [] };
    assert.deepEqual(await t.postJson("https://rpc.example/x", payload, 7), { result: "0x1" }, code);
    assert.deepEqual([requests(), fallback.calls], [1, [["https://rpc.example/x", payload, 7]]], `${code}: once through Node, once through fetch`);
  }
  // a proxy that asks for credentials (HTTP 407) is a proxy-class failure too
  const proxy = failingModule({ status: 407 });
  const fb = fallbackTransport({ result: "0x2" });
  assert.deepEqual(await new NodeHttpsTransport({ https: proxy.module, fallback: fb }).postJson("https://rpc.example/", {}, 5), { result: "0x2" });
  assert.equal(fb.calls.length, 1);
});

test("otherFailuresAreNotRetriedAndTheFallbacksOwnFailureIsTheAnswer", async () => {
  for (const failure of [{ code: "EPROTO" }, { code: "ERR_SOMETHING_ELSE" }, { status: 500 }, { status: 429 }]) {
    const fallback = fallbackTransport({ result: "never" });
    const t = new NodeHttpsTransport({ https: failingModule(failure).module, fallback });
    await assert.rejects(t.postJson("https://rpc.example/", {}, 5), (e: unknown) => e instanceof TransportError, JSON.stringify(failure));
    assert.equal(fallback.calls.length, 0, `${JSON.stringify(failure)}: not retried`);
  }
  // the fallback failing too: its failure (fetch's, also a TransportError) is the answer, once,
  // and its message keeps Node's original code (fetch's own says only "Failed to fetch")
  const down = new TransportError("Failed to fetch");
  const fallback = fallbackTransport(down);
  const t = new NodeHttpsTransport({ https: failingModule({ code: "CERT_HAS_EXPIRED" }).module, fallback });
  await assert.rejects(t.postJson("https://rpc.example/", {}, 5), (e: unknown) => e instanceof TransportError
    && e.message === "Failed to fetch (Node's https failed first: CERT_HAS_EXPIRED)" && (e as { code?: unknown }).code === undefined
    && (e as { cause?: unknown }).cause === down, "fetch's own error (and its stack) is the cause");
  assert.equal(fallback.calls.length, 1);
  // a proxy's 407 has no code: its message is kept instead; a fallback failure's own code stays
  const coded = Object.assign(new TransportError("connect ECONNREFUSED"), { code: "ECONNREFUSED" });
  const viaProxy = new NodeHttpsTransport({ https: failingModule({ status: 407 }).module, fallback: fallbackTransport(coded) });
  await assert.rejects(viaProxy.postJson("https://rpc.example/", {}, 5), (e: unknown) => e instanceof TransportError
    && e.message === "connect ECONNREFUSED (Node's https failed first: HTTP 407)" && (e as { code?: unknown }).code === "ECONNREFUSED");
  // what is not a TransportError (a bug in the fallback) is passed on as it is
  const bug = new TypeError("not a function");
  const buggy = new NodeHttpsTransport({ https: failingModule({ code: "ENOTFOUND" }).module, fallback: fallbackTransport(bug) });
  await assert.rejects(buggy.postJson("https://rpc.example/", {}, 5), (e: unknown) => e === bug);
  // without a fallback nothing changes: Node's failure as a TransportError, with its code
  const bare = new NodeHttpsTransport({ https: failingModule({ code: "CERT_HAS_EXPIRED" }).module });
  await assert.rejects(bare.postJson("https://rpc.example/", {}, 5), (e: unknown) => e instanceof TransportError && (e as { code?: unknown }).code === "CERT_HAS_EXPIRED");
});
