// No Python twin: hex, base64 and byte helpers (Python uses bytes.fromhex / .hex() / base64), mirroring
// rpgmaker/src/core/bytes.ts.
using System;

namespace Scfg.Core
{
    public static class Bytes
    {
        private const string Hex = "0123456789abcdef";

        /// <summary>Lowercase hex without a prefix.</summary>
        public static string ToHex(byte[] b)
        {
            var o = new char[b.Length * 2];
            for (int i = 0; i < b.Length; i++)
            {
                o[2 * i] = Hex[b[i] >> 4];
                o[2 * i + 1] = Hex[b[i] & 15];
            }
            return new string(o);
        }

        /// <summary>
        /// Decode hex (either case, optional lowercase <c>0x</c> prefix) into bytes. Odd length or a character
        /// that is not an ASCII hex digit is a ValueError.
        /// </summary>
        public static byte[] FromHex(string hex)
        {
            var body = hex.StartsWith("0x", StringComparison.Ordinal) ? hex.Substring(2) : hex;
            if (body.Length % 2 != 0) throw new ValueError("odd-length hex: " + hex);
            var o = new byte[body.Length / 2];
            for (int i = 0; i < o.Length; i++)
            {
                int hi = Nibble(body[2 * i]);
                int lo = Nibble(body[2 * i + 1]);
                if (hi < 0 || lo < 0) throw new ValueError("non-hex character in: " + hex);
                o[i] = (byte)((hi << 4) | lo);
            }
            return o;
        }

        private static int Nibble(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        /// <summary>Concatenate byte arrays.</summary>
        public static byte[] Concat(params byte[][] parts)
        {
            int n = 0;
            foreach (var p in parts) n += p.Length;
            var o = new byte[n];
            int at = 0;
            foreach (var p in parts)
            {
                Buffer.BlockCopy(p, 0, o, at, p.Length);
                at += p.Length;
            }
            return o;
        }

        /// <summary>Bytes of a string whose characters are all below U+0100 (ASCII in practice): each unit's low byte.</summary>
        public static byte[] AsciiBytes(string s)
        {
            var o = new byte[s.Length];
            for (int i = 0; i < s.Length; i++) o[i] = (byte)(s[i] & 0xff);
            return o;
        }

        /// <summary>Standard base64 with <c>=</c> padding (Python <c>base64.b64encode(b).decode("ascii")</c>).</summary>
        public static string Base64Encode(byte[] b) => Convert.ToBase64String(b);
    }
}
