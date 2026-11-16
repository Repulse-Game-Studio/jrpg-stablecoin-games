// No Python twin: the Tier 2 runner, replaying conformance/vectors/tier2/*.json against the C# Engine. A mirror of
// conformance/python/scfg_conformance/replay.py (build_config_from, build_engine, run_action, encode_returns,
// snapshot_json, ScriptedEntropy, ReplayTransport, replay) and of conformance/js/tier2.ts; conformance/README.md's
// "Tier 2" section is the contract.
//
// This file and JsonEq.cs do not reference NUnit: the console driver compiles them too. Replay returns the first
// difference as a string (null when the vector passes) and the *Tests.cs files assert on it.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Numerics;
using System.Threading.Tasks;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    /// <summary>A difference the runner found while a step ran (Python's AssertionError inside run_action).</summary>
    public sealed class ReplayError : Exception
    {
        public ReplayError(string message) : base(message) { }
    }

    /// <summary>Hands out one step's recorded draws in order; any difference is a problem.</summary>
    public sealed class ScriptedEntropy : IEntropy
    {
        private readonly Queue<JsonObject> _draws;
        public List<string> Problems { get; } = new List<string>();

        public ScriptedEntropy(IEnumerable<object?> draws)
        {
            _draws = new Queue<JsonObject>(draws.Select(d => d as JsonObject ?? throw new ReplayError("an entropy draw is not an object")));
        }

        private object? Take(string fn, long n)
        {
            if (_draws.Count == 0)
            {
                Problems.Add("entropy exhausted at " + fn + "(" + n.ToString(CultureInfo.InvariantCulture) + ")");
                throw new ReplayError(Problems[Problems.Count - 1]);
            }
            var draw = _draws.Dequeue();
            if (!(draw.Get("fn") is string dfn) || dfn != fn || !(draw.Get("n") is long dn) || dn != n)
            {
                Problems.Add("entropy mismatch: drew " + fn + "(" + n.ToString(CultureInfo.InvariantCulture) + "), recorded "
                             + Coio.Str(draw.Get("fn")) + "(" + Coio.Str(draw.Get("n")) + ")");
                throw new ReplayError(Problems[Problems.Count - 1]);
            }
            return draw.Get("value");
        }

        public string TokenHex(int n) => Take("token_hex", n) as string ?? throw new ReplayError("a token_hex value is not a string");

        public long RandBelow(long n) => Take("randbelow", n) is long v ? v : throw new ReplayError("a randbelow value is not an int");

        public byte[] TokenBytes(int n) =>
            Bytes.FromHex(Take("token_bytes", n) as string ?? throw new ReplayError("a token_bytes value is not a string"));

        public List<string> Leftover()
        {
            if (_draws.Count == 0) return new List<string>();
            var left = _draws.Select(d => "(" + Py.Repr(d.Get("fn")) + ", " + Py.Repr(d.Get("n")) + ")");
            return new List<string> { "unconsumed entropy: [" + string.Join(", ", left) + "]" };
        }
    }

    /// <summary>Swaps the draws per step while the Engine keeps one IEntropy.</summary>
    public sealed class StepEntropy : IEntropy
    {
        public ScriptedEntropy Current { get; set; } = new ScriptedEntropy(new List<object?>());
        public string TokenHex(int n) => Current.TokenHex(n);
        public long RandBelow(long n) => Current.RandBelow(n);
        public byte[] TokenBytes(int n) => Current.TokenBytes(n);
    }

    /// <summary>
    /// Answers each request from the current step's recorded calls, as a multiset keyed by (url, method, params):
    /// any order, each recorded call used once, identical requests take their copies in recorded order. The JSON-RPC
    /// id is ignored; params match by JSON equality (<see cref="JsonEq"/>: key order ignored, 1 equals 1.0, true is
    /// not 1). A recorded transport_error is the transport's failure (a TransportError).
    /// </summary>
    public sealed class ReplayTransport : ITransport
    {
        private List<JsonObject> _calls = new List<JsonObject>();
        private bool[] _used = new bool[0];
        public List<string> Problems { get; private set; } = new List<string>();

        /// <summary>Called first on every request (url, payload).</summary>
        public Action<string, JsonObject>? OnCall { get; set; }

        /// <summary>
        /// When set, every answer is handed to it as a function that produces the response (or throws the
        /// transport's failure), and the task it returns is the request's. Unset, the answer is an already
        /// completed task.
        /// </summary>
        public Func<Func<object?>, Task<object?>>? Deliver { get; set; }

        public void Load(IEnumerable<object?> calls)
        {
            _calls = calls.Select(c => c as JsonObject ?? throw new ReplayError("a recorded call is not an object")).ToList();
            _used = new bool[_calls.Count];
            Problems = new List<string>();
        }

        private static string Canonical(object? v)
        {
            try { return Json.Dumps(v, sortKeys: true, ensureAscii: false); }
            catch (Exception) { return "<unprintable>"; }
        }

        public Task<object?> PostJson(string url, JsonObject payload, double timeout)
        {
            OnCall?.Invoke(url, payload);
            var method = payload.Get("method");
            var prms = payload.Get("params");
            for (int i = 0; i < _calls.Count; i++)
            {
                var c = _calls[i];
                if (_used[i] || !(c.Get("url") is string u) || u != url || !Equals(c.Get("method"), method)
                    || !JsonEq.Equal(c.Get("params"), prms)) continue;
                _used[i] = true;
                Func<object?> answer = () =>
                {
                    if (c.Get("transport_error") is JsonObject err)
                    {
                        var status = err.Get("status");
                        throw new TransportError(err.Get("message") as string ?? "",
                                                 status is long s ? (int)s : (int?)null);
                    }
                    return Json.Loads(c.Get("response_text") as string ?? throw new ReplayError("a recorded call has no response_text"));
                };
                if (Deliver != null) return Deliver(answer);
                try
                {
                    return Task.FromResult(answer());
                }
                catch (Exception e)
                {
                    return Task.FromException<object?>(e);
                }
            }
            Problems.Add("unexpected call " + url + " " + Coio.Str(method) + " " + Canonical(prms));
            return Task.FromException<object?>(new ReplayError(Problems[Problems.Count - 1]));
        }

        public List<string> Leftover()
        {
            var o = new List<string>();
            for (int i = 0; i < _calls.Count; i++)
                if (!_used[i])
                    o.Add("expected call not made: " + Coio.Str(_calls[i].Get("url")) + " " + Coio.Str(_calls[i].Get("method"))
                          + " " + Canonical(_calls[i].Get("params")));
            return o;
        }
    }

    public static class Tier2
    {
        public static readonly string[] Actions =
        {
            "start_session", "build_solana_payment", "report_hint", "set_ui_status", "discard_session", "tick",
            "reverify", "restore_lookup", "restore_apply",
        };

        /// <summary>Id prefixes of the vector groups the C# core runs. A vector outside them is ignored, never passed.</summary>
        public static readonly IReadOnlyList<string> Enabled = new[] { "smoke/", "evm/", "config/", "custom_chains/", "rpc/" };

        /// <summary>
        /// Vectors of an enabled group that need code a later task ports, keyed by id, with the number of that task:
        /// 11 is the Solana backend, 12 reverify and restore. They are ignored with that reason, never passed.
        /// Task 12 removes this map and <see cref="Enabled"/>.
        /// </summary>
        public static readonly IReadOnlyDictionary<string, int> Deferred = new Dictionary<string, int>(StringComparer.Ordinal)
        {
            { "smoke/solana_unlock_paid", 11 }, // a Solana session
            { "config/rpc_and_confirmation_overrides", 11 }, // Solana sessions beside an EVM one
            { "evm/arc_large_restore", 12 }, // restore_lookup, restore_apply and reverify
            { "evm/arc_restore_and_reverify", 12 }, // restore_lookup and reverify
            { "evm/config_overrides_and_full_registry", 12 }, // reverify
            { "evm/restore_through_engine", 12 }, // restore_lookup and restore_apply
            { "evm/reverify_accepted_wallets", 12 }, // reverify
            { "evm/unwatchable_session_closes_at_horizon", 12 }, // reverify, whose readiness check is Solana's (task 11)
            { "custom_chains/pay_restore_reverify", 12 }, // restore_lookup and reverify
            { "custom_chains/removed_chain_receipt_kept", 12 }, // reverify
        };

        public static bool IsEnabled(string id) => Enabled.Any(p => id.StartsWith(p, StringComparison.Ordinal));

        /// <summary>Whether the runner replays this vector now: in an enabled group and not deferred.</summary>
        public static bool Runs(string id) => IsEnabled(id) && !Deferred.ContainsKey(id);

        /// <summary>A vector file's text as the runner reads it (Python's json.load).</summary>
        public static JsonObject Parse(string text) =>
            Json.Loads(text) as JsonObject ?? throw new ReplayError("a vector is not a JSON object");

        // ---- values that JSON cannot carry exactly --------------------------------------------------------------

        /// <summary>replay.py's decode_value: <c>{"float": "&lt;repr&gt;"}</c> is that float, anything else is itself.</summary>
        public static object? DecodeValue(object? v)
        {
            if (v is JsonObject o && o.Count == 1 && o.TryGetValue("float", out var repr) && repr is string text)
            {
                switch (text)
                {
                    case "inf": return double.PositiveInfinity;
                    case "-inf": return double.NegativeInfinity;
                    case "nan": return double.NaN;
                    default: return double.Parse(text, NumberStyles.Float, CultureInfo.InvariantCulture);
                }
            }
            return v;
        }

        private static JsonObject Decoded(object? d)
        {
            if (!(d is JsonObject o)) throw new ReplayError("expected an object, got " + Py.TypeName(d));
            var r = new JsonObject();
            foreach (var kv in o) r[kv.Key] = DecodeValue(kv.Value);
            return r;
        }

        private static List<object?> List(object? v, string what) =>
            v as List<object?> ?? throw new ReplayError(what + " is not a list");

        // ---- engine -----------------------------------------------------------------------------------------------

        /// <summary>replay.py's build_config_from: build_chain for each custom chain, build_config, then add_item in order.</summary>
        public static ConfigData BuildConfigFrom(JsonObject setup)
        {
            var kw = Decoded(setup["config"]);
            var custom = List(setup["custom_chains"], "setup.custom_chains");
            if (custom.Count > 0) kw["custom_chains"] = custom.Select(c => (object?)Config.BuildChain(Decoded(c))).ToList();
            var config = Config.BuildConfig(kw);
            foreach (var item in List(setup["items"], "setup.items"))
            {
                var k = Decoded(item);
                var id = k["item_id"];
                k.Remove("item_id");
                Config.AddItem(config, id, k);
            }
            return config;
        }

        /// <summary>The Engine of a vector's setup. One clock drives the Engine and every RpcClient.</summary>
        public static Engine BuildEngine(JsonObject setup, ITransport transport, Func<long> clock, Action<Exception> onError,
                                         IEntropy entropy)
        {
            object RpcFactory(Chain chain)
            {
                var rpc = Backends.BackendFor(chain).MakeRpc(chain, transport);
                var client = rpc is Solana.SolanaRpc s ? s.Client : (RpcClient)rpc; // SolanaRpc wraps its RpcClient
                client.Clock = clock;
                return rpc;
            }
            return new Engine(BuildConfigFrom(setup), transport, DeepCopy.Of(setup["pending"]) as JsonObject,
                              DeepCopy.Of(setup["receipts"]) as JsonObject, clock, RpcFactory, onError, entropy);
        }

        // ---- restore candidates -----------------------------------------------------------------------------------

        private static readonly string[] TransferFields = { "token", "sender", "recipient", "value", "tx_hash", "log_index", "block_number" };

        private static JsonObject TransferJson(object transfer)
        {
            switch (transfer)
            {
                case TransferLog t:
                    return new JsonObject
                    {
                        { "token", t.Token }, { "sender", t.Sender }, { "recipient", t.Recipient }, { "value", Int(t.Value, "transfer.value") },
                        { "tx_hash", t.TxHash }, { "log_index", t.LogIndex }, { "block_number", t.BlockNumber },
                        { "block_hash", t.BlockHash },
                    };
                case Transfer t:
                    return new JsonObject
                    {
                        { "token", t.Token }, { "sender", t.Sender }, { "recipient", t.Recipient }, { "value", Int(t.Value, "transfer.value") },
                        { "tx_hash", t.TxHash }, { "log_index", t.LogIndex }, { "block_number", t.BlockNumber },
                    };
                default:
                    throw new ReplayError("unsupported transfer " + Py.TypeName(transfer));
            }
        }

        /// <summary>replay.py's candidate_to_json: the candidate as plain JSON, its transfer tagged with the Python class name.</summary>
        public static JsonObject CandidateToJson(RestoreCandidate c)
        {
            var transfer = TransferJson(c.Transfer);
            transfer["class"] = c.Transfer is TransferLog ? "TransferLog" : "Transfer";
            return new JsonObject
            {
                { "chain", c.Chain }, { "token", c.Token }, { "transfer", transfer }, { "kind", c.Kind }, { "item", c.Item },
                { "choices", c.Choices.Cast<object?>().ToList() }, { "already_restored", c.AlreadyRestored },
            };
        }

        private static string S(JsonObject d, string key) =>
            Py.Get(d, key) as string ?? throw new TypeError(key + " must be a str");

        private static string? SOrNone(JsonObject d, string key)
        {
            var v = Py.Get(d, key);
            return v == null ? null : v as string ?? throw new TypeError(key + " must be a str or None");
        }

        private static long L(JsonObject d, string key) =>
            Py.Get(d, key) is long n ? n : throw new TypeError(key + " must be an int");

        /// <summary>
        /// replay.py's candidate_from_json, the inverse of <see cref="CandidateToJson"/>: an unknown class is a KeyError
        /// and the wrong fields a TypeError, as Python's dataclass call raises them.
        /// </summary>
        public static RestoreCandidate CandidateFromJson(JsonObject d)
        {
            if (!(Py.Get(d, "transfer") is JsonObject raw)) throw new TypeError("transfer must be an object");
            var fields = new JsonObject();
            foreach (var kv in raw) fields[kv.Key] = kv.Value;
            var cls = Py.Get(fields, "class");
            fields.Remove("class");
            string[] want;
            if (cls is string c && c == "Transfer") want = TransferFields;
            else if (cls is string c2 && c2 == "TransferLog") want = TransferFields.Concat(new[] { "block_hash" }).ToArray();
            else throw new KeyError(cls as string ?? Py.Repr(cls));
            if (fields.Count != want.Length || !want.All(fields.ContainsKey))
                throw new TypeError(cls + "() got the wrong fields: " + string.Join(", ", fields.Keys));
            var value = Py.Get(fields, "value") switch
            {
                long n => (BigInteger)n,
                BigInteger b => b,
                _ => throw new TypeError("value must be an int"),
            };
            object transfer = want.Length == TransferFields.Length
                ? (object)new Transfer(S(fields, "token"), S(fields, "sender"), S(fields, "recipient"), value, S(fields, "tx_hash"),
                                       L(fields, "log_index"), L(fields, "block_number"))
                : new TransferLog(S(fields, "token"), S(fields, "sender"), S(fields, "recipient"), value, S(fields, "tx_hash"),
                                  L(fields, "log_index"), L(fields, "block_number"), S(fields, "block_hash"));
            var choices = new List<string>();
            foreach (var x in Py.Get(d, "choices") as List<object?> ?? throw new TypeError("choices must be a list")) choices.Add(x as string ?? throw new TypeError("a choice must be a str"));
            if (!(Py.Get(d, "already_restored") is bool restored)) throw new TypeError("already_restored must be a bool");
            return new RestoreCandidate(S(d, "chain"), S(d, "token"), transfer, SOrNone(d, "kind"), SOrNone(d, "item"), choices, restored);
        }

        // ---- actions ----------------------------------------------------------------------------------------------

        private static string StrArg(JsonObject a, string key) =>
            Py.Get(a, key) as string ?? throw new TypeError(key + " must be a str");

        /// <summary>
        /// Run one action: (result, raises), raises null or the vector's <c>raises</c> value. <paramref name="steps"/>
        /// are the steps recorded before this one (restore_apply reads one). <paramref name="afterAwait"/> is called
        /// after the action's await returns.
        /// </summary>
        public static async Task<(object? Result, JsonObject? Raises)> RunAction(Engine engine, string action, JsonObject args,
                                                                                IReadOnlyList<object?> steps,
                                                                                Action<string>? afterAwait = null)
        {
            var a = Decoded(args);
            object? result;
            try
            {
                switch (action)
                {
                    case "start_session":
                    {
                        var kind = StrArg(a, "kind");
                        var chain = StrArg(a, "chain");
                        a.Remove("kind");
                        a.Remove("chain");
                        result = await engine.StartSession(kind, chain, a);
                        afterAwait?.Invoke(action);
                        break;
                    }
                    case "build_solana_payment":
                        result = await engine.BuildSolanaPayment(StrArg(a, "sid"), StrArg(a, "account"));
                        afterAwait?.Invoke(action);
                        break;
                    case "report_hint":
                        engine.ReportHint(StrArg(a, "sid"), Py.Get(a, "tx"));
                        result = null;
                        break;
                    case "set_ui_status":
                        engine.SetUiStatus(StrArg(a, "sid"), Py.Get(a, "status"));
                        result = null;
                        break;
                    case "discard_session":
                        engine.DiscardSession(StrArg(a, "sid"));
                        result = null;
                        break;
                    case "tick":
                        result = await engine.Tick();
                        afterAwait?.Invoke(action);
                        break;
                    case "reverify":
                        result = await engine.Reverify();
                        afterAwait?.Invoke(action);
                        break;
                    case "restore_lookup":
                        result = await engine.RestoreLookup(StrArg(a, "tx"));
                        afterAwait?.Invoke(action);
                        break;
                    case "restore_apply":
                    {
                        if (!(Py.Get(a, "from_step") is long from)) throw new TypeError("from_step must be an int");
                        var recorded = List(Py.Get(steps[(int)from] as JsonObject, "returns"), "returns");
                        var candidates = recorded.Select(c => CandidateFromJson(c as JsonObject ?? throw new TypeError("a candidate must be an object"))).ToList();
                        Dictionary<int, object?>? choices = null;
                        if (Py.Get(a, "choices") is JsonObject cs)
                        {
                            choices = new Dictionary<int, object?>();
                            foreach (var kv in cs) choices[(int)Py.Int(kv.Key)] = kv.Value; // int(k)
                        }
                        else if (Py.Get(a, "choices") != null)
                        {
                            throw new TypeError("choices must be an object or null");
                        }
                        result = engine.RestoreApply(candidates, choices);
                        break;
                    }
                    default:
                        throw new ValueError("unknown action " + Py.Repr(action));
                }
            }
            catch (ConfigError e)
            {
                return (null, new JsonObject { { "type", "ConfigError" }, { "code", e.Code } });
            }
            catch (RestoreError e)
            {
                return (null, new JsonObject { { "type", "RestoreError" }, { "code", e.Code } });
            }
            catch (Exception)
            {
                return (null, new JsonObject { { "type", "other" } });
            }
            return (result, null);
        }

        // ---- encoders (canonical.to_jsonable) ---------------------------------------------------------------------

        private static readonly BigInteger MaxSafe = BigInteger.Pow(2, 53);

        private static string Where(string path) => path.Length == 0 ? "<root>" : path;

        private static string Child(string path, string key) => path.Length == 0 ? key : path + "." + key;

        private static long Int(BigInteger n, string path)
        {
            if (BigInteger.Abs(n) >= MaxSafe)
                throw new ReplayError("integer " + n.ToString(CultureInfo.InvariantCulture) + " at " + Where(path) + " is not safe in JSON (|n| >= 2**53)");
            return (long)n;
        }

        private static JsonObject ResultJson(PaymentResult r, string path) => new JsonObject
        {
            { "status", r.Status }, { "kind", r.Kind }, { "item", r.Item },
            { "amount", r.Amount == null ? null : r.Amount.ToString() }, { "token", r.Token }, { "chain", r.Chain },
            { "tx_hash", r.TxHash }, { "payer", r.Payer }, { "session_id", r.SessionId }, { "error", r.Error },
        };

        private static JsonObject OutcomeJson(StepOutcome o, string path) => new JsonObject
        {
            { "session_id", o.SessionId }, { "status", o.Status }, { "confirmations", Int(o.Confirmations, Child(path, "confirmations")) },
            { "required", Int(o.Required, Child(path, "required")) },
            { "result", o.Result == null ? null : ResultJson(o.Result, Child(path, "result")) }, { "error", o.Error },
            { "hint_tx", o.HintTx },
        };

        /// <summary>canonical.to_jsonable: plain JSON, with the vector rules for numbers, Decimals and the Engine's objects.</summary>
        public static object? ToJsonable(object? v, string path = "")
        {
            switch (v)
            {
                case null:
                case bool _:
                case string _:
                    return v;
                case int i: return Int(i, path);
                case long l: return Int(l, path);
                case BigInteger b: return Int(b, path);
                case double d:
                    if (double.IsNaN(d) || double.IsInfinity(d) || Math.Floor(d) != d)
                        throw new ReplayError("non-integral float " + Py.FloatRepr(d) + " at " + Where(path));
                    return Int(new BigInteger(d), path);
                case Dec dec: return dec.ToString();
                case byte[] _: throw new ReplayError("bytes at " + Where(path) + ": hex-encode into a *_hex field");
                case JsonObject o:
                {
                    var r = new JsonObject();
                    foreach (var kv in o) r[kv.Key] = ToJsonable(kv.Value, Child(path, kv.Key));
                    return r;
                }
                case StepOutcome so: return OutcomeJson(so, path);
                case PaymentResult pr: return ResultJson(pr, path);
                case RestoreCandidate rc:
                {
                    var j = CandidateToJson(rc);
                    ((JsonObject)j["transfer"]!).Remove("class"); // to_jsonable of the dataclass carries no class tag
                    return j;
                }
                case Transfer _:
                case TransferLog _:
                    return TransferJson(v);
                case IEnumerable e:
                {
                    var list = new List<object?>();
                    int n = 0;
                    foreach (var x in e) list.Add(ToJsonable(x, path + "[" + (n++).ToString(CultureInfo.InvariantCulture) + "]"));
                    return list;
                }
                default:
                    throw new ReplayError("unsupported value of type " + Py.TypeName(v) + " at " + Where(path));
            }
        }

        /// <summary>replay.py's encode_returns: restore candidates carry their transfer's class; anything else is to_jsonable.</summary>
        public static object? EncodeReturns(string action, object? result)
        {
            if (action == "restore_lookup" && result is IEnumerable<RestoreCandidate> candidates)
                return candidates.Select(c => (object?)CandidateToJson(c)).ToList();
            return ToJsonable(result);
        }

        /// <summary>pending, receipts, owned (sorted), tip_total (str), unready (sorted [chain, token]).</summary>
        public static JsonObject SnapshotJson(Engine engine)
        {
            var snap = engine.Snapshot();
            var owned = snap.Owned.ToList();
            owned.Sort(Py.CompareCodePoints);
            var unready = snap.Unready.ToList();
            unready.Sort((x, y) =>
            {
                int c = Py.CompareCodePoints(x.Chain, y.Chain);
                return c != 0 ? c : Py.CompareCodePoints(x.Token, y.Token);
            });
            return new JsonObject
            {
                { "pending", ToJsonable(snap.Pending, "pending") },
                { "receipts", ToJsonable(snap.Receipts, "receipts") },
                { "owned", owned.Cast<object?>().ToList() },
                { "tip_total", snap.TipTotal.ToString() },
                { "unready", unready.Select(p => (object?)new List<object?> { p.Chain, p.Token }).ToList() },
            };
        }

        // ---- replay -----------------------------------------------------------------------------------------------

        /// <summary>Re-run a Tier 2 vector: null when it agrees, otherwise "step {i} ({action}): ..." for the first difference.</summary>
        public static Task<string?> Replay(JsonObject vector) => Replay(vector, new ReplayTransport());

        /// <summary>
        /// <see cref="Replay(JsonObject)"/> over a given transport. <paramref name="afterAwait"/> is called after every
        /// await in the runner (the single-thread test checks which thread resumed).
        /// </summary>
        public static async Task<string?> Replay(JsonObject vector, ReplayTransport transport, Action<string>? afterAwait = null)
        {
            if (!(vector.Get("tier") is long tier) || tier != 2) return "not a Tier 2 vector";
            var setup = vector["setup"] as JsonObject ?? throw new ReplayError("setup is not an object");
            long now = setup["clock"] is long clock ? clock : throw new ReplayError("setup.clock is not an int");
            int errors = 0;
            var entropy = new StepEntropy();
            var engine = BuildEngine(setup, transport, () => now, _ => errors++, entropy);
            var steps = List(vector["steps"], "steps");
            for (int i = 0; i < steps.Count; i++)
            {
                var step = steps[i] as JsonObject ?? throw new ReplayError("a step is not an object");
                var action = step["action"] as string ?? throw new ReplayError("a step's action is not a string");
                var where = "step " + i.ToString(CultureInfo.InvariantCulture) + " (" + action + ")";
                now = step["at"] is long at ? at : throw new ReplayError(where + ": at is not an int");
                errors = 0;
                transport.Load(List(step["calls"], "calls"));
                entropy.Current = new ScriptedEntropy(List(step["entropy"], "entropy"));
                var (result, raises) = await RunAction(engine, action, step["args"] as JsonObject ?? new JsonObject(),
                                                       steps.GetRange(0, i), afterAwait == null ? (Action<string>?)null : w => afterAwait(where + " " + w));
                afterAwait?.Invoke(where);
                var problems = transport.Problems.Concat(transport.Leftover()).Concat(entropy.Current.Problems)
                    .Concat(entropy.Current.Leftover()).ToList();
                if (problems.Count > 0) return where + ": " + string.Join("; ", problems);
                var recordedRaises = step.Get("raises");
                if (!JsonEq.Equal(recordedRaises, raises))
                    return where + ": raises: recorded " + Json.Dumps(recordedRaises) + ", got " + Json.Dumps(raises);
                object? returns;
                try
                {
                    returns = raises != null ? null : EncodeReturns(action, result);
                }
                catch (ReplayError e)
                {
                    return where + ": returns: " + e.Message;
                }
                var rd = JsonEq.FirstDiff(step.Get("returns"), returns);
                if (rd != null) return where + ": returns: " + rd + " (recorded != got)";
                if (!(step.Get("internal_errors") is long recordedErrors) || recordedErrors != errors)
                    return where + ": internal_errors: recorded " + Json.Dumps(step.Get("internal_errors")) + ", got "
                           + errors.ToString(CultureInfo.InvariantCulture);
                JsonObject snapshot;
                try
                {
                    snapshot = SnapshotJson(engine);
                }
                catch (ReplayError e)
                {
                    return where + ": snapshot: " + e.Message;
                }
                var sd = JsonEq.FirstDiff(step.Get("snapshot"), snapshot);
                if (sd != null) return where + ": snapshot: " + sd + " (recorded != got)";
            }
            return null;
        }
    }
}
