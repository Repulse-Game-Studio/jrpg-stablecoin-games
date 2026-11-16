// No Python twin: a finite Python decimal.Decimal under the default context (28 digits, round half even),
// limited to the operations the payment core uses. It mirrors rpgmaker/src/core/decimal.ts. ToString() is
// Python's str(Decimal).
//
// Differences from CPython, all outside any realistic amount: the exponent is an int, so Parse clamps an
// exponent beyond +-2^30 (that libmpdec accepts) to +-2^30. Such a value is far outside [Etiny, Emax], so
// comparisons with an in-range value and + still give Python's results (two values that both clamp compare
// equal here, where Python orders them), but ToString, Quantize and IntegralValue do not: such values
// are only ever compared, summed or range-checked.
#nullable enable
using System;
using System.Globalization;
using System.Numerics;
using System.Text;

namespace Scfg.Core
{
    /// <summary>
    /// An immutable finite decimal: <c>(-1)^Negative * Coef * 10^Exp</c>, with the magnitude <see cref="Coef"/>
    /// kept apart from the sign so that -0 exists, as in Python. Equality and ordering are by value
    /// (<c>1.0 == 1.00</c>, <c>-0 == 0</c>).
    /// </summary>
    public sealed class Dec : IEquatable<Dec>
    {
        /// <summary>The default context's precision.</summary>
        public const int Prec = 28;

        // The default context's exponent limits.
        private const long Emax = 999999;
        private const long Emin = -999999;
        private const long Etiny = Emin - Prec + 1; // -1000026
        private const long Etop = Emax - Prec + 1; // 999972

        // libmpdec's limits for a literal: the exponent, and the adjusted exponent.
        private static readonly BigInteger MinLiteralExp = BigInteger.Parse("-1999999999999999997", CultureInfo.InvariantCulture);
        private static readonly BigInteger MaxLiteralAdjusted = BigInteger.Parse("999999999999999999", CultureInfo.InvariantCulture);

        /// <summary>The bound Parse clamps exponents to (see the file header).</summary>
        public const int ExpClamp = 1 << 30;

        /// <summary>The magnitude of the coefficient, at least 0.</summary>
        public BigInteger Coef { get; }

        public int Exp { get; }

        public bool Negative { get; }

        private string? _digits;

        public Dec(BigInteger coef, int exp, bool negative)
        {
            if (coef.Sign < 0) throw new ArgumentOutOfRangeException(nameof(coef), "the coefficient is a magnitude");
            Coef = coef;
            Exp = exp;
            Negative = negative;
        }

        // The coefficient's decimal digits ("0" for zero, like Python).
        private string Digits => _digits ??= Coef.ToString(CultureInfo.InvariantCulture);

        private int DigitCount => Digits.Length;

        private static BigInteger Pow10(long n) => BigInteger.Pow(10, checked((int)n));

        /// <summary>Python <c>Decimal(n)</c> for an int.</summary>
        public static Dec FromInt(BigInteger n) => new Dec(BigInteger.Abs(n), 0, n.Sign < 0);

        /// <summary>
        /// Python <c>Decimal(x)</c> for a float: exact, no rounding (<c>0.1</c> has 55 fraction digits), an integral
        /// float the same value as the int, and <c>-0.0</c> is <c>-0</c>. Null for NaN and the infinities, which
        /// Python turns into a non-finite Decimal.
        /// </summary>
        public static Dec? FromDouble(double x)
        {
            if (double.IsNaN(x) || double.IsInfinity(x)) return null;
            long bits = BitConverter.DoubleToInt64Bits(x);
            bool negative = bits < 0;
            int biased = (int)((bits >> 52) & 0x7ff);
            long mant = bits & ((1L << 52) - 1);
            int e2 = biased == 0 ? -1074 : biased - 1075;
            if (biased != 0) mant |= 1L << 52;
            if (mant == 0) return new Dec(BigInteger.Zero, 0, negative);
            while ((mant & 1) == 0 && e2 < 0) // lowest terms: an odd numerator over 2^k
            {
                mant >>= 1;
                e2++;
            }
            if (e2 >= 0) return new Dec(new BigInteger(mant) << e2, 0, negative);
            return new Dec(new BigInteger(mant) * BigInteger.Pow(5, -e2), e2, negative);
        }

        /// <summary>
        /// Python <c>Decimal(str)</c>: surrounding whitespace (str.strip()'s set) is ignored, Unicode decimal
        /// digits count as their ASCII twins and every underscore is dropped, as CPython's C decimal does. Null
        /// for a NaN or an infinity, so callers can reject them. A malformed literal, or an exponent beyond
        /// libmpdec's, is a ValueError (Python's InvalidOperation).
        /// </summary>
        public static Dec? Parse(string text)
        {
            if (text == null) throw new ValueError("Decimal() needs a string");
            var s = Ascii(Py.Strip(text));
            if (s == null) throw Invalid(text);
            int i = 0;
            bool negative = false;
            if (i < s.Length && (s[i] == '+' || s[i] == '-'))
            {
                negative = s[i] == '-';
                i++;
            }
            var rest = s.Substring(i);
            if (IsSpecial(rest)) return null;

            int intStart = i;
            while (i < s.Length && IsAsciiDigit(s[i])) i++;
            var intPart = s.Substring(intStart, i - intStart);
            var frac = "";
            if (i < s.Length && s[i] == '.')
            {
                i++;
                int fracStart = i;
                while (i < s.Length && IsAsciiDigit(s[i])) i++;
                frac = s.Substring(fracStart, i - fracStart);
            }
            if (intPart.Length == 0 && frac.Length == 0) throw Invalid(text);

            var e = BigInteger.Zero;
            if (i < s.Length && (s[i] == 'e' || s[i] == 'E'))
            {
                i++;
                bool eNegative = false;
                if (i < s.Length && (s[i] == '+' || s[i] == '-'))
                {
                    eNegative = s[i] == '-';
                    i++;
                }
                while (i < s.Length - 1 && s[i] == '0') i++; // leading zeros cost nothing
                int eStart = i;
                while (i < s.Length && IsAsciiDigit(s[i])) i++;
                int eLength = i - eStart;
                if (eLength == 0) throw Invalid(text);
                // More than 20 significant digits is beyond libmpdec's limits whatever the digits around it.
                if (eLength > 20) throw OutOfRange(text);
                e = BigInteger.Parse(s.Substring(eStart, eLength), NumberStyles.None, CultureInfo.InvariantCulture);
                if (eNegative) e = -e;
            }
            if (i != s.Length) throw Invalid(text);

            var digits = intPart + frac;
            var coef = BigInteger.Parse(digits, NumberStyles.None, CultureInfo.InvariantCulture);
            var exp = e - frac.Length;
            var coefDigits = coef.IsZero ? 1 : digits.Length - LeadingZeros(digits);
            if (exp < MinLiteralExp || exp + coefDigits - 1 > MaxLiteralAdjusted) throw OutOfRange(text);
            if (exp > ExpClamp) exp = ExpClamp;
            if (exp < -ExpClamp) exp = -ExpClamp;
            return new Dec(coef, (int)exp, negative);
        }

        private static ValueError Invalid(string text) => new ValueError("invalid decimal literal: " + Py.Repr(text));

        private static ValueError OutOfRange(string text) => new ValueError("exponent out of range: " + Py.Repr(text));

        private static bool IsAsciiDigit(char c) => c >= '0' && c <= '9';

        private static int LeadingZeros(string digits)
        {
            int n = 0;
            while (n < digits.Length && digits[n] == '0') n++;
            return n;
        }

        // The text with every Unicode decimal digit mapped to ASCII and every underscore dropped; null when it
        // holds a code point outside ASCII that is not a digit (no literal can contain one).
        private static string? Ascii(string s)
        {
            var o = new StringBuilder(s.Length);
            for (int i = 0; i < s.Length;)
            {
                int cp = Py.CodePointAt(s, i, out int w);
                i += w;
                if (cp == '_') continue;
                if (cp < 0x80)
                {
                    o.Append((char)cp);
                    continue;
                }
                int d = Py.DecimalValue(cp);
                if (d < 0) return null;
                o.Append((char)('0' + d));
            }
            return o.ToString(); // culture-ok: StringBuilder
        }

        // inf, infinity, nan, snan and a NaN's payload digits, ASCII case-insensitively (not the culture's casing).
        private static bool IsSpecial(string s)
        {
            if (AsciiEqualsIgnoreCase(s, "inf") || AsciiEqualsIgnoreCase(s, "infinity")) return true;
            int k;
            if (AsciiStartsWithIgnoreCase(s, "nan")) k = 3;
            else if (AsciiStartsWithIgnoreCase(s, "snan")) k = 4;
            else return false;
            for (; k < s.Length; k++)
                if (!IsAsciiDigit(s[k])) return false;
            return true;
        }

        private static char AsciiLower(char c) => c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;

        private static bool AsciiStartsWithIgnoreCase(string s, string lowerPrefix)
        {
            if (s.Length < lowerPrefix.Length) return false;
            for (int k = 0; k < lowerPrefix.Length; k++)
                if (AsciiLower(s[k]) != lowerPrefix[k]) return false;
            return true;
        }

        private static bool AsciiEqualsIgnoreCase(string s, string lower) =>
            s.Length == lower.Length && AsciiStartsWithIgnoreCase(s, lower);

        public bool IsZero => Coef.IsZero;

        /// <summary>Python <c>adjusted()</c>: the exponent of the leading digit (Exp + digits - 1; a zero's is its Exp).</summary>
        public long Adjusted() => (long)Exp + DigitCount - 1;

        /// <summary>
        /// The value as an integer when it is integral, else null (Python's <c>q == q.to_integral_value()</c>, then
        /// <c>int(q)</c>). A zero is 0 whatever its exponent.
        /// </summary>
        public BigInteger? IntegralValue()
        {
            if (Coef.IsZero) return BigInteger.Zero;
            BigInteger mag;
            if (Exp >= 0)
            {
                mag = Coef * Pow10(Exp);
            }
            else
            {
                if (-(long)Exp > DigitCount) return null; // below 1 and not zero
                var p = Pow10(-(long)Exp);
                var q = BigInteger.DivRem(Coef, p, out var r);
                if (!r.IsZero) return null;
                mag = q;
            }
            return Negative ? -mag : mag;
        }

        /// <summary>
        /// The same digits with the decimal point moved: <c>Decimal((sign, digits, exponent + n))</c>, exact (no
        /// rounding to the context, unlike Python's scaleb). amounts.py's units_to_decimal and decimal_to_units.
        /// </summary>
        public Dec MovePoint(int n) => new Dec(Coef, checked(Exp + n), Negative);

        // Divide by 10^drop rounding half to even.
        private static BigInteger RoundDrop(BigInteger coef, long drop)
        {
            var p = Pow10(drop);
            var q = BigInteger.DivRem(coef, p, out var r);
            var twice = r * 2;
            if (twice > p || (twice == p && !q.IsEven)) q += 1;
            return q;
        }

        /// <summary>
        /// Python <c>quantize(Decimal(1).scaleb(exp))</c> under the default context: round half even to the
        /// exponent <paramref name="exp"/>, keeping the sign. InvalidOperation when the result needs more than
        /// 28 digits.
        /// </summary>
        public Dec Quantize(int exp)
        {
            long shift = (long)Exp - exp;
            BigInteger coef;
            if (shift >= 0)
            {
                if (Coef.IsZero) coef = BigInteger.Zero;
                else if (DigitCount + shift > Prec) throw new InvalidOperation("quantize result has too many digits for the current context");
                else coef = Coef * Pow10(shift);
            }
            else if (-shift > DigitCount)
            {
                coef = BigInteger.Zero; // below half of the last place: rounds to zero
            }
            else
            {
                coef = RoundDrop(Coef, -shift);
            }
            var result = new Dec(coef, exp, Negative);
            if (result.DigitCount > Prec) throw new InvalidOperation("quantize result has too many digits for the current context");
            return result;
        }

        /// <summary>
        /// Python <c>a + b</c> under the default context, exactly (<c>_pydecimal.Decimal.__add__</c>): operands whose
        /// exponents are far apart cost nothing, the result is rounded half even to 28 digits, rounded as a
        /// subnormal at Etiny, a zero's exponent is clamped into [Etiny, Emax], and a result above Emax is an
        /// <see cref="Overflow"/> (the trapped decimal.Overflow).
        /// </summary>
        public static Dec operator +(Dec a, Dec b)
        {
            int exp = Math.Min(a.Exp, b.Exp);
            if (a.IsZero && b.IsZero) return new Dec(BigInteger.Zero, exp, a.Negative && b.Negative).FixContext();
            if (a.IsZero || b.IsZero)
            {
                var kept = a.IsZero ? b : a; // the zero operand only decides the exponent
                long target = Math.Max(exp, (long)kept.Exp - Prec - 1); // <= kept.Exp: padding only, at most 29 digits
                return Fix(kept.Coef * Pow10(kept.Exp - target), target, kept.Negative);
            }
            // _normalize: an operand entirely below the other's rounding range becomes a sticky digit.
            var big = a.Exp < b.Exp ? b : a;
            var small = a.Exp < b.Exp ? a : b;
            long stickyExp = big.Exp + Math.Min(-1, big.DigitCount - Prec - 2);
            BigInteger smallCoef = small.Coef;
            long work = small.Exp;
            if (small.DigitCount + (long)small.Exp - 1 < stickyExp)
            {
                smallCoef = BigInteger.One;
                work = stickyExp;
            }
            var x = big.Coef * Pow10(big.Exp - work);
            if (big.Negative) x = -x;
            var y = small.Negative ? -smallCoef : smallCoef;
            var sum = x + y;
            if (sum.IsZero) return new Dec(BigInteger.Zero, exp, false).FixContext(); // ROUND_HALF_EVEN: +0
            return Fix(BigInteger.Abs(sum), work, sum.Sign < 0);
        }

        // _pydecimal.Decimal._fix for the default context on (coef, exp), raising Overflow as the trapped signal
        // does. exp is a long because the working exponent of + can sit just outside an int.
        private static Dec Fix(BigInteger coef, long exp, bool negative)
        {
            if (coef.IsZero) return new Dec(BigInteger.Zero, (int)Math.Min(Math.Max(exp, Etiny), Emax), negative);
            long digits = coef.ToString(CultureInfo.InvariantCulture).Length;
            long expMin = digits + exp - Prec;
            if (expMin > Etop) throw new Overflow("above Emax");
            if (expMin < Etiny) expMin = Etiny; // subnormal
            if (exp >= expMin) return new Dec(coef, checked((int)exp), negative);
            long keep = digits + exp - expMin;
            // keep < 0: the value is below half of the last place and rounds to zero (Python's sticky '1').
            var c = keep < 0 ? BigInteger.Zero : RoundDrop(coef, digits - keep);
            if (c.ToString(CultureInfo.InvariantCulture).Length > Prec) // the rounding carried into a 29th digit
            {
                c /= 10;
                expMin += 1;
            }
            if (expMin > Etop) throw new Overflow("above Emax");
            return new Dec(c, (int)expMin, negative);
        }

        private Dec FixContext() => Fix(Coef, Exp, Negative);

        /// <summary>Compares by value: -1, 0 or 1. Zeros of either sign are equal.</summary>
        public static int Cmp(Dec a, Dec b)
        {
            int sa = a.IsZero ? 0 : a.Negative ? -1 : 1;
            int sb = b.IsZero ? 0 : b.Negative ? -1 : 1;
            if (sa != sb) return sa < sb ? -1 : 1;
            if (sa == 0) return 0;
            long adjA = (long)a.Exp + a.DigitCount;
            long adjB = (long)b.Exp + b.DigitCount;
            int mag;
            if (adjA != adjB)
            {
                mag = adjA > adjB ? 1 : -1;
            }
            else
            {
                // Same leading-digit position, so the exponents differ by less than the digit counts.
                int exp = Math.Min(a.Exp, b.Exp);
                var x = a.Coef * Pow10((long)a.Exp - exp);
                var y = b.Coef * Pow10((long)b.Exp - exp);
                mag = x == y ? 0 : x > y ? 1 : -1;
            }
            return mag * sa;
        }

        public static bool operator ==(Dec? a, Dec? b)
        {
            if (a is null || b is null) return a is null && b is null;
            return Cmp(a, b) == 0;
        }

        public static bool operator !=(Dec? a, Dec? b) => !(a == b);
        public static bool operator <(Dec a, Dec b) => Cmp(a, b) < 0;
        public static bool operator <=(Dec a, Dec b) => Cmp(a, b) <= 0;
        public static bool operator >(Dec a, Dec b) => Cmp(a, b) > 0;
        public static bool operator >=(Dec a, Dec b) => Cmp(a, b) >= 0;

        public bool Equals(Dec? other) => !(other is null) && Cmp(this, other) == 0;

        public override bool Equals(object? obj) => obj is Dec d && Equals(d);

        /// <summary>By value, like Python's hash(Decimal): equal values (1.0 and 1.00, -0 and 0) hash alike.</summary>
        public override int GetHashCode()
        {
            if (IsZero) return 0;
            var coef = Coef;
            long exp = Exp;
            while (true)
            {
                var q = BigInteger.DivRem(coef, 10, out var r);
                if (!r.IsZero) break;
                coef = q;
                exp++;
            }
            return HashCode(coef.GetHashCode(), exp.GetHashCode(), Negative ? 1 : 0);
        }

        private static int HashCode(int a, int b, int c)
        {
            unchecked
            {
                int h = 17;
                h = h * 31 + a;
                h = h * 31 + b;
                return h * 31 + c;
            }
        }

        /// <summary>
        /// Python <c>str(Decimal)</c>: plain notation when Exp &lt;= 0 and the adjusted exponent is at least -6
        /// (<c>"4.990000"</c>, <c>"0.000000"</c>), otherwise scientific (<c>"1E+1"</c>, <c>"1E-7"</c>, <c>"0E+3"</c>).
        /// </summary>
        public override string ToString()
        {
            var digits = Digits;
            long leftdigits = (long)Exp + digits.Length;
            long dotplace = Exp <= 0 && leftdigits > -6 ? leftdigits : 1;
            var o = new StringBuilder(digits.Length + 16);
            if (Negative) o.Append('-');
            if (dotplace <= 0)
            {
                o.Append("0.").Append('0', (int)-dotplace).Append(digits);
            }
            else if (dotplace >= digits.Length)
            {
                o.Append(digits).Append('0', (int)(dotplace - digits.Length));
            }
            else
            {
                o.Append(digits, 0, (int)dotplace).Append('.').Append(digits, (int)dotplace, digits.Length - (int)dotplace);
            }
            long e = leftdigits - dotplace;
            if (e != 0) o.Append('E').Append(e > 0 ? '+' : '-').Append(Math.Abs(e).ToString(CultureInfo.InvariantCulture));
            return o.ToString(); // culture-ok: StringBuilder
        }
    }
}
