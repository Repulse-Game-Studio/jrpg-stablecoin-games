import { test } from "node:test";
import assert from "node:assert/strict";
import type { Handler } from "../../test/rpc_fakes.ts";
import { FakeClock, FakeTransport, down, err, ok } from "../../test/rpc_fakes.ts";
import type { Transport } from "./coio.ts";
import { TransportError } from "./coio.ts";
import { KeyError, ValueError } from "./errors.ts";
import { CALL_BUDGET, COOLDOWN_MAX, COOLDOWN_START, pyHexInt, MIN_ATTEMPT, MIN_LOG_RANGE, RpcClient, RpcError, RpcUnavailable, isRangeError, suggestedRange } from "./rpc.ts";

const A = "https://a.rpc";
const B = "https://b.rpc";

function client(handlers: Record<string, Handler>, kw: { log_range?: number } = {}) {
  const clock = new FakeClock();
  const transport = new FakeTransport(handlers);
  const c = new RpcClient(Object.keys(handlers), { transport, clock: clock.call, ...kw });
  return { c, t: transport, clock };
}
const urls = (t: FakeTransport) => t.calls.map((x) => x[0]);

test("test_requires_urls", () => {
  assert.throws(() => new RpcClient([], { transport: new FakeTransport({}) }), ValueError);
});

test("test_call_sends_jsonrpc_and_returns_result", async () => {
  const { c, t } = client({ [A]: () => ok("0x10") });
  assert.equal(await c.call("eth_blockNumber", []), "0x10");
  assert.deepEqual(t.calls, [[A, "eth_blockNumber", []]]);
  assert.deepEqual(t.payloads[0], { jsonrpc: "2.0", id: 1, method: "eth_blockNumber", params: [] });
});

test("test_jsonrpc_ids_count_from_one_per_client_across_endpoints", async () => {
  const { c, t } = client({ [A]: down, [B]: () => ok("0x1") });
  await c.blockNumber();
  await c.blockNumber();
  assert.deepEqual(t.payloads.map((p) => p.id), [1, 2, 3]);
});

test("test_block_number_parses_hex", async () => {
  const { c } = client({ [A]: () => ok("0x1b4") });
  assert.equal(await c.blockNumber(), 436);
});

test("test_falls_back_to_next_endpoint_on_transport_error", async () => {
  const { c, t } = client({ [A]: down, [B]: () => ok("0x1") });
  assert.equal(await c.blockNumber(), 1);
  assert.deepEqual(urls(t), [A, B]);
});

test("test_failed_endpoint_cools_down_then_is_retried", async () => {
  let aUp = false;
  const a: Handler = () => {
    if (!aUp) throw new TransportError("HTTP 503", 503);
    return ok("0xa");
  };
  const { c, t, clock } = client({ [A]: a, [B]: () => ok("0xb") });
  assert.equal(await c.blockNumber(), 0xb);
  t.calls.length = 0;
  assert.equal(await c.blockNumber(), 0xb);
  assert.deepEqual(urls(t), [B]);
  aUp = true;
  clock.advance(COOLDOWN_START + 0.1);
  assert.equal(await c.blockNumber(), 0xa);
});

test("test_cooldown_doubles_and_caps", async () => {
  const { c, clock } = client({ [A]: down });
  const delays: number[] = [];
  for (let i = 0; i < 8; i++) {
    await assert.rejects(c.call("eth_blockNumber", []), RpcUnavailable);
    const st = (c as any)._state.get(A);
    delays.push(st.delay);
    clock.advance(st.delay + 0.1);
  }
  assert.deepEqual(delays.slice(0, 3), [COOLDOWN_START, COOLDOWN_START * 2, COOLDOWN_START * 4]);
  assert.equal(Math.max(...delays), COOLDOWN_MAX);
});

test("test_cooldown_boundary_is_until_le_now", async () => {
  const { c, clock } = client({ [A]: down });
  await assert.rejects(c.call("m", []), RpcUnavailable);
  assert.deepEqual(c.liveUrls(), []);
  clock.advance(COOLDOWN_START - 0.001);
  assert.deepEqual(c.liveUrls(), []);
  clock.advance(0.001);
  assert.deepEqual(c.liveUrls(), [A]);
});

test("test_success_resets_cooldown", async () => {
  let fail = true;
  const a: Handler = () => { if (fail) throw new TransportError("x"); return ok("0x1"); };
  const { c, clock } = client({ [A]: a });
  await assert.rejects(c.call("eth_blockNumber", []), RpcUnavailable);
  fail = false;
  clock.advance(COOLDOWN_START + 1);
  await c.call("eth_blockNumber", []);
  assert.equal((c as any)._state.get(A).delay, 0);
});

test("test_all_endpoints_down_raises_unavailable", async () => {
  const { c } = client({ [A]: down, [B]: down });
  await assert.rejects(c.blockNumber(), RpcUnavailable);
});

/**
 * Each url takes `costs[url]` seconds of the clock (absent: its whole timeout, a hang) and then
 * fails, or answers when it is in `answers`. Records [url, timeout].
 */
class TimedTransport implements Transport {
  calls: Array<[string, number]> = [];
  private readonly clock: FakeClock;
  private readonly costs: Record<string, number>;
  private readonly answers: Set<string>;
  constructor(clock: FakeClock, costs: Record<string, number>, answers: string[] = []) {
    this.clock = clock;
    this.costs = costs;
    this.answers = new Set(answers);
  }
  async postJson(url: string, _payload: any, timeout: number): Promise<unknown> {
    this.calls.push([url, timeout]);
    const cost = this.costs[url];
    this.clock.advance(cost === undefined ? timeout : Math.min(cost, timeout));
    if (this.answers.has(url)) return ok("0x7");
    throw new TransportError(`timed out after ${timeout}s`);
  }
}

test("test_a_call_spends_at_most_its_budget_across_endpoints", async () => {
  const C = "https://c.rpc";
  const D = "https://d.rpc";
  const clock = new FakeClock();
  const t = new TimedTransport(clock, {}); // every endpoint hangs
  const rpc = new RpcClient([A, B, C, D], { transport: t, clock: clock.call });
  assert.equal(CALL_BUDGET, 15.0);
  await assert.rejects(rpc.blockNumber(), RpcUnavailable);
  assert.deepEqual(t.calls, [[A, 10.0], [B, 5.0]]); // the rest of the 15 s, then nothing is left for C, D
  assert.equal(clock.now, 1000.0 + CALL_BUDGET);
  assert.deepEqual(rpc.liveUrls(), [C, D]); // skipped, not cooled down: the next call asks C
});

test("test_each_attempt_waits_at_most_what_is_left_of_the_budget", async () => {
  const C = "https://c.rpc";
  const clock = new FakeClock();
  const t = new TimedTransport(clock, { [A]: 3, [B]: 10, [C]: 1 }, [C]);
  assert.equal(await new RpcClient([A, B, C], { transport: t, clock: clock.call }).blockNumber(), 7);
  assert.deepEqual(t.calls, [[A, 10.0], [B, 10.0], [C, 2.0]]);
});

test("test_an_endpoint_is_not_asked_with_less_than_the_floor_left", async () => {
  // Review T5: A hangs (10 s), B fails after 4 s: 1 s is left, under MIN_ATTEMPT, so C is not
  // asked with a sliver of time, nor cooled down for failing it.
  const C = "https://c.rpc";
  const clock = new FakeClock();
  const t = new TimedTransport(clock, { [B]: 4 });
  const rpc = new RpcClient([A, B, C], { transport: t, clock: clock.call });
  assert.equal(MIN_ATTEMPT, 2.0);
  await assert.rejects(rpc.blockNumber(), RpcUnavailable);
  assert.deepEqual(t.calls, [[A, 10.0], [B, 5.0]]);
  assert.deepEqual(rpc.liveUrls(), [C]);
});

for (const [bTakes, asked] of [[3, true], [3.1, false]] as const) {
  test(`test_the_floor_is_asked_at_exactly_min_attempt_left[${bTakes}-${asked}]`, async () => {
    // Review V5, the floor's boundary: with exactly MIN_ATTEMPT left C is asked, with that
    // timeout; with less (1.9 s) it is not, and stays live.
    const C = "https://c.rpc";
    const clock = new FakeClock();
    const t = new TimedTransport(clock, { [B]: bTakes });
    const rpc = new RpcClient([A, B, C], { transport: t, clock: clock.call });
    await assert.rejects(rpc.blockNumber(), RpcUnavailable);
    const expected: Array<[string, number]> = [[A, 10.0], [B, 5.0], ...(asked ? [[C, MIN_ATTEMPT] as [string, number]] : [])];
    assert.deepEqual(t.calls, expected);
    assert.equal(rpc.liveUrls().includes(C), !asked);
  });
}

for (const [name, bad] of [["not_a_dict", "not a dict"], ["no_result_or_error", { jsonrpc: "2.0", id: 1 }]] as const) {
  test(`test_malformed_response_counts_as_endpoint_failure[${name}]`, async () => {
    const { c } = client({ [A]: () => bad, [B]: () => ok("0x2") });
    assert.equal(await c.blockNumber(), 2);
  });
}

test("test_non_transport_exception_propagates_from_attempt", async () => {
  const { c } = client({ [A]: () => { throw new Error("boom"); } });
  await assert.rejects(c.call("m", []), /boom/);
});

test("test_jsonrpc_error_is_raised_without_rotation", async () => {
  const { c, t } = client({ [A]: () => err(-32000, "execution reverted"), [B]: () => ok("0x1") });
  await assert.rejects(c.call("eth_call", []), (e: any) => e instanceof RpcError && e.code === -32000 && e.url === A);
  assert.deepEqual(urls(t), [A]);
});

test("test_rate_limit_jsonrpc_error_rotates", async () => {
  const { c } = client({ [A]: () => err(-32005, "Too Many Requests: rate limit"), [B]: () => ok("0x3") });
  assert.equal(await c.blockNumber(), 3);
});

test("test_rpc_error_keeps_raw_message_and_python_str_form", () => {
  const e = new RpcError(-1, "boom");
  assert.equal(e.message, "boom");
  assert.equal(String(e), "-1: boom");
});

test("test_is_range_error", () => {
  assert.ok(isRangeError(new RpcError(-32600, "eth_getLogs block range is too large")));
  assert.ok(isRangeError(new RpcError(-32005, "query returned more than 10000 results; limit exceeded")));
  assert.ok(!isRangeError(new RpcError(-32000, "execution reverted")));
});

test("test_receipt_and_block_hash", async () => {
  const h: Handler = (m, p) => {
    if (m === "eth_getTransactionReceipt") return ok(p[0] === "0xmissing" ? null : { status: "0x1" });
    if (m === "eth_getBlockByNumber") {
      assert.deepEqual(p, ["0x64", false]);
      return ok({ hash: "0xABC" });
    }
    return undefined;
  };
  const { c } = client({ [A]: h });
  assert.equal(await c.getTransactionReceipt("0xmissing"), null);
  assert.deepEqual(await c.getTransactionReceipt("0xok"), { status: "0x1" });
  assert.equal(await c.getBlockHash(100), "0xabc");
});

test("test_block_hash_none_for_unknown_block", async () => {
  const { c } = client({ [A]: () => ok(null) });
  assert.equal(await c.getBlockHash(10 ** 9), null);
});

// --- eth_getLogs chunking ---

function logsHandler(record: Array<[number, number]>, maxSpan: number | null = null): Handler {
  return (_m, p) => {
    const f = p[0];
    const lo = parseInt(f.fromBlock, 16);
    const hi = parseInt(f.toBlock, 16);
    record.push([lo, hi]);
    if (maxSpan !== null && hi - lo + 1 > maxSpan) return err(-32600, "block range too large");
    return ok([{ blockNumber: "0x" + lo.toString(16) }]);
  };
}

test("test_get_logs_chunks_by_log_range", async () => {
  const seen: Array<[number, number]> = [];
  const { c } = client({ [A]: logsHandler(seen) }, { log_range: 100 });
  const logs = await c.getLogs("0xtoken", ["0xtopic"], 1000, 1250);
  assert.deepEqual(seen, [[1000, 1099], [1100, 1199], [1200, 1250]]);
  assert.equal(logs.length, 3);
});

test("test_get_logs_empty_range_makes_no_calls", async () => {
  const seen: Array<[number, number]> = [];
  const { c } = client({ [A]: logsHandler(seen) });
  assert.deepEqual(await c.getLogs("0xtoken", [], 10, 9), []);
  assert.deepEqual(seen, []);
});

test("test_get_logs_halves_range_on_range_error_and_remembers", async () => {
  const seen: Array<[number, number]> = [];
  const { c } = client({ [A]: logsHandler(seen, 300) }, { log_range: 1000 });
  await c.getLogs("0xtoken", [], 0, 499);
  assert.equal(c.logRange, 250);
  assert.deepEqual(seen, [[0, 499], [0, 249], [250, 499]]);
});

test("test_get_logs_gives_up_at_minimum_range", async () => {
  const seen: Array<[number, number]> = [];
  const { c } = client({ [A]: logsHandler(seen, 1) }, { log_range: 40 });
  await assert.rejects(c.getLogs("0xtoken", [], 0, 100), RpcError);
  assert.equal(c.logRange, MIN_LOG_RANGE);
});

test("test_get_logs_sends_filter_shape", async () => {
  const calls: any[] = [];
  const { c } = client({ [A]: (_m, p) => { calls.push(p); return ok([]); } }, { log_range: 1000 });
  await c.getLogs("0xToken", ["0xt0", null, "0xt2"], 16, 31);
  assert.deepEqual(calls, [[{ address: "0xToken", topics: ["0xt0", null, "0xt2"], fromBlock: "0x10", toBlock: "0x1f" }]]);
});

test("test_get_logs_null_result_counts_as_empty", async () => {
  const { c } = client({ [A]: () => ok(null) });
  assert.deepEqual(await c.getLogs("0xt", [], 1, 2), []);
});

test("test_get_logs_empty_object_result_counts_as_empty", async () => {
  // Python: `out.extend(logs or [])`; an empty dict is falsy there (a JS `{}` is truthy and not iterable)
  const { c } = client({ [A]: () => ok({}) });
  assert.deepEqual(await c.getLogs("0xt", [], 1, 2), []);
  // a dict that is not empty extends with its keys, as Python iterates a dict
  const { c: d } = client({ [A]: () => ok({ a: 1 }) });
  assert.deepEqual(await d.getLogs("0xt", [], 1, 1), ["a"]);
});

test("test_log_range_is_remembered_per_endpoint", async () => {
  let aDown = false;
  const seen: Array<[number, number]> = [];
  const a: Handler = (m, p) => { if (aDown) throw new TransportError("down"); return logsHandler(seen)(m, p); };
  const { c, clock } = client({ [A]: a, [B]: logsHandler(seen, 50) }, { log_range: 1000 });
  await c.getLogs("0xtoken", [], 0, 999);
  assert.equal(c.logRange, 1000);
  aDown = true;
  await c.getLogs("0xtoken", [], 0, 199);
  const ranges = (c as any)._ranges as Map<string, number>;
  assert.ok(ranges.get(B)! < 1000 && c.logRange === ranges.get(B));
  aDown = false;
  clock.advance(COOLDOWN_START + 1);
  assert.equal(c.logRange, 1000);
});

test("test_suggested_range_uses_stated_limit_or_halves", () => {
  assert.equal(suggestedRange(new RpcError(-32602, "eth_getLogs is limited to 0 - 50 blocks range"), 2000), 50);
  assert.equal(suggestedRange(new RpcError(35, "ranges over 10000 blocks are not supported on free plan"), 2000), 1000);
  assert.equal(suggestedRange(new RpcError(-32602, "Archive requests require a personal token"), 500), 250);
  assert.equal(suggestedRange(new RpcError(-32005, "block range is too large"), 15), MIN_LOG_RANGE);
});

// tests/unit/test_rpc_client.py::test_suggested_range_ignores_numbers_beyond_18_digits:
// a digit run beyond 18 significant digits is no number, and \d is any Unicode decimal digit
// (Python's re), so the cores agree on every message.
test("test_suggested_range_ignores_numbers_beyond_18_digits", () => {
  const rows: [string, number, number][] = [
    ["limited to 50 blocks; request " + "9".repeat(4301), 2000, 50],
    ["block range too large: " + "9".repeat(4301), 5000, 2500],
    ["block range too large: " + "9".repeat(1_000_000), 5000, 2500],
    ["max " + "0".repeat(30) + "50 blocks", 2000, 50],
    ["max " + "9".repeat(19) + " or 50", 2000, 50],
    ["max \u0665\u0660 blocks", 2000, 50],
    ["max " + "\u0660".repeat(25) + "\u0665\u0660", 2000, 50],
    ["max " + "\u0660".repeat(1_000_000) + "\u0665\u0660", 2000, 50],
    ["max \uff15\uff10", 2000, 50],
    ["max \u096b\u0966 or \u09eb\u09e6", 2000, 50],
    ["max \u{1d7d3}\u{1d7ce}", 2000, 50], // mathematical bold digits: outside the BMP
    ["max \u{16ac5}\u{16ac0}", 2000, 1000], // Tangsa digits (Unicode 14): not digits in Python 3.9
  ];
  for (const [message, tried, expected] of rows) {
    assert.equal(suggestedRange(new RpcError(-32005, message), tried), expected, message.slice(0, 40));
  }
});

test("test_archive_message_counts_as_range_error", () => {
  assert.ok(isRangeError(new RpcError(-32602, "Archive requests require a personal token. Get one at x")));
});

test("test_stated_limit_is_adopted_in_one_step", async () => {
  const seen: Array<[number, number]> = [];
  const h: Handler = (_m, p) => {
    const lo = parseInt(p[0].fromBlock, 16);
    const hi = parseInt(p[0].toBlock, 16);
    seen.push([lo, hi]);
    if (hi - lo + 1 > 50) return err(-32602, "eth_getLogs is limited to 0 - 50 blocks range");
    return ok([]);
  };
  const { c } = client({ [A]: h }, { log_range: 2000 });
  await c.getLogs("0xtoken", [], 0, 99);
  assert.deepEqual(seen, [[0, 99], [0, 49], [50, 99]]);
  assert.equal(c.logRange, 50);
});

for (const [name, error, code, message] of [
  ["bare_string", "query timeout", 0, "query timeout"],
  ["non_numeric_code", { code: "bad", message: "odd" }, 0, "odd"],
  ["no_code", { message: "no code" }, 0, "no code"],
] as const) {
  test(`test_malformed_error_objects_become_rpc_errors[${name}]`, async () => {
    const { c } = client({ [A]: () => ({ jsonrpc: "2.0", id: 1, error }) });
    await assert.rejects(c.call("eth_blockNumber", []), (e: any) => e instanceof RpcError && e.code === code && e.message === message);
  });
}

// --- malformed replies raise where Python raises ---

test("test_error_null_without_result_raises_key_error_and_does_not_cool_down", async () => {
  const { c } = client({ [A]: () => ({ jsonrpc: "2.0", id: 1, error: null }), [B]: () => ok("0x1") });
  await assert.rejects(c.call("eth_blockNumber", []), KeyError);
  assert.deepEqual(c.liveUrls(), [A, B]);
});

for (const bad of ["", "zz", "12 34", "0x", "1__2", 5, null, {}, undefined]) {
  test(`test_block_number_rejects_malformed_hex[${JSON.stringify(bad) ?? "undefined"}]`, async () => {
    const { c } = client({ [A]: () => ok(bad) });
    await assert.rejects(c.blockNumber(), (e: any) => e instanceof TypeError || e instanceof ValueError);
  });
}

test("test_py_hex_int_matches_python_int_base_16", () => {
  assert.equal(pyHexInt("0x1b4"), 436);
  assert.equal(pyHexInt("1B4"), 436);
  assert.equal(pyHexInt(" 0X10 "), 16);
  assert.equal(pyHexInt("-0x10"), -16);
  assert.equal(pyHexInt("1_0"), 16);
  assert.equal(pyHexInt("0x_f"), 15);
  assert.throws(() => pyHexInt("12abcg"), ValueError);
  assert.throws(() => pyHexInt("_1"), ValueError);
  assert.throws(() => pyHexInt("1_"), ValueError);
});

for (const [name, block] of [["missing_key", { number: "0x1" }], ["null_hash", { hash: null }], ["numeric_hash", { hash: 5 }], ["non_object", "str"]] as const) {
  test(`test_block_hash_rejects_malformed_block[${name}]`, async () => {
    const { c } = client({ [A]: () => ok(block) });
    await assert.rejects(c.getBlockHash(1), (e: any) => e instanceof KeyError || e instanceof TypeError);
  });
}

test("test_block_hash_none_for_empty_object_block", async () => {
  const { c } = client({ [A]: () => ok({}) });
  assert.equal(await c.getBlockHash(1), null);
});
