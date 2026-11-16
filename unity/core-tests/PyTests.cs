// No Python twin: tests of Py (Python 3.9 built-ins), Bytes and the exception tree.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Text;
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class PyTests
    {
        // Python's str.isspace() set (what str.strip() removes).
        private static readonly int[] PySpace =
        {
            0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x85, 0xa0, 0x1680,
            0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200a,
            0x2028, 0x2029, 0x202f, 0x205f, 0x3000,
        };

        [Test]
        public void StripMatchesStrStrip()
        {
            Assert.That(Py.Strip("\u001c\u0085 x\u3000"), Is.EqualTo("x"));
            Assert.That(Py.Strip("\ufeffx"), Is.EqualTo("\ufeffx")); // the BOM is not whitespace in Python
            Assert.That(Py.Strip("\u001c  x \u3000\n"), Is.EqualTo("x"));
            Assert.That(Py.Strip("\u200bx\u180e"), Is.EqualTo("\u200bx\u180e")); // .NET's Trim set differs
            Assert.That(Py.Strip("  "), Is.EqualTo(""));
            Assert.That(Py.Strip("a b"), Is.EqualTo("a b"));
            foreach (var cp in PySpace) Assert.That(Py.Strip((char)cp + "5" + (char)cp), Is.EqualTo("5"), cp.ToString("x"));
        }

        [Test]
        public void DecimalDigitsArePython39s()
        {
            Assert.That(Py.IsDecimalDigit(0x0663), Is.True);
            Assert.That(Py.IsDecimalDigit(0x1E950), Is.True);
            // Ruling R2: U+1FBF0 (Unicode 13) is a digit in CPython 3.9; U+16AC0 (Tangsa, Unicode 14) is not.
            Assert.That(Py.IsDecimalDigit(0x1FBF0), Is.True);
            Assert.That(Py.IsDecimalDigit(0x16AC0), Is.False);
            foreach (var zero in new[] { 0x10d40, 0x116d0, 0x16d70, 0x16ac0, 0x11f50, 0x1e4f0, 0x1ccf0 })
                Assert.That(Py.IsDecimalDigit(zero + 5), Is.False, zero.ToString("x"));
            Assert.That(Py.IsDecimalDigit('0') && Py.IsDecimalDigit('9') && !Py.IsDecimalDigit('a'), Is.True);
            Assert.That(Py.IsDecimalDigit(0xb2), Is.False); // superscript two: isdigit() but not isdecimal()
            Assert.That(Py.DecimalValue(0x1d7ff), Is.EqualTo(9));
            Assert.That(Py.DecimalValue(0xff10), Is.EqualTo(0));
            Assert.That(Py.DecimalValue('x'), Is.EqualTo(-1));
        }

        [Test]
        public void LenCountsCodePoints()
        {
            Assert.That(Py.Len(string.Concat(Enumerable.Repeat("\U0001F600", 60))), Is.EqualTo(60));
            Assert.That(Py.Len(""), Is.EqualTo(0));
            Assert.That(Py.Len("a\ud800b"), Is.EqualTo(3)); // a lone surrogate is one code point
            Assert.That(Py.Len("\ude00\ud83d"), Is.EqualTo(2)); // reversed halves are not a pair
        }

        [Test]
        public void IntMatchesPythonInt()
        {
            Assert.That(Py.Int("\u0663"), Is.EqualTo(new BigInteger(3)));
            Assert.That(Py.Int(" 7 "), Is.EqualTo(new BigInteger(7)));
            Assert.That(Py.Int("1_000"), Is.EqualTo(new BigInteger(1000))); // Ruling R3
            Assert.Throws<ValueError>(() => Py.Int("1__0"));
            Assert.That(Py.Int("-5"), Is.EqualTo(new BigInteger(-5)));
            Assert.That(Py.Int("+5"), Is.EqualTo(new BigInteger(5)));
            Assert.That(Py.Int("\uff11\uff12"), Is.EqualTo(new BigInteger(12)));
            Assert.That(Py.Int("\U00011d57"), Is.EqualTo(new BigInteger(7)));
            Assert.That(Py.Int("1" + new string('0', 30)), Is.EqualTo(BigInteger.Pow(10, 30)));
            foreach (var bad in new[] { "", "abc", "1.5", "_1", "1_", "--1", "0x10", "1e3", "- 1", "   ", "1 2", "\ufeff5" })
                Assert.Throws<ValueError>(() => Py.Int(bad), bad);
            Assert.That(Assert.Throws<ValueError>(() => Py.Int("x'"))!.Message, Is.EqualTo("invalid literal for int() with base 10: \"x'\""));
        }

        [Test]
        public void IntWhitespaceIsNotStripsWhitespace()
        {
            // int() skips non-ASCII whitespace but not the ASCII separators \x1c-\x1f that strip() removes
            foreach (var cp in PySpace)
            {
                var ws = ((char)cp).ToString();
                bool rejects = cp >= 0x1c && cp <= 0x1f;
                foreach (var s in new[] { ws + "5", "5" + ws })
                {
                    if (rejects) Assert.Throws<ValueError>(() => Py.Int(s), cp.ToString("x"));
                    else Assert.That(Py.Int(s), Is.EqualTo(new BigInteger(5)), cp.ToString("x"));
                }
            }
            Assert.That(Py.Int("\u0085\u00a0\u3000 12\t\n"), Is.EqualTo(new BigInteger(12)));
            Assert.That(Py.Int(new string(' ', 100000) + "7" + new string(' ', 100000)), Is.EqualTo(new BigInteger(7)));
        }

        [Test]
        public void IntOfOtherValues()
        {
            Assert.That(Py.Int(5L), Is.EqualTo(new BigInteger(5)));
            Assert.That(Py.Int(5), Is.EqualTo(new BigInteger(5)));
            Assert.That(Py.Int(-1.9), Is.EqualTo(new BigInteger(-1)));
            Assert.That(Py.Int(1e20), Is.EqualTo(BigInteger.Pow(10, 20)));
            Assert.That(Py.Int(true), Is.EqualTo(BigInteger.One));
            Assert.That(Py.Int(BigInteger.Pow(2, 70)), Is.EqualTo(BigInteger.Pow(2, 70)));
            Assert.Throws<TypeError>(() => Py.Int((object?)null));
            Assert.Throws<TypeError>(() => Py.Int(new List<object?>()));
            Assert.Throws<TypeError>(() => Py.Int(new JsonObject()));
            Assert.Throws<ValueError>(() => Py.Int(double.NaN));
            Assert.Throws<OverflowError>(() => Py.Int(double.PositiveInfinity)); // Python: OverflowError
            Assert.That(new OverflowError("x"), Is.InstanceOf<ArithmeticError>().And.Not.InstanceOf<ValueError>());
        }

        [Test]
        public void IntDigitLimit()
        {
            Assert.That(Py.Int(new string('9', 4300)), Is.EqualTo(BigInteger.Pow(10, 4300) - 1));
            Assert.That(Py.Int(" " + string.Concat(Enumerable.Repeat("1_", 2000)) + "1 "),
                Is.EqualTo(BigInteger.Parse(new string('1', 2001), System.Globalization.CultureInfo.InvariantCulture)));
            foreach (var text in new[] { new string('9', 4301), "-" + new string('9', 4301),
                         string.Concat(Enumerable.Repeat("1_", 4300)) + "1", new string('9', 100000) })
            {
                var start = DateTime.UtcNow;
                Assert.Throws<ValueError>(() => Py.Int(text), text.Length.ToString());
                Assert.That((DateTime.UtcNow - start).TotalMilliseconds, Is.LessThan(500));
            }
        }

        [Test]
        public void TypeNameAndTruthy()
        {
            var names = new (object?, string)[]
            {
                (null, "NoneType"), (true, "bool"), (1L, "int"), (1, "int"), (BigInteger.One, "int"), (1.0, "float"),
                ("s", "str"), (new List<object?>(), "list"), (new JsonObject(), "dict"),
            };
            foreach (var (v, name) in names) Assert.That(Py.TypeName(v), Is.EqualTo(name), name);
            foreach (var f in new object?[] { null, false, 0L, 0, BigInteger.Zero, 0.0, -0.0, "", new List<object?>(), new JsonObject() })
                Assert.That(Py.Truthy(f), Is.False, Py.TypeName(f));
            foreach (var t in new object?[] { true, 1L, -1, 0.5, double.NaN, BigInteger.One, "0", new List<object?> { 0L }, new JsonObject { ["a"] = 0L } })
                Assert.That(Py.Truthy(t), Is.True, Py.TypeName(t));
        }

        [Test]
        public void GetIsDictSubscript()
        {
            var d = new JsonObject { ["a"] = 1L, ["n"] = null };
            Assert.That(Py.Get(d, "a"), Is.EqualTo(1L));
            Assert.That(Py.Get(d, "n"), Is.Null);
            var e = Assert.Throws<KeyError>(() => Py.Get(d, "b"))!;
            Assert.That(e.Message, Is.EqualTo("'b'"));
            var msgs = new (object?, string)[]
            {
                (null, "'NoneType' object is not subscriptable"),
                (new List<object?>(), "list indices must be integers or slices, not str"),
                ("s", "string indices must be integers"),
                (5L, "'int' object is not subscriptable"),
            };
            foreach (var (v, msg) in msgs) Assert.That(Assert.Throws<TypeError>(() => Py.Get(v, "a"))!.Message, Is.EqualTo(msg));
        }

        [Test]
        public void QuoteMatchesUrllib()
        {
            Assert.That(Py.Quote("a b/c~\u00e9\U0001F600?&="), Is.EqualTo("a%20b/c~%C3%A9%F0%9F%98%80%3F%26%3D"));
            Assert.That(Py.Quote("a b~_.-/\u00e9", ""), Is.EqualTo("a%20b~_.-%2F%C3%A9"));
            Assert.That(Py.Quote("\u65e5\u672c", ""), Is.EqualTo("%E6%97%A5%E6%9C%AC"));
            Assert.That(Py.Quote("!*'()[]#%&?=+\t\n", ""), Is.EqualTo("%21%2A%27%28%29%5B%5D%23%25%26%3F%3D%2B%09%0A"));
            Assert.That(Py.Quote("a:b@c", ":@\u00e9"), Is.EqualTo("a:b@c"));
            Assert.That(Py.Quote("\u00e9", "\u00e9"), Is.EqualTo("%C3%A9")); // a non-ASCII safe character is ignored
            Assert.Throws<ValueError>(() => Py.Quote("\ud800"));
            Assert.That(Py.Utf8Bytes("\u20ac"), Is.EqualTo(new byte[] { 0xe2, 0x82, 0xac }));
        }

        [Test]
        public void ReprMatchesPython()
        {
            var cases = new (string, string)[]
            {
                ("x\u0000", "'x\\x00'"), ("a'b", "\"a'b\""), ("a\"b", "'a\"b'"), ("a'\"b", "'a\\'\"b'"),
                ("\u0085\u00a0\u00ad\u2028\ufeff\U0001F600\ud800", "'\\x85\\xa0\\xad\\u2028\\ufeff\U0001F600\\ud800'"),
                ("tab\there\\", "'tab\\there\\\\'"), ("\u00e9\n\r", "'\u00e9\\n\\r'"), ("\u007f", "'\\x7f'"),
            };
            foreach (var (s, want) in cases) Assert.That(Py.Repr(s), Is.EqualTo(want), want);
            var list = new List<object?> { 1L, "a", null, true, 1.5, new JsonObject { ["k"] = new List<object?> { 2L } } };
            Assert.That(Py.Repr((object?)list), Is.EqualTo("[1, 'a', None, True, 1.5, {'k': [2]}]"));
        }

        [Test]
        public void HexAndBytes()
        {
            Assert.That(Bytes.ToHex(new byte[] { 0x00, 0x0f, 0xab, 0xff }), Is.EqualTo("000fabff"));
            Assert.That(Bytes.FromHex("0x000FabfF"), Is.EqualTo(new byte[] { 0x00, 0x0f, 0xab, 0xff }));
            Assert.That(Bytes.FromHex(""), Is.Empty);
            Assert.That(Bytes.FromHex("0x"), Is.Empty);
            foreach (var bad in new[] { "abc", "0xg0", "zz", "0X00", " 00", "\u0661\u0661" }) Assert.Throws<ValueError>(() => Bytes.FromHex(bad), bad);
            Assert.That(Bytes.Concat(new byte[] { 1 }, new byte[0], new byte[] { 2, 3 }), Is.EqualTo(new byte[] { 1, 2, 3 }));
            Assert.That(Bytes.AsciiBytes("Az"), Is.EqualTo(Encoding.ASCII.GetBytes("Az")));
            Assert.That(Bytes.Base64Encode(new byte[] { 1, 2, 3, 4 }), Is.EqualTo("AQIDBA=="));
        }

        [Test]
        public void ExceptionTree()
        {
            var c = new ConfigError("bad", "chains.empty");
            Assert.That(c, Is.InstanceOf<ValueError>());
            Assert.That(c.Code, Is.EqualTo("chains.empty"));
            Assert.That(c.Message, Is.EqualTo("bad"));
            var r = new RestoreError("tx_not_found");
            Assert.That(r.Code, Is.EqualTo("tx_not_found"));
            Assert.That(r.Message, Is.EqualTo("tx_not_found"));
            Assert.That(r, Is.Not.InstanceOf<ValueError>());
            Assert.That(new IndexError().Message, Is.EqualTo("list index out of range"));
            Assert.That(new KeyError("k").Key, Is.EqualTo("k"));
            Assert.That(new TypeError("t"), Is.Not.InstanceOf<ValueError>());
        }
    }
}
