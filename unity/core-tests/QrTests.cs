// No Python twin: pins QrCodeGen paths the shared vectors do not reach (numeric and alphanumeric segments, large
// versions, too-long data). Hashes and sizes were computed with scfg_core.paylinks.qr_matrix.
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class QrTests
    {
        private static long Hash(bool[][] m)
        {
            long x = 0;
            foreach (var r in m)
                foreach (bool c in r)
                    x = (x * 131 + (c ? 1 : 0) + 7) % 1000000007;
            return x;
        }

        private static string Rep(string s, int n)
        {
            var sb = new System.Text.StringBuilder();
            for (int i = 0; i < n; i++) sb.Append(s);
            return sb.ToString(); // culture-ok: StringBuilder
        }

        private static void Check(string text, int size, long hash)
        {
            var m = Paylinks.QrMatrix(text);
            Assert.That(m.Length, Is.EqualTo(size));
            Assert.That(Hash(m), Is.EqualTo(hash));
        }

        [TestCase("", 21, 227443507L)]
        [TestCase("12345", 21, 688946133L)]
        [TestCase("0", 21, 544011565L)]
        [TestCase("HELLO WORLD", 21, 202162885L)]
        public void SmallModes(string text, int size, long hash) => Check(text, size, hash);

        [Test]
        public void LargeVersions()
        {
            Check(Rep("A", 50), 29, 452248346L);
            Check(Rep("1", 300), 49, 754031077L);
            Check(Rep("AB-C.D/E:F$%*+ ", 40), 77, 730634772L);
            Check(Rep("x", 1000), 121, 778860690L);
            Check(Rep("é", 500), 121, 374628804L);
            Check(Rep("a", 2000), 169, 653302781L);
            Check(Rep("x", 2331), 177, 711778971L);
        }

        [Test]
        public void TooLongIsAValueError()
        {
            Assert.Throws<DataTooLongError>(() => Paylinks.QrMatrix(Rep("x", 2332)));
            Assert.That(new DataTooLongError("x"), Is.InstanceOf<ValueError>());
        }

        [Test]
        public void LoneSurrogateIsAValueError()
        {
            Assert.Throws<ValueError>(() => Paylinks.QrMatrix("a\ud800"));
        }
    }
}
