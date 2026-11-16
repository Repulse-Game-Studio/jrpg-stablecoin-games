// Twin of scfg_core/sessions.py (and of rpgmaker/src/core/sessions.ts).
// Pending payment sessions.
//
// A session is one payment attempt, created when the player picks a network. It is stored as a plain JsonObject
// (the frozen `pending` schema, conformance/schema/state.json) so it survives restarts and the watcher can keep
// looking for the payment for up to 24 hours.
//
// Randomness goes through an IEntropy (Python's `secrets`), so the Tier 2 runner can script draws. The draw order
// and sizes equal Python's: randbelow(100) for the nonce, then token_hex(8) for the id (Solana's backend adds
// token_bytes(32) for its reference).
#nullable enable
using System;
using System.Collections.Generic;
using System.Numerics;
using System.Security.Cryptography;

namespace Scfg.Core
{
    /// <summary>Python's <c>secrets</c> module, as the payment core uses it.</summary>
    public interface IEntropy
    {
        /// <summary><c>secrets.token_hex(n)</c>: 2n lowercase hex characters.</summary>
        string TokenHex(int n);
        /// <summary><c>secrets.randbelow(n)</c>: a uniform integer in [0, n); a ValueError for n &lt;= 0.</summary>
        long RandBelow(long n);
        /// <summary><c>secrets.token_bytes(n)</c>.</summary>
        byte[] TokenBytes(int n);
    }

    /// <summary>The OS random source (<see cref="RandomNumberGenerator"/>).</summary>
    public sealed class SystemEntropy : IEntropy
    {
        public static readonly SystemEntropy Instance = new SystemEntropy();

        public byte[] TokenBytes(int n)
        {
            if (n < 0) throw new ValueError("negative argument not allowed");
            var b = new byte[n];
            if (n > 0)
                using (var rng = RandomNumberGenerator.Create())
                    rng.GetBytes(b);
            return b;
        }

        public string TokenHex(int n) => Bytes.ToHex(TokenBytes(n));

        public long RandBelow(long n)
        {
            if (n <= 0) throw new ValueError("Upper bound must be positive.");
            int bits = 0;
            for (ulong m = (ulong)(n - 1); m != 0; m >>= 1) bits++;
            if (bits == 0) return 0;
            int len = (bits + 7) / 8;
            ulong mask = bits == 64 ? ulong.MaxValue : (1UL << bits) - 1;
            // Rejection sampling on `bits` random bits (CPython's randbelow): uniform, no modulo bias.
            while (true)
            {
                var b = TokenBytes(len);
                ulong x = 0;
                foreach (var by in b) x = (x << 8) | by;
                x &= mask;
                if (x < (ulong)n) return (long)x;
            }
        }
    }

    public static class Sessions
    {
        public const int SessionWindowS = 30 * 60;
        public const int ScanHorizonS = 24 * 3600;
        public const int HintTimeoutS = 30 * 60;
        /// <summary>Twin of SOLANA_HINT_TIMEOUT_S in scfg_core/backends/solana.py.</summary>
        public const int SolanaHintTimeoutS = 180;
        public static readonly string[] UiStatuses = { "open", "pending", "cancelled", "expired" };

        public static string NewSessionId(IEntropy entropy) => "ps_" + entropy.TokenHex(8);

        private static bool IsHex(char c) => (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');

        /// <summary>A 0x-prefixed 32-byte hash (surrounding whitespace allowed), lowercased; else a ValueError.</summary>
        public static string NormalizeTxHash(object? txHash)
        {
            if (txHash is string s)
            {
                var t = Py.Strip(s);
                if (t.Length == 66 && t[0] == '0' && t[1] == 'x')
                {
                    bool ok = true;
                    for (int i = 2; i < 66 && ok; i++) ok = IsHex(t[i]);
                    if (ok) return t.ToLowerInvariant();
                }
            }
            throw new ValueError("not a transaction hash: " + Py.Repr(txHash));
        }

        private static object IntValue(BigInteger n) => n >= long.MinValue && n <= long.MaxValue ? (object)(long)n : n;

        /// <summary>
        /// Python <c>create_session</c>. <paramref name="now"/> is stored as given (an integer or float clock).
        /// </summary>
        public static JsonObject CreateSession(JsonObject store, string kind, string? item, string title, string token,
                                               string chain, object? price, long code, long startBlock, object now,
                                               long? nonce, JsonObject? extra, IEntropy entropy)
        {
            if (kind != "unlock" && kind != "tip") throw new ValueError("invalid kind " + Py.Repr(kind));
            long n = nonce ?? entropy.RandBelow(100);
            var p = Amounts.ParseAmount(price);
            var sid = NewSessionId(entropy);
            var rec = new JsonObject
            {
                { "id", sid }, { "kind", kind }, { "item", item }, { "title", title }, { "token", token }, { "chain", chain },
                { "price", p.ToString() }, // culture-ok: Dec.ToString is invariant
                { "code", code }, { "nonce", n }, { "units", IntValue(Amounts.Encode(p, code, n)) },
                { "start_block", startBlock }, { "cursor", startBlock }, { "created_at", now },
                { "state", "awaiting" }, { "ui_status", "open" },
                { "hint_tx", null }, { "hint_at", null }, { "match", null },
            };
            if (extra != null)
            {
                foreach (var kv in extra)
                    if (rec.ContainsKey(kv.Key)) throw new ValueError("session field " + Py.Repr(kv.Key) + " is reserved");
                foreach (var kv in extra) rec[kv.Key] = kv.Value;
            }
            store[sid] = rec;
            return rec;
        }

        /// <summary><c>store.get(sid)</c></summary>
        public static JsonObject? GetSession(JsonObject store, string sid) => store.Get(sid) as JsonObject;

        /// <summary><c>store.pop(sid, None)</c></summary>
        public static void RemoveSession(JsonObject store, string sid) => store.Remove(sid);

        /// <summary>
        /// <c>rec[key]</c> used as a number: KeyError when missing, TypeError unless a number (None and str do not
        /// do arithmetic or order against a number).
        /// </summary>
        public static double NumField(object? rec, string key)
        {
            var v = Py.Get(rec, key);
            switch (v)
            {
                case bool b: return b ? 1 : 0;
                case long l: return l;
                case int i: return i;
                case double d: return d;
                case BigInteger bi: return (double)bi;
                default: throw new TypeError(key + " must be a number, not " + Py.TypeName(v));
            }
        }

        private static bool IsNumber(object? v) => v is bool || v is long || v is int || v is double || v is BigInteger;

        private static double ToNumber(object? v) => v is bool b ? (b ? 1 : 0) : v is long l ? l : v is int i ? i : v is BigInteger bi ? (double)bi : (double)v!;

        /// <summary>Oldest first (a stable sort, like Python's <c>sorted</c>; one record is never compared).</summary>
        public static List<JsonObject> ListSessions(JsonObject store)
        {
            var items = new List<(JsonObject Rec, object? Key, int Index)>();
            int n = 0;
            foreach (var kv in store)
            {
                // Python's key=lambda r: r["created_at"] reads every record: a non-dict one is a KeyError/TypeError
                var key = Py.Get(kv.Value, "created_at");
                items.Add(((JsonObject)kv.Value!, key, n++));
            }
            // Python orders numbers (bool, int, float) among themselves and strings among themselves, and raises
            // TypeError for anything else. With two or more records some comparison across a mixed pair must happen
            // (any sort has to connect them), so validate up front; a lone record is never compared. Doing it here
            // also keeps exceptions out of List.Sort's comparer, which would wrap them.
            for (int i = 1; i < items.Count; i++)
            {
                var a = items[i].Key;
                var b = items[i - 1].Key;
                if (!(IsNumber(a) && IsNumber(b)) && !(a is string && b is string))
                    throw new TypeError("'<' not supported between instances of '" + Py.TypeName(a) + "' and '" + Py.TypeName(b) + "'");
            }
            items.Sort((a, b) =>
            {
                int c;
                if (a.Key is string sa) c = Py.CompareCodePoints(sa, (string)b.Key!);
                else
                {
                    var x = ToNumber(a.Key);
                    var y = ToNumber(b.Key);
                    c = x < y ? -1 : x > y ? 1 : 0;
                }
                return c != 0 ? c : (a.Index < b.Index ? -1 : 1);
            });
            var result = new List<JsonObject>(items.Count);
            foreach (var it in items) result.Add(it.Rec);
            return result;
        }

        public static void SetHint(JsonObject store, string sid, object? txHash, object now, Func<object?, string>? normalize = null)
        {
            if (!(store.Get(sid) is JsonObject rec)) throw new KeyError(sid);
            rec["hint_tx"] = (normalize ?? NormalizeTxHash)(txHash);
            rec["hint_at"] = now;
        }

        public static void ClearHint(JsonObject rec)
        {
            rec["hint_tx"] = null;
            rec["hint_at"] = null;
        }

        public static void SetUiStatus(JsonObject store, string sid, object? status)
        {
            if (!(status is string s) || !Array.Exists(UiStatuses, u => u == s))
                throw new ValueError("invalid ui status " + Py.Repr(status));
            if (!(store.Get(sid) is JsonObject rec)) throw new KeyError(sid);
            rec["ui_status"] = s;
        }

        public static bool IsPastWindow(JsonObject rec, double now) => now - NumField(rec, "created_at") >= SessionWindowS;

        public static bool IsPastHorizon(JsonObject rec, double now) => now - NumField(rec, "created_at") >= ScanHorizonS;

        /// <summary>Python <c>stored_amount(rec["price"])</c>: a ValueError for a price out of the amount range.</summary>
        public static Dec SessionPrice(JsonObject rec) => Amounts.StoredAmount(Py.Get(rec, "price"));
    }
}
