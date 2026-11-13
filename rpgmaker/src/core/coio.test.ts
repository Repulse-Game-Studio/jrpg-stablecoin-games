import { test } from "node:test";
import assert from "node:assert/strict";
import * as http from "node:http";
import type { AddressInfo } from "node:net";
import { FetchTransport, MAX_RESPONSE_BYTES, TransportError } from "./coio.ts";
import { jsonLoads } from "./pystr.ts";

type Handler = (body: string, res: http.ServerResponse) => void;

async function withServer(handler: Handler, fn: (url: string) => Promise<void>): Promise<void> {
  const server = http.createServer((req, res) => {
    let body = "";
    req.on("data", (d) => (body += d));
    req.on("end", () => {
      res.setHeader("x-content-type", String(req.headers["content-type"]));
      handler(body, res);
    });
  });
  await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
  try {
    await fn(`http://127.0.0.1:${(server.address() as AddressInfo).port}/`);
  } finally {
    server.closeAllConnections();
    await new Promise((r) => server.close(r));
  }
}

test("posts JSON and parses the answer", async () => {
  let seen = "";
  let ctype = "";
  await withServer((body, res) => {
    seen = body;
    ctype = String(res.getHeader("x-content-type"));
    res.end('{"jsonrpc":"2.0","id":1,"result":"0x1"}');
  }, async (url) => {
    const out = await new FetchTransport().postJson(url, { method: "eth_x", params: [1] }, 5);
    assert.deepEqual(out, { jsonrpc: "2.0", id: 1, result: "0x1" });
    assert.deepEqual(JSON.parse(seen), { method: "eth_x", params: [1] });
    assert.equal(ctype, "application/json");
  });
});

test("a non-2xx JSON-RPC error body is returned as-is", async () => {
  await withServer((_b, res) => {
    res.statusCode = 400;
    res.end('{"error":{"code":-32005,"message":"range too large"}}');
  }, async (url) => {
    const out = await new FetchTransport().postJson(url, {}, 5);
    assert.deepEqual(out, { error: { code: -32005, message: "range too large" } });
  });
});

test("any other non-2xx answer is TransportError('HTTP <code>', code)", async () => {
  for (const body of ["oops", '{"result":1}', '{"error":null}', '{"error":{}}', "[1]", ""]) {
    await withServer((_b, res) => {
      res.statusCode = 503;
      res.end(body);
    }, async (url) => {
      await assert.rejects(new FetchTransport().postJson(url, {}, 5), (e: unknown) => {
        assert.ok(e instanceof TransportError);
        assert.equal(e.message, "HTTP 503");
        assert.equal(e.status, 503);
        return true;
      });
    });
  }
});

test("an unparsable 2xx body is 'invalid JSON response'", async () => {
  await withServer((_b, res) => res.end("<html>"), async (url) => {
    await assert.rejects(new FetchTransport().postJson(url, {}, 5), (e: unknown) => {
      assert.ok(e instanceof TransportError);
      assert.equal(e.message, "invalid JSON response");
      assert.equal(e.status, null);
      return true;
    });
  });
});

test("a connection failure is a TransportError", async () => {
  await assert.rejects(new FetchTransport().postJson("http://127.0.0.1:1/", {}, 5), (e: unknown) => {
    assert.ok(e instanceof TransportError);
    assert.equal(e.status, null);
    return true;
  });
});

test("a timeout aborts the request as a TransportError", async () => {
  await withServer(() => { /* never answers */ }, async (url) => {
    const t0 = Date.now();
    await assert.rejects(new FetchTransport().postJson(url, {}, 0.1), (e: unknown) => {
      assert.ok(e instanceof TransportError);
      assert.equal(e.status, null);
      return true;
    });
    assert.ok(Date.now() - t0 < 3000);
  });
});

test("TransportError defaults status to null", () => {
  assert.equal(new TransportError("x").status, null);
  assert.equal(new TransportError("x", 500).status, 500);
});

// tests/unit/test_coio.py::test_an_overlong_json_integer_is_invalid_json: Python reads
// JSON integer literals with ints.int_from, so one of more than 4300 digits is not JSON. JSON.parse
// would read it as a float (Infinity); jsonLoads refuses it as Python does.
const LONG = "9".repeat(4301);

test("an overlong JSON integer is 'invalid JSON response'", async () => {
  let reply: [number, string] = [200, `{"result": ${LONG}}`];
  await withServer((_b, res) => {
    res.statusCode = reply[0];
    res.end(reply[1]);
  }, async (url) => {
    await assert.rejects(new FetchTransport().postJson(url, {}, 5), (e: unknown) => {
      assert.ok(e instanceof TransportError);
      assert.equal(e.message, "invalid JSON response");
      return true;
    });
    reply = [400, `{"error": {"code": ${LONG}, "message": "x"}}`];
    await assert.rejects(new FetchTransport().postJson(url, {}, 5), (e: unknown) => {
      assert.ok(e instanceof TransportError);
      assert.equal(e.status, 400);
      return true;
    });
  });
});

test("jsonLoads refuses integer literals of more than 4300 digits, as Python's json with int_from", () => {
  for (const text of [`{"result": ${LONG}}`, `[1, -${LONG}]`, `${LONG}`, `{"a": "x\\", "b": ${LONG}}`]) {
    assert.throws(() => jsonLoads(text), SyntaxError, text.slice(0, 30));
  }
  assert.deepEqual(jsonLoads(`{"r": ${"9".repeat(4300)}}`), { r: Infinity }, "4300 digits are read (as a float)");
  assert.deepEqual(jsonLoads(`[-${"9".repeat(4300)}]`), [-Infinity]);
  // only integer literals count: strings (escaped quotes included), fractions and exponents do not
  assert.deepEqual(jsonLoads(`{"s": "${LONG}", "t": "\\"${LONG}", "f": 1.${LONG}, "e": 1e-${LONG}, "g": 1E+${LONG}}`),
    { s: LONG, t: `"${LONG}`, f: 2, e: 0, g: Infinity }); // 1.999... rounds to 2.0, in Python too
  assert.throws(() => jsonLoads("not json"), SyntaxError);
  assert.deepEqual(jsonLoads('{"a": [1, 2.5, -3e2, "4"]}'), { a: [1, 2.5, -300, "4"] });
});

// --- the response size cap (no Python twin: UrllibTransport reads without one) ---

const tooLarge = (e: unknown): boolean => {
  assert.ok(e instanceof TransportError);
  assert.equal(e.message, "response too large");
  assert.equal(e.status, null);
  return true;
};

test("the cap is 16 MiB by default", () => {
  assert.equal(MAX_RESPONSE_BYTES, 16 * 1024 * 1024);
  assert.equal(new FetchTransport().maxResponseBytes, MAX_RESPONSE_BYTES);
});

test("an answer at the cap is read, one byte more is 'response too large'", async () => {
  const body = JSON.stringify({ result: "x".repeat(100) });
  const size = Buffer.byteLength(body);
  // chunked (no Content-Length), so the bytes are counted as they arrive
  await withServer((_b, res) => {
    res.write(body.slice(0, 10));
    res.end(body.slice(10));
  }, async (url) => {
    assert.deepEqual(await new FetchTransport(size).postJson(url, {}, 5), JSON.parse(body));
    await assert.rejects(new FetchTransport(size - 1).postJson(url, {}, 5), tooLarge);
  });
});

test("a multi-byte answer is decoded across chunks and counted in bytes", async () => {
  const body = JSON.stringify({ result: "é€😀".repeat(20) });
  const bytes = Buffer.from(body, "utf8");
  await withServer((_b, res) => {
    // one byte at a time: every character is split
    for (const b of bytes) res.write(Buffer.from([b]));
    res.end();
  }, async (url) => {
    assert.deepEqual(await new FetchTransport(bytes.length).postJson(url, {}, 5), JSON.parse(body));
    assert.ok(body.length < bytes.length - 1);
    await assert.rejects(new FetchTransport(bytes.length - 1).postJson(url, {}, 5), tooLarge);
  });
});

test("an endless answer is cut off at the cap, not buffered", async () => {
  let sent = 0;
  let closed = false;
  await withServer((_b, res) => {
    const chunk = Buffer.alloc(16 * 1024, 0x20);
    const pump = (): void => {
      if (closed) return;
      sent += chunk.length;
      if (res.write(chunk)) setImmediate(pump);
      else res.once("drain", pump);
    };
    res.on("close", () => { closed = true; });
    pump();
  }, async (url) => {
    await assert.rejects(new FetchTransport(64 * 1024).postJson(url, {}, 5), tooLarge);
    for (let i = 0; i < 200 && !closed; i++) await new Promise((r) => setTimeout(r, 10));
    assert.ok(closed, "the request was aborted");
    assert.ok(sent < 64 * 1024 * 1024, `the server stopped sending (${sent} bytes)`);
  });
});

test("a declared Content-Length over the cap is refused before the body is read", async () => {
  await withServer((_b, res) => {
    res.setHeader("Content-Length", "1000");
    res.end("x".repeat(1000));
  }, async (url) => {
    await assert.rejects(new FetchTransport(999).postJson(url, {}, 5), tooLarge);
  });
});

test("an oversized error answer is 'response too large' too", async () => {
  await withServer((_b, res) => {
    res.statusCode = 400;
    res.write('{"error":{"code":-32005,');
    res.end(`"message":"${"x".repeat(200)}"}}`);
  }, async (url) => {
    await assert.rejects(new FetchTransport(100).postJson(url, {}, 5), tooLarge);
  });
});

test("without a streaming body the text is measured after it is read", async () => {
  const g = globalThis as unknown as { fetch: unknown };
  const real = g.fetch;
  let aborted = false;
  g.fetch = async (_url: string, init: { signal: AbortSignal }) => {
    init.signal.addEventListener("abort", () => { aborted = true; });
    return { ok: true, status: 200, text: async () => '{"result":"0123456789"}' };
  };
  try {
    assert.deepEqual(await new FetchTransport(23).postJson("http://x/", {}, 5), { result: "0123456789" });
    assert.equal(aborted, false);
    await assert.rejects(new FetchTransport(22).postJson("http://x/", {}, 5), tooLarge);
    assert.equal(aborted, true);
  } finally {
    g.fetch = real;
  }
});
