// No Python twin: the Tier 1 runner, replaying conformance/vectors/tier1/*.json against the C# core.
// Modeled on conformance/js/tier1.ts. Funcs maps the Python function name of a case (its "fn") to an adapter
// from the JSON-friendly input to a JSON-friendly output; each later task adds its topics to Implemented and
// its adapters to Funcs, in a block of its own under RegisterAll.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Numerics;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public static class Tier1
    {
        /// <summary>Adapters keyed by Python <c>fn</c>. Outputs are JSON values (string, long, bool, JsonObject, List).</summary>
        public static readonly Dictionary<string, Func<JsonObject, object?>> Funcs =
            new Dictionary<string, Func<JsonObject, object?>>(StringComparer.Ordinal);

        /// <summary>Topics (vector file names without .json) whose adapters are all in <see cref="Funcs"/>.</summary>
        public static readonly HashSet<string> Implemented = new HashSet<string>(StringComparer.Ordinal)
        {
            "amounts",
            "keccak",
            "evm_address",
        };

        static Tier1()
        {
            RegisterAll();
        }

        // One block per topic group, in conformance/js/tier1.ts's order; later tasks add theirs here.
        private static void RegisterAll()
        {
            RegisterAmounts();
            RegisterKeccak();
            RegisterEvm();
        }

        // ---- shared input helpers --------------------------------------------------------------------------

        /// <summary>
        /// Shape check for vector inputs, so a malformed input is a ValueError, not an incidental TypeError.
        /// </summary>
        internal static string Str(JsonObject i, string key)
        {
            if (i.TryGetValue(key, out var v) && v is string s) return s;
            throw new ValueError("vector input " + key + " must be a string");
        }

        /// <summary>
        /// Token units: vectors write them as decimal integer strings, read with Python's <c>int(str)</c>. Anything
        /// else is a TypeError, as in tier1.ts.
        /// </summary>
        internal static BigInteger Units(object? v)
        {
            if (!(v is string s)) throw new TypeError("vector units must be a decimal string");
            return Py.Int(s);
        }

        /// <summary>A vector value: <c>{"float": "&lt;repr&gt;"}</c> is that float, anything else is itself (replay.py's decode_value).</summary>
        internal static object? VectorValue(object? v)
        {
            if (v is JsonObject o && o.Count == 1 && o.TryGetValue("float", out var repr) && repr is string text)
                return double.Parse(text, NumberStyles.Float, CultureInfo.InvariantCulture);
            return v;
        }

        private static string Str(Dec d) => d.ToString();

        // ---- amounts ---------------------------------------------------------------------------------------

        private static void RegisterAmounts()
        {
            Funcs["parse_amount"] = i =>
            {
                var type = i["type"] as string;
                object? value = type == "float" ? 4.99 : type == "bool" ? (object)Py.Truthy(i["value"]) : i["value"];
                return Str(Amounts.ParseAmount(value));
            };
            Funcs["encode"] = i => Amounts.Encode(i["price"], i["code"], i["nonce"]).ToString(CultureInfo.InvariantCulture);
            Funcs["decode"] = i =>
            {
                var (price, code, nonce) = Amounts.Decode(Units(i["units"]));
                return new JsonObject { { "price", Str(price) }, { "code", (long)code }, { "nonce", (long)nonce } };
            };
            Funcs["units_to_decimal"] = i => Str(Amounts.UnitsToDecimal(Units(i["units"])));
            Funcs["decimal_to_units"] = i =>
            {
                var d = Dec.Parse(Str(i, "amount")) ?? throw new ValueError("amount must be finite");
                return Amounts.DecimalToUnits(d).ToString(CultureInfo.InvariantCulture);
            };
            Funcs["format_units"] = i => Amounts.FormatUnits(Units(i["units"]));
            Funcs["stored_amount"] = i => Str(Amounts.StoredAmount(VectorValue(i["value"])));
        }

        // ---- keccak ----------------------------------------------------------------------------------------

        private static void RegisterKeccak()
        {
            Funcs["keccak256"] = i => Bytes.ToHex(Keccak.Keccak256(Bytes.FromHex(Str(i, "data_hex"))));
        }

        // ---- evm_address (the address fields are passed through untouched: non-strings are Python's errors) --

        private static void RegisterEvm()
        {
            Funcs["to_checksum_address"] = i => Evm.ToChecksumAddress(i.Get("address"));
            Funcs["is_checksum_address"] = i => Evm.IsChecksumAddress(i.Get("address"));
            Funcs["normalize_address"] = i => Evm.NormalizeAddress(i.Get("address"));
            Funcs["address_topic"] = i => Evm.AddressTopic(i.Get("address"));
            Funcs["parse_transfer_log"] = i =>
            {
                if (!(i.Get("log") is JsonObject log)) throw new ValueError("vector input log must be an object");
                var t = Evm.ParseTransferLog(log);
                if (t == null) return null;
                return new JsonObject
                {
                    { "token", t.Token }, { "sender", t.Sender }, { "recipient", t.Recipient },
                    { "value", t.Value.ToString(CultureInfo.InvariantCulture) },
                    { "tx_hash", t.TxHash }, { "log_index", t.LogIndex }, { "block_number", t.BlockNumber },
                    { "block_hash", t.BlockHash },
                };
            };
        }

        // ---- the runner ------------------------------------------------------------------------------------

        /// <summary>The cases of a topic, as the vector file holds them.</summary>
        public static List<JsonObject> Cases(string topic)
        {
            var doc = Json.Loads(Vectors.ReadText("tier1/" + topic + ".json")) as JsonObject
                      ?? throw new InvalidOperationException("tier1/" + topic + ".json is not an object");
            if (!(doc.Get("topic") is string named) || named != topic)
                throw new InvalidOperationException("tier1/" + topic + ".json names topic " + Json.Dumps(doc.Get("topic")));
            if (!(doc.Get("cases") is List<object?> cases))
                throw new InvalidOperationException("tier1/" + topic + ".json has no cases list");
            return cases.Select(c => c as JsonObject
                                     ?? throw new InvalidOperationException("tier1/" + topic + ".json: a case is not an object"))
                .ToList();
        }

        /// <summary>Every failure message of a topic's cases, in file order (empty when all pass).</summary>
        public static List<string> RunTopic(string topic)
        {
            var failures = new List<string>();
            var cases = Cases(topic);
            for (int n = 0; n < cases.Count; n++)
            {
                var diff = RunCase(cases[n]);
                if (diff != null) failures.Add("#" + n.ToString(CultureInfo.InvariantCulture) + " " + diff);
            }
            return failures;
        }

        /// <summary>
        /// Recomputes a case through <see cref="Funcs"/>: null when it agrees with the vector, otherwise a message
        /// naming the first difference. An "error" with a dot is a ConfigError with that code; "invalid" is a
        /// ValueError or TypeError that is not a ConfigError. Any other exception fails the case (stricter than
        /// Python on purpose), and so does an fn without an adapter.
        /// </summary>
        public static string? RunCase(JsonObject c)
        {
            var fn = c.Get("fn") as string ?? "<no fn>";
            var input = c.Get("in");
            if (!Funcs.TryGetValue(fn, out var f)) return "no C# adapter for " + fn;
            if (!(input is JsonObject args)) return fn + ": the vector's in is not an object";

            var got = new JsonObject();
            try
            {
                got["out"] = f(args);
            }
            catch (ConfigError e)
            {
                got["error"] = e.Code;
            }
            catch (Exception e) when (e is ValueError || e is TypeError)
            {
                got["error"] = "invalid";
            }
            catch (Exception e)
            {
                return fn + "(" + Show(input) + "): raised " + e.GetType().Name + ": " + e.Message
                       + " (only ValueError, TypeError and ConfigError are rejections)";
            }

            var want = new JsonObject();
            if (c.TryGetValue("error", out var error) && error != null) want["error"] = error;
            else want["out"] = c.Get("out");

            var diff = JsonEq.FirstDiff(got, want);
            if (diff == null) return null;
            return fn + "(" + Show(input) + "): " + diff + " (got " + Show(got) + ", vector says " + Show(want) + ")";
        }

        private static string Show(object? v)
        {
            string s;
            try { s = Json.Dumps(v, ensureAscii: false); }
            catch (Exception) { s = "<" + (v?.GetType().Name ?? "null") + ">"; }
            return s.Length > 300 ? s.Substring(0, 297) + "..." : s;
        }
    }
}
