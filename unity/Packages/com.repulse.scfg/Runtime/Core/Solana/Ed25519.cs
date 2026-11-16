// Twin of scfg_core/solana/ed25519.py (and of rpgmaker/src/core/solana/ed25519.ts).
// Is a 32-byte value a valid ed25519 public key (a point on the curve)? Solana uses this to tell wallet addresses
// (on the curve) from program-derived addresses such as token accounts (deliberately off the curve). It mirrors
// curve25519-dalek's CompressedEdwardsY::decompress: y is read little-endian with the sign bit masked and reduced
// mod p (non-canonical encodings accepted), and the point exists iff (y^2 - 1) / (d*y^2 + 1) is a square mod p.
#nullable enable
using System.Numerics;

namespace Scfg.Core.Solana
{
    public static class Ed25519
    {
        private static readonly BigInteger P = BigInteger.Pow(2, 255) - 19;
        private static readonly BigInteger D = Mod(-121665 * BigInteger.ModPow(121666, P - 2, P));
        private static readonly BigInteger Mask255 = (BigInteger.One << 255) - 1;

        private static BigInteger Mod(BigInteger x)
        {
            var r = x % P;
            return r.Sign < 0 ? r + P : r;
        }

        public static bool IsOnCurve(byte[] point)
        {
            if (point.Length != 32) return false;
            var y = (new BigInteger(point, isUnsigned: true, isBigEndian: false) & Mask255) % P;
            var yy = y * y % P;
            var u = Mod(yy - 1);
            var v = (D * yy + 1) % P; // never 0: d is not a square mod p
            var x2 = u * BigInteger.ModPow(v, P - 2, P) % P;
            return x2.IsZero || BigInteger.ModPow(x2, (P - 1) / 2, P).IsOne;
        }
    }
}
