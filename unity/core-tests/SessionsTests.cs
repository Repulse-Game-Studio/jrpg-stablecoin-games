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
            public string TokenHex(int n) => new string('a', 2 * n);
            public long RandBelow(long n) => 42;
            public byte[] TokenBytes(int n) => new byte[n];
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
