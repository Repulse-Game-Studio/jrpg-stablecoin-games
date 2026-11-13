/**
 * The desktop checkout server (local_server.ts) over real sockets on Node: what the shared
 * security suite (tests/server_security, its `node` half) cannot reach from outside the process:
 * the per-connection deadline, /build held on a Job the test controls, the callbacks released by
 * stop(), the URL before listen() completes, the connection limit and the total head cap.
 * Routes, status codes and headers are the suite's.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import net from "node:net";
import { runInNewContext } from "node:vm";
import { BUILD_TIMEOUT_S, Job } from "../core/index.ts";
import {
  CheckoutServer, DEADLINE_S, MAX_CONNECTIONS, MAX_HEAD, MAX_LINE, isBytes, stripLineEnds, type ServerOptions,
} from "./local_server.ts";

const PLAYER = "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3";
const TX = "0x" + "ab".repeat(32);
const PNG = new Uint8Array([0x89, 0x50, 0x4e, 0x47]);
const ASSETS: Record<string, string | Uint8Array> = {
  "index.html": "<html>page</html>", "app.js": "// js", "evm.js": "// evm.js", "solana.js": "// solana.js",
};

let servers: CheckoutServer[] = [];
let sockets: net.Socket[] = [];

afterEach(() => {
  for (const s of sockets) s.destroy();
  for (const s of servers) s.stop();
  servers = [];
  sockets = [];
});

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

function make(opts: Partial<ServerOptions> = {}, calls: unknown[][] = []): CheckoutServer {
  const srv = new CheckoutServer({
    net,
    load_asset: (name: string) => {
      if (!(name in ASSETS)) throw new Error("no such asset");
      return ASSETS[name]!;
    },
    session_info: { chain_id: 42161, amount_units: "4993742" },
    on_tx: (tx: string, sender: string | null) => void calls.push(["on_tx", tx, sender]),
    get_status: () => ({ state: "waiting" }),
    ...opts,
  });
  servers.push(srv);
  return srv;
}

async function started(srv: CheckoutServer): Promise<CheckoutServer> {
  srv.start();
  await srv.ready();
  return srv;
}

const base = (srv: CheckoutServer) => `/c/${srv.secret}/`;
const host = (srv: CheckoutServer) => `Host: 127.0.0.1:${srv.port}\r\n`;

interface Reply { status: number; headers: http.IncomingHttpHeaders; body: string }

function request(srv: CheckoutServer, method: string, path: string, body?: string): Promise<Reply> {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: "127.0.0.1", port: srv.port, method, path, agent: false }, (res) => {
      const parts: Buffer[] = [];
      res.on("data", (b: Buffer) => parts.push(b));
      res.on("end", () => resolve({ status: res.statusCode!, headers: res.headers, body: Buffer.concat(parts).toString("utf8") }));
      res.on("error", reject);
    });
    req.on("error", reject);
    if (body !== undefined) req.setHeader("Content-Type", "application/json");
    req.end(body);
  });
}

/** Send `data` on a new connection and collect everything until the server closes (at most `limitMs`). */
function talk(port: number, data: string | Uint8Array, limitMs = 5000): Promise<{ out: Buffer; ms: number; open: net.Socket }> {
  return new Promise((resolve, reject) => {
    const t0 = performance.now();
    const s = net.connect(port, "127.0.0.1");
    sockets.push(s);
    const parts: Buffer[] = [];
    const limit = setTimeout(() => reject(new Error(`the server kept the connection open for ${limitMs} ms`)), limitMs);
    s.on("data", (b) => parts.push(b));
    s.on("error", () => undefined);
    s.on("close", () => {
      clearTimeout(limit);
      resolve({ out: Buffer.concat(parts), ms: performance.now() - t0, open: s });
    });
    s.write(data);
  });
}

/** A connection that sends `data` and stays open; `out` collects what the server sends. */
function hold(port: number, data = ""): Promise<{ s: net.Socket; out: Buffer[]; closed: { v: boolean } }> {
  return new Promise((resolve, reject) => {
    const s = net.connect(port, "127.0.0.1", () => {
      if (data) s.write(data);
      resolve({ s, out, closed });
    });
    sockets.push(s);
    const out: Buffer[] = [];
    const closed = { v: false };
    s.on("data", (b) => out.push(b));
    s.on("error", () => undefined);
    s.on("close", () => (closed.v = true));
    s.on("connect", () => undefined);
    s.once("error", reject);
  });
}

const statusOf = (out: Buffer) => Number(out.toString("latin1").split(" ")[1]);

// --- the URL, the secret and start() --------------------------------------------------------

test("urlIsEmptyUntilListening", async () => {
  const srv = make();
  assert.equal(srv.url, "", "nothing before start()");
  srv.start();
  assert.equal(srv.url, "", "start() returns before listen() completes: the URL is \"\" until then");
  assert.equal(srv.running, true, "running from start()");
  await srv.ready();
  assert.ok(srv.port > 0);
  assert.equal(srv.url, `http://127.0.0.1:${srv.port}/c/${srv.secret}/`);
  srv.start(); // a second start() while running does nothing
  assert.equal(srv.url, `http://127.0.0.1:${srv.port}/c/${srv.secret}/`);
  const r = await request(srv, "GET", base(srv));
  assert.equal(r.status, 200);
  assert.equal(r.body, "<html>page</html>");
});

test("secretIs16RandomBytesBase64url", () => {
  const filled: number[] = [];
  const srv = make({
    crypto: {
      getRandomValues(b: Uint8Array) {
        filled.push(b.length);
        for (let i = 0; i < b.length; i++) b[i] = 0xf8 + (i % 8); // 0xf8..0xff: "+" and "/" in base64
        return b;
      },
    },
  });
  assert.deepEqual(filled, [16], "one getRandomValues call for 16 bytes");
  assert.equal(srv.secret, Buffer.from(Array.from({ length: 16 }, (_, i) => 0xf8 + (i % 8))).toString("base64url"));
  assert.equal(srv.secret.length, 22);
  const other = make();
  assert.match(other.secret, /^[A-Za-z0-9_-]{22}$/);
  assert.notEqual(other.secret, make().secret, "a fresh secret per server (the page's crypto by default)");
});

test("aPortThatCannotBeBoundIsReported", async () => {
  const srv = make({ host: "203.0.113.1" }); // TEST-NET-3: not an address of this machine
  srv.start();
  await assert.rejects(srv.ready());
  assert.ok(srv.failed instanceof Error, "the listen error is kept");
  assert.equal(srv.running, false);
  assert.equal(srv.url, "");
});

test("stopBeforeListeningNeverListens", async () => {
  const srv = make();
  srv.start();
  srv.stop();
  await assert.rejects(srv.ready(), /stopped/);
  await sleep(50);
  assert.equal(srv.url, "", "the pending listen() was closed when it completed");
  assert.equal(srv.running, false);
});

// --- the deadline ---------------------------------------------------------------------------

test("slowClientIsDroppedAfterTheDeadline", async () => {
  assert.equal(DEADLINE_S, 10);
  const srv = make();
  assert.equal(srv.deadline_s, DEADLINE_S, "10 s per connection by default");
  srv.deadline_s = 0.3;
  await started(srv);
  // a head that never ends
  let got = await talk(srv.port, `GET ${base(srv)} HTTP/1.1\r\n${host(srv)}`);
  assert.equal(got.out.length, 0, "closed without an answer");
  assert.ok(got.ms >= 250 && got.ms < 3000, `dropped after the deadline (${got.ms} ms)`);
  // a body that never comes
  got = await talk(srv.port, `POST ${base(srv)}tx HTTP/1.1\r\n${host(srv)}Content-Length: 80\r\n\r\n{"tx_`);
  assert.equal(got.out.length, 0);
  assert.ok(got.ms >= 250 && got.ms < 3000, `dropped after the deadline (${got.ms} ms)`);
  // a client that sends nothing at all
  got = await talk(srv.port, "");
  assert.equal(got.out.length, 0);
  // the deadline is for the whole request, not per byte: trickling does not extend it
  const t = await hold(srv.port, `GET ${base(srv)} HTTP/1.1\r\n`);
  for (let i = 0; i < 8 && !t.closed.v; i++) {
    await sleep(60);
    if (!t.closed.v) t.s.write("X-A: b\r\n");
  }
  await sleep(200);
  assert.ok(t.closed.v, "a trickling client is dropped at the same deadline");
  assert.equal(Buffer.concat(t.out).length, 0);
  // others are still served
  assert.equal((await request(srv, "GET", base(srv) + "status")).status, 200);
});

// --- /build ---------------------------------------------------------------------------------

test("buildWaitsForTheJob", async () => {
  assert.equal(BUILD_TIMEOUT_S, 15);
  const calls: unknown[][] = [];
  const jobs: Job<any>[] = [];
  const srv = make({
    build_tx: (account: string) => {
      calls.push(["build_tx", account]);
      const job = new Job<any>();
      jobs.push(job);
      return job;
    },
  }, calls);
  assert.equal(srv.build_timeout_s, BUILD_TIMEOUT_S);
  await started(srv);
  const body = JSON.stringify({ account: PLAYER });
  let answered = false;
  const pending = request(srv, "POST", base(srv) + "build", body);
  void pending.then(() => (answered = true));
  await sleep(250);
  assert.equal(answered, false, "the answer waits for the job");
  assert.deepEqual(calls, [["build_tx", PLAYER]]);
  assert.equal((await request(srv, "GET", base(srv) + "status")).status, 200, "other requests are served meanwhile");
  jobs[0]!._start();
  jobs[0]!._succeed({ transaction: "AA==", token_balance: "1" });
  const r = await pending;
  assert.equal(r.status, 200);
  assert.equal(r.headers["content-type"], "application/json");
  assert.deepEqual(JSON.parse(r.body), { transaction: "AA==", token_balance: "1" });
  assert.equal(jobs[0]!.cancelled(), false);

  // BUILD_TIMEOUT_S: 503 and the job is cancelled
  srv.build_timeout_s = 0.15;
  const t0 = performance.now();
  const late = await request(srv, "POST", base(srv) + "build", body);
  assert.equal(late.status, 503);
  assert.deepEqual(JSON.parse(late.body), { error: "unavailable" });
  assert.ok(performance.now() - t0 >= 120, "after build_timeout_s");
  assert.equal(jobs[1]!.cancelled(), true, "don't build it later for a page that stopped waiting");

  // a failed job, or one whose value is not an object, is a 503
  srv.build_timeout_s = 5;
  const failing = request(srv, "POST", base(srv) + "build", body);
  await sleep(50);
  jobs[2]!._fail(new Error("rpc down"));
  assert.equal((await failing).status, 503);
  const odd = request(srv, "POST", base(srv) + "build", body);
  await sleep(50);
  jobs[3]!._succeed(["not", "an", "object"]);
  assert.equal((await odd).status, 503);

  // stop() while a page waits cancels its job
  const held = hold(srv.port, `POST ${base(srv)}build HTTP/1.1\r\n${host(srv)}Content-Length: ${body.length}\r\n\r\n${body}`);
  const h = await held;
  for (let i = 0; i < 50 && jobs.length < 5; i++) await sleep(10);
  assert.equal(jobs.length, 5);
  srv.stop();
  assert.equal(jobs[4]!.cancelled(), true, "stop() cancels the build a page still waits for");
  await sleep(50);
  assert.ok(h.closed.v, "and closes its connection");
});

test("buildTxThatThrowsOrReturnsNoJobIs503", async () => {
  const srv = make({ build_tx: () => { throw new Error("submit failed"); } });
  await started(srv);
  assert.equal((await request(srv, "POST", base(srv) + "build", JSON.stringify({ account: PLAYER }))).status, 503);
  srv.build_tx = (() => ({ not: "a job" })) as any;
  assert.equal((await request(srv, "POST", base(srv) + "build", JSON.stringify({ account: PLAYER }))).status, 503);
});

// --- the callbacks --------------------------------------------------------------------------

test("callbacksReleasedAfterStop", async () => {
  const srv = make({ load_logo: () => [PNG, "image/png"], build_tx: () => new Job() });
  await started(srv);
  srv.stop();
  for (const name of ["load_asset", "session_info", "on_tx", "get_status", "load_logo", "build_tx"] as const) {
    assert.equal(srv[name], null, `${name} is released: a stopped page keeps no controller alive`);
  }
  assert.equal(srv.running, false);
  srv.stop(); // a second stop() does nothing
  assert.throws(() => srv.start(), /stopped/, "a stopped server does not start again (its callbacks are gone)");
});

test("callbacksAreCheckedBeforeUse", async () => {
  const calls: unknown[][] = [];
  const srv = make({ load_logo: () => [PNG, "image/png"], build_tx: () => new Job() }, calls);
  await started(srv);
  srv.load_asset = null;
  srv.get_status = null;
  srv.on_tx = null;
  srv.load_logo = null;
  srv.build_tx = null;
  const b = base(srv);
  assert.equal((await request(srv, "GET", b)).status, 500, "an asset without load_asset is a 500");
  assert.equal((await request(srv, "GET", b + "status")).status, 503);
  assert.equal((await request(srv, "GET", b + "logo")).status, 404);
  assert.equal((await request(srv, "POST", b + "tx", JSON.stringify({ tx_hash: TX }))).status, 503);
  assert.equal((await request(srv, "POST", b + "build", JSON.stringify({ account: PLAYER }))).status, 404,
    "no build_tx: no /build route");
  // callbacks that throw
  srv.load_asset = () => { throw new Error("gone"); };
  srv.get_status = () => { throw new Error("gone"); };
  srv.on_tx = () => { throw new Error("gone"); };
  srv.load_logo = () => { throw new Error("gone"); };
  assert.equal((await request(srv, "GET", b + "app.js")).status, 500);
  assert.equal((await request(srv, "GET", b + "status")).status, 503);
  assert.equal((await request(srv, "POST", b + "tx", JSON.stringify({ tx_hash: TX }))).status, 503);
  assert.equal((await request(srv, "GET", b + "logo")).status, 404);
  // a logo that is not a raster image, or not bytes, is never served
  srv.load_logo = () => [new TextEncoder().encode("<svg/>"), "image/svg+xml"];
  assert.equal((await request(srv, "GET", b + "logo")).status, 404);
  srv.load_logo = (() => ["text", "image/png"]) as any; // not bytes
  assert.equal((await request(srv, "GET", b + "logo")).status, 404);
  srv.load_logo = () => [PNG, "image/webp"];
  const logo = await request(srv, "GET", b + "logo");
  assert.equal(logo.status, 200);
  assert.equal(logo.headers["content-type"], "image/webp");
  assert.equal(logo.headers["x-content-type-options"], "nosniff");
  // a status that cannot be sent as JSON
  srv.get_status = () => ({ n: 1n });
  assert.equal((await request(srv, "GET", b + "status")).status, 503);
  assert.deepEqual(calls, []);
});

test("assetsMayBeTextOrBytes", async () => {
  const srv = make({
    load_asset: (name: string) => (name === "app.js" ? new TextEncoder().encode("// bytes é") : "<p>é</p>"),
  });
  await started(srv);
  let r = await request(srv, "GET", base(srv) + "app.js");
  assert.equal(r.body, "// bytes é");
  assert.equal(r.headers["content-type"], "application/javascript; charset=utf-8");
  r = await request(srv, "GET", base(srv));
  assert.equal(r.body, "<p>é</p>", "text is sent as UTF-8");
  assert.equal(r.headers["content-length"], String(Buffer.byteLength("<p>é</p>")));
});

test("onTxGetsTheNormalisedHintFromTheEventLoop", async () => {
  const calls: unknown[][] = [];
  const srv = make({}, calls);
  await started(srv);
  const r = await request(srv, "POST", base(srv) + "tx", JSON.stringify({ tx_hash: TX.toUpperCase().replace("0X", "0x"), from: "0x52908400098527886e0f7030069857d2e4169ee7" }));
  assert.equal(r.status, 202);
  assert.deepEqual(JSON.parse(r.body), { ok: true });
  assert.deepEqual(calls, [["on_tx", TX, "0x52908400098527886E0F7030069857D2E4169EE7"]]);
  // on_tx may stop the server (the controller closes): the request still ends cleanly
  srv.on_tx = () => srv.stop();
  const got = await talk(srv.port, `POST ${base(srv)}tx HTTP/1.1\r\n${host(srv)}Content-Length: ${JSON.stringify({ tx_hash: TX }).length}\r\n\r\n${JSON.stringify({ tx_hash: TX })}`);
  assert.equal(got.out.length, 0, "a server stopped by on_tx answers nothing more");
  assert.equal(srv.running, false);
});

// --- limits ---------------------------------------------------------------------------------

test("atMost32ConnectionsAreServedAtOnce", async () => {
  assert.equal(MAX_CONNECTIONS, 32);
  const srv = make();
  await started(srv);
  const idle = [];
  for (let i = 0; i < MAX_CONNECTIONS; i++) idle.push(await hold(srv.port));
  await sleep(50);
  const waiting = await hold(srv.port, `GET ${base(srv)}status HTTP/1.1\r\n${host(srv)}\r\n`);
  await sleep(250);
  assert.equal(Buffer.concat(waiting.out).length, 0, "the 33rd connection waits while 32 are open");
  idle[0]!.s.destroy();
  for (let i = 0; i < 100 && !waiting.closed.v; i++) await sleep(10);
  assert.equal(statusOf(Buffer.concat(waiting.out)), 200, "and is served once one closes");
});

test("aWaitingConnectionGetsItsDeadlineOnlyWhenServed", async () => {
  const srv = make({ build_tx: () => new Job() }); // jobs that never finish
  srv.deadline_s = 0.5;
  srv.build_timeout_s = 0.6;
  await started(srv);
  const body = JSON.stringify({ account: PLAYER });
  const builds = [];
  for (let i = 0; i < MAX_CONNECTIONS; i++) {
    builds.push(await hold(srv.port, `POST ${base(srv)}build HTTP/1.1\r\n${host(srv)}Content-Length: ${body.length}\r\n\r\n${body}`));
  }
  await sleep(50);
  const waiting = await hold(srv.port); // connected now, sends its request only later
  await sleep(750); // the builds time out (503) after 0.6 s: the waiting one is served from then on
  assert.ok(builds.every((b) => statusOf(Buffer.concat(b.out)) === 503));
  assert.equal(waiting.closed.v, false, "its 0.5 s started when it was served, not when it connected");
  waiting.s.write(`GET ${base(srv)}status HTTP/1.1\r\n${host(srv)}\r\n`);
  for (let i = 0; i < 100 && !waiting.closed.v; i++) await sleep(10);
  assert.equal(statusOf(Buffer.concat(waiting.out)), 200);
});

test("headAboveTheTotalCapIs431", async () => {
  assert.equal(MAX_HEAD, 262144);
  const srv = make();
  await started(srv);
  const start = `GET ${base(srv)} HTTP/1.1\r\n${host(srv)}`;
  const line = (n: number, len: number) => `X-${n}: ` + "a".repeat(len - `X-${n}: `.length - 2) + "\r\n";
  // four full-length lines plus the rest: exactly MAX_HEAD + 1 bytes with the blank line is still read
  let head = start + [0, 1, 2].map((n) => line(n, MAX_LINE)).join("");
  const rest = MAX_HEAD + 1 - head.length - 2;
  let r = await talk(srv.port, head + line(3, rest) + "\r\n");
  assert.equal(statusOf(r.out), 200);
  head = start + [0, 1, 2].map((n) => line(n, MAX_LINE)).join("") + line(3, rest + 1);
  r = await talk(srv.port, head + "\r\n");
  assert.equal(statusOf(r.out), 431, "one byte more is a 431");
  assert.match(r.out.toString("latin1"), /headers too large$/);
});

// --- linear-time parsing, bytes from another realm, errors after listen() -------------------

test("runsOfLineEndsAndSpacesAreParsedInLinearTime", async () => {
  // bytes a client sends before any check (Host, secret) must never meet a backtracking pattern:
  // an unanchored `[\r\n]+$` took seconds on a 64 KiB CR run, on the game's main thread
  assert.equal(stripLineEnds("GET /\r\n\r\r"), "GET /");
  assert.equal(stripLineEnds("a\rb\n"), "a\rb");
  assert.equal(stripLineEnds(""), "");
  const srv = make();
  await started(srv);
  const run = 65000;
  const attacks: [string, string, number][] = [
    ["a CR run in the request line", "GET " + "\r".repeat(run) + " x\r\n\r\n", 400],
    ["a CR run ending a header line", `GET ${base(srv)} HTTP/1.1\r\n${host(srv)}X-A: a` + "\r".repeat(run) + "\n\r\n", 200],
    ["a space run inside a Content-Length", `POST ${base(srv)}tx HTTP/1.1\r\n${host(srv)}Content-Length: 1` + " ".repeat(run) + "2\r\n\r\n", 400],
    // int("1   ...") is 1: the one-byte body that follows is not a JSON object
    ["a space run after a Content-Length", `POST ${base(srv)}tx HTTP/1.1\r\n${host(srv)}Content-Length: 1` + " ".repeat(run) + "\r\n\r\nx", 400],
    ["a CRLF run of continuation lines", `GET ${base(srv)} HTTP/1.1\r\n${host(srv)}X-A: a\r\n` + " \r\n".repeat(90) + "\r\n", 200],
    ["more continuation lines than headers allowed", `GET ${base(srv)} HTTP/1.1\r\n${host(srv)}X-A: a\r\n` + " \r\n".repeat(20000) + "\r\n", 431],
  ];
  // An absolute 1 s bound: backtracking on a 64 KiB run takes seconds, a linear scan under a
  // millisecond; a ratio of two small timings would be scheduler noise on a loaded CI runner.
  for (const [what, data, status] of attacks) {
    const t0 = performance.now();
    const got = await talk(srv.port, data);
    const ms = performance.now() - t0;
    assert.equal(statusOf(got.out), status, what);
    assert.ok(ms < 1000, `${what}: answered in ${ms.toFixed(0)} ms`);
  }
});

test("bytesFromAnotherRealmAreServed", async () => {
  // NW.js's Node may run in its own context: its Buffers are not the page's Uint8Arrays
  const foreign = runInNewContext("new Uint8Array([104, 105])") as Uint8Array;
  assert.equal(foreign instanceof Uint8Array, false, "the premise: instanceof fails across realms");
  assert.equal(isBytes(foreign), true);
  assert.equal(isBytes(Buffer.from("x")), true, "a Buffer is bytes");
  assert.equal(isBytes(new Uint16Array(2)), false);
  assert.equal(isBytes([104, 105]), false);
  assert.equal(isBytes("hi"), false);
  const logo = runInNewContext("new Uint8Array([0x89, 0x50, 0x4e, 0x47])") as Uint8Array;
  const srv = make({ load_asset: () => foreign, load_logo: () => [logo, "image/png"] });
  await started(srv);
  const page = await request(srv, "GET", base(srv) + "app.js");
  assert.deepEqual([page.status, page.body], [200, "hi"]);
  const served = await request(srv, "GET", base(srv) + "logo");
  assert.deepEqual([served.status, served.headers["content-type"]], [200, "image/png"]);
});

test("anErrorAfterListeningIsReportedAndTheServerKeepsServing", async () => {
  const reported: unknown[] = [];
  const srv = make({ on_error: (e: unknown) => void reported.push(e) });
  await started(srv);
  const url = srv.url;
  const accept = Object.assign(new Error("accept EMFILE"), { code: "EMFILE" });
  (srv as any)._server.emit("error", accept); // as Node does when an accept fails
  assert.deepEqual(reported, [accept], "reported");
  assert.equal(srv.failed, null, "not a start failure");
  assert.equal(srv.running, true);
  assert.equal(srv.url, url, "the URL stays");
  assert.equal((await request(srv, "GET", base(srv) + "status")).status, 200, "still serving");
  srv.on_error = () => { throw new Error("a broken reporter"); };
  (srv as any)._server.emit("error", accept); // never reaches Node's event loop
  assert.equal((await request(srv, "GET", base(srv) + "status")).status, 200);
  srv.stop();
  assert.equal(srv.on_error, null, "released with the other callbacks");
});
