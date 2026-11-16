// No Python twin: the CPython 3.9 built-ins the core relies on (str.strip(), int(), len(), d[key],
// truthiness, type names, repr(), urllib.parse.quote), mirroring rpgmaker/src/core/pystr.ts, whose
// tables (the whitespace sets and the Unicode 13.0.0 decimal digits) are copied here. Use these instead
// of Trim(), char.IsWhiteSpace, char.IsDigit or \d / \s in a Regex: .NET's Unicode tables differ.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using System.Text;

namespace Scfg.Core
{
    public static class Py
    {
        /// <summary>CPython's default <c>sys.get_int_max_str_digits()</c>.</summary>
        public const int IntMaxStrDigits = 4300;

        /// <summary>
        /// Python's <c>str.isspace()</c> set, what <c>str.strip()</c> removes: \t \n \v \f \r, the separators
        /// \x1c-\x1f, space, \x85, \xa0, U+1680, U+2000-U+200A, U+2028, U+2029, U+202F, U+205F, U+3000.
        /// Not the BOM U+FEFF, U+180E or U+200B, which .NET's Trim() treats differently.
        /// </summary>
        public static bool IsSpace(int cp) =>
            (cp >= 0x09 && cp <= 0x0d) || (cp >= 0x1c && cp <= 0x20) || cp == 0x85 || cp == 0xa0 || cp == 0x1680
            || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f
            || cp == 0x3000;

        // What int() skips around the digits: CPython maps every non-ASCII whitespace character to a space but
        // leaves ASCII ones as they are, then skips only ASCII whitespace (Py_ISSPACE), so the four separators
        // \x1c-\x1f that str.strip() removes make int() fail: int("\x1c5") is a ValueError.
        private static bool IsIntSpace(int cp) => IsSpace(cp) && !(cp >= 0x1c && cp <= 0x1f);

        /// <summary>Exactly Python's <c>str.strip()</c> with no argument. Linear: one scan from each end.</summary>
        public static string Strip(string s)
        {
            // Every whitespace character is in the BMP, so a UTF-16 unit test is exact (a surrogate never matches).
            int start = 0;
            int end = s.Length;
            while (start < end && IsSpace(s[start])) start++;
            while (end > start && IsSpace(s[end - 1])) end--;
            return start == 0 && end == s.Length ? s : s.Substring(start, end - start);
        }

        // The zero of every run of ten Unicode decimal digits (Nd) in CPython 3.9's unicodedata (Unicode
        // 13.0.0, the project's interpreter), copied from pystr.ts ND_ZEROS. Not .NET's tables: a newer
        // runtime has more digit sets (Tangsa U+16AC0, Kawi, Nag Mundari, ...) that Python 3.9 refuses.
        private static readonly int[] NdZeros =
        {
            0x30, 0x660, 0x6f0, 0x7c0, 0x966, 0x9e6, 0xa66, 0xae6, 0xb66, 0xbe6, 0xc66, 0xce6, 0xd66,
            0xde6, 0xe50, 0xed0, 0xf20, 0x1040, 0x1090, 0x17e0, 0x1810, 0x1946, 0x19d0, 0x1a80, 0x1a90,
            0x1b50, 0x1bb0, 0x1c40, 0x1c50, 0xa620, 0xa8d0, 0xa900, 0xa9d0, 0xa9f0, 0xaa50, 0xabf0,
            0xff10, 0x104a0, 0x10d30, 0x11066, 0x110f0, 0x11136, 0x111d0, 0x112f0, 0x11450, 0x114d0,
            0x11650, 0x116c0, 0x11730, 0x118e0, 0x11950, 0x11c50, 0x11d50, 0x11da0, 0x16a60, 0x16b50,
            0x1d7ce, 0x1d7d8, 0x1d7e2, 0x1d7ec, 0x1d7f6, 0x1e140, 0x1e2f0, 0x1e950, 0x1fbf0,
        };

        /// <summary>The value (0-9) of a Python 3.9 decimal digit of any script at code point <paramref name="cp"/>, or -1.</summary>
        public static int DecimalValue(int cp)
        {
            if (cp >= 0x30 && cp <= 0x39) return cp - 0x30;
            if (cp < 0x660) return -1;
            foreach (var z in NdZeros)
                if (cp >= z && cp < z + 10) return cp - z;
            return -1;
        }

        /// <summary>Python 3.9's <c>str.isdecimal()</c> for one code point, which is also <c>re</c>'s <c>\d</c>.</summary>
        public static bool IsDecimalDigit(int codePoint) => DecimalValue(codePoint) >= 0;

        /// <summary>
        /// The code point at UTF-16 index <paramref name="i"/> and its width in units (2 for a surrogate pair).
        /// A lone surrogate is its own code point, as in a Python str.
        /// </summary>
        internal static int CodePointAt(string s, int i, out int width)
        {
            char c = s[i];
            if (char.IsHighSurrogate(c) && i + 1 < s.Length && char.IsLowSurrogate(s[i + 1]))
            {
                width = 2;
                return char.ConvertToUtf32(c, s[i + 1]);
            }
            width = 1;
            return c;
        }

        /// <summary>Python's <c>len(s)</c>: code points, not UTF-16 units. A lone surrogate counts as one.</summary>
        public static int Len(string s)
        {
            int n = 0;
            for (int i = 0; i < s.Length; n++)
            {
                CodePointAt(s, i, out int w);
                i += w;
            }
            return n;
        }

        /// <summary>
        /// Python's lexicographic <c>str</c> ordering, by code point. <see cref="StringComparer.Ordinal"/>
        /// compares UTF-16 units, which puts U+E000-U+FFFF above the astral planes.
        /// </summary>
        public static int CompareCodePoints(string a, string b)
        {
            int i = 0, j = 0;
            while (i < a.Length && j < b.Length)
            {
                int ca = CodePointAt(a, i, out int wa);
                int cb = CodePointAt(b, j, out int wb);
                if (ca != cb) return ca < cb ? -1 : 1;
                i += wa;
                j += wb;
            }
            bool aDone = i >= a.Length, bDone = j >= b.Length;
            return aDone == bDone ? 0 : aDone ? -1 : 1;
        }

        /// <summary>
        /// Python <c>int(x)</c> for a JSON-like value:
        /// bool is 0/1, an int as it is, a finite float truncated toward zero (<c>int(1.9) == 1</c>), a str as
        /// <see cref="Int(string)"/>. NaN is a ValueError and an infinity an OverflowError (as Python); anything
        /// else is a TypeError.
        /// </summary>
        public static BigInteger Int(object? x)
        {
            switch (x)
            {
                case bool b: return b ? BigInteger.One : BigInteger.Zero;
                case int i: return i;
                case long l: return l;
                case BigInteger bi: return bi;
                case double d:
                    if (double.IsNaN(d)) throw new ValueError("cannot convert float NaN to integer");
                    if (double.IsInfinity(d)) throw new OverflowError("cannot convert float infinity to integer");
                    return new BigInteger(Math.Truncate(d));
                case string s: return Int(s);
                default:
                    throw new TypeError("int() argument must be a string, a bytes-like object or a number, not '" + TypeName(x) + "'");
            }
        }

        /// <summary>
        /// Python <c>int(s)</c>: surrounding whitespace (int()'s set: str.strip()'s minus \x1c-\x1f), an optional
        /// sign, then decimal digits of any script with single underscores between digits. More than
        /// <see cref="IntMaxStrDigits"/> digits is a ValueError (CPython 3.9.25's limit), as is any malformed
        /// text. Linear in the length of <paramref name="s"/>.
        /// </summary>
        public static BigInteger Int(string s)
        {
            int start = 0;
            int end = s.Length;
            while (start < end && IsIntSpace(s[start])) start++;
            while (end > start && IsIntSpace(s[end - 1])) end--;
            int i = start;
            bool negative = false;
            if (i < end && (s[i] == '+' || s[i] == '-'))
            {
                negative = s[i] == '-';
                i++;
            }
            var digits = new StringBuilder(end - i);
            bool needDigit = true; // at the start, or after an underscore
            while (i < end)
            {
                int cp = CodePointAt(s, i, out int w);
                i += w;
                if (cp == '_')
                {
                    if (needDigit) throw InvalidLiteral(s);
                    needDigit = true;
                    continue;
                }
                int d = DecimalValue(cp);
                if (d < 0) throw InvalidLiteral(s);
                digits.Append((char)('0' + d));
                needDigit = false;
            }
            if (needDigit) throw InvalidLiteral(s);
            if (digits.Length > IntMaxStrDigits)
            {
                throw new ValueError("Exceeds the limit (" + IntMaxStrDigits.ToString(CultureInfo.InvariantCulture)
                                     + ") for integer string conversion: value has "
                                     + digits.Length.ToString(CultureInfo.InvariantCulture)
                                     + " digits; use sys.set_int_max_str_digits() to increase the limit");
            }
            BigInteger value;
            if (digits.Length <= 18)
            {
                long v = 0;
                for (int k = 0; k < digits.Length; k++) v = v * 10 + (digits[k] - '0');
                value = v;
            }
            else
            {
                var text = digits.ToString(); // culture-ok: StringBuilder of ASCII digits
                value = BigInteger.Parse(text, NumberStyles.None, CultureInfo.InvariantCulture);
            }
            return negative ? -value : value;
        }

        // CPython: "invalid literal for int() with base 10: %.200R".
        private static ValueError InvalidLiteral(string s) =>
            new ValueError("invalid literal for int() with base 10: " + Truncate(Repr(s), 200));

        // The first n code points of s.
        private static string Truncate(string s, int n)
        {
            int i = 0;
            for (int k = 0; k < n && i < s.Length; k++)
            {
                CodePointAt(s, i, out int w);
                i += w;
            }
            return i >= s.Length ? s : s.Substring(0, i);
        }

        /// <summary>Python's type name of a JSON value, for error messages (<c>type(v).__name__</c>).</summary>
        public static string TypeName(object? v)
        {
            switch (v)
            {
                case null: return "NoneType";
                case bool _: return "bool";
                case int _:
                case long _:
                case BigInteger _: return "int";
                case double _: return "float";
                case string _: return "str";
                case List<object?> _: return "list";
                case JsonObject _: return "dict";
                case byte[] _: return "bytes";
                default: return v.GetType().Name;
            }
        }

        /// <summary>Python truthiness of a JSON value: None, False, 0, 0.0, "", [] and {} are falsy.</summary>
        public static bool Truthy(object? v)
        {
            switch (v)
            {
                case null: return false;
                case bool b: return b;
                case int i: return i != 0;
                case long l: return l != 0;
                case BigInteger bi: return !bi.IsZero;
                case double d: return d != 0.0; // NaN is truthy
                case string s: return s.Length != 0;
                case List<object?> list: return list.Count != 0;
                case JsonObject o: return o.Count != 0;
                case byte[] bytes: return bytes.Length != 0;
                default: return true;
            }
        }

        /// <summary>
        /// Python <c>d[key]</c> on a JSON value: the value of a dict's key, a KeyError for a missing key, and a
        /// TypeError (with CPython's message) when <paramref name="d"/> is not a dict.
        /// </summary>
        public static object? Get(object? d, string key)
        {
            switch (d)
            {
                case JsonObject o:
                    if (o.TryGetValue(key, out var v)) return v;
                    throw new KeyError(key);
                case List<object?> _:
                    throw new TypeError("list indices must be integers or slices, not str");
                case string _:
                    throw new TypeError("string indices must be integers");
                default:
                    throw new TypeError("'" + TypeName(d) + "' object is not subscriptable");
            }
        }

        /// <summary>
        /// UTF-8 bytes of a string. A lone surrogate is a ValueError, as Python's UnicodeEncodeError
        /// (<c>s.encode("utf-8")</c>).
        /// </summary>
        public static byte[] Utf8Bytes(string s)
        {
            var o = new List<byte>(s.Length);
            int pos = 0;
            for (int i = 0; i < s.Length; pos++)
            {
                int cp = CodePointAt(s, i, out int w);
                i += w;
                if (cp >= 0xd800 && cp <= 0xdfff)
                {
                    throw new ValueError("'utf-8' codec can't encode character " + Repr(char.ToString((char)cp))
                                         + " in position " + pos.ToString(CultureInfo.InvariantCulture)
                                         + ": surrogates not allowed");
                }
                if (cp < 0x80)
                {
                    o.Add((byte)cp);
                }
                else if (cp < 0x800)
                {
                    o.Add((byte)(0xc0 | (cp >> 6)));
                    o.Add((byte)(0x80 | (cp & 0x3f)));
                }
                else if (cp < 0x10000)
                {
                    o.Add((byte)(0xe0 | (cp >> 12)));
                    o.Add((byte)(0x80 | ((cp >> 6) & 0x3f)));
                    o.Add((byte)(0x80 | (cp & 0x3f)));
                }
                else
                {
                    o.Add((byte)(0xf0 | (cp >> 18)));
                    o.Add((byte)(0x80 | ((cp >> 12) & 0x3f)));
                    o.Add((byte)(0x80 | ((cp >> 6) & 0x3f)));
                    o.Add((byte)(0x80 | (cp & 0x3f)));
                }
            }
            return o.ToArray();
        }

        private const string UpperHex = "0123456789ABCDEF";
        private const string LowerHex = "0123456789abcdef";

        /// <summary>
        /// Python <c>urllib.parse.quote(s, safe)</c>: UTF-8 percent-encoding (uppercase hex) of every byte except
        /// <c>A-Za-z0-9_.-~</c> and the ASCII characters of <paramref name="safe"/> (a non-ASCII one is ignored,
        /// as Python's <c>safe.encode('ascii', 'ignore')</c>). A lone surrogate is a ValueError.
        /// </summary>
        public static string Quote(string s, string safe = "/")
        {
            var keep = new bool[128];
            for (int c = 'A'; c <= 'Z'; c++) keep[c] = true;
            for (int c = 'a'; c <= 'z'; c++) keep[c] = true;
            for (int c = '0'; c <= '9'; c++) keep[c] = true;
            keep['_'] = keep['.'] = keep['-'] = keep['~'] = true;
            foreach (char c in safe)
                if (c < 128) keep[c] = true;
            var o = new StringBuilder(s.Length);
            foreach (byte b in Utf8Bytes(s))
            {
                if (b < 128 && keep[b])
                {
                    o.Append((char)b);
                }
                else
                {
                    o.Append('%');
                    o.Append(UpperHex[b >> 4]);
                    o.Append(UpperHex[b & 15]);
                }
            }
            return o.ToString(); // culture-ok: StringBuilder
        }

        /// <summary>
        /// Python's <c>repr(s)</c> of a str, for messages: single quotes unless the text has a single quote and
        /// no double quote; \\, the quote, \t \n \r escaped; other non-printable characters as \xNN, \uNNNN or
        /// \UNNNNNNNN. Port limit: "non-printable" is CPython's set for the control, format, separator, surrogate
        /// and private-use characters listed below; an unassigned code point is printed as it is.
        /// </summary>
        public static string Repr(string s)
        {
            char q = s.Contains("'") && !s.Contains("\"") ? '"' : '\'';
            var o = new StringBuilder(s.Length + 2);
            o.Append(q);
            for (int i = 0; i < s.Length;)
            {
                int cp = CodePointAt(s, i, out int w);
                if (cp == q || cp == '\\')
                {
                    o.Append('\\');
                    o.Append((char)cp);
                }
                else if (cp == '\t') o.Append("\\t");
                else if (cp == '\n') o.Append("\\n");
                else if (cp == '\r') o.Append("\\r");
                else if (cp < 0x20 || cp == 0x7f) AppendEscape(o, cp);
                else if (cp < 0x7f) o.Append((char)cp);
                else if (!IsPrintable(cp)) AppendEscape(o, cp);
                else o.Append(s, i, w);
                i += w;
            }
            o.Append(q);
            return o.ToString(); // culture-ok: StringBuilder
        }

        private static void AppendEscape(StringBuilder o, int cp)
        {
            int digits;
            if (cp <= 0xff)
            {
                o.Append("\\x");
                digits = 2;
            }
            else if (cp <= 0xffff)
            {
                o.Append("\\u");
                digits = 4;
            }
            else
            {
                o.Append("\\U");
                digits = 8;
            }
            for (int k = digits - 1; k >= 0; k--) o.Append(LowerHex[(cp >> (4 * k)) & 15]);
        }

        // CPython's Py_UNICODE_ISPRINTABLE for the non-ASCII characters that matter in practice: Cc, Cf, Zs, Zl,
        // Zp, Cs and Co (not Cn, which would need the full Unicode 13 table).
        private static bool IsPrintable(int cp)
        {
            if (cp >= 0x80 && cp <= 0xa0) return false;
            if (cp == 0xad || cp == 0x61c || cp == 0x6dd || cp == 0x70f || cp == 0x8e2 || cp == 0x180e) return false;
            if (cp >= 0x600 && cp <= 0x605) return false;
            if (cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200f) || (cp >= 0x2028 && cp <= 0x202f)) return false;
            if ((cp >= 0x205f && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206f) || cp == 0x3000) return false;
            if (cp >= 0xd800 && cp <= 0xf8ff) return false; // lone surrogates, private use
            if (cp == 0xfeff || (cp >= 0xfff9 && cp <= 0xfffb) || cp == 0xfffe || cp == 0xffff) return false;
            if (cp == 0x110bd || cp == 0x110cd || (cp >= 0x13430 && cp <= 0x13438)) return false;
            if ((cp >= 0x1bca0 && cp <= 0x1bca3) || (cp >= 0x1d173 && cp <= 0x1d17a)) return false;
            if (cp == 0xe0001 || (cp >= 0xe0020 && cp <= 0xe007f) || cp >= 0xf0000) return false;
            return true;
        }

        /// <summary>
        /// Python's <c>repr(v)</c> of a JSON value, for messages: None, True, False, ints, floats
        /// (<see cref="FloatRepr"/>), str (<see cref="Repr(string)"/>), lists and dicts as Python prints them.
        /// </summary>
        public static string Repr(object? v)
        {
            switch (v)
            {
                case null: return "None";
                case bool b: return b ? "True" : "False";
                case int i: return i.ToString(CultureInfo.InvariantCulture);
                case long l: return l.ToString(CultureInfo.InvariantCulture);
                case BigInteger bi: return bi.ToString(CultureInfo.InvariantCulture);
                case double d: return FloatRepr(d);
                case string s: return Repr(s);
                case List<object?> list:
                {
                    var o = new StringBuilder("[");
                    for (int k = 0; k < list.Count; k++)
                    {
                        if (k > 0) o.Append(", ");
                        o.Append(Repr(list[k]));
                    }
                    return o.Append(']').ToString(); // culture-ok: StringBuilder
                }
                case JsonObject obj:
                {
                    var o = new StringBuilder("{");
                    bool first = true;
                    foreach (var kv in obj)
                    {
                        if (!first) o.Append(", ");
                        first = false;
                        o.Append(Repr(kv.Key)).Append(": ").Append(Repr(kv.Value));
                    }
                    return o.Append('}').ToString(); // culture-ok: StringBuilder
                }
                default:
                    return "<" + TypeName(v) + " object>";
            }
        }

        /// <summary>
        /// Python's <c>repr(x)</c> of a float: the shortest digits that round-trip, written as <c>1e+16</c> /
        /// <c>1e-05</c> when the decimal exponent is below -4 or at least 16, otherwise positionally with at
        /// least one fractional digit (<c>1.0</c>, <c>1000000000000000.0</c>); <c>nan</c>, <c>inf</c>, <c>-inf</c>,
        /// <c>-0.0</c>.
        /// </summary>
        public static string FloatRepr(double x)
        {
            if (double.IsNaN(x)) return "nan";
            if (double.IsInfinity(x)) return x > 0 ? "inf" : "-inf";
            if (x == 0.0) return BitConverter.DoubleToInt64Bits(x) < 0 ? "-0.0" : "0.0";
            ShortestDigits(Math.Abs(x), out string digits, out int decpt);
            var o = new StringBuilder(digits.Length + 8);
            if (x < 0) o.Append('-');
            int exp = decpt - 1;
            if (exp < -4 || exp >= 16)
            {
                o.Append(digits[0]);
                if (digits.Length > 1) o.Append('.').Append(digits, 1, digits.Length - 1);
                o.Append('e').Append(exp < 0 ? '-' : '+');
                int a = Math.Abs(exp);
                if (a < 10) o.Append('0');
                o.Append(a.ToString(CultureInfo.InvariantCulture));
            }
            else if (decpt <= 0)
            {
                o.Append("0.").Append('0', -decpt).Append(digits);
            }
            else if (decpt >= digits.Length)
            {
                o.Append(digits).Append('0', decpt - digits.Length).Append(".0");
            }
            else
            {
                o.Append(digits, 0, decpt).Append('.').Append(digits, decpt, digits.Length - decpt);
            }
            return o.ToString(); // culture-ok: StringBuilder
        }

        // The shortest round-trip significant digits of a > 0 (no leading or trailing zeros) and the position of
        // the decimal point: a = 0.<digits> * 10^decpt. "R" is the shortest form on .NET Core 3.0+; on a runtime
        // whose "R" falls back to 17 digits (older Mono), a shorter correctly rounded form is searched for.
        private static void ShortestDigits(double a, out string digits, out int decpt)
        {
            SplitDigits(a.ToString("R", CultureInfo.InvariantCulture), out digits, out decpt);
            for (int p = 15; p < digits.Length; p++)
            {
                var format = "E" + (p - 1).ToString(CultureInfo.InvariantCulture);
                var text = a.ToString(format, CultureInfo.InvariantCulture);
                if (double.Parse(text, NumberStyles.Float, CultureInfo.InvariantCulture) == a)
                {
                    SplitDigits(text, out digits, out decpt);
                    return;
                }
            }
        }

        // Digits and decimal point of a positive number formatted as "123.45", "1E+16", "1.5E-05" or "1.2E+017".
        private static void SplitDigits(string text, out string digits, out int decpt)
        {
            int e = 0;
            int ePos = text.Length;
            for (int k = 0; k < text.Length; k++)
            {
                if (text[k] == 'E' || text[k] == 'e')
                {
                    ePos = k;
                    break;
                }
            }
            if (ePos < text.Length)
            {
                int k = ePos + 1;
                bool neg = false;
                if (k < text.Length && (text[k] == '+' || text[k] == '-'))
                {
                    neg = text[k] == '-';
                    k++;
                }
                for (; k < text.Length; k++) e = e * 10 + (text[k] - '0');
                if (neg) e = -e;
            }
            var all = new StringBuilder(ePos);
            int intLen = ePos;
            for (int k = 0; k < ePos; k++)
            {
                if (text[k] == '.') intLen = k;
                else all.Append(text[k]);
            }
            decpt = intLen + e;
            int lead = 0;
            while (lead < all.Length && all[lead] == '0') lead++;
            int end = all.Length;
            while (end > lead && all[end - 1] == '0') end--;
            decpt -= lead;
            digits = all.ToString(lead, end - lead); // culture-ok: StringBuilder slice
        }
    }
}
