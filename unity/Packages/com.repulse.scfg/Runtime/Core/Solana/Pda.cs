// Twin of scfg_core/solana/pda.py (and of rpgmaker/src/core/solana/pda.ts).
// Program-derived addresses and associated token accounts (classic SPL Token program).
#nullable enable
using System;
using System.Text;

namespace Scfg.Core.Solana
{
    public static class Pda
    {
        public const string TokenProgramId = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
        public const string AssociatedTokenProgramId = "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL";
        public const int MaxSeedLen = 32;
        public const int MaxSeeds = 16;
        private static readonly byte[] Marker = Encoding.ASCII.GetBytes("ProgramDerivedAddress"); // static-ok: private constant bytes, only copied from

        /// <summary>The address, or null when the hash lands on the curve.</summary>
        public static byte[]? CreateProgramAddress(byte[][] seeds, byte[] programId)
        {
            if (seeds.Length > MaxSeeds) throw new ValueError("too many seeds or a seed longer than 32 bytes");
            foreach (var s in seeds)
                if (s.Length > MaxSeedLen) throw new ValueError("too many seeds or a seed longer than 32 bytes");
            int total = programId.Length + Marker.Length;
            foreach (var s in seeds) total += s.Length;
            var buf = new byte[total];
            int pos = 0;
            foreach (var s in seeds) { Buffer.BlockCopy(s, 0, buf, pos, s.Length); pos += s.Length; }
            Buffer.BlockCopy(programId, 0, buf, pos, programId.Length); pos += programId.Length;
            Buffer.BlockCopy(Marker, 0, buf, pos, Marker.Length);
            var digest = Sha256.Hash(buf);
            return Ed25519.IsOnCurve(digest) ? null : digest;
        }

        public static (byte[] Address, int Bump) FindProgramAddress(byte[][] seeds, byte[] programId)
        {
            var withBump = new byte[seeds.Length + 1][];
            Array.Copy(seeds, withBump, seeds.Length);
            for (int bump = 255; bump >= 0; bump--)
            {
                withBump[seeds.Length] = new[] { (byte)bump };
                var address = CreateProgramAddress(withBump, programId);
                if (address != null) return (address, bump);
            }
            throw new ValueError("no viable bump seed");
        }

        /// <summary>The developer's (or player's) token account for <paramref name="mint"/>, as wallets create it.</summary>
        public static string AssociatedTokenAddress(string owner, string mint)
        {
            var seeds = new[]
            {
                Base58.DecodeFixed(owner, 32), Base58.DecodeFixed(TokenProgramId, 32), Base58.DecodeFixed(mint, 32),
            };
            var (address, _) = FindProgramAddress(seeds, Base58.DecodeFixed(AssociatedTokenProgramId, 32));
            return Base58.B58Encode(address);
        }
    }
}
