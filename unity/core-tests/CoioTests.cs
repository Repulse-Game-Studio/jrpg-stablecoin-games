// No Python twin: the transport rules of Coio (ParseRpcBody, the body rules of rpc.UrllibTransport and of the
// GDScript ScfgCoio.parse_rpc_body; ResponseFromEnvelope, tests/unit/test_coio.py's envelope cases).
using System;
using System.Collections.Generic;
using System.Text;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class CoioTests
    {
        private static byte[] U(string s) => Encoding.UTF8.GetBytes(s);

        private static TransportError Fails(int status, byte[] body) =>
            Assert.Throws<TransportError>(() => Coio.ParseRpcBody(status, body))!;

        private static readonly string Long = new string('9', 4301);

        [Test]
        public void A2xxJsonBodyIsParsed()
        {
            var got = Coio.ParseRpcBody(200, U("{\"result\": 1}"));
            Assert.That(JsonEq.FirstDiff(got, new JsonObject { { "result", 1L } }), Is.Null);
            Assert.That(Coio.ParseRpcBody(204, U("null")), Is.Null);
            Assert.That(Coio.ParseRpcBody(299, U("[1, \"x\"]")), Is.EqualTo(new List<object?> { 1L, "x" }));
        }

        [Test]
        public void An2xxBodyThatIsNotJsonIsInvalidJson()
        {
            foreach (var body in new[] { "not json", "", "<html>", "{\"a\": 1", "1 2", "\ufeff1" })
            {
                var e = Fails(200, U(body));
                Assert.That(e.Message, Is.EqualTo("invalid JSON response"), body);
                Assert.That(e.Status, Is.Null);
            }
        }

        [Test]
        public void BytesThatAreNotStrictUtf8AreInvalidJson()
        {
            var bad = new[]
            {
                new byte[] { 0x22, 0xff, 0x22 },             // not UTF-8 at all
                new byte[] { 0x22, 0xc0, 0x80, 0x22 },       // overlong NUL
                new byte[] { 0x22, 0xed, 0xa0, 0x80, 0x22 }, // a UTF-16 surrogate
                new byte[] { 0x22, 0xf4, 0x90, 0x80, 0x80, 0x22 }, // above U+10FFFF
                new byte[] { 0x22, 0xe2, 0x82, 0x22 },       // truncated sequence
            };
            foreach (var body in bad)
                Assert.That(Fails(200, body).Message, Is.EqualTo("invalid JSON response"), BitConverter.ToString(body));
            Assert.That(Coio.ParseRpcBody(200, new byte[] { 0x22, 0xe2, 0x82, 0xac, 0x22 }), Is.EqualTo("\u20ac"));
            Assert.That(Coio.ParseRpcBody(200, new byte[] { 0x22, 0xf0, 0x9f, 0x98, 0x80, 0x22 }), Is.EqualTo("\U0001F600"));
        }

        [Test]
        public void AnIntegerOfMoreThan4300DigitsIsInvalidJson()
        {
            Assert.That(Fails(200, U("{\"result\": " + Long + "}")).Message, Is.EqualTo("invalid JSON response"));
            Assert.That(Fails(200, U("[1, -" + Long + "]")).Message, Is.EqualTo("invalid JSON response"));
            var http = Fails(400, U("{\"error\": {\"code\": " + Long + "}}"));
            Assert.That(http.Message, Is.EqualTo("HTTP 400"));
            Assert.That(http.Status, Is.EqualTo(400));
            // 4300 digits are read; strings, fractions and exponents do not count
            var r = (JsonObject)Coio.ParseRpcBody(200, U("{\"r\": " + new string('9', 4300) + "}"))!;
            Assert.That(r["r"], Is.EqualTo(System.Numerics.BigInteger.Pow(10, 4300) - 1));
            var ok = (JsonObject)Coio.ParseRpcBody(200, U("{\"s\": \"" + Long + "\", \"f\": 1." + Long + ", \"e\": 1e-" + Long + "}"))!;
            Assert.That(ok["s"], Is.EqualTo(Long));
            Assert.That(ok["f"], Is.EqualTo(2.0)); // Python: float("1." + "9" * 4301) == 2.0
            Assert.That(ok["e"], Is.EqualTo(0.0));
        }

        [Test]
        public void ANon2xxAnswerIsHttpStatus()
        {
            var e = Fails(502, U("bad gateway"));
            Assert.That(e.Message, Is.EqualTo("HTTP 502"));
            Assert.That(e.Status, Is.EqualTo(502));
            foreach (var (status, body) in new[]
                     {
                         (503, ""), (429, "{\"jsonrpc\": \"2.0\"}"), (404, "{\"error\": 0}"), (404, "{\"error\": null}"),
                         (404, "[{\"error\": 1}]"), (199, "{\"result\": 1}"), (300, "{\"result\": 1}"), (0, ""),
                     })
            {
                var x = Fails(status, U(body));
                Assert.That(x.Message, Is.EqualTo("HTTP " + status.ToString(System.Globalization.CultureInfo.InvariantCulture)), body);
                Assert.That(x.Status, Is.EqualTo(status));
            }
        }

        [Test]
        public void ANon2xxJsonRpcErrorBodyPassesThrough()
        {
            var got = Coio.ParseRpcBody(400, U("{\"error\": {\"code\": -32005, \"message\": \"range too large\"}}"));
            var want = new JsonObject { { "error", new JsonObject { { "code", -32005L }, { "message", "range too large" } } } };
            Assert.That(JsonEq.FirstDiff(got, want), Is.Null);
            Assert.That(JsonEq.FirstDiff(Coio.ParseRpcBody(500, U("{\"error\": \"x\"}")), new JsonObject { { "error", "x" } }), Is.Null);
        }

        [Test]
        public void ABodyOverTheCapIsResponseTooLarge()
        {
            Assert.That(Coio.MaxResponseBytes, Is.EqualTo(16 * 1024 * 1024));
            var body = new byte[Coio.MaxResponseBytes + 1];
            for (int i = 0; i < body.Length; i++) body[i] = (byte)' ';
            body[0] = (byte)'1';
            var e = Fails(200, body);
            Assert.That(e.Message, Is.EqualTo("response too large"));
            Assert.That(e.Status, Is.Null);
            Assert.That(Fails(502, body).Message, Is.EqualTo("response too large"));
            // exactly at the cap is read
            Assert.That(Coio.ParseRpcBody(200, new ArraySegment<byte>(body, 0, Coio.MaxResponseBytes).ToArray()), Is.EqualTo(1L));
        }

        [Test]
        public void TransportErrorStatusDefaultsToNull()
        {
            Assert.That(new TransportError("x").Status, Is.Null);
            Assert.That(new TransportError("x", 500).Status, Is.EqualTo(500));
            Assert.That(new TransportError("x", 500).Message, Is.EqualTo("x"));
        }

        // ---- ResponseFromEnvelope (tests/unit/test_coio.py) ---------------------------------------------------

        private static JsonObject Env(object? status, object? body) => new JsonObject { { "status", status }, { "body", body } };

        [Test]
        public void EnvelopeToResponse()
        {
            Assert.That(JsonEq.FirstDiff(Coio.ResponseFromEnvelope(Env(200L, "{\"result\": 1}")), new JsonObject { { "result", 1L } }), Is.Null);
            Assert.That(JsonEq.FirstDiff(Coio.ResponseFromEnvelope(Env(400L, "{\"error\": {\"code\": -32005, \"message\": \"range\"}}")),
                new JsonObject { { "error", new JsonObject { { "code", -32005L }, { "message", "range" } } } }), Is.Null);
            Assert.That(Coio.ResponseFromEnvelope(Env(200L, "null")), Is.Null);
            // int_from(status): a str or a float status is read as Python's int() reads it
            Assert.That(Coio.ResponseFromEnvelope(Env("200", "1")), Is.EqualTo(1L));
            Assert.That(Coio.ResponseFromEnvelope(Env(200.7, "[]")), Is.EqualTo(new List<object?>()));
        }

        [Test]
        public void EnvelopeFailuresAreTransportErrors()
        {
            var rows = new (JsonObject, string, int?)[]
            {
                (new JsonObject { { "error", "timed out" } }, "timed out", null),
                (new JsonObject { { "error", null } }, "None", null),
                (new JsonObject { { "error", new JsonObject { { "a", new List<object?> { 1L, true } } } } }, "{'a': [1, True]}", null),
                (Env(503L, "busy"), "HTTP 503", 503),
                (Env(429L, "{\"jsonrpc\": \"2.0\"}"), "HTTP 429", 429),
                (Env(200L, "<html>"), "invalid JSON response", null),
                (Env(200L, "\ufeff1"), "invalid JSON response", null),
                (Env(200L, "{\"result\": " + Long + "}"), "invalid JSON response", null),
                (Env(0L, ""), "HTTP 0", 0),
                (Env(null, null), "HTTP 0", 0),
                (Env(502L, ""), "HTTP 502", 502),
                (Env(404L, "{\"error\": 0}"), "HTTP 404", 404),
            };
            foreach (var (env, message, status) in rows)
            {
                var e = Assert.Throws<TransportError>(() => Coio.ResponseFromEnvelope(env))!;
                Assert.That(e.Message, Is.EqualTo(message), message);
                Assert.That(e.Status, Is.EqualTo(status), message);
            }
        }

        [Test]
        public void AnEnvelopeBodyThatIsNotAStrIsATypeError()
        {
            // json.loads(5) is a TypeError, which response_from_envelope does not catch
            Assert.Throws<TypeError>(() => Coio.ResponseFromEnvelope(Env(200L, 5L)));
        }
    }
}
