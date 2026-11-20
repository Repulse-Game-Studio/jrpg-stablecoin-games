/**
 * The desktop RPC transport (node_transport.ts) against local Node HTTP servers:
 * the twin of scfg_core.rpc.UrllibTransport's timeouts and error mapping. The Anvil run is
 * tests/integration/test_js_core.py::test_node_transport_against_anvil.
 */
import { afterEach, mock, test } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import https from "node:https";
import type { AddressInfo } from "node:net";
import { TransportError } from "../core/index.ts";
import { Abandoned, CancelSource } from "../core/coio.ts";
import { RpcClient, RpcUnavailable, CALL_BUDGET } from "../core/rpc.ts";
import { MIN_FALLBACK, NodeHttpsTransport } from "./node_transport.ts";

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
    // (a clock that stands still: the fallback is given all of the timeout, nothing being spent)
    const t = new NodeHttpsTransport({ https: module, fallback, clock: () => 0 });
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

// --- the response size cap (coio.ts MAX_RESPONSE_BYTES; UrllibTransport has none) ---

test("theCapDefaultsToTheCoresAndAnAnswerAtItIsRead", async () => {
  const body = JSON.stringify({ result: "é".repeat(50) });
  const size = Buffer.byteLength(body);
  const { url } = await serve((_req, res) => {
    res.write(body.slice(0, 7)); // chunked: counted as the bytes arrive
    res.end(body.slice(7));
  });
  assert.deepEqual(await transport().postJson(url, {}, 5), JSON.parse(body), "the default (16 MiB)");
  assert.deepEqual(await new NodeHttpsTransport({ https, http, max_response_bytes: size }).postJson(url, {}, 5), JSON.parse(body));
  await assert.rejects(new NodeHttpsTransport({ https, http, max_response_bytes: size - 1 }).postJson(url, {}, 5),
    isTransportError(null, /^response too large$/));
});

test("anEndlessAnswerIsCutOffAtTheCap", async () => {
  let sent = 0;
  let closed = false;
  const { url } = await serve((_req, res) => {
    const chunk = Buffer.alloc(16 * 1024, 0x20);
    const pump = (): void => {
      if (closed) return;
      sent += chunk.length;
      if (res.write(chunk)) setImmediate(pump);
      else res.once("drain", pump);
    };
    res.on("close", () => { closed = true; });
    pump();
  });
  const fallback = fallbackTransport({ never: true });
  const t = new NodeHttpsTransport({ https, http, max_response_bytes: 64 * 1024, fallback });
  await assert.rejects(within(t.postJson(url, {}, 5), 4000), isTransportError(null, /^response too large$/));
  assert.equal(fallback.calls.length, 0, "not a trust or proxy failure: no second request");
  for (let i = 0; i < 200 && !closed; i++) await new Promise((r) => setTimeout(r, 10));
  assert.ok(closed, "the request was destroyed");
  assert.ok(sent < 64 * 1024 * 1024, `the server stopped sending (${sent} bytes)`);
});

test("aDeclaredLengthOverTheCapIsRefusedBeforeTheBody", async () => {
  let closed = false;
  const { url } = await serve((_req, res) => {
    res.on("close", () => { closed = true; });
    res.writeHead(200, { "Content-Length": "1000000" });
    res.write("x"); // the rest never comes: only the header can refuse it
  });
  const t = new NodeHttpsTransport({ https, http, max_response_bytes: 999999 });
  await assert.rejects(within(t.postJson(url, {}, 5), 4000), isTransportError(null, /^response too large$/));
  for (let i = 0; i < 200 && !closed; i++) await new Promise((r) => setTimeout(r, 10));
  assert.ok(closed, "the request was destroyed");
});

test("anOversizedErrorAnswerIsTooLarge", async () => {
  const { url } = await serve((_req, res) => {
    res.writeHead(400);
    res.write('{"error":{"code":-32005,');
    res.end(`"message":"${"x".repeat(200)}"}}`);
  });
  await assert.rejects(new NodeHttpsTransport({ https, http, max_response_bytes: 100 }).postJson(url, {}, 5),
    isTransportError(null, /^response too large$/));
});

// A DNS server that never answers. Node's request timeout starts only once
// the socket connects, so the lookup needs a timer of its own (rpc.py's resolve_within).
test("aNameLookupThatNeverAnswersIsBoundedByTheTimeout", async () => {
  const stuck = {
    request() {
      const req = {
        on() { return req; },
        setTimeout() { return req; }, // Node's socket timer: never armed while the lookup hangs
        destroy() { return req; },
        end() { return req; },
      };
      return req;
    },
  };
  const t = new NodeHttpsTransport({ https: stuck, http: stuck });
  const started = Date.now();
  await assert.rejects(within(t.postJson("https://never-resolves.example/", {}, 0.2), 3000),
    (e: unknown) => e instanceof TransportError && /timed out resolving never-resolves.example/.test(e.message));
  const ms = Date.now() - started;
  assert.ok(ms >= 150 && ms < 2000, `at the timeout (${ms} ms)`);
});

/** Node's https whose name lookup never answers (no socket, no "error"). */
const stuckLookup = {
  request() {
    const req = {
      on() { return req; },
      setTimeout() { return req; }, // Node's socket timer: never armed while the lookup hangs
      destroy() { return req; },
      end() { return req; },
    };
    return req;
  },
};

/**
 * Runs `body` with setTimeout and Date mocked (no real wait, so no wall-clock slack to tune) and a
 * clock in seconds read from the mocked Date.
 */
async function onMockedTimers(body: (clock: () => number) => Promise<void>): Promise<void> {
  mock.timers.enable({ apis: ["setTimeout", "Date"], now: 1_000_000 });
  try {
    await body(() => Date.now() / 1000);
  } finally {
    mock.timers.reset();
  }
}

/** Inside onMockedTimers: `promise`, the mocked time moved on in 50 ms steps until it settles. */
async function settled<T>(promise: Promise<T>): Promise<T> {
  let done = false;
  const watched = promise.finally(() => { done = true; });
  watched.catch(() => undefined);
  for (let i = 0; i < 1000 && !done; i++) {
    await new Promise((r) => setImmediate(r));
    if (!done) mock.timers.tick(50);
  }
  return watched;
}

/** A fallback that never answers: fails "timed out" after its `timeout`, as FetchTransport's abort. Records [url, timeout]. */
function hangingFallback() {
  const calls: Array<[string, number]> = [];
  return {
    calls,
    postJson(url: string, _payload: object, timeout: number): Promise<unknown> {
      calls.push([url, timeout]);
      return new Promise((_r, reject) => setTimeout(() => reject(new TransportError("timed out")), timeout * 1000));
    },
  };
}

// Behind a proxy only the fallback (Chromium's fetch, which uses the OS proxy)
// reaches the RPC. A lookup Node could not finish in time is a DNS failure, retried through it.
// The retry shares the request's timeout: the lookup has half of it, the fallback what
// is left (not the whole timeout again).
test("aLookupThatTimesOutGoesOnceMoreThroughTheFallback", async () => {
  const asked: Array<[string, number]> = [];
  const fallback = {
    postJson(url: string, _payload: object, timeout: number) {
      asked.push([url, timeout]);
      return Promise.resolve({ jsonrpc: "2.0", id: 1, result: "0x1" });
    },
  };
  await onMockedTimers(async (clock) => {
    const t = new NodeHttpsTransport({ https: stuckLookup, http: stuckLookup, fallback, clock });
    const started = clock();
    const answer = await settled(t.postJson("https://behind-a-proxy.example/", {}, 1.0));
    assert.deepEqual(answer, { jsonrpc: "2.0", id: 1, result: "0x1" });
    assert.equal(clock() - started, 0.5, "the lookup had half of the timeout");
    assert.deepEqual(asked, [["https://behind-a-proxy.example/", 0.5]],
      "the fallback gets what is left of the timeout, not all of it");
  });
});

test("aLookupTimeoutAndItsFallbackStayWithinTheRequestsTimeout", async () => {
  // A DNS that never answers and a fallback that hangs too end together at `timeout`,
  // not at twice it.
  const fallback = hangingFallback();
  await onMockedTimers(async (clock) => {
    const t = new NodeHttpsTransport({ https: stuckLookup, http: stuckLookup, fallback, clock });
    const started = clock();
    await assert.rejects(settled(t.postJson("https://nowhere.example/", {}, 0.8)),
      (e: unknown) => e instanceof TransportError && /timed out/.test(e.message));
    const took = clock() - started;
    assert.equal(fallback.calls.length, 1, "the fallback was asked");
    assert.ok(Math.abs(fallback.calls[0]![1] - 0.4) < 1e-9, `with the half left (${fallback.calls[0]![1]} s)`);
    assert.ok(Math.abs(took - 0.8) < 1e-9, `the attempt took its timeout, not more (${took} s)`);
  });
});

test("theFallbackIsNotAskedWithLessThanTheFloorLeft", async () => {
  // half of 0.4 s is left after the lookup's 0.2 s: under MIN_FALLBACK, so the lookup's failure is the answer
  assert.equal(MIN_FALLBACK, 0.25);
  const fallback = hangingFallback();
  const t = new NodeHttpsTransport({ https: stuckLookup, http: stuckLookup, fallback });
  await assert.rejects(within(t.postJson("https://nowhere.example/", {}, 0.4), 3000),
    (e: unknown) => e instanceof TransportError && /timed out resolving nowhere.example/.test(e.message)
      && (e as TransportError & { code?: string }).code === "EAI_AGAIN");
  assert.deepEqual(fallback.calls, []);
  // a trust failure with too little left (the clock says 0.9 s of 1 s went by) is not retried either
  let now = 0;
  const late = fallbackTransport({ result: "never" });
  const failing = failingModule({ code: "CERT_HAS_EXPIRED" }).module;
  const lateT = new NodeHttpsTransport({ https: failing, fallback: late, clock: () => (now += 0.9) - 0.9 });
  await assert.rejects(lateT.postJson("https://rpc.example/", {}, 1.0),
    (e: unknown) => e instanceof TransportError && (e as TransportError & { code?: string }).code === "CERT_HAS_EXPIRED");
  assert.equal(late.calls.length, 0);
});

test("aCallOverFourEndpointsWhoseLookupsAndFallbackHangEndsWithinTheCallBudget", async () => {
  // Over the real RpcClient (CALL_BUDGET, MIN_ATTEMPT) and transport, on mocked timers:
  // A's attempt is 10 s (5 s lookup, 5 s fallback), B's the 5 s left (2.5 + 2.5); C and D are not
  // asked. Before, A's attempt alone took 20 s (a full timeout twice).
  await onMockedTimers(async (clock) => {
    const fallback = hangingFallback();
    const t = new NodeHttpsTransport({ https: stuckLookup, http: stuckLookup, fallback, clock });
    const urls = ["https://a.example/", "https://b.example/", "https://c.example/", "https://d.example/"];
    const rpc = new RpcClient(urls, { transport: t, clock });
    const started = clock();
    await assert.rejects(settled(rpc.blockNumber()), RpcUnavailable);
    const took = clock() - started;
    assert.ok(took <= CALL_BUDGET + 0.1, `the call ended within its ${CALL_BUDGET} s budget (${took} s)`);
    assert.deepEqual(fallback.calls, [[urls[0], 5], [urls[1], 2.5]]);
  });
});

// A request on a kept-alive socket (Node 19+ keeps them by default) never sees "connect",
// so the lookup timer must stop at once; else an answer streamed for longer than `timeout` in
// total, every read well inside it, failed as "timed out resolving".
test("aKeptAliveSocketStopsTheLookupTimer", async () => {
  let n = 0;
  const { url } = await serve((_req, res) => {
    n += 1;
    if (n === 1) {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ jsonrpc: "2.0", id: 1, result: "first" }));
      return;
    }
    // the second answer streams for 1.2 s in all, each part 0.3 s after the last (timeout 0.8 s)
    res.writeHead(200, { "Content-Type": "application/json" });
    const text = JSON.stringify({ jsonrpc: "2.0", id: 2, result: "second" });
    const parts = [text.slice(0, 5), text.slice(5, 10), text.slice(10, 15), text.slice(15)];
    let i = 0;
    const next = () => {
      res.write(parts[i]);
      i += 1;
      if (i < parts.length) setTimeout(next, 300);
      else res.end();
    };
    setTimeout(next, 300);
  });
  const agent = new http.Agent({ keepAlive: true, maxSockets: 1 });
  const keptAlive = { request: (o: Record<string, unknown>, cb: (res: any) => void) => http.request({ ...o, agent }, cb) };
  const t = new NodeHttpsTransport({ https: keptAlive, http: keptAlive });
  try {
    assert.deepEqual(await t.postJson(url, {}, 0.8), { jsonrpc: "2.0", id: 1, result: "first" });
    assert.deepEqual(await within(t.postJson(url, {}, 0.8), 5000), { jsonrpc: "2.0", id: 2, result: "second" });
  } finally {
    agent.destroy();
  }
});

// A request nobody waits for any more (the job was abandoned, or the tick yields to a command) is
// destroyed at once and fails with Abandoned, never retried through the fallback; one cancelled
// before it starts never reaches the server.
test("aCancelledRequestIsDestroyedAtOnceAndNeverRetried", async () => {
  let closed!: () => void;
  const serverSawClose = new Promise<void>((r) => { closed = r; });
  const { url, seen } = await serve((req) => {
    req.socket.on("close", () => closed()); // never answers
  });
  const asked: string[] = [];
  const fallback = {
    postJson(u: string) {
      asked.push(u);
      return Promise.resolve({ jsonrpc: "2.0", id: 1, result: "0x1" });
    },
  };
  const t = new NodeHttpsTransport({ https, http, fallback });
  const cancel = new CancelSource();
  const answer = t.postJson(url, { jsonrpc: "2.0", id: 1, method: "eth_blockNumber", params: [] }, 10, cancel);
  for (let i = 0; i < 100 && seen.length === 0; i++) await new Promise((r) => setTimeout(r, 10));
  assert.equal(seen.length, 1, "the request reached the server");
  const t0 = performance.now();
  cancel.cancel();
  const isCancelled = (e: unknown) => e instanceof Abandoned && e.message === `the request to ${url} was cancelled`;
  await assert.rejects(within(answer, 3000), isCancelled);
  assert.ok(performance.now() - t0 < 1000, "at once");
  await within(serverSawClose, 3000);
  assert.deepEqual(asked, [], "a cancelled request never goes through the fallback");
  await assert.rejects(within(t.postJson(url, {}, 10, cancel), 3000), isCancelled);
  assert.equal(seen.length, 1, "one cancelled already is never sent");
});
