// Twin of scfg_core/solana/base58.py (and of rpgmaker/src/core/solana/base58.ts).
// Base58 (Bitcoin alphabet), as used for Solana addresses and signatures.
#nullable enable
using System;
using System.Globalization;
using System.Numerics;
using System.Text;

namespace Scfg.Core.Solana
{
    public static class Base58
    {
        public const string Alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

        private static readonly int[] Index = BuildIndex(); // static-ok: private lookup table, only read

        private static int[] BuildIndex()
        {
            var t = new int[128];
            for (int i = 0; i < t.Length; i++) t[i] = -1;
            for (int i = 0; i < Alphabet.Length; i++) t[Alphabet[i]] = i;
            return t;
        }

        public static string B58Encode(byte[] data)
        {
            var n = new BigInteger(data, isUnsigned: true, isBigEndian: true);
            var digits = new StringBuilder();
            var fiftyEight = new BigInteger(58);
            while (!n.IsZero)
            {
                n = BigInteger.DivRem(n, fiftyEight, out var r);
                digits.Insert(0, Alphabet[(int)r]);
            }
            int zeros = 0;
            while (zeros < data.Length && data[zeros] == 0) zeros++;
            return new string('1', zeros) + digits; // culture-ok: a StringBuilder of ASCII
        }

        /// <summary>Text that is not a non-empty str, or has a character outside the alphabet, is a ValueError.</summary>
        public static byte[] B58Decode(object? text)
        {
            if (!(text is string s) || s.Length == 0) throw new ValueError("not base58: " + Py.Repr(text));
            var n = BigInteger.Zero;
            foreach (char c in s)
            {
                int d = c < 128 ? Index[c] : -1;
                if (d < 0) throw new ValueError("invalid base58 character " + Py.Repr(new string(c, 1)));
                n = n * 58 + d;
            }
            int zeros = 0;
            while (zeros < s.Length && s[zeros] == '1') zeros++;
            var body = n.IsZero ? new byte[0] : n.ToByteArray(isUnsigned: true, isBigEndian: true);
            var raw = new byte[zeros + body.Length];
            Array.Copy(body, 0, raw, zeros, body.Length);
            return raw;
        }

        /// <summary>
        /// <paramref name="length"/> bytes from base58 <paramref name="text"/>. Text longer than any encoding of
        /// that many bytes (44 characters for 32, 88 for 64) is refused before it is decoded. Bad input is a
        /// ValueError, a length that is not an int (a bool, a float, a str, ...) included.
        /// </summary>
        public static byte[] DecodeFixed(object? text, object? length)
        {
            BigInteger n;
            switch (length)
            {
                case int i: n = i; break;
                case long l: n = l; break;
                case BigInteger b: n = b; break;
                default: throw new ValueError("not a byte count: " + Py.Repr(length));
            }
            // Python: math.ceil(length * math.log(256) / math.log(58)); beyond a float it is an OverflowError there.
            double asFloat = (double)n;
            if (double.IsInfinity(asFloat)) throw new ValueError("not a byte count: " + Py.Repr(length));
            double limit = Math.Ceiling(asFloat * Math.Log(256) / Math.Log(58));
            if (text is string s && Py.Len(s) > limit)
                throw new ValueError("too long for " + n.ToString(CultureInfo.InvariantCulture) + " bytes");
            var raw = B58Decode(text);
            if (raw.Length != n)
                throw new ValueError("expected " + n.ToString(CultureInfo.InvariantCulture) + " bytes, got "
                                     + raw.Length.ToString(CultureInfo.InvariantCulture));
            return raw;
        }

        public static string NormalizePubkey(object? text)
        {
            if (!(text is string s)) throw new ValueError("not an address: " + Py.Repr(text));
            return B58Encode(DecodeFixed(Py.Strip(s), 32));
        }

        public static string NormalizeSignature(object? text)
        {
            if (!(text is string s)) throw new ValueError("not a signature: " + Py.Repr(text));
            return B58Encode(DecodeFixed(Py.Strip(s), 64));
        }
    }
}
