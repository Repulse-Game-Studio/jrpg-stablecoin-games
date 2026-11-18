// No Python twin: tests of Sha256 (hashlib.sha256's role; twin of rpgmaker/src/core/sha256.ts), of SystemEntropy's
// one RNG, and of the package's link.xml. The FIPS 180-4 example digests are the standard's; the other lengths are
// compared with the BCL's SHA256, which only the tests use.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class Sha256Tests
    {
        private static string Hex(byte[] b) => Bytes.ToHex(b);

        [TestCase("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")]
        [TestCase("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")]
        [TestCase("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")]
        [TestCase("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
                  "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1")]
        public void FipsExamples(string message, string digest)
        {
            Assert.That(Hex(Sha256.Hash(Encoding.ASCII.GetBytes(message))), Is.EqualTo(digest));
        }

        [Test]
        public void OneMillionAs()
        {
            Assert.That(Hex(Sha256.Hash(Encoding.ASCII.GetBytes(new string('a', 1_000_000)))),
                        Is.EqualTo("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
        }

        [Test]
        public void PaddingBoundariesMatchTheBcl()
        {
            var rnd = new Random(1234);
            foreach (var n in new[] { 1, 31, 32, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000 })
            {
                var data = new byte[n];
                rnd.NextBytes(data);
                using var bcl = System.Security.Cryptography.SHA256.Create();
                Assert.That(Sha256.Hash(data), Is.EqualTo(bcl.ComputeHash(data)), "length " + n);
            }
        }

        [Test]
        public void TheInputIsNotChanged()
        {
            var data = Encoding.ASCII.GetBytes("abc");
            Sha256.Hash(data);
            Assert.That(data, Is.EqualTo(Encoding.ASCII.GetBytes("abc")));
            Assert.Throws<TypeError>(() => Sha256.Hash(null!));
        }

        // ---- SystemEntropy ---------------------------------------------------------------------------------------

        /// <summary>A byte source that answers from a script and counts its draws.</summary>
        private sealed class Script
        {
            private readonly Queue<byte[]> _q;
            public int Draws;
            public Script(params byte[][] answers) { _q = new Queue<byte[]>(answers); }
            public byte[] Next(int n)
            {
                Draws++;
                var b = _q.Dequeue();
                Assert.That(b.Length, Is.EqualTo(n), "draw size");
                return b;
            }
        }

        [Test]
        public void RandBelowRejectsDrawsAtOrAboveTheBound()
        {
            // n = 5: 3 bits from one byte (mask 7); 7, 6 and 5 are rejected, 4 is taken
            var s = new Script(new byte[] { 0xFF }, new byte[] { 0x06 }, new byte[] { 0x0D }, new byte[] { 0x04 });
            Assert.That(SystemEntropy.RandBelow(5, s.Next), Is.EqualTo(4L));
            Assert.That(s.Draws, Is.EqualTo(4));
            // n = 256: 8 bits, every byte is in range
            s = new Script(new byte[] { 0xFF });
            Assert.That(SystemEntropy.RandBelow(256, s.Next), Is.EqualTo(255L));
            // n = 257: 9 bits from two big-endian bytes (mask 0x1ff); 0x1ff and 0x101 are rejected
            s = new Script(new byte[] { 0xFF, 0xFF }, new byte[] { 0x01, 0x01 }, new byte[] { 0xFE, 0x00 });
            Assert.That(SystemEntropy.RandBelow(257, s.Next), Is.EqualTo(0L));
            Assert.That(s.Draws, Is.EqualTo(3));
            // n = long.MaxValue: 63 bits from 8 bytes
            s = new Script(Enumerable.Repeat((byte)0xFF, 8).ToArray(), new byte[] { 0, 0, 0, 0, 0, 0, 0, 9 });
            Assert.That(SystemEntropy.RandBelow(long.MaxValue, s.Next), Is.EqualTo(9L));
            // n = 1: no draw at all
            s = new Script();
            Assert.That(SystemEntropy.RandBelow(1, s.Next), Is.EqualTo(0L));
            Assert.That(s.Draws, Is.EqualTo(0));
            Assert.Throws<ValueError>(() => SystemEntropy.RandBelow(0, s.Next));
            Assert.Throws<ValueError>(() => SystemEntropy.RandBelow(-1, s.Next));
        }

        [Test]
        public void TokenHexIsLowercaseHexOfTheRightLength()
        {
            var e = SystemEntropy.Instance;
            var hex = new Regex("^[0-9a-f]*$", RegexOptions.CultureInvariant);
            var seen = new HashSet<char>();
            for (int i = 0; i < 64; i++)
            {
                var t = e.TokenHex(16);
                Assert.That(t, Has.Length.EqualTo(32));
                Assert.That(hex.IsMatch(t), t);
                foreach (var c in t) seen.Add(c);
            }
            Assert.That(seen.Count, Is.EqualTo(16), "2048 hex digits use every one");
            Assert.That(e.TokenHex(0), Is.EqualTo(""));
            Assert.That(e.TokenBytes(0), Is.Empty);
            Assert.Throws<ValueError>(() => e.TokenBytes(-1));
        }

        [Test]
        public void FillDrawsFreshBytes()
        {
            var a = new byte[32];
            var b = new byte[32];
            SystemEntropy.Fill(a);
            SystemEntropy.Fill(b);
            Assert.That(a, Is.Not.EqualTo(b));
            Assert.That(a.Any(x => x != 0));
            SystemEntropy.Fill(new byte[0]);
            Assert.Throws<TypeError>(() => SystemEntropy.Fill(null!));
        }

        // ---- the package -----------------------------------------------------------------------------------------

        private static string Runtime()
        {
            var root = Vectors.FindUp("unity/Packages/com.repulse.scfg/Runtime");
            return Path.Combine(root, "unity", "Packages", "com.repulse.scfg", "Runtime");
        }

        [Test]
        public void LinkXmlPreservesBothAssembliesAndTheRng()
        {
            var doc = XDocument.Load(Path.Combine(Runtime(), "link.xml")); // throws when not well-formed
            Assert.That(doc.Root!.Name.LocalName, Is.EqualTo("linker"));
            var assemblies = doc.Root.Elements("assembly").ToList();
            string? Attr(XElement e, string name) => e.Attribute(name)?.Value;
            foreach (var name in new[] { "Scfg.Core", "Scfg.Desktop" })
            {
                var a = assemblies.SingleOrDefault(x => Attr(x, "fullname") == name);
                Assert.That(a, Is.Not.Null, name);
                Assert.That(Attr(a!, "preserve"), Is.EqualTo("all"), name);
            }
            // Scfg.Desktop is not built for WebGL, and the IL2CPP linker fails on an assembly it cannot resolve
            Assert.That(Attr(assemblies.Single(x => Attr(x, "fullname") == "Scfg.Desktop"), "ignoreIfMissing"), Is.EqualTo("1"));
            Assert.That(Attr(assemblies.Single(x => Attr(x, "fullname") == "Scfg.Core"), "ignoreIfMissing"), Is.Null);
            var mscorlib = assemblies.Single(x => Attr(x, "fullname") == "mscorlib");
            var rng = mscorlib.Elements("type").Single(t => Attr(t, "fullname") == "System.Security.Cryptography.RNGCryptoServiceProvider");
            Assert.That(Attr(rng, "preserve"), Is.EqualTo("all"));
        }

        [Test]
        public void OnlySessionsUsesTheBclCrypto()
        {
            var users = Directory.GetFiles(Runtime(), "*.cs", SearchOption.AllDirectories)
                .Where(f => File.ReadAllText(f).Contains("System.Security.Cryptography"))
                .Select(f => Path.GetRelativePath(Runtime(), f).Replace('\\', '/'))
                .ToList();
            Assert.That(users, Is.EqualTo(new[] { "Core/Sessions.cs" }));
        }
    }
}
