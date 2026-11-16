// No Python twin: tests of Dec and of Amounts beyond the vectors, ported from rpgmaker/src/core/decimal.test.ts
// and amounts.test.ts. Every expected string is CPython's str(Decimal(...)).
using System.Numerics;
using NUnit.Framework;

namespace Scfg.Core.Tests
{
    public class DecTests
    {
        private static Dec D(string s)
        {
            var v = Dec.Parse(s);
            Assert.That(v, Is.Not.Null, s + " should be finite");
            return v!;
        }

        [TestCase("4.99", "4.99")]
        [TestCase("1E+1", "1E+1")]
        [TestCase("0.0000001", "1E-7")]
        [TestCase("1_000", "1000")]
        [TestCase("-0", "-0")]
        [TestCase("0E-7", "0E-7")]
        [TestCase("0E+3", "0E+3")]
        [TestCase("123E5", "1.23E+7")]
        [TestCase("0.000001", "0.000001")]
        [TestCase("-0.00", "-0.00")]
        [TestCase("007", "7")]
        [TestCase("5.", "5")]
        [TestCase("+.5", "0.5")]
        [TestCase("1_0.5_5", "10.55")]
        [TestCase(" 5", "5")]
        [TestCase(" 5\n", "5")]
        [TestCase("５", "5")] // fullwidth digit
        [TestCase("١٢", "12")] // Arabic-Indic digits
        [TestCase("1e5_0", "1E+50")]
        [TestCase("1__0", "10")] // the C decimal drops every underscore
        [TestCase("_1", "1")]
        [TestCase("1e0000000000000000000000000000005", "1E+5")]
        [TestCase("4.990000", "4.990000")]
        [TestCase("0E-999999", "0E-999999")]
        [TestCase("-1.5E-40", "-1.5E-40")]
        public void StrMatchesPython(string text, string want)
        {
            Assert.That(D(text).ToString(), Is.EqualTo(want));
        }

        [TestCase("nan")]
        [TestCase("-NaN")]
        [TestCase("sNaN")]
        [TestCase("NaN12")]
        [TestCase("snan7")]
        [TestCase("inf")]
        [TestCase("+Inf")]
        [TestCase("-Infinity")]
        [TestCase("INFINITY")]
        public void NonFiniteIsNull(string text)
        {
            Assert.That(Dec.Parse(text), Is.Null);
        }

        [TestCase("")]
        [TestCase(" ")]
        [TestCase(".")]
        [TestCase("1e")]
        [TestCase("1E+")]
        [TestCase("abc")]
        [TestCase("0x10")]
        [TestCase("infin")]
        [TestCase("1.2.3")]
        [TestCase("--1")]
        [TestCase("1 2")]
        [TestCase("e5")]
        [TestCase("NaN1x")]
        [TestCase("_")]
        [TestCase("1,5")]
        [TestCase("ınf")] // dotless i: no culture casing
        [TestCase("1E+1000000000000000000")] // beyond libmpdec's exponent limit
        [TestCase("1E-2000000000000000000")]
        [TestCase("1E+100000000000000000000000")]
        [TestCase("1E-1999999999999999998")]
        [TestCase("12E+999999999999999999")]
        [TestCase("0.1E-1999999999999999997")]
        public void MalformedIsValueError(string text)
        {
            Assert.Throws<ValueError>(() => Dec.Parse(text));
        }

        // The edges of libmpdec's literal limits, which CPython accepts.
        [TestCase("1E-1999999999999999997")]
        [TestCase("1E+999999999999999999")]
        [TestCase("12E+999999999999999998")]
        [TestCase("0.1E-1999999999999999996")]
        [TestCase("0E+999999999999999999")]
        [TestCase("00E+999999999999999999")]
        public void LiteralExponentLimitsAreLibmpdecs(string text)
        {
            Assert.That(Dec.Parse(text), Is.Not.Null);
        }

        [Test]
        public void ExponentsBeyondAnIntAreClampedNotRefused()
        {
            Assert.That(D("1E+9999999999999999").Exp, Is.EqualTo(Dec.ExpClamp));
            Assert.That(D("1E-9999999999999999").Exp, Is.EqualTo(-Dec.ExpClamp));
            Assert.That(D("1E+9999999999999999") > D("1E+999999"), Is.True);
        }

        [Test]
        public void UnitsToDecimalForms()
        {
            Assert.That(Amounts.UnitsToDecimal(4990000).ToString(), Is.EqualTo("4.990000"));
            Assert.That(Amounts.UnitsToDecimal(0).ToString(), Is.EqualTo("0.000000"));
            Assert.That(Amounts.UnitsToDecimal(-5000000).ToString(), Is.EqualTo("-5.000000"));
        }

        [Test]
        public void FloatConversionIsExactLikePython()
        {
            Assert.That(Dec.FromDouble(0.1)!.ToString(), Is.EqualTo("0.1000000000000000055511151231257827021181583404541015625"));
            Assert.That(Dec.FromDouble(1.5)!.ToString(), Is.EqualTo("1.5"));
            Assert.That(Dec.FromDouble(-2.5)!.ToString(), Is.EqualTo("-2.5"));
            Assert.That(Dec.FromDouble(4)!.ToString(), Is.EqualTo("4"));
            Assert.That(Dec.FromDouble(-7)!.ToString(), Is.EqualTo("-7"));
            Assert.That(Dec.FromDouble(-0.0)!.ToString(), Is.EqualTo("-0"));
            Assert.That(Dec.FromDouble(1e22)!.ToString(), Is.EqualTo("10000000000000000000000"));
            Assert.That(Dec.FromDouble(5e-324)!.Adjusted(), Is.EqualTo(-324));
            Assert.That(Dec.FromDouble(double.NaN), Is.Null);
            Assert.That(Dec.FromDouble(double.PositiveInfinity), Is.Null);
        }

        [TestCase("-0.001", -2, "-0.00")]
        [TestCase("0.005", -2, "0.00")]
        [TestCase("0.015", -2, "0.02")]
        [TestCase("0.025", -2, "0.02")]
        [TestCase("1E+1", -2, "10.00")]
        [TestCase("0.999", -2, "1.00")]
        [TestCase("1E-30", -2, "0.00")]
        [TestCase("0E+30", -2, "0.00")]
        [TestCase("0.5", 0, "0")]
        [TestCase("1.5", 0, "2")]
        [TestCase("2.5", 0, "2")]
        [TestCase("15", 1, "2E+1")]
        [TestCase("25", 1, "2E+1")]
        [TestCase("999999999999999999999999999.5", 0, "1000000000000000000000000000")]
        public void QuantizeRoundsHalfEvenAndKeepsSign(string text, int exp, string want)
        {
            Assert.That(D(text).Quantize(exp).ToString(), Is.EqualTo(want));
        }

        [TestCase("1E+30")] // 33 digits
        [TestCase("999999999999999999999999999")]
        public void QuantizeBeyondThePrecisionIsInvalidOperation(string text)
        {
            var e = Assert.Throws<InvalidOperation>(() => D(text).Quantize(-2));
            Assert.That(e, Is.InstanceOf<ArithmeticError>());
            Assert.That(e, Is.Not.InstanceOf<ValueError>());
        }

        [Test]
        public void CmpComparesByValue()
        {
            Assert.That(Dec.Cmp(D("1.0"), D("1.00")), Is.EqualTo(0));
            Assert.That(Dec.Cmp(D("-0"), D("0")), Is.EqualTo(0));
            Assert.That(Dec.Cmp(D("-1"), D("0")), Is.EqualTo(-1));
            Assert.That(Dec.Cmp(D("1E+1"), D("9.99")), Is.EqualTo(1));
            Assert.That(Dec.Cmp(D("1E-7"), D("0")), Is.EqualTo(1));
            Assert.That(Dec.Cmp(D("1E+30"), D("1E+29")), Is.EqualTo(1));
            Assert.That(Dec.Cmp(D("-2"), D("-10")), Is.EqualTo(1));
            Assert.That(Dec.Cmp(D("4.99"), D("4.980000")), Is.EqualTo(1));
            Assert.That(D("1.0") == D("1.00") && D("-0") == D("0") && D("1") != D("1.01"), Is.True);
            Assert.That(D("1") < D("1.01") && D("1.01") >= D("1.010") && D("-5") <= D("-5"), Is.True);
            Assert.That(D("1.0").Equals(D("1.00")) && D("1.0").GetHashCode() == D("1.00").GetHashCode(), Is.True);
            Assert.That(D("0E+5").GetHashCode(), Is.EqualTo(D("-0.00").GetHashCode()));
            Assert.That(D("0.00").IsZero && D("-0E+5").IsZero && !D("1E-9").IsZero, Is.True);
        }

        [Test]
        public void IntegralValue()
        {
            Assert.That(D("5.000").IntegralValue(), Is.EqualTo((BigInteger?)5));
            Assert.That(D("1E+3").IntegralValue(), Is.EqualTo((BigInteger?)1000));
            Assert.That(D("-12.0").IntegralValue(), Is.EqualTo((BigInteger?)(-12)));
            Assert.That(D("5.5").IntegralValue(), Is.Null);
            Assert.That(D("0.0000001").IntegralValue(), Is.Null);
            Assert.That(D("-0").IntegralValue(), Is.EqualTo((BigInteger?)0));
            Assert.That(D("0E+999999").IntegralValue(), Is.EqualTo((BigInteger?)0));
        }

        // Decimal(a) + Decimal(b) under the default context, as CPython prints it.
        [TestCase("1.5", "0", "1.5")]
        [TestCase("1.000000", "0.5", "1.500000")]
        [TestCase("0.00", "0", "0.00")]
        [TestCase("-0", "-0", "-0")]
        [TestCase("1", "-1", "0")]
        [TestCase("0", "-1E-1000030", "-0E-1000026")]
        [TestCase("-0", "1E-1000030", "0E-1000026")]
        [TestCase("0", "-0E-5", "0.00000")]
        [TestCase("1.0", "-1.0", "0.0")]
        [TestCase("0E+5", "0E+3", "0E+3")]
        [TestCase("0", "12345678901234567890123456789", "1.234567890123456789012345679E+28")]
        [TestCase("0", "0E+1000005", "0")]
        [TestCase("9.999999999999999999999999999E+999999", "0.0000000000000000000000000004E+999999", "9.999999999999999999999999999E+999999")]
        [TestCase("0", "5E-1000027", "0E-1000026")]
        [TestCase("0", "5.1E-1000027", "1E-1000026")]
        [TestCase("0", "15E-1000027", "2E-1000026")]
        [TestCase("0", "25E-1000027", "2E-1000026")]
        [TestCase("1E-1000026", "-1E-1000026", "0E-1000026")]
        [TestCase("1E+200000", "1", "1.000000000000000000000000000E+200000")]
        [TestCase("1", "1E+200000", "1.000000000000000000000000000E+200000")]
        [TestCase("4.25", "0.000001", "4.250001")]
        [TestCase("-7", "2.5", "-4.5")]
        [TestCase("10000000000000000000000000000", "1", "1.000000000000000000000000000E+28")]
        [TestCase("10000000000000000000000000000", "5", "1.000000000000000000000000000E+28")]
        [TestCase("10000000000000000000000000000", "15", "1.000000000000000000000000002E+28")]
        [TestCase("9999999999999999999999999999", "5", "1.000000000000000000000000000E+28")]
        [TestCase("0E-100", "1", "1.000000000000000000000000000")]
        public void AddMatchesPythonContextAddition(string a, string b, string want)
        {
            Assert.That((D(a) + D(b)).ToString(), Is.EqualTo(want), a + " + " + b);
        }

        [TestCase("1E+999999", "9E+999999")]
        [TestCase("9.999999999999999999999999999E+999999", "0.0000000000000000000000000005E+999999")]
        [TestCase("0", "1E+1000000")]
        public void AddAboveEmaxIsOverflow(string a, string b)
        {
            var e = Assert.Throws<Overflow>(() => { var _ = D(a) + D(b); });
            Assert.That(e, Is.InstanceOf<ArithmeticError>());
            Assert.That(e, Is.Not.InstanceOf<ValueError>());
        }

        [Test]
        public void DecodeBoundaryMatchesPythonDecimalPrecision()
        {
            var ten32 = BigInteger.Pow(10, 32);
            var (price, code, nonce) = Amounts.Decode(ten32 - 1);
            Assert.That(price.ToString(), Is.EqualTo("99999999999999999999999999.99"));
            Assert.That((code, nonce), Is.EqualTo((99, 99)));
            Assert.That(Amounts.Decode(BigInteger.Pow(10, 31)).Price.ToString(), Is.EqualTo("10000000000000000000000000.00"));
            Assert.Throws<InvalidOperation>(() => Amounts.Decode(ten32));
            Assert.Throws<InvalidOperation>(() => Amounts.Decode(ten32 + 12345));
            Assert.Throws<InvalidOperation>(() => Amounts.Decode(BigInteger.Pow(10, 40)));
            Assert.Throws<ValueError>(() => Amounts.Decode(-1));
        }

        [Test]
        public void ParseAmountTakesADecAndRefusesOtherTypes()
        {
            Assert.That(Amounts.ParseAmount(D("4.9")).ToString(), Is.EqualTo("4.90"));
            Assert.That(Amounts.ParseAmount(new BigInteger(3)).ToString(), Is.EqualTo("3.00"));
            Assert.That(Amounts.ParseAmount(3).ToString(), Is.EqualTo("3.00"));
            Assert.Throws<TypeError>(() => Amounts.ParseAmount(null));
            Assert.Throws<TypeError>(() => Amounts.ParseAmount(new JsonObject()));
            Assert.Throws<TypeError>(() => Amounts.ParseAmount(5.0));
            Assert.Throws<ValueError>(() => Amounts.ParseAmount(D("1E+41")));
            // The text limit counts code points, as Python's len(): 50 astral digits are 100 UTF-16 units.
            var astral = string.Concat(System.Linq.Enumerable.Repeat("\U0001D7CE", 60)); // MATHEMATICAL BOLD DIGIT ZERO
            Assert.That(Py.Len(astral), Is.EqualTo(60));
            Assert.That(Amounts.StoredAmount(astral).ToString(), Is.EqualTo("0"));
        }

        [Test]
        public void Constants()
        {
            Assert.That(Amounts.TokenDecimals, Is.EqualTo(6));
            Assert.That(Amounts.MaxAmount.ToString(), Is.EqualTo("1000000"));
            Assert.That(Amounts.AmountExponentLimit, Is.EqualTo(40));
            Assert.That(Amounts.AmountTextLimit, Is.EqualTo(100));
        }
    }
}
