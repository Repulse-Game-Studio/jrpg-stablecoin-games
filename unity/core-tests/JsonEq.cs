// No Python twin: JSON value equality as conformance/README.md defines it for params and outputs.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Numerics;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    /// <summary>
    /// Objects ignore key order, arrays are ordered, strings compare exactly, numbers compare by exact value
    /// (1 equals 1.0; no lossy conversion), and a boolean never equals a number (true is not 1).
    /// </summary>
    public static class JsonEq
    {
        public static bool Equal(object? a, object? b) => FirstDiff(a, b) == null;

        /// <summary>null when equal, otherwise "path: left != right" for the first difference found.</summary>
        public static string? FirstDiff(object? a, object? b) => Diff(a, b, "");

        private static string Here(string path, string detail) => path.Length == 0 ? detail : path + ": " + detail;

        private static string Show(object? v)
        {
            string s;
            try { s = Json.Dumps(v); }
            catch (Exception) { s = "<" + (v?.GetType().Name ?? "null") + ">"; }
            return s.Length > 120 ? s.Substring(0, 117) + "..." : s;
        }

        private static string? Diff(object? a, object? b, string path)
        {
            if (a is List<object?> la || b is List<object?>)
            {
                if (!(a is List<object?> l) || !(b is List<object?> r)) return Here(path, Show(a) + " != " + Show(b));
                int n = Math.Min(l.Count, r.Count);
                for (int i = 0; i < n; i++)
                {
                    var d = Diff(l[i], r[i], path + "[" + i.ToString(CultureInfo.InvariantCulture) + "]");
                    if (d != null) return d;
                }
                if (l.Count != r.Count)
                    return Here(path, "length " + l.Count.ToString(CultureInfo.InvariantCulture) + " != " + r.Count.ToString(CultureInfo.InvariantCulture));
                return null;
            }
            if (a is JsonObject || b is JsonObject)
            {
                if (!(a is JsonObject l) || !(b is JsonObject r)) return Here(path, Show(a) + " != " + Show(b));
                string Sub(string k) => path.Length == 0 ? k : path + "." + k;
                foreach (var k in l.Keys.OrderBy(k => k, StringComparer.Ordinal))
                {
                    if (!r.TryGetValue(k, out var rv)) return Sub(k) + ": missing on the right";
                    var d = Diff(l[k], rv, Sub(k));
                    if (d != null) return d;
                }
                foreach (var k in r.Keys.OrderBy(k => k, StringComparer.Ordinal))
                    if (!l.ContainsKey(k)) return Sub(k) + ": missing on the left";
                return null;
            }
            return Scalar(a, b) ? null : Here(path, Show(a) + " != " + Show(b));
        }

        private static bool Scalar(object? a, object? b)
        {
            if (a == null || b == null) return a == null && b == null;
            if (a is bool ba) return b is bool bb && ba == bb;
            if (b is bool) return false;
            if (a is string sa) return b is string sb && string.Equals(sa, sb, StringComparison.Ordinal);
            if (b is string) return false;
            var na = Number(a);
            var nb = Number(b);
            if (na == null || nb == null) return false;
            return na.Value.Equals(nb.Value);
        }

        // A number as an exact (BigInteger, or a non-integral / non-finite double) value.
        private readonly struct Num : IEquatable<Num>
        {
            public readonly bool IsInt;
            public readonly BigInteger Int;
            public readonly double Float;
            public Num(BigInteger i) { IsInt = true; Int = i; Float = 0; }
            public Num(double f) { IsInt = false; Int = BigInteger.Zero; Float = f; }
            public bool Equals(Num o) => IsInt == o.IsInt && (IsInt ? Int == o.Int : Float.Equals(o.Float));
        }

        private static Num? Number(object v)
        {
            switch (v)
            {
                case int i: return new Num(new BigInteger(i));
                case long l: return new Num(new BigInteger(l));
                case BigInteger b: return new Num(b);
                case double d:
                    if (!double.IsNaN(d) && !double.IsInfinity(d) && Math.Floor(d) == d) return new Num(new BigInteger(d));
                    return new Num(d);
                default: return null;
            }
        }
    }
}
