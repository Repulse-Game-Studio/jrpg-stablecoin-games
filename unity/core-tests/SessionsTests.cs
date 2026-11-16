// No Python twin: direct checks of Sessions/Receipts against scfg_core output (normalize_tx_hash lives in the
// tx_ids topic, which is enabled with normalize_any_tx in task 10; these keep it covered until then).
using System;
using System.Collections.Generic;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class SessionsTests
    {
        private const string H = "0xABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789";

        private sealed class Scripted : IEntropy
        {
            public readonly List<string> Log = new List<string>();
            public string TokenHex(int n) { Log.Add("token_hex " + n); return new string('a', 2 * n); }
            public long RandBelow(long n) { Log.Add("randbelow " + n); return 42; }
            public byte[] TokenBytes(int n) { Log.Add("token_bytes " + n); return new byte[n]; }
        }

        private static JsonObject Create(string kind, object? price, long? nonce, Scripted e, JsonObject? extra = null) =>
            Sessions.CreateSession(new JsonObject(), kind, null, "t", "USDC", "base", price, 2, 100, 1000L, nonce, extra, e);

        [Test]
        public void CreateSessionDrawOrder()
        {
            var e = new Scripted();
            Create("unlock", "4.99", null, e);
            Assert.That(e.Log, Is.EqualTo(new[] { "randbelow 100", "token_hex 8" }));

            e = new Scripted();
            Create("unlock", "4.99", 7, e);
            Assert.That(e.Log, Is.EqualTo(new[] { "token_hex 8" }));

            e = new Scripted();
            Assert.Throws<ValueError>(() => Create("refund", "4.99", null, e));
            Assert.That(e.Log, Is.Empty);

            e = new Scripted();
            Assert.Throws<ValueError>(() => Create("unlock", "abc", null, e));
            Assert.That(e.Log, Is.EqualTo(new[] { "randbelow 100" })); // Python draws the nonce before parsing the price
        }

        private static JsonObject Store(params object?[] createdAt)
        {
            var s = new JsonObject();
            for (int i = 0; i < createdAt.Length; i++)
                s["k" + i] = new JsonObject { { "id", (long)i }, { "created_at", createdAt[i] } };
            return s;
        }

        private static string Order(params object?[] createdAt)
        {
            var ids = new List<string>();
            foreach (var r in Sessions.ListSessions(Store(createdAt))) ids.Add(r["id"]!.ToString()!);
            return string.Join(",", ids);
        }

        // Expected values from scfg_core.sessions.list_sessions run under CPython.
        [Test]
        public void ListSessionsOrdersAsPythonSorted()
        {
            Assert.That(Order("x"), Is.EqualTo("0"));             // a lone record is never compared
            Assert.That(Order((object?)null), Is.EqualTo("0"));
            Assert.That(Order("b", "a"), Is.EqualTo("1,0"));      // strings order by code point
            Assert.That(Order(2L, 1.5), Is.EqualTo("1,0"));       // int vs float
            Assert.That(Order(true, 0L), Is.EqualTo("1,0"));      // bool vs int
            Assert.That(Order(true, 1L, 0.5), Is.EqualTo("2,0,1"));
            Assert.That(Order(1L, 1.0, 1L), Is.EqualTo("0,1,2")); // stable
        }

        [Test]
        public void ListSessionsRaisesPythonTypeError()
        {
            Assert.Throws<TypeError>(() => Order("x", 1L));
            Assert.Throws<TypeError>(() => Order(1L, "x"));
            Assert.Throws<TypeError>(() => Order(1L, 2L, "x"));
            Assert.Throws<TypeError>(() => Order(null, null));
        }

        [Test]
        public void NormalizeTxHashLowercasesAndStrips()
        {
            Assert.That(Sessions.NormalizeTxHash(H), Is.EqualTo(H.ToLowerInvariant()));
            Assert.That(Sessions.NormalizeTxHash(" \t" + H + "\n"), Is.EqualTo(H.ToLowerInvariant()));
            Assert.That(Sessions.NormalizeTxHash(" " + H), Is.EqualTo(H.ToLowerInvariant())); // Py.Strip: em space
        }

        [TestCase("")]
        [TestCase("0xabc")]
        [TestCase("0Xabcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789")]
        [TestCase("0xabcdef0123456789abcdef0123456789abcdef0123456789abcdef012345678g")]
        [TestCase("abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567890a")]
        public void NormalizeTxHashRejectsMalformed(string text)
        {
            Assert.Throws<ValueError>(() => Sessions.NormalizeTxHash(text));
        }

        [Test]
        public void NormalizeTxHashRejectsNonStrings()
        {
            var e = Assert.Throws<ValueError>(() => Sessions.NormalizeTxHash(null))!;
            Assert.That(e.Message, Is.EqualTo("not a transaction hash: None"));
            Assert.Throws<ValueError>(() => Sessions.NormalizeTxHash(5L));
        }

        [Test]
        public void CreateSessionDrawsNonceThenId()
        {
            var store = new JsonObject();
            var rec = Sessions.CreateSession(store, "unlock", "ch2", "Chapter 2", "USDC", "base", "4.99", 2, 100, 1000L,
                null, null, new Scripted());
            Assert.That(rec["id"], Is.EqualTo("ps_aaaaaaaaaaaaaaaa"));
            Assert.That(rec["nonce"], Is.EqualTo(42L));
            Assert.That(rec["price"], Is.EqualTo("4.99"));
            Assert.That(rec["units"], Is.EqualTo(4990242L)); // encode(Decimal("4.99"), 2, 42) in Python
            Assert.That(store["ps_aaaaaaaaaaaaaaaa"], Is.SameAs(rec));
            Assert.Throws<ValueError>(() => Sessions.CreateSession(store, "tip", null, "t", "USDC", "base", "1", 1, 1, 1L,
                0, new JsonObject { { "cursor", 1L } }, new Scripted()));
        }

        [Test]
        public void SystemEntropyIsInRangeAndSized()
        {
            var e = SystemEntropy.Instance;
            for (int i = 0; i < 500; i++) Assert.That(e.RandBelow(100), Is.InRange(0L, 99L));
            Assert.That(e.RandBelow(1), Is.EqualTo(0L));
            Assert.That(e.TokenHex(8), Has.Length.EqualTo(16));
            Assert.That(e.TokenBytes(32), Has.Length.EqualTo(32));
            Assert.Throws<ValueError>(() => e.RandBelow(0));
        }

        [Test]
        public void TipTotalKeepsWidestFraction()
        {
            var store = new JsonObject
            {
                { "a", new JsonObject { { "kind", "tip" }, { "amount", "1.5" } } },
                { "b", new JsonObject { { "kind", "tip" }, { "amount", "0" } } },
            };
            var items = new Dictionary<string, Item>();
            Assert.That(Receipts.ComputeOwnership(store, items).TipTotal.ToString(), Is.EqualTo("1.5"));
            Assert.That(Receipts.ComputeOwnership(new JsonObject(), items).TipTotal.ToString(), Is.EqualTo("0"));
        }
    }
}
