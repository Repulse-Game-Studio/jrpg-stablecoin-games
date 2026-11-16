// No Python twin: tests of Ints (twin of scfg_core/ints.py). Expected values are ints.py's own outputs.
using System.Numerics;
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class IntsTests
    {
        [Test]
        public void Limits()
        {
            Assert.That(Ints.IntMaxStrDigits, Is.EqualTo(4300));
            Assert.That(Ints.DigitRunLimit, Is.EqualTo(18));
        }

        [TestCase("111111111111111111", 111111111111111111L)] // 18 significant digits
        [TestCase("999999999999999999", 999999999999999999L)]
        [TestCase("1111111111111111111", null)] // 19
        [TestCase("0100000000000000000", 100000000000000000L)] // 19 characters, 18 significant
        [TestCase("000000000000000000", 0L)]
        [TestCase("", 0L)]
        [TestCase("7", 7L)]
        public void DigitRunValue(string run, long? want)
        {
            Assert.That(Ints.DigitRunValue(run), Is.EqualTo(want));
        }

        [Test]
        public void DigitRunLeadingZerosAreFree()
        {
            Assert.That(Ints.DigitRunValue(new string('0', 100) + new string('1', 18)), Is.EqualTo(111111111111111111L));
            Assert.That(Ints.DigitRunValue(new string('0', 100) + new string('1', 19)), Is.Null);
            Assert.That(Ints.DigitRunValue(new string('0', 100)), Is.EqualTo(0L));
            // zeros of any script: Arabic-Indic, and a mix of scripts
            Assert.That(Ints.DigitRunValue(new string('\u0660', 30) + "\u0665"), Is.EqualTo(5L));
            var mixed = new System.Text.StringBuilder();
            for (int i = 0; i < 20; i++) mixed.Append("0\u0660");
            Assert.That(Ints.DigitRunValue(mixed + "7"), Is.EqualTo(7L));
            Assert.That(Ints.DigitRunValue(string.Concat(System.Linq.Enumerable.Repeat("\U0001D7CE", 30)) + "\U0001D7D7"), Is.EqualTo(9L));
            Assert.That(Ints.DigitRunValue(new string('9', 1_000_000)), Is.Null);
        }

        [Test]
        public void IntFromRefusesOverlongDigitStrings()
        {
            Assert.That(Ints.IntFrom(new string(' ', 5000) + "7"), Is.EqualTo(new BigInteger(7)));
            Assert.That(Ints.IntFrom("-" + new string('9', 4300)), Is.EqualTo(1 - BigInteger.Pow(10, 4300)));
            var e = Assert.Throws<ValueError>(() => Ints.IntFrom(new string('9', 4301)))!;
            Assert.That(e.Message, Is.EqualTo("Exceeds the limit (4300 digits) for integer string conversion: value has 4301 digits; " +
                                              "use sys.set_int_max_str_digits() to increase the limit"));
            Assert.Throws<ValueError>(() => Ints.IntFrom("-" + new string('9', 4301)));
            Assert.That(Assert.Throws<ValueError>(() => Ints.IntFrom(new string('x', 5000)))!.Message,
                Does.StartWith("invalid literal for int() with base 10: 'xxx"));
            Assert.That(Ints.IntFrom(12L), Is.EqualTo(new BigInteger(12)));
        }

        [Test]
        public void JsonLoadsReadsIntegersWithIntFrom()
        {
            Assert.That(Ints.JsonLoads("1" + new string('0', 4299)), Is.TypeOf<BigInteger>());
            Assert.That(Ints.JsonLoads("[1.5e3]"), Is.EqualTo(new System.Collections.Generic.List<object?> { 1500.0 }));
            Assert.That(Assert.Throws<ValueError>(() => Ints.JsonLoads("[" + new string('1', 4301) + "]"))!.Message,
                Does.StartWith("Exceeds the limit (4300 digits) for integer string conversion: value has 4301 digits"));
        }
    }
}
