// No Python twin: Python's json module (json.loads as ints.json_loads calls it, and json.dumps), by hand:
// no reflection-based serializer survives IL2CPP stripping.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using System.Text;

namespace Scfg.Core
{
    /// <summary>
    /// JSON values are null, bool, long, BigInteger (only for an integer outside long), double, string,
    /// <c>List&lt;object?&gt;</c> and <see cref="JsonObject"/>.
    /// </summary>
    public static class Json
    {
        /// <summary>
        /// The deepest nesting of arrays and objects Loads reads and Dumps writes. Python raises RecursionError
        /// near its recursion limit (1000); here deeper nesting is a ValueError rather than a stack overflow.
        /// </summary>
        public const int MaxDepth = 1000;

        /// <summary>
        /// Python's <c>json.loads(text, parse_int=ints.int_from)</c>: an integer literal stays an integer (long, or
        /// BigInteger outside long) and one of more than 4300 digits is a ValueError; a fraction or exponent makes
        /// a double (<c>1e400</c> is infinity, as Python's float()). The constants <c>NaN</c>, <c>Infinity</c> and
        /// <c>-Infinity</c> are read as Python reads them (double.NaN and the infinities; unlike the TypeScript
        /// core's JSON.parse), so Loads(Dumps(x)) round-trips a non-finite double. Malformed text, trailing data
        /// and a BOM are ValueErrors, with json.JSONDecodeError's message.
        /// </summary>
        public static object? Loads(string text) => new Reader(text).Document();

        private sealed class Reader
        {
            private readonly string _s;
            private int _i;
            private int _depth;

            public Reader(string s)
            {
                _s = s;
            }

            public object? Document()
            {
                if (_s.Length > 0 && _s[0] == '\ufeff') throw Error("Unexpected UTF-8 BOM (decode using utf-8-sig)", 0);
                SkipWhitespace();
                var v = Value();
                SkipWhitespace();
                if (_i != _s.Length) throw Error("Extra data", _i);
                return v;
            }

            private ValueError Error(string message, int pos)
            {
                // JSONDecodeError's "msg: line L column C (char N)", counted in code points as Python does.
                int line = 1;
                int lineStart = 0;
                for (int k = 0; k < pos; k++)
                {
                    if (_s[k] == '\n')
                    {
                        line++;
                        lineStart = k + 1;
                    }
                }
                int col = Py.Len(_s.Substring(lineStart, pos - lineStart)) + 1;
                int ch = Py.Len(_s.Substring(0, pos));
                return new ValueError(message + ": line " + line.ToString(CultureInfo.InvariantCulture)
                                      + " column " + col.ToString(CultureInfo.InvariantCulture)
                                      + " (char " + ch.ToString(CultureInfo.InvariantCulture) + ")");
            }

            private void SkipWhitespace()
            {
                while (_i < _s.Length)
                {
                    char c = _s[_i];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
                    _i++;
                }
            }

            private bool At(string word) => string.CompareOrdinal(_s, _i, word, 0, word.Length) == 0;

            private object? Value()
            {
                if (_i >= _s.Length) throw Error("Expecting value", _i);
                char c = _s[_i];
                switch (c)
                {
                    case '"':
                        return String();
                    case '{':
                        return Object();
                    case '[':
                        return Array();
                    case 'n' when At("null"):
                        _i += 4;
                        return null;
                    case 't' when At("true"):
                        _i += 4;
                        return true;
                    case 'f' when At("false"):
                        _i += 5;
                        return false;
                    // json's default parse_constant: exactly NaN, Infinity and -Infinity (not nan, +Infinity, -NaN).
                    case 'N' when At("NaN"):
                        _i += 3;
                        return double.NaN;
                    case 'I' when At("Infinity"):
                        _i += 8;
                        return double.PositiveInfinity;
                    case '-' when At("-Infinity"):
                        _i += 9;
                        return double.NegativeInfinity;
                    default:
                        if (c == '-' || (c >= '0' && c <= '9')) return Number();
                        throw Error("Expecting value", _i);
                }
            }

            private void Enter(int pos)
            {
                if (++_depth > MaxDepth) throw Error("JSON nested deeper than " + MaxDepth.ToString(CultureInfo.InvariantCulture) + " levels", pos);
            }

            private JsonObject Object()
            {
                Enter(_i);
                _i++; // {
                var o = new JsonObject();
                SkipWhitespace();
                if (_i < _s.Length && _s[_i] == '}')
                {
                    _i++;
                    _depth--;
                    return o;
                }
                while (true)
                {
                    if (_i >= _s.Length || _s[_i] != '"') throw Error("Expecting property name enclosed in double quotes", _i);
                    var key = String();
                    SkipWhitespace();
                    if (_i >= _s.Length || _s[_i] != ':') throw Error("Expecting ':' delimiter", _i);
                    _i++;
                    SkipWhitespace();
                    o[key] = Value();
                    SkipWhitespace();
                    if (_i < _s.Length && _s[_i] == '}')
                    {
                        _i++;
                        _depth--;
                        return o;
                    }
                    if (_i >= _s.Length || _s[_i] != ',') throw Error("Expecting ',' delimiter", _i);
                    _i++;
                    SkipWhitespace();
                }
            }

            private List<object?> Array()
            {
                Enter(_i);
                _i++; // [
                var list = new List<object?>();
                SkipWhitespace();
                if (_i < _s.Length && _s[_i] == ']')
                {
                    _i++;
                    _depth--;
                    return list;
                }
                while (true)
                {
                    list.Add(Value());
                    SkipWhitespace();
                    if (_i < _s.Length && _s[_i] == ']')
                    {
                        _i++;
                        _depth--;
                        return list;
                    }
                    if (_i >= _s.Length || _s[_i] != ',') throw Error("Expecting ',' delimiter", _i);
                    _i++;
                    SkipWhitespace();
                }
            }

            private string String()
            {
                int open = _i;
                _i++; // "
                int runStart = _i;
                StringBuilder? sb = null;
                while (true)
                {
                    if (_i >= _s.Length) throw Error("Unterminated string starting at", open);
                    char c = _s[_i];
                    if (c == '"')
                    {
                        string result;
                        if (sb == null)
                        {
                            result = _s.Substring(runStart, _i - runStart);
                        }
                        else
                        {
                            sb.Append(_s, runStart, _i - runStart);
                            result = sb.ToString(); // culture-ok: StringBuilder
                        }
                        _i++;
                        return result;
                    }
                    if (c < 0x20) throw Error("Invalid control character at", _i);
                    if (c != '\\')
                    {
                        _i++;
                        continue;
                    }
                    sb ??= new StringBuilder();
                    sb.Append(_s, runStart, _i - runStart);
                    int esc = _i;
                    if (_i + 1 >= _s.Length) throw Error("Unterminated string starting at", open);
                    char e = _s[_i + 1];
                    _i += 2;
                    switch (e)
                    {
                        case '"': sb.Append('"'); break;
                        case '\\': sb.Append('\\'); break;
                        case '/': sb.Append('/'); break;
                        case 'b': sb.Append('\b'); break;
                        case 'f': sb.Append('\f'); break;
                        case 'n': sb.Append('\n'); break;
                        case 'r': sb.Append('\r'); break;
                        case 't': sb.Append('\t'); break;
                        case 'u':
                        {
                            // CPython wants a character after the four digits (the closing quote at least), so text
                            // that ends right after them reports the escape, not the unterminated string.
                            if (_i + 4 >= _s.Length) throw Error("Invalid \\uXXXX escape", esc + 1);
                            int u = 0;
                            for (int k = 0; k < 4; k++)
                            {
                                int h = HexValue(_s[_i + k]);
                                if (h < 0) throw Error("Invalid \\uXXXX escape", esc + 1);
                                u = u * 16 + h;
                            }
                            _i += 4;
                            // Surrogate halves are appended as they are: an escaped high-low pair is one code point in
                            // UTF-16, and a lone one stays lone, as in Python.
                            sb.Append((char)u);
                            break;
                        }
                        default:
                            throw Error("Invalid \\escape", esc);
                    }
                    runStart = _i;
                }
            }

            private static int HexValue(char c)
            {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            }

            private bool DigitAt(int k) => k < _s.Length && _s[k] >= '0' && _s[k] <= '9';

            // CPython's _json scanner: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)? in ASCII digits. A '.' or an
            // exponent without digits after it is not part of the number (what follows is then an error).
            private object Number()
            {
                int start = _i;
                if (_s[_i] == '-') _i++;
                if (_i < _s.Length && _s[_i] == '0') _i++;
                else if (DigitAt(_i)) while (DigitAt(_i)) _i++;
                else throw Error("Expecting value", start);
                bool isFloat = false;
                if (_i < _s.Length && _s[_i] == '.' && DigitAt(_i + 1))
                {
                    _i++;
                    while (DigitAt(_i)) _i++;
                    isFloat = true;
                }
                if (_i < _s.Length && (_s[_i] == 'e' || _s[_i] == 'E'))
                {
                    int k = _i + 1;
                    if (k < _s.Length && (_s[k] == '+' || _s[k] == '-')) k++;
                    if (DigitAt(k))
                    {
                        while (DigitAt(k)) k++;
                        _i = k;
                        isFloat = true;
                    }
                }
                var literal = _s.Substring(start, _i - start);
                return isFloat ? FloatFrom(literal) : IntegerFrom(literal);
            }

            private static object IntegerFrom(string literal)
            {
                bool negative = literal[0] == '-';
                int digits = literal.Length - (negative ? 1 : 0);
                if (digits <= 18)
                {
                    long v = 0;
                    for (int k = negative ? 1 : 0; k < literal.Length; k++) v = v * 10 + (literal[k] - '0');
                    return negative ? -v : v;
                }
                if (digits > Ints.IntMaxStrDigits) throw Ints.TooManyDigits(digits);
                var big = BigInteger.Parse(literal, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture);
                if (big >= long.MinValue && big <= long.MaxValue) return (long)big;
                return big;
            }

            private static double FloatFrom(string literal)
            {
                try
                {
                    return double.Parse(literal, NumberStyles.Float, CultureInfo.InvariantCulture);
                }
                catch (OverflowException)
                {
                    // .NET Core 3.0+ returns the infinity itself; older runtimes throw.
                    return literal[0] == '-' ? double.NegativeInfinity : double.PositiveInfinity;
                }
            }
        }

        /// <summary>
        /// Python's <c>json.dumps(v, sort_keys=sortKeys, indent=indent, ensure_ascii=ensureAscii)</c>, byte for
        /// byte: separators <c>", "</c> and <c>": "</c>, or <c>","</c> and newlines when indented; floats as
        /// Python's repr (NaN, Infinity, -Infinity as Python writes them); keys sorted by code point. Lists may be
        /// any <c>IReadOnlyList&lt;object?&gt;</c>; ints may also be int. Anything else is a TypeError.
        /// </summary>
        public static string Dumps(object? v, bool sortKeys = false, int? indent = null, bool ensureAscii = true)
        {
            var w = new Writer(sortKeys, indent, ensureAscii);
            w.Write(v, 0);
            return w.Text;
        }

        private sealed class Writer
        {
            private readonly StringBuilder _o = new StringBuilder();
            private readonly bool _sortKeys;
            private readonly int? _indent;
            private readonly bool _ensureAscii;

            public Writer(bool sortKeys, int? indent, bool ensureAscii)
            {
                _sortKeys = sortKeys;
                _indent = indent;
                _ensureAscii = ensureAscii;
            }

            public string Text => _o.ToString(); // culture-ok: StringBuilder

            private void Newline(int level)
            {
                _o.Append('\n');
                _o.Append(' ', _indent!.Value * level);
            }

            private string ItemSeparator => _indent == null ? ", " : ",";

            public void Write(object? v, int depth)
            {
                switch (v)
                {
                    case null: _o.Append("null"); return;
                    case bool b: _o.Append(b ? "true" : "false"); return;
                    case int i: _o.Append(i.ToString(CultureInfo.InvariantCulture)); return;
                    case long l: _o.Append(l.ToString(CultureInfo.InvariantCulture)); return;
                    case BigInteger bi: _o.Append(bi.ToString(CultureInfo.InvariantCulture)); return;
                    case double d: _o.Append(FloatText(d)); return;
                    case string s: WriteString(s); return;
                    case JsonObject obj: WriteObject(obj, depth + 1); return;
                    case IReadOnlyList<object?> list: WriteList(list, depth + 1); return;
                    default: throw new TypeError("Object of type " + Py.TypeName(v) + " is not JSON serializable");
                }
            }

            private static string FloatText(double d)
            {
                if (double.IsNaN(d)) return "NaN";
                if (double.IsPositiveInfinity(d)) return "Infinity";
                if (double.IsNegativeInfinity(d)) return "-Infinity";
                return Py.FloatRepr(d);
            }

            private static void CheckDepth(int depth)
            {
                if (depth > MaxDepth)
                    throw new ValueError("JSON nested deeper than " + MaxDepth.ToString(CultureInfo.InvariantCulture) + " levels (or a circular reference)");
            }

            private void WriteList(IReadOnlyList<object?> list, int depth)
            {
                CheckDepth(depth);
                if (list.Count == 0)
                {
                    _o.Append("[]");
                    return;
                }
                _o.Append('[');
                for (int k = 0; k < list.Count; k++)
                {
                    if (k > 0) _o.Append(ItemSeparator);
                    if (_indent != null) Newline(depth);
                    Write(list[k], depth);
                }
                if (_indent != null) Newline(depth - 1);
                _o.Append(']');
            }

            private void WriteObject(JsonObject obj, int depth)
            {
                CheckDepth(depth);
                if (obj.Count == 0)
                {
                    _o.Append("{}");
                    return;
                }
                IEnumerable<KeyValuePair<string, object?>> items = obj;
                if (_sortKeys)
                {
                    var sorted = new List<KeyValuePair<string, object?>>(obj);
                    sorted.Sort((a, b) => Py.CompareCodePoints(a.Key, b.Key));
                    items = sorted;
                }
                _o.Append('{');
                bool first = true;
                foreach (var kv in items)
                {
                    if (!first) _o.Append(ItemSeparator);
                    first = false;
                    if (_indent != null) Newline(depth);
                    WriteString(kv.Key);
                    _o.Append(": ");
                    Write(kv.Value, depth);
                }
                if (_indent != null) Newline(depth - 1);
                _o.Append('}');
            }

            private const string Hex = "0123456789abcdef";

            // ensure_ascii: everything outside ' '..'~' as \uXXXX (UTF-16 units, so astral characters become
            // surrogate pairs, as Python writes them). Otherwise only the quote, backslash and controls.
            private void WriteString(string s)
            {
                _o.Append('"');
                foreach (char c in s)
                {
                    switch (c)
                    {
                        case '"': _o.Append("\\\""); break;
                        case '\\': _o.Append("\\\\"); break;
                        case '\n': _o.Append("\\n"); break;
                        case '\r': _o.Append("\\r"); break;
                        case '\t': _o.Append("\\t"); break;
                        case '\b': _o.Append("\\b"); break;
                        case '\f': _o.Append("\\f"); break;
                        default:
                            if (c < 0x20 || (_ensureAscii && c > 0x7e))
                            {
                                _o.Append("\\u");
                                _o.Append(Hex[(c >> 12) & 15]).Append(Hex[(c >> 8) & 15]).Append(Hex[(c >> 4) & 15]).Append(Hex[c & 15]);
                            }
                            else
                            {
                                _o.Append(c);
                            }
                            break;
                    }
                }
                _o.Append('"');
            }
        }
    }
}
