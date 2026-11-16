// No Python twin: SHA-256 (FIPS 180-4), what scfg_core gets from hashlib.sha256 (solana/pda.py). Twin of
// rpgmaker/src/core/sha256.ts.
// Hand-written because the BCL's SHA256.Create() resolves its implementation by name through CryptoConfig on
// Mono/IL2CPP, which code stripping can break; this one is plain code the linker keeps.
#nullable enable

namespace Scfg.Core
{
    public static class Sha256
    {
        private static uint[] RoundConstants() => new uint[]
        {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };

        private static readonly uint[] K = RoundConstants(); // static-ok: private constant table, only read

        private static uint Rotr(uint x, int n) => (x >> n) | (x << (32 - n));

        /// <summary><c>hashlib.sha256(data).digest()</c>: the 32-byte digest. The input is not changed.</summary>
        public static byte[] Hash(byte[] data)
        {
            if (data == null) throw new TypeError("object supporting the buffer API required");
            uint h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
            uint h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;

            // the message, 0x80, zeros, and its length in bits as a 64-bit big-endian number, to a multiple of 64 bytes
            long length = data.Length;
            var padded = new byte[((length + 9 + 63) / 64) * 64];
            System.Buffer.BlockCopy(data, 0, padded, 0, data.Length);
            padded[length] = 0x80;
            ulong bits = (ulong)length * 8;
            for (int i = 0; i < 8; i++) padded[padded.Length - 1 - i] = (byte)(bits >> (8 * i));

            var w = new uint[64];
            for (int off = 0; off < padded.Length; off += 64)
            {
                for (int i = 0; i < 16; i++)
                {
                    int p = off + 4 * i;
                    w[i] = ((uint)padded[p] << 24) | ((uint)padded[p + 1] << 16) | ((uint)padded[p + 2] << 8) | padded[p + 3];
                }
                for (int i = 16; i < 64; i++)
                {
                    uint w15 = w[i - 15], w2 = w[i - 2];
                    uint s0 = Rotr(w15, 7) ^ Rotr(w15, 18) ^ (w15 >> 3);
                    uint s1 = Rotr(w2, 17) ^ Rotr(w2, 19) ^ (w2 >> 10);
                    w[i] = unchecked(w[i - 16] + s0 + w[i - 7] + s1);
                }
                uint a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, hh = h7;
                for (int i = 0; i < 64; i++)
                {
                    uint bigS1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
                    uint ch = (e & f) ^ (~e & g);
                    uint t1 = unchecked(hh + bigS1 + ch + K[i] + w[i]);
                    uint bigS0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
                    uint maj = (a & b) ^ (a & c) ^ (b & c);
                    uint t2 = unchecked(bigS0 + maj);
                    hh = g;
                    g = f;
                    f = e;
                    e = unchecked(d + t1);
                    d = c;
                    c = b;
                    b = a;
                    a = unchecked(t1 + t2);
                }
                unchecked
                {
                    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e; h5 += f; h6 += g; h7 += hh;
                }
            }

            var output = new byte[32];
            var hs = new[] { h0, h1, h2, h3, h4, h5, h6, h7 };
            for (int i = 0; i < 8; i++)
            {
                output[4 * i] = (byte)(hs[i] >> 24);
                output[4 * i + 1] = (byte)(hs[i] >> 16);
                output[4 * i + 2] = (byte)(hs[i] >> 8);
                output[4 * i + 3] = (byte)hs[i];
            }
            return output;
        }
    }
}
