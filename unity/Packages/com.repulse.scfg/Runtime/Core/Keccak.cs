// Twin of scfg_core/keccak.py and scfg_core/_xkcp.py (and of rpgmaker/src/core/keccak.ts).
// Keccak-256 as used by Ethereum: the original Keccak padding (suffix 0x01), not NIST SHA3-256 (0x06).
// The permutation and sponge follow the Keccak team's public-domain reference implementation (XKCP's
// CompactFIPS202), on 64-bit lanes instead of Python integers.
#nullable enable
using System;

namespace Scfg.Core
{
    public static class Keccak
    {
        private const int RateBytes = 136; // rate 1088 bits, capacity 512 bits
        private const byte Suffix = 0x01;  // NIST SHA3 uses 0x06; Ethereum uses the original Keccak 0x01

        /// <summary>The 32-byte Keccak-256 digest of <paramref name="data"/>.</summary>
        public static byte[] Keccak256(byte[] data)
        {
            if (data == null) throw new TypeError("keccak256 needs bytes, not NoneType");
            var state = new ulong[25];
            int offset = 0;
            // Absorb all whole blocks.
            while (data.Length - offset >= RateBytes)
            {
                Xor(state, data, offset, RateBytes);
                F1600(state);
                offset += RateBytes;
            }
            // Pad the last (possibly empty) partial block: suffix after the data, 0x80 on the last rate byte.
            var block = new byte[RateBytes];
            int rest = data.Length - offset;
            Buffer.BlockCopy(data, offset, block, 0, rest);
            block[rest] ^= Suffix;
            block[RateBytes - 1] ^= 0x80;
            Xor(state, block, 0, RateBytes);
            F1600(state);
            // Squeeze 32 bytes (less than one rate block).
            var output = new byte[32];
            for (int i = 0; i < 32; i++) output[i] = (byte)(state[i / 8] >> (8 * (i % 8)));
            return output;
        }

        // XOR n bytes (n a multiple of 8) into the state's lanes, little-endian.
        private static void Xor(ulong[] state, byte[] src, int offset, int n)
        {
            for (int i = 0; i < n / 8; i++)
            {
                ulong lane = 0;
                for (int k = 7; k >= 0; k--) lane = (lane << 8) | src[offset + 8 * i + k];
                state[i] ^= lane;
            }
        }

        private static ulong Rol(ulong a, int n) => n == 0 ? a : (a << n) | (a >> (64 - n));

        // Keccak-f[1600]; lane (x, y) is state[x + 5 * y].
        private static void F1600(ulong[] a)
        {
            var c = new ulong[5];
            var d = new ulong[5];
            var t = new ulong[5];
            uint r = 1;
            for (int round = 0; round < 24; round++)
            {
                // theta
                for (int x = 0; x < 5; x++) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
                for (int x = 0; x < 5; x++) d[x] = c[(x + 4) % 5] ^ Rol(c[(x + 1) % 5], 1);
                for (int i = 0; i < 25; i++) a[i] ^= d[i % 5];
                // rho and pi
                int px = 1, py = 0;
                ulong current = a[px + 5 * py];
                for (int s = 0; s < 24; s++)
                {
                    int nx = py;
                    int ny = (2 * px + 3 * py) % 5;
                    px = nx;
                    py = ny;
                    ulong next = a[px + 5 * py];
                    a[px + 5 * py] = Rol(current, ((s + 1) * (s + 2) / 2) % 64);
                    current = next;
                }
                // chi
                for (int y = 0; y < 5; y++)
                {
                    for (int x = 0; x < 5; x++) t[x] = a[x + 5 * y];
                    for (int x = 0; x < 5; x++) a[x + 5 * y] = t[x] ^ (~t[(x + 1) % 5] & t[(x + 2) % 5]);
                }
                // iota
                for (int j = 0; j < 7; j++)
                {
                    r = ((r << 1) ^ ((r >> 7) * 0x71)) % 256;
                    if ((r & 2) != 0) a[0] ^= 1UL << ((1 << j) - 1);
                }
            }
        }
    }
}
