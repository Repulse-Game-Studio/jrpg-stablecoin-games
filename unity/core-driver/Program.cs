// No Python twin: the JSON-lines driver that runs the C# Engine as a child process for the Python integration
// tests (tests/integration/cs_driver.py), so the same Anvil / solana-test-validator fixtures exercise every core.
//
// Protocol: rpgmaker/test/integration/engine_driver.ts. One JSON command per stdin line, one JSON reply per stdout
// line, in order: {"ok": <result>} or {"error": {"type", "code", "message"}}. Nothing but replies goes to stdout;
// diagnostics go to stderr. Ops: init, transport_stats, start_session, report_hint, tick, reverify, restore_lookup,
// restore_apply, build_solana_payment, snapshot, set_receipt, clear_receipts. Results are encoded exactly like the
// Tier 2 `returns` / `snapshot`.
#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Scfg.Core;
using Scfg.Core.Tests;

namespace Scfg.Core.Driver
{
    public sealed class DriverError : Exception
    {
        public DriverError(string message) : base(message) { }
    }

    public static class Program
    {
        private static Engine? _engine;
        private static HttpClientTransport _transport = new HttpClientTransport();

        /// <summary>
        /// Points one built-in registry entry at a local node through the core's internal hook (the registries are
        /// read-only to games). Each init starts from the built-in registries, so an earlier override never leaks.
        /// </summary>
        private static void ApplyOverride(JsonObject o)
        {
            bool testnet = Opt(o, "network") as string == "testnet";
            var registry = Chains.GetChains(testnet);
            var key = Opt(o, "key") as string;
            if (key == null || !registry.TryGetValue(key, out var baseChain)) throw new DriverError("unknown chain " + Json.Dumps(Opt(o, "key")));
            var rpc = (Opt(o, "rpc") as List<object?> ?? throw new DriverError("override.rpc must be a list")).Select(u => (string)u!).ToList();
            long? confirmations = Opt(o, "confirmations") as long?;
            Dictionary<string, Token>? tokens = null;
            if (Opt(o, "tokens") is JsonObject t && t.Count > 0)
            {
                tokens = new Dictionary<string, Token>();
                foreach (var kv in t) tokens[kv.Key] = Chains.MakeToken((string)kv.Value!);
            }
            Chains.Override(testnet, baseChain.With(rpc, confirmations, tokens));
        }

        /// <summary>A missing key reads as null (JavaScript's <c>cmd.key</c>), not as Py.Get's KeyError.</summary>
        private static object? Opt(JsonObject d, string key) => d.TryGetValue(key, out var v) ? v : null;

        private static Engine Need() => _engine ?? throw new DriverError("init first");

        private static async Task<object?> Run(JsonObject cmd)
        {
            var op = Opt(cmd, "op") as string ?? throw new DriverError("a command needs a string op");
            if (op == "init")
            {
                Chains.ResetRegistries();
                if (Opt(cmd, "override") is JsonObject o) ApplyOverride(o);
                var setup = new JsonObject
                {
                    { "config", Opt(cmd, "config") },
                    { "custom_chains", Opt(cmd, "custom_chains") ?? new List<object?>() },
                    { "items", Opt(cmd, "items") ?? new List<object?>() },
                };
                _transport = new HttpClientTransport();
                // Whole seconds: the Tier 2 encoders (reused below) refuse the fractional timestamps of a wall clock.
                _engine = new Engine(Tier2.BuildConfigFrom(setup), _transport, null, null, () => DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                                     null, e => Console.Error.WriteLine("engine error: " + e));
                return null;
            }
            if (op == "transport_stats") return new JsonObject { { "kind", "HttpClientTransport" }, { "posts", (long)_transport.Posts } };
            var engine = Need();
            switch (op)
            {
                case "start_session":
                {
                    var opts = new JsonObject();
                    foreach (var kv in cmd) if (kv.Key != "op" && kv.Key != "kind" && kv.Key != "chain") opts[kv.Key] = kv.Value;
                    var kind = Opt(cmd, "kind") as string ?? throw new TypeError("kind must be a str");
                    var chain = Opt(cmd, "chain") as string ?? throw new TypeError("chain must be a str");
                    return Tier2.EncodeReturns(op, await engine.StartSession(kind, chain, opts));
                }
                case "report_hint":
                    engine.ReportHint(Opt(cmd, "sid") as string ?? throw new TypeError("sid must be a str"), Opt(cmd, "tx"));
                    return null;
                case "tick":
                    return Tier2.EncodeReturns(op, await engine.Tick());
                case "reverify":
                    return Tier2.EncodeReturns(op, await engine.Reverify());
                case "restore_lookup":
                    return Tier2.EncodeReturns(op, await engine.RestoreLookup(Opt(cmd, "tx") as string ?? throw new TypeError("tx must be a str")));
                case "restore_apply":
                {
                    Dictionary<int, object?>? choices = null;
                    if (Opt(cmd, "choices") is JsonObject cs)
                    {
                        choices = new Dictionary<int, object?>();
                        foreach (var kv in cs) choices[(int)Py.Int(kv.Key)] = kv.Value;
                    }
                    var list = Opt(cmd, "candidates") as List<object?> ?? throw new TypeError("candidates must be a list");
                    var candidates = list.Select(c => Tier2.CandidateFromJson(c as JsonObject ?? throw new TypeError("a candidate must be an object"))).ToList();
                    return Tier2.EncodeReturns(op, engine.RestoreApply(candidates, choices));
                }
                case "build_solana_payment":
                    return Tier2.EncodeReturns(op, await engine.BuildSolanaPayment(
                        Opt(cmd, "sid") as string ?? throw new TypeError("sid must be a str"),
                        Opt(cmd, "account") as string ?? throw new TypeError("account must be a str")));
                case "snapshot":
                    return Tier2.SnapshotJson(engine);
                case "set_receipt":
                    engine.Receipts[Opt(cmd, "key") as string ?? throw new TypeError("key must be a str")] = Opt(cmd, "receipt");
                    return null;
                case "clear_receipts":
                    foreach (var k in engine.Receipts.Keys.ToList()) engine.Receipts.Remove(k);
                    return null;
                default:
                    throw new DriverError("unknown op " + Json.Dumps(op));
            }
        }

        /// <summary>The error as JSON: its class, its code (ConfigError/RestoreError strings, RpcError numbers) or null.</summary>
        private static JsonObject Describe(Exception err)
        {
            object? code = err switch
            {
                ConfigError c => c.Code,
                RestoreError r => r.Code,
                RpcError rpc => rpc.Code,
                _ => null,
            };
            return new JsonObject { { "type", err.GetType().Name }, { "code", code }, { "message", err.Message } };
        }

        public static async Task Main()
        {
            var stdin = new StreamReader(Console.OpenStandardInput(), new UTF8Encoding(false));
            var stdout = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
            string? line;
            while ((line = stdin.ReadLine()) != null) // sequential: the next command waits for the previous reply
            {
                if (line.Trim().Length == 0) continue;
                string reply;
                try
                {
                    var cmd = Json.Loads(line) as JsonObject ?? throw new DriverError("not a JSON object: " + (line.Length > 200 ? line.Substring(0, 200) : line));
                    reply = Json.Dumps(new JsonObject { { "ok", await Run(cmd) } });
                }
                catch (Exception err)
                {
                    reply = Json.Dumps(new JsonObject { { "error", Describe(err) } });
                }
                stdout.WriteLine(reply);
            }
        }
    }
}
