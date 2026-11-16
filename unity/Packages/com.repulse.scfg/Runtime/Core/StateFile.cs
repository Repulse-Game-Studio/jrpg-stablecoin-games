// No Python twin as a module: the record checks are conformance/python/scfg_conformance/state_schema.py's _record /
// _value rules, and the load rules (and the `why` texts) are godot/addons/scfg/store.gd's load.
// The player's payment state file is {"version": 1, "pending": {...}, "receipts": {...}}, the Engine's two stores as
// they are, in the frozen format of conformance/schema/state.json. StateSchema holds that schema as a C# table (no
// file is read at runtime: an IL2CPP or WebGL build has no repo); a test compares it with the file. A loaded file
// whose records do not conform is refused as a whole (Parse's Why), so the Engine never sees a malformed store: the
// Unity store moves such a file aside as `.corrupt`, as Godot's does.
#nullable enable
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Numerics;

namespace Scfg.Core
{
    /// <summary>
    /// conformance/schema/state.json as a table. A field's spec is a string (<c>"str"</c>, <c>"int"</c>,
    /// <c>"bool"</c>, <c>"number"</c>, one of those with <c>"|null"</c>, or <c>"match|null"</c>) or an enum, the
    /// list <c>["enum", value, ...]</c>. The fields of <c>pending</c>, <c>match</c> and <c>receipt</c> are required,
    /// the <c>*_optional</c> ones may be absent, any other field is an error.
    /// </summary>
    public static class StateSchema
    {
        private static IReadOnlyList<string> Enum(params string[] values)
        {
            var all = new string[values.Length + 1];
            all[0] = "enum";
            values.CopyTo(all, 1);
            return Array.AsReadOnly(all);
        }

        private static IReadOnlyDictionary<string, object> Table(params (string Field, object Spec)[] fields)
        {
            var d = new Dictionary<string, object>();
            foreach (var (field, spec) in fields) d.Add(field, spec);
            return new ReadOnlyDictionary<string, object>(d);
        }

        public static readonly IReadOnlyDictionary<string, object> Match = Table(
            ("block", "int"), ("log_index", "int"), ("payer", "str|null"), ("recipient", "str"), ("tx_hash", "str"),
            ("value", "int"));

        public static readonly IReadOnlyDictionary<string, object> MatchOptional = Table(("block_hash", "str"));

        public static readonly IReadOnlyDictionary<string, object> Pending = Table(
            ("chain", "str"), ("code", "int"), ("created_at", "number"), ("cursor", "int"), ("hint_at", "number|null"),
            ("hint_tx", "str|null"), ("id", "str"), ("item", "str|null"), ("kind", Enum("unlock", "tip")),
            ("match", "match|null"), ("nonce", "int"), ("price", "str"), ("start_block", "int"),
            ("state", Enum("awaiting", "confirming")), ("title", "str"), ("token", "str"),
            ("ui_status", Enum("open", "pending", "cancelled", "expired")), ("units", "int"));

        public static readonly IReadOnlyDictionary<string, object> PendingOptional = Table(
            ("dest", "str"), ("reference", "str"), ("scan_before", "str|null"), ("scan_cursor", "str|null"));

        public static readonly IReadOnlyDictionary<string, object> Receipt = Table(
            ("amount", "str"), ("block", "int"), ("chain", "str"), ("item", "str|null"), ("kind", Enum("unlock", "tip")),
            ("log_index", "int"), ("payer", "str|null"), ("recipient", "str"), ("source", Enum("payment", "restore")),
            ("token", "str"), ("tx_hash", "str"));

        /// <summary>The section names of state.json, in its order, with their tables.</summary>
        public static readonly IReadOnlyList<KeyValuePair<string, IReadOnlyDictionary<string, object>>> Sections =
            Array.AsReadOnly(new[]
            {
                new KeyValuePair<string, IReadOnlyDictionary<string, object>>("match", Match),
                new KeyValuePair<string, IReadOnlyDictionary<string, object>>("match_optional", MatchOptional),
                new KeyValuePair<string, IReadOnlyDictionary<string, object>>("pending", Pending),
                new KeyValuePair<string, IReadOnlyDictionary<string, object>>("pending_optional", PendingOptional),
                new KeyValuePair<string, IReadOnlyDictionary<string, object>>("receipt", Receipt),
            });

        /// <summary>The schema as the JSON document of conformance/schema/state.json (a fresh copy).</summary>
        public static JsonObject ToJson()
        {
            var o = new JsonObject();
            foreach (var section in Sections)
            {
                var fields = new JsonObject();
                foreach (var kv in section.Value)
                {
                    if (kv.Value is IReadOnlyList<string> values)
                    {
                        var list = new List<object?>();
                        foreach (var v in values) list.Add(v);
                        fields[kv.Key] = list;
                    }
                    else fields[kv.Key] = kv.Value;
                }
                o[section.Key] = fields;
            }
            return o;
        }
    }

    /// <summary>What <see cref="StateFile.Parse"/> read: both stores, or neither and why.</summary>
    public sealed class StateLoad
    {
        public JsonObject? Pending { get; }
        public JsonObject? Receipts { get; }
        /// <summary>Null for a good file; else a predicate for "the payment state file &lt;path&gt; ..." (store.gd's why).</summary>
        public string? Why { get; }

        public StateLoad(JsonObject? pending, JsonObject? receipts, string? why)
        {
            Pending = pending;
            Receipts = receipts;
            Why = why;
        }
    }

    public static class StateFile
    {
        public const long Version = 1;

        private static readonly IReadOnlyDictionary<string, object> NoFields =
            new ReadOnlyDictionary<string, object>(new Dictionary<string, object>());

        private static bool IsInt(object? v) => v is long || v is int || v is BigInteger;

        private static bool TypeOk(string type, object? v)
        {
            switch (type)
            {
                case "str": return v is string;
                case "int": return IsInt(v); // a C# bool is never one, as the checker refuses Python's
                case "bool": return v is bool;
                case "number": return IsInt(v) || v is double;
                default: throw new ValueError("unknown schema type " + Py.Repr(type));
            }
        }

        private static List<object?> EnumValues(IReadOnlyList<string> spec)
        {
            var o = new List<object?>();
            for (int i = 1; i < spec.Count; i++) o.Add(spec[i]);
            return o;
        }

        // state_schema.py's _value.
        private static void Value(object? value, object spec, string path, List<string> problems)
        {
            if (spec is IReadOnlyList<string> en)
            {
                // `value in spec[1:]`: the enum values are strs, which only an equal str matches
                bool found = false;
                if (value is string s)
                    for (int i = 1; i < en.Count && !found; i++) found = en[i] == s;
                if (!found) problems.Add(path + ": expected one of " + Py.Repr(EnumValues(en)) + ", got " + Py.Repr(value));
                return;
            }
            var text = (string)spec;
            if (text == "match|null")
            {
                if (value == null) return;
                if (!(value is JsonObject m))
                {
                    problems.Add(path + ": expected object or null, got " + Py.Repr(value));
                    return;
                }
                Record(m, StateSchema.Match, StateSchema.MatchOptional, path, problems);
                return;
            }
            int bar = text.IndexOf("|", StringComparison.Ordinal);
            string type = bar < 0 ? text : text.Substring(0, bar);
            bool nullable = bar >= 0 && text.Substring(bar + 1) == "null";
            if (nullable && value == null) return;
            if (!TypeOk(type, value)) problems.Add(path + ": expected " + text + ", got " + Py.Repr(value));
        }

        // state_schema.py's _record: missing fields first (in schema order), then each field in record order.
        private static void Record(object? rec, IReadOnlyDictionary<string, object> required,
                                   IReadOnlyDictionary<string, object> optional, string path, List<string> problems)
        {
            if (!(rec is JsonObject r))
            {
                problems.Add(path + ": expected object");
                return;
            }
            foreach (var k in required.Keys)
                if (!r.ContainsKey(k)) problems.Add(path + ": missing " + Py.Repr(k));
            foreach (var kv in r)
            {
                if (!required.TryGetValue(kv.Key, out var spec) && !optional.TryGetValue(kv.Key, out spec))
                    problems.Add(path + ": unknown " + Py.Repr(kv.Key));
                else Value(kv.Value, spec, path + "." + kv.Key, problems);
            }
        }

        // state_schema.py's _receipt_key, for a receipt that passed its record check (all fields typed). The case
        // folding is the core's own (Receipts.ReceiptKey): a 0x hash is ASCII hex.
        private static string ReceiptKeyOf(JsonObject r)
        {
            var txHash = (string)r["tx_hash"]!;
            var tx = txHash.StartsWith("0x", StringComparison.Ordinal) ? txHash.ToLowerInvariant() : txHash;
            return (string)r["chain"]! + ":" + tx + ":" + Py.Repr(r["log_index"]);
        }

        /// <summary>
        /// The problems with the two stores against the state schema, [] when they conform: state_schema.py's checks of
        /// the pending and receipt records, with its texts. A pending record's <c>id</c> must be its key, and a
        /// receipt's key must be <c>chain:tx_hash:log_index</c> (the hash lowercased when it starts with 0x).
        /// </summary>
        public static List<string> Check(JsonObject pending, JsonObject receipts)
        {
            if (pending == null) throw new TypeError("pending must be a dict");
            if (receipts == null) throw new TypeError("receipts must be a dict");
            var problems = new List<string>();
            foreach (var kv in pending)
            {
                var path = "pending[" + kv.Key + "]";
                var found = new List<string>();
                Record(kv.Value, StateSchema.Pending, StateSchema.PendingOptional, path, found);
                if (found.Count == 0 && (string)((JsonObject)kv.Value!)["id"]! != kv.Key)
                    found.Add(path + ": id does not match its key");
                problems.AddRange(found);
            }
            foreach (var kv in receipts)
            {
                var path = "receipts[" + kv.Key + "]";
                var found = new List<string>();
                Record(kv.Value, StateSchema.Receipt, NoFields, path, found);
                if (found.Count == 0 && ReceiptKeyOf((JsonObject)kv.Value!) != kv.Key)
                    found.Add(path + ": key does not match chain:tx_hash:log_index");
                problems.AddRange(found);
            }
            return problems;
        }

        /// <summary>
        /// Reads a state file's text (store.gd's load, after the read). Not JSON, not an object, a version other than
        /// the int 1 (<c>true</c> is not 1), no pending/receipts objects, or a record that fails <see cref="Check"/>:
        /// <see cref="StateLoad.Why"/> says which, and both stores are null. Otherwise the two stores, as parsed.
        /// Other top-level fields are ignored, as Godot's store ignores them.
        /// </summary>
        public static StateLoad Parse(string text)
        {
            if (text == null) throw new TypeError("text must be a str");
            object? doc;
            try
            {
                doc = Json.Loads(text);
            }
            catch (ValueError)
            {
                return new StateLoad(null, null, "is not valid JSON");
            }
            if (!(doc is JsonObject o)) return new StateLoad(null, null, "is not a JSON object");
            o.TryGetValue("version", out var version);
            if (!(version is long v && v == Version))
                return new StateLoad(null, null, "has an unknown version " + Py.Repr(version));
            if (!(o.TryGetValue("pending", out var p) && p is JsonObject pending
                  && o.TryGetValue("receipts", out var r) && r is JsonObject receipts))
                return new StateLoad(null, null, "has no pending/receipts objects");
            var problems = Check(pending, receipts);
            if (problems.Count > 0)
            {
                var why = "does not match the state schema (" + problems[0];
                if (problems.Count > 1)
                    why += ", and " + (problems.Count - 1).ToString(CultureInfo.InvariantCulture) + " more";
                return new StateLoad(null, null, why + ")");
            }
            return new StateLoad(pending, receipts, null);
        }

        /// <summary>
        /// The file's text: <c>{"version": 1, "pending": ..., "receipts": ...}</c> through <see cref="Json.Dumps"/>,
        /// keys in insertion order as Python's json.dump writes them (the store order is visible: reverify reports
        /// removed receipts in it). <c>Parse(Serialize(p, r))</c> gives the stores back.
        /// </summary>
        public static string Serialize(JsonObject pending, JsonObject receipts)
        {
            if (pending == null) throw new TypeError("pending must be a dict");
            if (receipts == null) throw new TypeError("receipts must be a dict");
            return Json.Dumps(new JsonObject { { "version", Version }, { "pending", pending }, { "receipts", receipts } });
        }
    }
}
