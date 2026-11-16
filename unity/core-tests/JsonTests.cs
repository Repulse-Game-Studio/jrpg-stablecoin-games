// No Python twin: tests of Json (Python's json.loads / json.dumps), JsonObject and JsonEq.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Numerics;
using System.Threading;
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class JsonTests
    {
        [Test]
        public void IntegersStayIntegers()
        {
            Assert.That(Json.Loads("12345678901234567890123"), Is.EqualTo(BigInteger.Parse("12345678901234567890123", CultureInfo.InvariantCulture)));
            Assert.That(Json.Loads("12345678901234567890123"), Is.TypeOf<BigInteger>());
            Assert.That(Json.Loads("1"), Is.TypeOf<long>().And.EqualTo(1L));
            Assert.That(Json.Loads("1.0"), Is.TypeOf<double>().And.EqualTo(1.0));
            Assert.That(Json.Loads("-9223372036854775808"), Is.TypeOf<long>().And.EqualTo(long.MinValue));
            Assert.That(Json.Loads("9223372036854775808"), Is.TypeOf<BigInteger>());
            Assert.That(Json.Loads("1e2"), Is.TypeOf<double>().And.EqualTo(100.0));
            Assert.That(Json.Loads("-0"), Is.TypeOf<long>().And.EqualTo(0L));
            Assert.That(Json.Loads("1e400"), Is.EqualTo(double.PositiveInfinity)); // float('1e400') is inf
        }

        [Test]
        public void IntegerDigitLimit()
        {
            Assert.Throws<ValueError>(() => Json.Loads(new string('9', 4301)));
            Assert.Throws<ValueError>(() => Json.Loads("[-" + new string('9', 4301) + "]"));
            Assert.That(Json.Loads(new string('9', 4300)), Is.TypeOf<BigInteger>());
            Assert.That(Json.Loads("-" + new string('9', 4300)), Is.TypeOf<BigInteger>());
            // a fraction or an exponent makes it a float, which has no digit limit
            Assert.That(Json.Loads(new string('9', 4301) + ".0"), Is.TypeOf<double>());
        }

        [Test]
        public void RoundTripsPythonLayout()
        {
            Assert.That(Json.Dumps(Json.Loads("{\"b\":1,\"a\":[true,null,\"\u00e9\"]}")), Is.EqualTo("{\"b\": 1, \"a\": [true, null, \"\\u00e9\"]}"));
            Assert.That(Json.Dumps(Json.Loads("{\"b\":1,\"a\":[true,null,\"\u00e9\"]}"), ensureAscii: false), Is.EqualTo("{\"b\": 1, \"a\": [true, null, \"\u00e9\"]}"));
            Assert.That(Json.Dumps(Json.Loads("{\"b\":1,\"a\":[],\"c\":{},\"d\":[1,{\"x\":2}]}"), indent: 2),
                Is.EqualTo("{\n  \"b\": 1,\n  \"a\": [],\n  \"c\": {},\n  \"d\": [\n    1,\n    {\n      \"x\": 2\n    }\n  ]\n}"));
            Assert.That(Json.Dumps(Json.Loads("{\"b\":1,\"a\":[]}"), indent: 0), Is.EqualTo("{\n\"b\": 1,\n\"a\": []\n}"));
        }

        [Test]
        public void SortKeysOrdersByCodePoint()
        {
            var o = new JsonObject { ["b"] = 1L, ["B"] = 2L, ["a"] = 3L, ["\uff5e"] = 4L, ["\U0001F600"] = 5L, ["ab"] = 6L };
            // Python sorts str by code point: U+FF5E (one UTF-16 unit, 0xFF5E) is below U+1F600 (0xD83D...).
            Assert.That(Json.Dumps(o, sortKeys: true, ensureAscii: false),
                Is.EqualTo("{\"B\": 2, \"a\": 3, \"ab\": 6, \"b\": 1, \"\uff5e\": 4, \"\U0001F600\": 5}"));
        }

        [Test]
        public void FloatsUsePythonRepr()
        {
            Assert.That(Json.Dumps(1e16), Is.EqualTo("1e+16"));
            Assert.That(Json.Dumps(0.1), Is.EqualTo("0.1"));
            var cases = new (double, string)[]
            {
                (1e-5, "1e-05"), (1e15, "1000000000000000.0"), (0.1 + 0.2, "0.30000000000000004"), (-0.0, "-0.0"),
                (1.5e300, "1.5e+300"), (123456789.0, "123456789.0"), (5e-324, "5e-324"), (1e22, "1e+22"),
                (0.0001, "0.0001"), (1.0, "1.0"), (-2.5, "-2.5"), (1.7976931348623157e308, "1.7976931348623157e+308"),
                (123456789012345678.0, "1.2345678901234568e+17"), (0.000123, "0.000123"), (1234.5e-10, "1.2345e-07"),
            };
            foreach (var (x, want) in cases) Assert.That(Json.Dumps(x), Is.EqualTo(want), want);
            Assert.That(Json.Dumps(new List<object?> { double.NaN, double.PositiveInfinity, double.NegativeInfinity }),
                Is.EqualTo("[NaN, Infinity, -Infinity]"));
            Assert.That(Py.FloatRepr(double.NaN), Is.EqualTo("nan"));
            Assert.That(Py.FloatRepr(double.NegativeInfinity), Is.EqualTo("-inf"));
        }

        [Test]
        public void CultureDoesNotLeak()
        {
            var saved = Thread.CurrentThread.CurrentCulture;
            try
            {
                foreach (var name in new[] { "de-DE", "tr-TR", "ar-SA" })
                {
                    Thread.CurrentThread.CurrentCulture = new CultureInfo(name);
                    Assert.That(Json.Dumps(Json.Loads("[1.5, -2, 12345678901234567890123, 1e-07]")),
                        Is.EqualTo("[1.5, -2, 12345678901234567890123, 1e-07]"), name);
                }
            }
            finally
            {
                Thread.CurrentThread.CurrentCulture = saved;
            }
        }

        [Test]
        public void StringEscapes()
        {
            Assert.That(Json.Dumps("\u007f\u001f\u2028\U0001F600\"\\\b\f\n\r\t/"),
                Is.EqualTo("\"\\u007f\\u001f\\u2028\\ud83d\\ude00\\\"\\\\\\b\\f\\n\\r\\t/\""));
            Assert.That(Json.Dumps("\u007f\u001f\u2028\U0001F600", ensureAscii: false), Is.EqualTo("\"\u007f\\u001f\u2028\U0001F600\""));
            Assert.That(Json.Loads("\"\\ud83d\\ude00\\u00E9\\/\\b\\f\\n\\r\\t\\\"\\\\\""), Is.EqualTo("\U0001F600\u00e9/\b\f\n\r\t\"\\"));
            Assert.That(Json.Loads("\"\\ud800\""), Is.EqualTo("\ud800")); // Python keeps a lone surrogate
        }

        [TestCase("{\"a\":1,}")]
        [TestCase("[1,]")]
        [TestCase("01")]
        [TestCase("\"\u0001\"")]
        [TestCase("\ufeff1")]
        [TestCase("\"\\x\"")]
        [TestCase("1 2")]
        [TestCase("")]
        [TestCase("   ")]
        [TestCase("-")]
        [TestCase("[")]
        [TestCase("{\"a\" 1}")]
        [TestCase("\"\\u12\"")]
        [TestCase("\"abc")]
        [TestCase("NaN")]
        [TestCase("Infinity")]
        [TestCase("-Infinity")]
        [TestCase("[1.]")]
        [TestCase("[.5]")]
        [TestCase("[1e]")]
        [TestCase("{1: 2}")]
        [TestCase("tru")]
        [TestCase("[1] x")]
        [TestCase("\u00a01")] // only space, tab, CR and LF are JSON whitespace
        [TestCase("[\uff11]")] // a fullwidth digit is not a JSON digit
        public void RejectsWhatJsonLoadsRejects(string text)
        {
            Assert.Throws<ValueError>(() => Json.Loads(text));
        }

        [Test]
        public void DuplicateKeysKeepFirstPositionLastValue()
        {
            var o = (JsonObject)Json.Loads(" {\"a\":1,\"b\":3,\"a\":2}\r\n\t")!;
            Assert.That(o.Keys, Is.EqualTo(new[] { "a", "b" }));
            Assert.That(o["a"], Is.EqualTo(2L));
        }

        [Test]
        public void DeepNestingIsAValueErrorNotACrash()
        {
            Assert.That(Json.Loads(new string('[', 500) + new string(']', 500)), Is.Not.Null);
            Assert.Throws<ValueError>(() => Json.Loads(new string('[', 100000) + new string(']', 100000)));
        }

        [Test]
        public void JsonObjectKeepsInsertionOrder()
        {
            var o = new JsonObject();
            o["a"] = 1L;
            o["b"] = 2L;
            o.Remove("a");
            o["a"] = 3L;
            Assert.That(o.Keys, Is.EqualTo(new[] { "b", "a" }));
            Assert.That(o.Count, Is.EqualTo(2));
            Assert.That(o.ContainsKey("a") && !o.ContainsKey("c"), Is.True);
            Assert.That(o.TryGetValue("b", out var b) && Equals(b, 2L), Is.True);
            Assert.That(o.TryGetValue("c", out _), Is.False);
            Assert.That(o.Remove("c"), Is.False);
            Assert.Throws<KeyError>(() => { var _ = o["c"]; });
            Assert.That(o.Select(kv => kv.Key + "=" + Json.Dumps(kv.Value)), Is.EqualTo(new[] { "b=2", "a=3" }));
            o["b"] = 4L; // reassigning keeps the position
            Assert.That(Json.Dumps(o), Is.EqualTo("{\"b\": 4, \"a\": 3}"));
        }

        [Test]
        public void JsonObjectSurvivesManyRemovals()
        {
            var o = new JsonObject();
            for (int i = 0; i < 1000; i++) o["k" + i.ToString(CultureInfo.InvariantCulture)] = (long)i;
            for (int i = 0; i < 999; i++) Assert.That(o.Remove("k" + i.ToString(CultureInfo.InvariantCulture)), Is.True);
            Assert.That(o.Keys, Is.EqualTo(new[] { "k999" }));
            o["z"] = null;
            Assert.That(Json.Dumps(o), Is.EqualTo("{\"k999\": 999, \"z\": null}"));
        }

        [Test]
        public void JsonObjectRefusesMutationWhileIterating()
        {
            var o = new JsonObject { ["a"] = 1L, ["b"] = 2L };
            Assert.Throws<InvalidOperationException>(() => { foreach (var kv in o) o["c"] = 3L; });
        }

        [Test]
        public void DumpsRefusesWhatJsonCannotSerialize()
        {
            Assert.Throws<TypeError>(() => Json.Dumps(new object()));
            Assert.Throws<TypeError>(() => Json.Dumps(new byte[] { 1 }));
            Assert.That(Json.Dumps(new List<object?> { 1, 2L, BigInteger.Pow(2, 70), "x", false }),
                Is.EqualTo("[1, 2, 1180591620717411303424, \"x\", false]"));
        }

        // Every vector file is json.dumps(obj, sort_keys=True, indent=1, ensure_ascii=False) + "\n" (README):
        // reading and re-dumping it reproduces it byte for byte.
        [Test]
        public void EveryVectorFileRoundTrips()
        {
            var root = Vectors.Root;
            var files = System.IO.Directory.GetFiles(root, "*.json", System.IO.SearchOption.AllDirectories);
            Assert.That(files.Length, Is.GreaterThan(100));
            foreach (var file in files)
            {
                var rel = System.IO.Path.GetRelativePath(root, file).Replace('\\', '/');
                var text = Vectors.ReadText(rel);
                var again = Json.Dumps(Json.Loads(text), sortKeys: true, indent: 1, ensureAscii: false) + "\n";
                Assert.That(again == text, Is.True, rel);
            }
        }

        [Test]
        public void JsonEqIsTheReadmeRule()
        {
            Assert.That(JsonEq.Equal(Json.Loads("{\"a\":1,\"b\":[1,2]}"), Json.Loads("{\"b\":[1,2.0],\"a\":1.0}")), Is.True);
            Assert.That(JsonEq.Equal(true, 1L), Is.False);
            Assert.That(JsonEq.Equal(false, 0.0), Is.False);
            Assert.That(JsonEq.Equal(1, 1L), Is.True);
            Assert.That(JsonEq.Equal(BigInteger.Pow(2, 70), Math.Pow(2, 70)), Is.True);
            Assert.That(JsonEq.Equal(9007199254740993L, 9007199254740992.0), Is.False); // no lossy conversion
            Assert.That(JsonEq.Equal(null, null), Is.True);
            Assert.That(JsonEq.Equal(null, ""), Is.False);
            Assert.That(JsonEq.Equal(Json.Loads("[1,2]"), Json.Loads("[2,1]")), Is.False);
            Assert.That(JsonEq.FirstDiff(Json.Loads("{\"a\":{\"b\":[1,2]}}"), Json.Loads("{\"a\":{\"b\":[1,3]}}")), Is.EqualTo("a.b[1]: 2 != 3"));
            Assert.That(JsonEq.FirstDiff(Json.Loads("{\"a\":1}"), Json.Loads("{\"a\":1,\"c\":2}")), Is.EqualTo("c: missing on the left"));
            Assert.That(JsonEq.FirstDiff(Json.Loads("{\"a\":1,\"c\":2}"), Json.Loads("{\"a\":1}")), Is.EqualTo("c: missing on the right"));
            Assert.That(JsonEq.FirstDiff(Json.Loads("[1]"), Json.Loads("[1,2]")), Is.EqualTo("length 1 != 2"));
            Assert.That(JsonEq.FirstDiff("a", "a"), Is.Null);
        }

        [Test]
        public void DeepCopyCopiesContainers()
        {
            var src = (JsonObject)Json.Loads("{\"a\":[1,{\"b\":2}],\"c\":\"s\"}")!;
            var copy = (JsonObject)DeepCopy.Of(src)!;
            Assert.That(JsonEq.Equal(src, copy), Is.True);
            ((JsonObject)((List<object?>)copy["a"]!)[1]!)["b"] = 3L;
            Assert.That(Json.Dumps(src), Is.EqualTo("{\"a\": [1, {\"b\": 2}], \"c\": \"s\"}"));
            Assert.That(DeepCopy.Of(BigInteger.One), Is.EqualTo(BigInteger.One));
            Assert.Throws<TypeError>(() => DeepCopy.Of(new object()));
        }
    }
}
