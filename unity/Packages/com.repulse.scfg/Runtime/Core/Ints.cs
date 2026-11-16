// Twin of scfg_core/ints.py: Python's int() for text the core reads from storage, a page or an RPC, with
// CPython's limit on the number of decimal digits enforced explicitly (converting a very long digit string
// costs quadratic time); json_loads (an RPC answer's integer literals through int_from) and
// digit_run_value (a number in a node's error message, at most 18 significant digits, Ruling 6).
using System.Globalization;
using System.Numerics;

namespace Scfg.Core
{
    public static class Ints
    {
        /// <summary>CPython's default <c>sys.get_int_max_str_digits()</c>.</summary>
        public const int IntMaxStrDigits = 4300;

        /// <summary>
        /// A digit run read out of a node's error message (an EVM range limit, a Solana transaction version) is a
        /// number only up to this many significant digits (Ruling 6): every range or version a request can carry
        /// is below 10**18.
        /// </summary>
        public const int DigitRunLimit = 18;

        internal static ValueError TooManyDigits(int digits) =>
            new ValueError("Exceeds the limit (" + IntMaxStrDigits.ToString(CultureInfo.InvariantCulture)
                           + " digits) for integer string conversion: value has " + digits.ToString(CultureInfo.InvariantCulture)
                           + " digits; use sys.set_int_max_str_digits() to increase the limit");

        /// <summary>
        /// <c>int(value)</c> (<see cref="Py.Int(object)"/>), refusing a str with more than
        /// <see cref="IntMaxStrDigits"/> decimal digits as CPython does (ValueError, with CPython's message)
        /// before any conversion.
        /// </summary>
        public static BigInteger IntFrom(object? value)
        {
            if (value is string s && Py.Len(s) > IntMaxStrDigits)
            {
                int digits = 0;
                for (int i = 0; i < s.Length;)
                {
                    if (Py.IsDecimalDigit(Py.CodePointAt(s, i, out int w))) digits++;
                    i += w;
                }
                if (digits > IntMaxStrDigits) throw TooManyDigits(digits);
            }
            return Py.Int(value);
        }

        /// <summary>
        /// <c>int(run)</c> for a run of Unicode decimal digits (what Python's <c>\d+</c> matches), or null when it
        /// has more than <see cref="DigitRunLimit"/> significant digits. Linear: leading zeros (of any script) are
        /// stripped first, so no long text is converted. A run with a character that is not a decimal digit is a
        /// ValueError, as Python's <c>int(c)</c>.
        /// </summary>
        public static long? DigitRunValue(string run)
        {
            if (Py.Len(run) > DigitRunLimit)
            {
                int start = -1;
                for (int i = 0; i < run.Length;)
                {
                    int cp = Py.CodePointAt(run, i, out int w);
                    int d = Py.DecimalValue(cp);
                    if (d < 0) Py.Int(run.Substring(i, w)); // throws Python's ValueError
                    if (d != 0 && start < 0) start = i;
                    i += w;
                }
                run = start < 0 ? "" : run.Substring(start);
                if (Py.Len(run) > DigitRunLimit) return null;
            }
            return run.Length == 0 ? 0L : (long)Py.Int(run);
        }

        /// <summary>
        /// <c>json.loads(text)</c>, its integer literals read with int_from: one of more than
        /// <see cref="IntMaxStrDigits"/> digits is a ValueError. Fractions and exponents are floats, as json makes
        /// them. See <see cref="Json.Loads"/>.
        /// </summary>
        public static object? JsonLoads(string text) => Json.Loads(text);
    }
}
