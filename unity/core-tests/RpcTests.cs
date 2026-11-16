// No Python twin: RpcClient and SolanaRpc driven by a fake ITransport, ported from tests/unit/test_rpc_client.py,
// tests/unit/test_solana_rpc.py and their TypeScript twins (rpgmaker/src/core/rpc.test.ts, solana/rpc.test.ts).
// The scenarios marked "Python:" were run against scfg_core's RpcClient with a scripted fake transport and an
// integer clock, and the request logs and outcomes below are the ones Python produced.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;
using Scfg.Core.Solana;

namespace Scfg.Core.Tests
{
    public delegate object? Handler(string method, List<object?> prms);

    /// <summary>A transport answering from one handler per URL (rpc_fakes.ts's FakeTransport).</summary>
    public sealed class FakeTransport : ITransport
    {
        public readonly List<(string Url, string Method, List<object?> Params)> Calls = new List<(string, string, List<object?>)>();
        public readonly List<JsonObject> Payloads = new List<JsonObject>();
        public readonly List<double> Timeouts = new List<double>();
        private readonly Dictionary<string, Handler> _handlers;

        public FakeTransport(Dictionary<string, Handler> handlers) { _handlers = handlers; }

        public Task<object?> PostJson(string url, JsonObject payload, double timeout)
        {
            Payloads.Add(payload);
            Timeouts.Add(timeout);
            var prms = (List<object?>)payload["params"]!;
            Calls.Add((url, (string)payload["method"]!, prms));
            try
            {
                return Task.FromResult(_handlers[url]((string)payload["method"]!, prms));
            }
            catch (Exception e)
            {
                return Task.FromException<object?>(e);
            }
        }
    }

    /// <summary>A transport answering from a script per URL: a response, "down" (a TransportError) or an exception.</summary>
    public sealed class ScriptedTransport : ITransport
    {
        public readonly List<(string Url, long Id, string Method, List<object?> Params)> Log = new List<(string, long, string, List<object?>)>();
        private readonly Dictionary<string, Queue<object?>> _script;

        public ScriptedTransport(Dictionary<string, object?[]> script)
        {
            _script = script.ToDictionary(kv => kv.Key, kv => new Queue<object?>(kv.Value));
        }

        public Task<object?> PostJson(string url, JsonObject payload, double timeout)
        {
            Log.Add((url, (long)payload["id"]!, (string)payload["method"]!, (List<object?>)payload["params"]!));
            var q = _script[url];
            if (q.Count == 0) return Task.FromException<object?>(new IndexError("pop from empty list"));
            var r = q.Dequeue();
            if (r is string s && s == "down") return Task.FromException<object?>(new TransportError("down"));
            if (r is Exception e) return Task.FromException<object?>(e);
            return Task.FromResult(r);
        }
    }

    public static class Fakes
    {
        public static JsonObject Ok(object? result, long id = 1) =>
            new JsonObject { { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };

        public static JsonObject Err(long code, string message) =>
            new JsonObject { { "jsonrpc", "2.0" }, { "id", 1L }, { "error", new JsonObject { { "code", code }, { "message", message } } } };

        public static JsonObject ErrRaw(object? error) =>
            new JsonObject { { "jsonrpc", "2.0" }, { "id", 1L }, { "error", error } };

        public static readonly Handler Down = (m, p) => throw new TransportError("connection refused");

        public static List<object?> L(params object?[] xs) => xs.ToList();

        /// <summary>Python <c>int(x, 16)</c> for a test's hex param.</summary>
        public static long H(object? x) => (long)Rpc.HexInt(x);
    }

    public class RpcClientTests
    {
        private const string A = "https://a.rpc";
        private const string B = "https://b.rpc";

        private sealed class Clock
        {
            public long Now = 1000;
            public long Call() => Now;
        }

        private static (RpcClient c, FakeTransport t, Clock clock) Client(Dictionary<string, Handler> handlers, long logRange = 1000)
        {
            var clock = new Clock();
            var t = new FakeTransport(handlers);
            var c = new RpcClient(handlers.Keys.ToList(), t, logRange: logRange, clock: clock.Call);
            return (c, t, clock);
        }

        private static List<string> Urls(FakeTransport t) => t.Calls.Select(x => x.Url).ToList();

        private static Handler Ok(object? r) => (m, p) => Fakes.Ok(r);

        [Test]
        public void RequiresUrls()
        {
            Assert.Throws<ValueError>(() => new RpcClient(new List<string>(), new FakeTransport(new Dictionary<string, Handler>())));
        }

        [Test]
        public async Task CallSendsJsonRpcAndReturnsResult()
        {
            var (c, t, _) = Client(new Dictionary<string, Handler> { { A, Ok("0x10") } });
            Assert.That(await c.Call("eth_blockNumber", Fakes.L()), Is.EqualTo("0x10"));
            Assert.That(t.Calls.Count, Is.EqualTo(1));
            Assert.That(t.Calls[0].Url, Is.EqualTo(A));
            var want = new JsonObject { { "jsonrpc", "2.0" }, { "id", 1L }, { "method", "eth_blockNumber" }, { "params", new List<object?>() } };
            Assert.That(JsonEq.FirstDiff(t.Payloads[0], want), Is.Null);
            Assert.That(t.Payloads[0].Keys, Is.EqualTo(new[] { "jsonrpc", "id", "method", "params" }));
            Assert.That(t.Timeouts[0], Is.EqualTo(10.0));
        }

        [Test]
        public async Task JsonRpcIdsCountFromOnePerClientAcrossEndpoints()
        {
            var (c, t, _) = Client(new Dictionary<string, Handler> { { A, Fakes.Down }, { B, Ok("0x1") } });
            await c.BlockNumber();
            await c.BlockNumber();
            Assert.That(t.Payloads.Select(p => p["id"]), Is.EqualTo(new object[] { 1L, 2L, 3L }));
        }

        [Test]
        public async Task BlockNumberParsesHex()
        {
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, Ok("0x1b4") } });
            Assert.That(await c.BlockNumber(), Is.EqualTo(436L));
        }

        [Test]
        public async Task FallsBackToNextEndpointOnTransportError()
        {
            var (c, t, _) = Client(new Dictionary<string, Handler> { { A, Fakes.Down }, { B, Ok("0x1") } });
            Assert.That(await c.BlockNumber(), Is.EqualTo(1L));
            Assert.That(Urls(t), Is.EqualTo(new[] { A, B }));
        }

        [Test]
        public async Task FailedEndpointCoolsDownThenIsRetried()
        {
            bool aUp = false;
            Handler a = (m, p) => aUp ? Fakes.Ok("0xa") : throw new TransportError("HTTP 503", 503);
            var (c, t, clock) = Client(new Dictionary<string, Handler> { { A, a }, { B, Ok("0xb") } });
            Assert.That(await c.BlockNumber(), Is.EqualTo(0xbL));
            t.Calls.Clear();
            Assert.That(await c.BlockNumber(), Is.EqualTo(0xbL));
            Assert.That(Urls(t), Is.EqualTo(new[] { B }));
            aUp = true;
            clock.Now += (long)Rpc.CooldownStart;
            Assert.That(await c.BlockNumber(), Is.EqualTo(0xaL));
        }

        [Test]
        public void CooldownDoublesAndCaps()
        {
            var (c, _, clock) = Client(new Dictionary<string, Handler> { { A, Fakes.Down } });
            var delays = new List<double>();
            for (int i = 0; i < 8; i++)
            {
                Assert.ThrowsAsync<RpcUnavailable>(() => c.Call("eth_blockNumber", Fakes.L()));
                double d = c.CooldownOf(A);
                delays.Add(d);
                clock.Now += (long)d;
            }
            Assert.That(delays, Is.EqualTo(new[] { 30.0, 60.0, 120.0, 240.0, 480.0, 600.0, 600.0, 600.0 }));
            Assert.That(delays.Max(), Is.EqualTo(Rpc.CooldownMax));
        }

        [Test]
        public void CooldownBoundaryIsUntilLeNow()
        {
            var (c, _, clock) = Client(new Dictionary<string, Handler> { { A, Fakes.Down } });
            Assert.ThrowsAsync<RpcUnavailable>(() => c.Call("m", Fakes.L()));
            Assert.That(c.LiveUrls(), Is.Empty);
            clock.Now += 29;
            Assert.That(c.LiveUrls(), Is.Empty);
            clock.Now += 1;
            Assert.That(c.LiveUrls(), Is.EqualTo(new[] { A }));
        }

        [Test]
        public async Task SuccessResetsCooldown()
        {
            bool fail = true;
            Handler a = (m, p) => fail ? throw new TransportError("x") : Fakes.Ok("0x1");
            var (c, _, clock) = Client(new Dictionary<string, Handler> { { A, a } });
            Assert.ThrowsAsync<RpcUnavailable>(() => c.Call("eth_blockNumber", Fakes.L()));
            fail = false;
            clock.Now += 31;
            await c.Call("eth_blockNumber", Fakes.L());
            Assert.That(c.CooldownOf(A), Is.EqualTo(0.0));
        }

        [Test]
        public void AllEndpointsDownRaisesUnavailable()
        {
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, Fakes.Down }, { B, Fakes.Down } });
            var e = Assert.ThrowsAsync<RpcUnavailable>(() => c.BlockNumber())!;
            Assert.That(e.Message, Is.EqualTo("all endpoints failed for eth_blockNumber"));
        }

        [Test]
        public void TheClockIsSettableAndReadOnEveryCall()
        {
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, Fakes.Down } });
            long now = 5;
            c.Clock = () => now;
            Assert.ThrowsAsync<RpcUnavailable>(() => c.Call("m", Fakes.L()));
            now = 34;
            Assert.That(c.LiveUrls(), Is.Empty);
            now = 35;
            Assert.That(c.LiveUrls(), Is.EqualTo(new[] { A }));
        }

        [Test]
        public void ANonTransportExceptionPropagatesFromAttempt()
        {
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, (m, p) => throw new InvalidOperationException("boom") }, { B, Ok(1L) } });
            var e = Assert.ThrowsAsync<InvalidOperationException>(() => c.Call("m", Fakes.L()))!;
            Assert.That(e.Message, Is.EqualTo("boom"));
            Assert.That(c.LiveUrls(), Is.EqualTo(new[] { A, B }));
        }

        [Test]
        public void JsonRpcErrorIsRaisedWithoutRotation()
        {
            var (c, t, _) = Client(new Dictionary<string, Handler> { { A, (m, p) => Fakes.Err(-32000, "execution reverted") }, { B, Ok("0x1") } });
            var e = Assert.ThrowsAsync<RpcError>(() => c.Call("eth_call", Fakes.L()))!;
            Assert.That(e.Code, Is.EqualTo(-32000L));
            Assert.That(e.Url, Is.EqualTo(A));
            Assert.That(e.Message, Is.EqualTo("execution reverted"));
            Assert.That(e.PyStr, Is.EqualTo("-32000: execution reverted"));
            Assert.That(Urls(t), Is.EqualTo(new[] { A }));
        }

        [Test]
        public async Task RateLimitJsonRpcErrorRotates()
        {
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, (m, p) => Fakes.Err(-32005, "Too Many Requests: rate limit") }, { B, Ok("0x3") } });
            Assert.That(await c.BlockNumber(), Is.EqualTo(3L));
            Assert.That(c.LiveUrls(), Is.EqualTo(new[] { B }));
        }

        [Test]
        public void IsRangeError()
        {
            Assert.That(Rpc.IsRangeError(new RpcError(-32600, "eth_getLogs block range is too large")), Is.True);
            Assert.That(Rpc.IsRangeError(new RpcError(-32005, "query returned more than 10000 results; limit exceeded")), Is.True);
            Assert.That(Rpc.IsRangeError(new RpcError(-32602, "Archive requests require a personal token. Get one at x")), Is.True);
            Assert.That(Rpc.IsRangeError(new RpcError(-32000, "execution reverted")), Is.False);
            Assert.That(Rpc.IsRangeError(new RpcError(1, "LIMIT")), Is.True);
            // Python: "L\u0130MIT".lower() is "li\u0307mit", which does not contain "limit" (.NET's invariant lower would)
            Assert.That(Rpc.IsRangeError(new RpcError(1, "L\u0130MIT")), Is.False);
            Assert.That(Rpc.IsRangeError(new RpcError(1, "ARCH\u0130VE")), Is.False);
        }

        [Test]
        public async Task ReceiptAndBlockHash()
        {
            Handler h = (m, p) =>
            {
                if (m == "eth_getTransactionReceipt") return Fakes.Ok((string)p[0]! == "0xmissing" ? null : new JsonObject { { "status", "0x1" } });
                if (m == "eth_getBlockByNumber")
                {
                    Assert.That(JsonEq.FirstDiff(p, Fakes.L("0x64", false)), Is.Null);
                    return Fakes.Ok(new JsonObject { { "hash", "0xABC" } });
                }
                return null;
            };
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, h } });
            Assert.That(await c.GetTransactionReceipt("0xmissing"), Is.Null);
            Assert.That(JsonEq.FirstDiff(await c.GetTransactionReceipt("0xok"), new JsonObject { { "status", "0x1" } }), Is.Null);
            Assert.That(await c.GetBlockHash(100), Is.EqualTo("0xabc"));
        }

        [Test]
        public async Task BlockHashNoneForUnknownOrEmptyBlock()
        {
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, Ok(null) } });
            Assert.That(await c.GetBlockHash(1000000000), Is.Null);
            var (d, _, _) = Client(new Dictionary<string, Handler> { { A, Ok(new JsonObject()) } });
            Assert.That(await d.GetBlockHash(1), Is.Null);
        }

        [Test]
        public void BlockHashRejectsMalformedBlock()
        {
            Assert.ThrowsAsync<KeyError>(() => Client(new Dictionary<string, Handler> { { A, Ok(new JsonObject { { "number", "0x1" } }) } }).c.GetBlockHash(1));
            foreach (var block in new object?[] { new JsonObject { { "hash", null } }, new JsonObject { { "hash", 5L } }, "str", Fakes.L(1L), 5L, true })
                Assert.ThrowsAsync<TypeError>(() => Client(new Dictionary<string, Handler> { { A, Ok(block) } }).c.GetBlockHash(1), Py.Repr(block));
        }

        // ---- eth_getLogs chunking ----

        private static Handler LogsHandler(List<(long, long)> record, long? maxSpan = null) => (m, p) =>
        {
            var f = (JsonObject)p[0]!;
            long lo = Fakes.H(f["fromBlock"]), hi = Fakes.H(f["toBlock"]);
            record.Add((lo, hi));
            if (maxSpan != null && hi - lo + 1 > maxSpan) return Fakes.Err(-32600, "block range too large");
            return Fakes.Ok(Fakes.L(new JsonObject { { "blockNumber", Rpc.Hex(lo) } }));
        };

        [Test]
        public async Task GetLogsChunksByLogRange()
        {
            var seen = new List<(long, long)>();
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, LogsHandler(seen) } }, logRange: 100);
            var logs = await c.GetLogs("0xtoken", Fakes.L("0xtopic"), 1000, 1250);
            Assert.That(seen, Is.EqualTo(new[] { (1000L, 1099L), (1100L, 1199L), (1200L, 1250L) }));
            Assert.That(logs.Count, Is.EqualTo(3));
        }

        [Test]
        public async Task GetLogsEmptyRangeMakesNoCalls()
        {
            var seen = new List<(long, long)>();
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, LogsHandler(seen) } });
            Assert.That(await c.GetLogs("0xtoken", Fakes.L(), 10, 9), Is.Empty);
            Assert.That(seen, Is.Empty);
        }

        [Test]
        public async Task GetLogsHalvesRangeOnRangeErrorAndRemembers()
        {
            var seen = new List<(long, long)>();
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, LogsHandler(seen, 300) } }, logRange: 1000);
            await c.GetLogs("0xtoken", Fakes.L(), 0, 499);
            Assert.That(c.LogRange, Is.EqualTo(250L));
            Assert.That(seen, Is.EqualTo(new[] { (0L, 499L), (0L, 249L), (250L, 499L) }));
        }

        [Test]
        public void GetLogsGivesUpAtMinimumRange()
        {
            var seen = new List<(long, long)>();
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, LogsHandler(seen, 1) } }, logRange: 40);
            Assert.ThrowsAsync<RpcError>(() => c.GetLogs("0xtoken", Fakes.L(), 0, 100));
            Assert.That(c.LogRange, Is.EqualTo(Rpc.MinLogRange));
        }

        [Test]
        public async Task GetLogsSendsFilterShape()
        {
            var calls = new List<object?>();
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, (m, p) => { calls.Add(p); return Fakes.Ok(Fakes.L()); } } });
            await c.GetLogs("0xToken", Fakes.L("0xt0", null, "0xt2"), 16, 31);
            var want = Fakes.L(Fakes.L(new JsonObject
            {
                { "address", "0xToken" }, { "topics", Fakes.L("0xt0", null, "0xt2") }, { "fromBlock", "0x10" }, { "toBlock", "0x1f" },
            }));
            Assert.That(JsonEq.FirstDiff(calls, want), Is.Null);
            Assert.That(((JsonObject)((List<object?>)calls[0]!)[0]!).Keys, Is.EqualTo(new[] { "address", "topics", "fromBlock", "toBlock" }));
        }

        [Test]
        public async Task GetLogsExtendsAsPythonsListExtend()
        {
            // Python: out.extend(logs or []): None, {} and "" are empty; a dict extends with its keys, a str with its
            // characters; anything else is "'int' object is not iterable"
            foreach (var empty in new object?[] { null, new JsonObject(), "", 0L, false })
                Assert.That(await Client(new Dictionary<string, Handler> { { A, Ok(empty) } }).c.GetLogs("0xt", Fakes.L(), 1, 2), Is.Empty);
            Assert.That(await Client(new Dictionary<string, Handler> { { A, Ok(new JsonObject { { "a", 1L } }) } }).c.GetLogs("0xt", Fakes.L(), 1, 1),
                Is.EqualTo(new[] { "a" }));
            Assert.That(await Client(new Dictionary<string, Handler> { { A, Ok("a\U0001F600") } }).c.GetLogs("0xt", Fakes.L(), 1, 1),
                Is.EqualTo(new[] { "a", "\U0001F600" }));
            var e = Assert.ThrowsAsync<TypeError>(() => Client(new Dictionary<string, Handler> { { A, Ok(5L) } }).c.GetLogs("0xt", Fakes.L(), 1, 1))!;
            Assert.That(e.Message, Is.EqualTo("'int' object is not iterable"));
        }

        [Test]
        public async Task LogRangeIsRememberedPerEndpoint()
        {
            bool aDown = false;
            var seen = new List<(long, long)>();
            var aLogs = LogsHandler(seen);
            Handler a = (m, p) => aDown ? throw new TransportError("down") : aLogs(m, p);
            var (c, _, clock) = Client(new Dictionary<string, Handler> { { A, a }, { B, LogsHandler(seen, 50) } }, logRange: 1000);
            await c.GetLogs("0xtoken", Fakes.L(), 0, 999);
            Assert.That(c.LogRange, Is.EqualTo(1000L));
            aDown = true;
            await c.GetLogs("0xtoken", Fakes.L(), 0, 199);
            Assert.That(c.RangeOf(B), Is.LessThan(1000L));
            Assert.That(c.LogRange, Is.EqualTo(c.RangeOf(B)));
            aDown = false;
            clock.Now += 31;
            Assert.That(c.LogRange, Is.EqualTo(1000L));
        }

        [Test]
        public void SuggestedRangeUsesStatedLimitOrHalves()
        {
            Assert.That(Rpc.SuggestedRange(new RpcError(-32602, "eth_getLogs is limited to 0 - 50 blocks range"), 2000), Is.EqualTo(50L));
            Assert.That(Rpc.SuggestedRange(new RpcError(35, "ranges over 10000 blocks are not supported on free plan"), 2000), Is.EqualTo(1000L));
            Assert.That(Rpc.SuggestedRange(new RpcError(-32602, "Archive requests require a personal token"), 500), Is.EqualTo(250L));
            Assert.That(Rpc.SuggestedRange(new RpcError(-32005, "block range is too large"), 15), Is.EqualTo(Rpc.MinLogRange));
        }

        [Test]
        public void SuggestedRangeIgnoresNumbersBeyond18Digits()
        {
            var rows = new (string, long, long)[]
            {
                ("limited to 50 blocks; request " + new string('9', 4301), 2000, 50),
                ("block range too large: " + new string('9', 4301), 5000, 2500),
                ("block range too large: " + new string('9', 1_000_000), 5000, 2500),
                ("max " + new string('0', 30) + "50 blocks", 2000, 50),
                ("max " + new string('9', 19) + " or 50", 2000, 50),
                ("max \u0665\u0660 blocks", 2000, 50),
                ("max " + new string('\u0660', 25) + "\u0665\u0660", 2000, 50),
                ("max " + new string('\u0660', 1_000_000) + "\u0665\u0660", 2000, 50),
                ("max \uff15\uff10", 2000, 50),
                ("max \u096b\u0966 or \u09eb\u09e6", 2000, 50),
                ("max \U0001D7D3\U0001D7CE", 2000, 50), // mathematical bold digits: outside the BMP
                ("max \U00016AC5\U00016AC0", 2000, 1000), // Tangsa digits (Unicode 14): not digits in Python 3.9
            };
            foreach (var (message, tried, expected) in rows)
                Assert.That(Rpc.SuggestedRange(new RpcError(-32005, message), tried), Is.EqualTo(expected), message.Substring(0, Math.Min(40, message.Length)));
            long big = 9_000_000_000_000_000_000;
            Assert.That(Rpc.SuggestedRange(new RpcError(0, "max " + new string('9', 18)), big), Is.EqualTo(999_999_999_999_999_999L));
            Assert.That(Rpc.SuggestedRange(new RpcError(0, "max 00000" + new string('9', 18)), big), Is.EqualTo(999_999_999_999_999_999L));
            Assert.That(Rpc.SuggestedRange(new RpcError(0, "max 1" + new string('0', 18)), big), Is.EqualTo(big / 2));
        }

        [Test]
        public async Task AnOverlongNumberInARangeErrorDoesNotBreakGetLogs()
        {
            var message = "block range too large: " + new string('9', 4301);
            Handler h = (m, p) =>
            {
                var f = (JsonObject)p[0]!;
                return Fakes.H(f["toBlock"]) - Fakes.H(f["fromBlock"]) + 1 > 100 ? Fakes.Err(-32005, message) : Fakes.Ok(Fakes.L());
            };
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, h } }, logRange: 400);
            Assert.That(await c.GetLogs("0xT", Fakes.L(), 1, 400), Is.Empty);
            Assert.That(c.LogRange, Is.EqualTo(100L));
        }

        [Test]
        public async Task StatedLimitIsAdoptedInOneStep()
        {
            var seen = new List<(long, long)>();
            Handler h = (m, p) =>
            {
                var f = (JsonObject)p[0]!;
                long lo = Fakes.H(f["fromBlock"]), hi = Fakes.H(f["toBlock"]);
                seen.Add((lo, hi));
                return hi - lo + 1 > 50 ? Fakes.Err(-32602, "eth_getLogs is limited to 0 - 50 blocks range") : Fakes.Ok(Fakes.L());
            };
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, h } }, logRange: 2000);
            await c.GetLogs("0xtoken", Fakes.L(), 0, 99);
            Assert.That(seen, Is.EqualTo(new[] { (0L, 99L), (0L, 49L), (50L, 99L) }));
            Assert.That(c.LogRange, Is.EqualTo(50L));
        }

        // ---- malformed answers (Python: _parse_error and _attempt) ----

        private static readonly (string Name, object? Response, object Expected, string[] Urls, string[] Live)[] Classify =
        {
            // Expected: an RpcError's (code, message, str) or the call's result
            ("str_code_int_message", Fakes.ErrRaw(new JsonObject { { "code", "12" }, { "message", 5L } }), (12L, "5", "12: 5"), new[] { "a" }, new[] { "a", "b" }),
            ("falsy_error_zero", Fakes.ErrRaw(0L), (0L, "0", "0: 0"), new[] { "a" }, new[] { "a", "b" }),
            ("error_list", Fakes.ErrRaw(Fakes.L()), (0L, "[]", "0: []"), new[] { "a" }, new[] { "a", "b" }),
            ("float_code", Fakes.ErrRaw(new JsonObject { { "code", 1.9 } }), (1L, "", "1: "), new[] { "a" }, new[] { "a", "b" }),
            ("bool_code", Fakes.ErrRaw(new JsonObject { { "code", true }, { "message", null } }), (1L, "None", "1: None"), new[] { "a" }, new[] { "a", "b" }),
            ("nan_code", new JsonObject { { "error", new JsonObject { { "code", double.NaN }, { "message", "m" } } } }, (0L, "m", "0: m"), new[] { "a" }, new[] { "a", "b" }),
            ("spaced_str_code", Fakes.ErrRaw(new JsonObject { { "code", " 7 " } }), (7L, "", "7: "), new[] { "a" }, new[] { "a", "b" }),
            ("underscored_code", Fakes.ErrRaw(new JsonObject { { "code", "1_0" } }), (10L, "", "10: "), new[] { "a" }, new[] { "a", "b" }),
            ("list_code", Fakes.ErrRaw(new JsonObject { { "code", Fakes.L(1L) }, { "message", 1.5 } }), (0L, "1.5", "0: 1.5"), new[] { "a" }, new[] { "a", "b" }),
            ("dict_message", Fakes.ErrRaw(new JsonObject { { "message", new JsonObject { { "a", null } } } }), (0L, "{'a': None}", "0: {'a': None}"), new[] { "a" }, new[] { "a", "b" }),
            ("error_dict_empty", Fakes.ErrRaw(new JsonObject()), (0L, "", "0: "), new[] { "a" }, new[] { "a", "b" }),
            ("rate_limit_dotted_I", Fakes.ErrRaw(new JsonObject { { "code", -1L }, { "message", "RATE L\u0130MIT" } }), (-1L, "RATE L\u0130MIT", "-1: RATE L\u0130MIT"), new[] { "a" }, new[] { "a", "b" }),
            ("result_and_null_error", new JsonObject { { "result", 7L }, { "error", null } }, 7L, new[] { "a" }, new[] { "a", "b" }),
            ("ids_ignored", Fakes.Ok(5L, id: 999), 5L, new[] { "a" }, new[] { "a", "b" }),
            ("list_response", Fakes.L(1L), "fallback", new[] { "a", "b" }, new[] { "b" }),
            ("string_response", "x", "fallback", new[] { "a", "b" }, new[] { "b" }),
            ("null_response", null, "fallback", new[] { "a", "b" }, new[] { "b" }),
            ("only_id", new JsonObject { { "id", 3L } }, "fallback", new[] { "a", "b" }, new[] { "b" }),
            ("rate_limit_string", Fakes.ErrRaw("Rate Limit exceeded"), "fallback", new[] { "a", "b" }, new[] { "b" }),
        };

        private static IEnumerable<string> ClassifyNames() => Classify.Select(c => c.Name);

        [TestCaseSource(nameof(ClassifyNames))]
        public async Task ErrorClassificationMatchesPython(string name)
        {
            // Python: scen.py "classify"
            var row = Classify.Single(c => c.Name == name);
            const string a = "https://a", b = "https://b";
            var t = new ScriptedTransport(new Dictionary<string, object?[]> { { a, new[] { row.Response } }, { b, new object?[] { Fakes.Ok("fallback") } } });
            var c = new RpcClient(new[] { a, b }, t, clock: () => 1000);
            if (row.Expected is ValueTuple<long, string, string> err)
            {
                var e = Assert.ThrowsAsync<RpcError>(() => c.Call("m", Fakes.L(1L)))!;
                Assert.That((e.Code, e.Message, e.PyStr, e.Url), Is.EqualTo((err.Item1, err.Item2, err.Item3, a)));
            }
            else
            {
                Assert.That(await c.Call("m", Fakes.L(1L)), Is.EqualTo(row.Expected));
            }
            Assert.That(t.Log.Select(x => x.Url.Substring(8)), Is.EqualTo(row.Urls));
            Assert.That(c.LiveUrls().Select(u => u.Substring(8)), Is.EqualTo(row.Live));
        }

        [Test]
        public void ACodeBeyondALongIsKeptForStrButIsNoLong()
        {
            // Python: RpcError(10**30, "m"): str "1000000000000000000000000000000: m"
            var t = new ScriptedTransport(new Dictionary<string, object?[]>
            {
                { A, new object?[] { Fakes.ErrRaw(new JsonObject { { "code", BigInteger.Pow(10, 30) }, { "message", "m" } }) } },
            });
            var c = new RpcClient(new[] { A }, t, clock: () => 0);
            var e = Assert.ThrowsAsync<RpcError>(() => c.Call("m", Fakes.L()))!;
            Assert.That(e.Code, Is.Null);
            Assert.That(e.FullCode, Is.EqualTo(BigInteger.Pow(10, 30)));
            Assert.That(e.PyStr, Is.EqualTo("1000000000000000000000000000000: m"));
        }

        [Test]
        public void AnInfiniteCodeIsAnOverflowErrorAsInPython()
        {
            // Python: int_from(inf) raises OverflowError, which _parse_error does not catch
            var (c, _, _) = Client(new Dictionary<string, Handler> { { A, (m, p) => Fakes.ErrRaw(new JsonObject { { "code", double.PositiveInfinity } }) } });
            Assert.ThrowsAsync<OverflowError>(() => c.Call("m", Fakes.L()));
        }

        [Test]
        public void ErrorNullWithoutResultRaisesKeyErrorAndDoesNotCoolDown()
        {
            var (c, _, _) = Client(new Dictionary<string, Handler>
            {
                { A, (m, p) => new JsonObject { { "jsonrpc", "2.0" }, { "id", 1L }, { "error", null } } }, { B, Ok("0x1") },
            });
            Assert.ThrowsAsync<KeyError>(() => c.Call("eth_blockNumber", Fakes.L()));
            Assert.That(c.LiveUrls(), Is.EqualTo(new[] { A, B }));
        }

        [Test]
        public void BlockNumberRejectsMalformedHex()
        {
            foreach (var bad in new object?[] { "", "zz", "12 34", "0x", "1__2", "0x__1", "ff_", "\u001c0x1", "\uff46", 5L, null, new JsonObject(), true, 1.5 })
            {
                var e = Assert.CatchAsync(() => Client(new Dictionary<string, Handler> { { A, Ok(bad) } }).c.BlockNumber())!;
                Assert.That(e is TypeError || e is ValueError, Is.True, Py.Repr(bad) + ": " + e.GetType().Name);
            }
        }

        [Test]
        public void HexIntMatchesPythonIntBase16()
        {
            // Python: int(x, 16), checked with uv run python
            var rows = new (string, long)[]
            {
                ("0x1b4", 436), ("1B4", 436), (" 0X10 ", 16), (" 0X1F\n", 31), ("-0x10", -16), ("+1f", 31), ("1_0", 16), ("0x_f", 15),
                ("\u0661\u0662", 18), ("0x\u0661\u0662", 18), ("\u0660x1", 1), ("\u00a0 0x1 \u00a0", 1), ("\u2028 1", 1), ("0b1", 177),
                ("\uff11", 1), ("0", 0),
            };
            foreach (var (text, want) in rows) Assert.That(Rpc.HexInt(text), Is.EqualTo(new BigInteger(want)), Py.Repr(text));
            foreach (var text in new[] { "12abcg", "_1", "1_", "0x", "0x__1", "- 1", "", "\u001c0x1", "\uff46", "0_x1" })
                Assert.Throws<ValueError>(() => Rpc.HexInt(text), Py.Repr(text));
            foreach (var v in new object?[] { 5L, null, 1.5, true })
                Assert.Throws<TypeError>(() => Rpc.HexInt(v), Py.Repr(v));
            // base 16 has no digit limit in Python
            Assert.That(Rpc.HexInt("0x1" + new string('0', 5000)), Is.EqualTo(BigInteger.Pow(16, 5000)));
            Assert.That(Rpc.Hex(-5), Is.EqualTo("-0x5"));
            Assert.That(Rpc.Hex(0), Is.EqualTo("0x0"));
            Assert.That(Rpc.Hex(255), Is.EqualTo("0xff"));
            Assert.That(Rpc.Hex(long.MinValue), Is.EqualTo("-0x8000000000000000"));
        }

        // ---- scenarios recorded from Python (scen.py) ----

        [Test]
        public async Task RotationAndCooldownOnAnIntegerClockMatchPython()
        {
            const string a = "https://a", b = "https://b";
            long now = 1000;
            var t = new ScriptedTransport(new Dictionary<string, object?[]>
            {
                { a, new object?[] { "down", "down", Fakes.Ok("0xa") } },
                { b, new object?[] { Fakes.Ok("0xb"), Fakes.Ok("0xb"), Fakes.Ok("0xb"), Fakes.Ok("0xb") } },
            });
            var c = new RpcClient(new[] { a, b }, t, clock: () => now);
            Assert.That(await c.BlockNumber(), Is.EqualTo(11L));
            now += 29;
            Assert.That(await c.BlockNumber(), Is.EqualTo(11L));
            Assert.That(c.LiveUrls(), Is.EqualTo(new[] { b }));
            now += 1;
            Assert.That(c.LiveUrls(), Is.EqualTo(new[] { a, b }));
            Assert.That(await c.BlockNumber(), Is.EqualTo(11L));
            Assert.That((c.CooldownOf(a), c.CooldownUntil(a)), Is.EqualTo((60.0, 1090.0)));
            now += 60;
            Assert.That(await c.BlockNumber(), Is.EqualTo(10L));
            Assert.That((c.CooldownOf(a), c.CooldownUntil(a)), Is.EqualTo((0.0, 0.0)));
            Assert.That(t.Log.Select(x => (x.Url, x.Id, x.Method)), Is.EqualTo(new[]
            {
                (a, 1L, "eth_blockNumber"), (b, 2L, "eth_blockNumber"), (b, 3L, "eth_blockNumber"),
                (a, 4L, "eth_blockNumber"), (b, 5L, "eth_blockNumber"), (a, 6L, "eth_blockNumber"),
            }));
        }

        [Test]
        public void GetLogsSplittingAcrossAFailoverMatchesPython()
        {
            const string a = "https://a", b = "https://b";
            var t = new ScriptedTransport(new Dictionary<string, object?[]>
            {
                { a, new object?[] { Fakes.Err(-32005, "block range is too large"), Fakes.Ok(Fakes.L(new JsonObject { { "n", 1L } })), "down" } },
                {
                    b, new object?[]
                    {
                        Fakes.Err(-32602, "limited to 0 - 50 blocks range"), Fakes.Ok(Fakes.L(new JsonObject { { "n", 2L } })), Fakes.Ok("ab"),
                        Fakes.Ok(new JsonObject { { "k", 1L } }), Fakes.Ok(null), Fakes.Ok(Fakes.L()), "down",
                    }
                },
            });
            var c = new RpcClient(new[] { a, b }, t, logRange: 400, clock: () => 1000);
            var e = Assert.ThrowsAsync<RpcUnavailable>(() => c.GetLogs("0xT", Fakes.L("0xt"), 100, 599))!;
            Assert.That(e.Message, Is.EqualTo("all endpoints failed for eth_getLogs"));
            Assert.That(t.Log.Select(x =>
            {
                var f = (JsonObject)x.Params[0]!;
                return (x.Url.Substring(8), x.Id, (string)f["fromBlock"]!, (string)f["toBlock"]!);
            }), Is.EqualTo(new[]
            {
                ("a", 1L, "0x64", "0x1f3"), ("a", 2L, "0x64", "0x12b"), ("a", 3L, "0x12c", "0x1f3"), ("b", 4L, "0x12c", "0x1f3"),
                ("b", 5L, "0x12c", "0x15d"), ("b", 6L, "0x15e", "0x18f"), ("b", 7L, "0x190", "0x1c1"), ("b", 8L, "0x1c2", "0x1f3"),
                ("b", 9L, "0x1f4", "0x225"), ("b", 10L, "0x226", "0x257"),
            }));
            Assert.That((c.RangeOf(a), c.RangeOf(b)), Is.EqualTo((200L, 50L)));
            Assert.That(c.LogRange, Is.EqualTo(200L)); // every endpoint cooling down: the first one's
        }

        [Test]
        public async Task GetLogsCollectsWhatPythonCollects()
        {
            const string a = "https://a";
            var t = new ScriptedTransport(new Dictionary<string, object?[]>
            {
                { a, new object?[] { Fakes.Ok(Fakes.L(new JsonObject { { "n", 1L } })), Fakes.Ok("ab"), Fakes.Ok(new JsonObject { { "k", 1L } }), Fakes.Ok(null) } },
            });
            var c = new RpcClient(new[] { a }, t, logRange: 10, clock: () => 0);
            var logs = await c.GetLogs("0xT", Fakes.L(), 0, 39);
            Assert.That(JsonEq.FirstDiff(logs, Fakes.L(new JsonObject { { "n", 1L } }, "a", "b", "k")), Is.Null);
        }

        [Test]
        public async Task CallAtAndTryAtMatchPython()
        {
            const string a = "https://a", b = "https://b";
            var t = new ScriptedTransport(new Dictionary<string, object?[]>
            {
                { a, new object?[] { "down", Fakes.Err(3, "x"), Fakes.Ok(1L) } }, { b, new object?[0] },
            });
            var c = new RpcClient(new[] { a, b }, t, clock: () => 1000);
            var e = Assert.ThrowsAsync<RpcUnavailable>(() => c.CallAt(a, "m", Fakes.L()))!;
            Assert.That(e.Message, Is.EqualTo("https://a failed for m"));
            // call_at/try_at ask the endpoint even while it cools down
            var err = await c.TryAt(a, "m", Fakes.L());
            Assert.That(err!.IsError, Is.True);
            Assert.That(err.Error!.PyStr, Is.EqualTo("3: x"));
            Assert.That(err.Value, Is.SameAs(err.Error));
            var ok = await c.TryAt(a, "m", Fakes.L());
            Assert.That((ok!.IsError, ok.ResultValue), Is.EqualTo((false, (object)1L)));
            Assert.That(c.CooldownOf(a), Is.EqualTo(0.0));
            Assert.That(await Client(new Dictionary<string, Handler> { { A, Fakes.Down } }).c.TryAt(A, "m", Fakes.L()), Is.Null);
        }

        [Test]
        public async Task BlockHashAndBlockNumberFormsMatchPython()
        {
            const string a = "https://a";
            var t = new ScriptedTransport(new Dictionary<string, object?[]>
            {
                { a, new object?[] { Fakes.Ok(new JsonObject { { "hash", "0xAB" } }), Fakes.Ok(new JsonObject { { "hash", "0xCD" } }), Fakes.Ok(" 0X_1F "), Fakes.Ok("\u0661\u0662") } },
            });
            var c = new RpcClient(new[] { a }, t, clock: () => 0);
            Assert.That(await c.GetBlockHash(-5), Is.EqualTo("0xab"));
            Assert.That(await c.GetBlockHash(1L << 40), Is.EqualTo("0xcd"));
            Assert.That(await c.BlockNumber(), Is.EqualTo(31L));
            Assert.That(await c.BlockNumber(), Is.EqualTo(18L));
            Assert.That(JsonEq.FirstDiff(t.Log.Select(x => (object?)x.Params).ToList(),
                Fakes.L(Fakes.L("-0x5", false), Fakes.L("0x10000000000", false), Fakes.L(), Fakes.L())), Is.Null);
        }

        // ---- the continuations never block ----

        /// <summary>A transport whose answers arrive later, as a frame-driven transport's do.</summary>
        private sealed class DeferredTransport : ITransport
        {
            public readonly List<(string Url, TaskCompletionSource<object?> Answer)> Pending = new List<(string, TaskCompletionSource<object?>)>();

            public Task<object?> PostJson(string url, JsonObject payload, double timeout)
            {
                var tcs = new TaskCompletionSource<object?>();
                Pending.Add((url, tcs));
                return tcs.Task;
            }
        }

        [Test]
        public void ACallWaitsForEachAnswerWithoutBlocking()
        {
            var t = new DeferredTransport();
            var c = new RpcClient(new[] { A, B }, t, clock: () => 0);
            var call = c.BlockNumber();
            Assert.That(call.IsCompleted, Is.False);
            Assert.That(t.Pending.Select(p => p.Url), Is.EqualTo(new[] { A }));
            t.Pending[0].Answer.SetException(new TransportError("down"));
            Assert.That(call.IsCompleted, Is.False);
            Assert.That(t.Pending.Select(p => p.Url), Is.EqualTo(new[] { A, B }));
            t.Pending[1].Answer.SetResult(Fakes.Ok("0x2a"));
            Assert.That(call.IsCompleted, Is.True);
            Assert.That(call.Result, Is.EqualTo(42L));
        }
    }

    public class SolanaRpcTests
    {
        private const string A = "https://a.example";
        private const string B = "https://b.example";

        private static (SolanaRpc rpc, FakeTransport t) Client(Dictionary<string, Handler> handlers)
        {
            var t = new FakeTransport(handlers);
            var c = new RpcClient(handlers.Keys.ToList(), t, clock: () => 0);
            return (new SolanaRpc(c), t);
        }

        private static Handler Ok(object? r) => (m, p) => Fakes.Ok(r);

        private static long MaxVersion(List<object?> p) => (long)((JsonObject)p[1]!)["maxSupportedTransactionVersion"]!;

        [Test]
        public async Task GetTransactionAsksForNewestVersionAndJson()
        {
            var (rpc, t) = Client(new Dictionary<string, Handler> { { A, Ok(new JsonObject { { "slot", 5L } }) } });
            Assert.That(JsonEq.FirstDiff(await rpc.GetTransaction("SIG"), new JsonObject { { "slot", 5L } }), Is.Null);
            Assert.That(t.Calls[0].Method, Is.EqualTo("getTransaction"));
            Assert.That(JsonEq.FirstDiff(t.Calls[0].Params, Fakes.L("SIG", new JsonObject
            {
                { "encoding", "json" }, { "commitment", "confirmed" }, { "maxSupportedTransactionVersion", Scfg.Core.Solana.SolanaRpcErrors.TxVersion },
            })), Is.Null);
        }

        [Test]
        public async Task GetTransactionLowersVersionWhenTheNodeRejectsTheParameter()
        {
            Handler h = (m, p) => MaxVersion(p) == 1
                ? Fakes.Err(-32602, "Invalid params: unknown maxSupportedTransactionVersion 1")
                : Fakes.Ok(new JsonObject { { "slot", 5L } });
            var (rpc, _) = Client(new Dictionary<string, Handler> { { A, h } });
            Assert.That(JsonEq.FirstDiff(await rpc.GetTransaction("SIG"), new JsonObject { { "slot", 5L } }), Is.Null);
            Assert.That(rpc.TxVersion, Is.EqualTo(0L));
        }

        [Test]
        public async Task TryAtEachLiveUrlReportsPerEndpointAndDeadOnesCoolDown()
        {
            var (rpc, _) = Client(new Dictionary<string, Handler> { { A, Fakes.Down }, { B, (m, p) => Fakes.Err(-1, "boom") } });
            var c = rpc.Client;
            var outs = new List<(string, RpcAttempt)>();
            foreach (var url in c.LiveUrls())
            {
                var res = await c.TryAt(url, "getSlot", Fakes.L());
                if (res != null) outs.Add((url, res));
            }
            Assert.That(outs.Count, Is.EqualTo(1));
            Assert.That(outs[0].Item1, Is.EqualTo(B));
            Assert.That(outs[0].Item2.IsError && outs[0].Item2.Value is RpcError, Is.True);
            Assert.That(c.LiveUrls(), Is.EqualTo(new[] { B }));
        }

        private static Dictionary<string, Handler> History(long firstA, long firstB, object? txB = null)
        {
            Handler Make(long first, object? tx) => (m, p) => m == "getFirstAvailableBlock" ? Fakes.Ok(first) : Fakes.Ok(tx);
            return new Dictionary<string, Handler> { { A, Make(firstA, null) }, { B, Make(firstB, txB) } };
        }

        [Test]
        public async Task GetTransactionAnyReturnsFirstFound()
        {
            var (rpc, _) = Client(History(1000000000, 0, new JsonObject { { "slot", 50L } }));
            var (tx, authoritative) = await rpc.GetTransactionAny("SIG", minSlot: 50);
            Assert.That(JsonEq.FirstDiff(tx, new JsonObject { { "slot", 50L } }), Is.Null);
            Assert.That(authoritative, Is.False);
        }

        [Test]
        public async Task NullFromFullHistoryEndpointIsAuthoritative()
        {
            var (rpc, _) = Client(History(1000000000, 0));
            Assert.That(await rpc.GetTransactionAny("SIG", minSlot: 50), Is.EqualTo(((object?)null, true)));
        }

        [Test]
        public async Task NullOnlyFromPrunedEndpointsIsNotAuthoritative()
        {
            var (rpc, _) = Client(History(1000000000, 1000000000));
            Assert.That(await rpc.GetTransactionAny("SIG", minSlot: 50), Is.EqualTo(((object?)null, false)));
        }

        [Test]
        public async Task UnreadableFirstAvailableBlockCountsAsPruned()
        {
            Handler garbage = (m, p) => m == "getFirstAvailableBlock" ? Fakes.Ok("abc") : Fakes.Ok(null);
            var (rpc, t) = Client(new Dictionary<string, Handler> { { A, garbage }, { B, garbage } });
            Assert.That(await rpc.GetTransactionAny("SIG", minSlot: 50), Is.EqualTo(((object?)null, false)));
            // not cached: asked again next time
            await rpc.GetTransactionAny("SIG", minSlot: 50);
            Assert.That(t.Calls.Count(x => x.Method == "getFirstAvailableBlock"), Is.EqualTo(4));
        }

        [Test]
        public void GetTransactionAnyWithoutAnswersRaises()
        {
            var (rpc, _) = Client(new Dictionary<string, Handler> { { A, Fakes.Down }, { B, Fakes.Down } });
            var e = Assert.ThrowsAsync<RpcUnavailable>(() => rpc.GetTransactionAny("SIG", minSlot: 1))!;
            Assert.That(e.Message, Is.EqualTo("no endpoint answered getTransaction for SIG"));
        }

        [Test]
        public async Task GetTransactionAnyDefaultCommitmentIsFinalized()
        {
            var (rpc, t) = Client(new Dictionary<string, Handler> { { A, Ok(new JsonObject { { "slot", 1L } }) } });
            await rpc.GetTransactionAny("SIG");
            Assert.That(((JsonObject)t.Calls[0].Params[1]!)["commitment"], Is.EqualTo("finalized"));
        }

        [Test]
        public async Task SmallHelpers()
        {
            var answers = new Dictionary<string, object?>
            {
                { "getSlot", 77L },
                { "getSignaturesForAddress", Fakes.L(new JsonObject { { "signature", "S2" } }, new JsonObject { { "signature", "S1" } }) },
                { "getSignatureStatuses", new JsonObject { { "value", Fakes.L(null, new JsonObject { { "confirmationStatus", "finalized" } }) } } },
                { "getAccountInfo", new JsonObject { { "value", null } } },
                { "getLatestBlockhash", new JsonObject { { "value", new JsonObject { { "blockhash", "BH" }, { "lastValidBlockHeight", 9L } } } } },
                { "getBalance", new JsonObject { { "value", 1234L } } },
                {
                    "getRecentPrioritizationFees",
                    Fakes.L(new JsonObject { { "slot", 1L }, { "prioritizationFee", 9L } }, new JsonObject { { "slot", 1L }, { "prioritizationFee", 1L } },
                        new JsonObject { { "slot", 1L }, { "prioritizationFee", 5L } })
                },
            };
            var (rpc, t) = Client(new Dictionary<string, Handler> { { A, (m, p) => Fakes.Ok(answers[m]) } });
            Assert.That(await rpc.GetSlot(), Is.EqualTo(77L));
            var sigs = (List<object?>)(await rpc.GetSignaturesForAddress("X", until: "S0"))!;
            Assert.That(sigs.Select(s => ((JsonObject)s!)["signature"]), Is.EqualTo(new[] { "S2", "S1" }));
            Assert.That(JsonEq.FirstDiff(t.Calls.Last().Params, Fakes.L("X", new JsonObject { { "limit", 100L }, { "commitment", "confirmed" }, { "until", "S0" } })), Is.Null);
            var statuses = (List<object?>)(await rpc.GetSignatureStatuses(new[] { "a", "b" }))!;
            Assert.That(((JsonObject)statuses[1]!)["confirmationStatus"], Is.EqualTo("finalized"));
            Assert.That(JsonEq.FirstDiff(t.Calls.Last().Params, Fakes.L(Fakes.L("a", "b"), new JsonObject { { "searchTransactionHistory", true } })), Is.Null);
            Assert.That(await rpc.AccountExists("X"), Is.False);
            Assert.That(await rpc.GetTokenBalance("X"), Is.EqualTo(BigInteger.Zero));
            Assert.That(await rpc.GetLatestBlockhash(), Is.EqualTo("BH"));
            Assert.That(await rpc.GetBalance("X"), Is.EqualTo(new BigInteger(1234)));
            Assert.That(await rpc.GetPriorityFee(new[] { "X" }), Is.EqualTo(new BigInteger(5)));
            Assert.That(JsonEq.FirstDiff(t.Calls.Last().Params, Fakes.L(Fakes.L("X"))), Is.Null);
        }

        [Test]
        public async Task SignatureOptionsBeforeAndEmptyResult()
        {
            var (rpc, t) = Client(new Dictionary<string, Handler> { { A, Ok(null) } });
            Assert.That(await rpc.GetSignaturesForAddress("X", before: "B1", limit: 5, commitment: "finalized"), Is.EqualTo(new List<object?>()));
            Assert.That(JsonEq.FirstDiff(t.Calls[0].Params, Fakes.L("X", new JsonObject { { "limit", 5L }, { "commitment", "finalized" }, { "before", "B1" } })), Is.Null);
        }

        [Test]
        public async Task EmptyResultsReadAsEmptyListsLikePythonOr()
        {
            foreach (var empty in new object?[] { new JsonObject(), "", 0L, false })
            {
                var (rpc, _) = Client(new Dictionary<string, Handler> { { A, Ok(empty) } });
                Assert.That(await rpc.GetSignaturesForAddress("X"), Is.EqualTo(new List<object?>()));
                Assert.That(await rpc.GetPriorityFee(new[] { "X" }), Is.EqualTo(BigInteger.Zero));
            }
        }

        [Test]
        public async Task TokenBalanceReadsParsedAmount()
        {
            var body = new JsonObject
            {
                { "value", new JsonObject { { "data", new JsonObject { { "parsed", new JsonObject { { "info", new JsonObject { { "tokenAmount", new JsonObject { { "amount", "18446744073709551615" } } } } } } } } } } },
            };
            var (rpc, _) = Client(new Dictionary<string, Handler> { { A, Ok(body) } });
            Assert.That(await rpc.GetTokenBalance("T"), Is.EqualTo(BigInteger.Parse("18446744073709551615", System.Globalization.CultureInfo.InvariantCulture)));
        }

        private const string V2 = "Transaction version (2) is not supported by the requesting client. Please try the "
                                  + "request again with the following configuration parameter: \"maxSupportedTransactionVersion\": 2";

        private static Handler Versioned(long needs) => (m, p) =>
        {
            if (m == "getFirstAvailableBlock") return Fakes.Ok(0L);
            var n = needs.ToString(System.Globalization.CultureInfo.InvariantCulture);
            if (MaxVersion(p) < needs) return Fakes.Err(-32015, V2.Replace("(2)", "(" + n + ")").Replace(": 2", ": " + n));
            return Fakes.Ok(new JsonObject { { "slot", 5L } });
        };

        [Test]
        public async Task GetTransactionRetriesOnceWithTheVersionTheNodeNames()
        {
            var (rpc, t) = Client(new Dictionary<string, Handler> { { A, Versioned(2) } });
            Assert.That(JsonEq.FirstDiff(await rpc.GetTransaction("SIG"), new JsonObject { { "slot", 5L } }), Is.Null);
            Assert.That(t.Calls.Select(c => MaxVersion(c.Params)), Is.EqualTo(new[] { Scfg.Core.Solana.SolanaRpcErrors.TxVersion, 2L }));
            Assert.That(rpc.TxVersion, Is.EqualTo(Scfg.Core.Solana.SolanaRpcErrors.TxVersion));
        }

        [Test]
        public void UnsupportedVersionErrorNamingAnOlderVersionIsRaised()
        {
            var (rpc, t) = Client(new Dictionary<string, Handler> { { A, (m, p) => Fakes.Err(-32015, V2.Replace("(2)", "(0)")) } });
            Assert.ThrowsAsync<RpcError>(() => rpc.GetTransaction("SIG"));
            Assert.That(t.Calls.Count, Is.EqualTo(1));
        }

        [Test]
        public void NamedVersionIgnoresNumbersBeyond18Digits()
        {
            var rows = new (string, long?)[]
            {
                ("Transaction version (" + new string('9', 19) + ") is not supported", null),
                ("Transaction version (" + new string('9', 4301) + ") is not supported", null),
                ("Transaction version (" + new string('9', 1_000_000) + ") is not supported", null),
                ("Transaction version (" + new string('9', 15) + ") is not supported", 999_999_999_999_999),
                ("Transaction version (" + new string('0', 30) + "3) is not supported", 3),
                ("version (" + new string('9', 19) + ") version (5)", null),
                ("version (12 version (5)", 5),
                ("version (\u0663)", 3),
                ("Transaction VER\u017fION (2) x", 2),
                ("vers\u0131on (4)", 4),
                ("vers\u0130on (4)", 4),
                ("Version (2", null),
                ("version(2)", null),
                ("version (x) version (6)", 6),
                ("version (1)", null),
                ("version (\U00011F53)", null), // a Kawi digit (Unicode 15): not a digit in Python 3.9
                ("version (\U0001D7D1)", 3), // a mathematical bold digit: outside the BMP, a digit in Python 3.9
                ("VERSION (\uff17)", 7),
            };
            foreach (var (message, expected) in rows)
                Assert.That(Scfg.Core.Solana.SolanaRpcErrors.NamedVersion(new RpcError(-32015, message), 1), Is.EqualTo(expected), message.Substring(0, Math.Min(40, message.Length)));
            Assert.That(Scfg.Core.Solana.SolanaRpcErrors.NamedVersion(new RpcError(-32000, "version (5)"), 1), Is.Null, "only for -32015");
            Assert.That(Scfg.Core.Solana.SolanaRpcErrors.NamedVersion(new RpcError(-32015, "version (5)"), 5), Is.Null, "not newer");
        }

        [Test]
        public void AnOverlongNamedVersionRaisesTheNodeError()
        {
            var (rpc, t) = Client(new Dictionary<string, Handler> { { A, (m, p) => Fakes.Err(-32015, V2.Replace("(2)", "(" + new string('9', 4301) + ")")) } });
            Assert.ThrowsAsync<RpcError>(() => rpc.GetTransaction("SIG"));
            Assert.That(t.Calls.Count, Is.EqualTo(1));
        }

        [Test]
        public async Task GetTransactionAnyRetriesWithTheNamedVersion()
        {
            var (rpc, _) = Client(new Dictionary<string, Handler> { { A, Versioned(3) } });
            var (tx, authoritative) = await rpc.GetTransactionAny("SIG", minSlot: 1);
            Assert.That(JsonEq.FirstDiff(tx, new JsonObject { { "slot", 5L } }), Is.Null);
            Assert.That(authoritative, Is.False);
        }

        [Test]
        public async Task GetTransactionAnyOverThreeEndpointsMatchesPython()
        {
            // Python: scen.py "sol_any"
            const string a = "https://a", b = "https://b", c = "https://c";
            var t = new ScriptedTransport(new Dictionary<string, object?[]>
            {
                { a, new object?[] { "down" } },
                { b, new object?[] { Fakes.Err(-32015, "Transaction version (3) is not supported"), Fakes.Ok(null), Fakes.Ok(5L) } },
                { c, new object?[] { Fakes.Ok(null), Fakes.Ok("bad") } },
            });
            var rpc = new SolanaRpc(new RpcClient(new[] { a, b, c }, t, clock: () => 1000));
            Assert.That(await rpc.GetTransactionAny("SIG", minSlot: 10), Is.EqualTo(((object?)null, true)));
            Assert.That(t.Log.Select(x => (x.Url.Substring(8), x.Id, x.Method,
                x.Method == "getTransaction" ? MaxVersion(x.Params) : -1L)), Is.EqualTo(new[]
            {
                ("a", 1L, "getTransaction", 1L), ("b", 2L, "getTransaction", 1L), ("b", 3L, "getTransaction", 3L),
                ("b", 4L, "getFirstAvailableBlock", -1L), ("c", 5L, "getTransaction", 1L), ("c", 6L, "getFirstAvailableBlock", -1L),
            }));
            Assert.That(((JsonObject)t.Log[0].Params[1]!)["commitment"], Is.EqualTo("finalized"));
        }

        [Test]
        public async Task PriorityFeeAndSignatureFormsMatchPython()
        {
            // Python: scen.py "sol_misc"
            const string a = "https://a";
            var t = new ScriptedTransport(new Dictionary<string, object?[]>
            {
                {
                    a, new object?[]
                    {
                        Fakes.Ok(new JsonObject { { "a", 1L } }),
                        Fakes.Ok(Fakes.L(new JsonObject { { "prioritizationFee", "7" } }, new JsonObject { { "prioritizationFee", 3L } },
                            new JsonObject { { "prioritizationFee", 5.9 } }, new JsonObject { { "prioritizationFee", true } })),
                        Fakes.Ok(new JsonObject { { "x", 1L } }),
                    }
                },
            });
            var rpc = new SolanaRpc(new RpcClient(new[] { a }, t, clock: () => 1000));
            var e = Assert.ThrowsAsync<TypeError>(() => rpc.GetPriorityFee(new[] { "X" }))!;
            Assert.That(e.Message, Is.EqualTo("string indices must be integers"));
            Assert.That(await rpc.GetPriorityFee(new[] { "X" }), Is.EqualTo(new BigInteger(5)));
            Assert.That(JsonEq.FirstDiff(await rpc.GetSignaturesForAddress("X", until: "", before: "B", limit: 3), new JsonObject { { "x", 1L } }), Is.Null);
            Assert.That(JsonEq.FirstDiff(t.Log[2].Params, Fakes.L("X", new JsonObject { { "limit", 3L }, { "commitment", "confirmed" }, { "before", "B" } })), Is.Null);
        }

        [Test]
        public void MalformedRepliesRaiseWherePythonRaises()
        {
            var rows = new (string, object?, Func<SolanaRpc, Task>)[]
            {
                ("account_exists_missing_value", new JsonObject(), r => r.AccountExists("X")),
                ("account_exists_non_dict", null, r => r.AccountExists("X")),
                ("signature_statuses_missing_value", new JsonObject(), r => r.GetSignatureStatuses(new[] { "a" })),
                ("latest_blockhash_missing_value", new JsonObject(), r => r.GetLatestBlockhash()),
                ("latest_blockhash_missing_blockhash", new JsonObject { { "value", new JsonObject() } }, r => r.GetLatestBlockhash()),
                ("token_balance_missing_value", new JsonObject(), r => r.GetTokenBalance("X")),
                ("token_balance_missing_nested", new JsonObject { { "value", new JsonObject { { "data", new JsonObject() } } } }, r => r.GetTokenBalance("X")),
                ("token_balance_non_object_nested", new JsonObject { { "value", new JsonObject { { "data", "x" } } } }, r => r.GetTokenBalance("X")),
                ("balance_missing_value", new JsonObject(), r => r.GetBalance("X")),
                ("priority_fee_missing_field", Fakes.L(new JsonObject { { "slot", 1L } }), r => r.GetPriorityFee(new[] { "X" })),
            };
            foreach (var (name, body, run) in rows)
            {
                var (rpc, _) = Client(new Dictionary<string, Handler> { { A, Ok(body) } });
                var e = Assert.CatchAsync(() => run(rpc))!;
                Assert.That(e is KeyError || e is TypeError, Is.True, name + ": " + e.GetType().Name);
            }
        }
    }
}
