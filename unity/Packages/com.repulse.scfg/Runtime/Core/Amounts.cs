// Twin of scfg_core/amounts.py (and of rpgmaker/src/core/amounts.ts).
// Money handling: parsing developer/player amounts and the price|code|nonce codec.
//
// On-chain USDC/USDT amounts are integers in base units (6 decimals). The last four decimals of every payment
// carry an item code (2 digits) and a random nonce (2 digits), so a transaction can be traced back to what it
// bought:
//
//     4.99 | 37 | 42   ->   4.993742 USDC   (units = 4_993_742)
//
// Values are loosely typed where Python's are: a Python int is an int, long or BigInteger (never a bool), a
// float is a double, a Decimal is a Dec. Token units are a BigInteger, which, like a Python int, may be
// negative or huge (format_units and units_to_decimal accept those; decode does the range check).
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;

namespace Scfg.Core
{
    public static class Amounts
    {
        public const int TokenDecimals = 6;

        public static readonly Dec MaxAmount = Dec.FromInt(1000000);

        // _CENT = Decimal("0.01"): its exponent.
        private const int CentExp = -2;

        /// <summary>
        /// The magnitude bound of every amount the core reads: a nonzero amount's adjusted exponent (the power of
        /// ten of its leading digit) must lie in [-40, 40]. That is far beyond any token amount, and it keeps
        /// near-Emax values (1E+999999) from overflowing or building million-digit integers.
        /// </summary>
        public const int AmountExponentLimit = 40;

        /// <summary>The longest amount text the core parses (parse_amount, stored_amount), in code points.</summary>
        public const int AmountTextLimit = 100;

        private static bool InRange(Dec d) =>
            d.IsZero || (-AmountExponentLimit <= d.Adjusted() && d.Adjusted() <= AmountExponentLimit);

        private static bool IsInt(object? v) => v is int || v is long || v is BigInteger;

        private static BigInteger AsInt(object? v)
        {
            switch (v)
            {
                case int i: return i;
                case long l: return l;
                default: return (BigInteger)v!;
            }
        }

        // Python's repr of a value in a message; a Dec is repr'd as Python's Decimal.
        private static string Repr(object? v) => v is Dec d ? "Decimal('" + d + "')" : Py.Repr(v);

        /// <summary>
        /// An amount read back from storage (a receipt's amount, a session's price): Decimal(value) when it is a
        /// finite number within the amount range (<see cref="AmountExponentLimit"/>) and, as text, at most
        /// <see cref="AmountTextLimit"/> characters, else a ValueError (a TypeError for a list, or a value
        /// Decimal() does not take). A string takes any exponent libmpdec accepts, and digit-group underscores.
        /// </summary>
        public static Dec StoredAmount(object? value)
        {
            Dec? d;
            switch (value)
            {
                case List<object?> _: // Decimal() reads a (sign, digits, exponent) tuple
                    throw new TypeError("a stored amount is a number, not a list");
                case string s:
                    if (Py.Len(s) > AmountTextLimit)
                        throw new ValueError("amount text longer than " + AmountTextLimit.ToString(CultureInfo.InvariantCulture) + " characters");
                    try
                    {
                        d = Dec.Parse(s);
                    }
                    catch (ValueError)
                    {
                        throw new ValueError("not a number: " + Py.Repr(s));
                    }
                    break;
                case bool b:
                    d = Dec.FromInt(b ? 1 : 0); // Decimal(True) == 1
                    break;
                case Dec x:
                    d = x;
                    break;
                case double f:
                    d = Dec.FromDouble(f);
                    break;
                default:
                    if (!IsInt(value))
                        throw new TypeError("conversion from " + Py.TypeName(value) + " to Decimal is not supported");
                    d = Dec.FromInt(AsInt(value));
                    break;
            }
            if (d == null || !InRange(d)) throw new ValueError("amount out of range: " + Repr(value));
            return d;
        }

        /// <summary>
        /// Validates a price/tip amount and returns it quantized to cents.
        ///
        /// Accepts str, int or Decimal. Floats are refused: binary floating point and money do not mix. A str is
        /// a plain decimal number: Decimal's digit-group underscores ("1_000") are refused like any other
        /// malformed amount.
        /// </summary>
        public static Dec ParseAmount(object? value)
        {
            if (value is bool || value is double)
                throw new TypeError("amounts must be str, int or Decimal (never float)");
            Dec? d;
            if (IsInt(value))
            {
                d = Dec.FromInt(AsInt(value));
            }
            else if (value is Dec x)
            {
                d = x;
            }
            else if (value is string s)
            {
                if (Py.Len(s) > AmountTextLimit)
                    throw new ValueError("amount text longer than " + AmountTextLimit.ToString(CultureInfo.InvariantCulture) + " characters");
                if (s.Contains("_")) throw new ValueError("not a number: " + Py.Repr(s));
                try
                {
                    d = Dec.Parse(Py.Strip(s));
                }
                catch (ValueError)
                {
                    throw new ValueError("not a number: " + Py.Repr(s));
                }
            }
            else
            {
                throw new TypeError("unsupported amount type: " + Py.TypeName(value));
            }
            if (d == null) throw new ValueError("amount must be finite");
            if (!InRange(d)) throw new ValueError("amount out of range");
            if (d <= Dec.FromInt(0)) throw new ValueError("amount must be positive");
            if (d > MaxAmount) throw new ValueError("amount too large");
            var q = d.Quantize(CentExp);
            if (q != d) throw new ValueError("amount must have at most 2 decimals");
            return q;
        }

        private static int CheckField(string name, object? value)
        {
            if (!IsInt(value)) throw new ValueError(name + " must be an int in 0..99");
            var n = AsInt(value);
            if (n < 0 || n > 99) throw new ValueError(name + " must be an int in 0..99");
            return (int)n;
        }

        /// <summary>price (2 decimals) + code (0 = tip, 1..99 = item) + nonce -> base units.</summary>
        public static BigInteger Encode(object? price, object? code, object? nonce)
        {
            var p = ParseAmount(price);
            int c = CheckField("code", code);
            int n = CheckField("nonce", nonce);
            // p has exactly 2 decimals, so int(p * 100) is its cents, exactly.
            var cents = p.MovePoint(2).IntegralValue()!.Value;
            return cents * 10000 + c * 100 + n;
        }

        /// <summary>
        /// Inverse of encode: base units -> (price, code, nonce).
        ///
        /// From 10**32 units the price needs more than Decimal's 28 digits and this raises InvalidOperation
        /// (an ArithmeticError, not a ValueError), as Python's decimal does; callers that read transfers off the
        /// chain skip such a value.
        /// </summary>
        public static (Dec Price, int Code, int Nonce) Decode(BigInteger units)
        {
            if (units.Sign < 0) throw new ValueError("units must be >= 0");
            // Python: (Decimal(units // 10_000) / 100).quantize(_CENT) needs more than 28 digits from 10**28 cents.
            var cents = units / 10000;
            if (cents >= BigInteger.Pow(10, Dec.Prec))
                throw new InvalidOperation("quantize result has too many digits for the current context");
            var price = new Dec(cents, CentExp, false);
            return (price, (int)(units / 100 % 100), (int)(units % 100));
        }

        /// <summary>
        /// Base units -> token amount, exactly (4993742 -> 4.993742), however many digits: the value is built from
        /// its digits instead of through Decimal's 28-digit arithmetic.
        /// </summary>
        public static Dec UnitsToDecimal(BigInteger units) => Dec.FromInt(units).MovePoint(-TokenDecimals);

        /// <summary>
        /// Token amount -> base units, exactly (no rounding to Decimal's 28 digits). ValueError for more than 6
        /// decimals, or for an amount out of range (<see cref="AmountExponentLimit"/>).
        /// </summary>
        public static BigInteger DecimalToUnits(Dec amount)
        {
            if (!InRange(amount)) throw new ValueError("amount out of range: " + amount);
            var q = amount.MovePoint(TokenDecimals).IntegralValue();
            if (q == null) throw new ValueError("more than 6 decimals");
            return q.Value;
        }

        /// <summary>Human display of a token amount with all 6 decimals: 4993742 -> "4.993742".</summary>
        public static string FormatUnits(BigInteger units)
        {
            var s = BigInteger.Abs(units).ToString(CultureInfo.InvariantCulture)
                .PadLeft(TokenDecimals + 1, '0');
            int cut = s.Length - TokenDecimals;
            return (units.Sign < 0 ? "-" : "") + s.Substring(0, cut) + "." + s.Substring(cut);
        }
    }
}
