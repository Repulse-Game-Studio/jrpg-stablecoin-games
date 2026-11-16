// Twin of scfg_core/solana/tx.py (and of rpgmaker/src/core/solana/tx.ts).
// Legacy Solana transactions, serialized without any library.
//
// Message layout: header (3 bytes) | compact-u16 #keys | keys (32 bytes each) | recent blockhash (32) |
// compact-u16 #instructions | per instruction: program index, compact-u16 #accounts, account indexes,
// compact-u16 #data, data. Account order: fee payer, then writable signers, readonly signers, writable
// non-signers, readonly non-signers, each group sorted by raw key bytes. Program ids are readonly non-signers.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Numerics;

namespace Scfg.Core.Solana
{
    public sealed class AccountMeta
    {
        public string Pubkey { get; }
        public bool IsSigner { get; }
        public bool IsWritable { get; }

        public AccountMeta(string pubkey, bool isSigner, bool isWritable)
        {
            Pubkey = pubkey;
            IsSigner = isSigner;
            IsWritable = isWritable;
        }
    }

    public sealed class Instruction
    {
        public string ProgramId { get; }
        public IReadOnlyList<AccountMeta> Accounts { get; }
        public byte[] Data { get; }

        public Instruction(string programId, IReadOnlyList<AccountMeta> accounts, byte[] data)
        {
            ProgramId = programId;
            Accounts = accounts;
            Data = data;
        }
    }

    public static class Tx
    {
        public const string ComputeBudgetProgramId = "ComputeBudget111111111111111111111111111111";

        public static byte[] CompactU16(long n)
        {
            if (n < 0 || n > 0xFFFF)
                throw new ValueError("compact-u16 out of range: " + n.ToString(CultureInfo.InvariantCulture));
            var result = new List<byte>();
            while (true)
            {
                byte b = (byte)(n & 0x7F);
                n >>= 7;
                if (n != 0) result.Add((byte)(b | 0x80));
                else { result.Add(b); return result.ToArray(); }
            }
        }

        private sealed class KeyOrder : IComparer<byte[]>
        {
            public int Compare(byte[]? a, byte[]? b)
            {
                for (int i = 0; i < a!.Length && i < b!.Length; i++)
                    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
                return a!.Length < b!.Length ? -1 : a.Length > b.Length ? 1 : 0;
            }
        }

        private sealed class Entry
        {
            public byte[] Raw = new byte[0];
            public bool Signer;
            public bool Writable;
        }

        public static byte[] CompileMessage(string payer, IReadOnlyList<Instruction> instructions, string recentBlockhash)
        {
            // Keyed by hex of the raw key; the raw bytes are sorted afterwards.
            var flags = new Dictionary<string, Entry>(StringComparer.Ordinal);
            void Add(string key, bool signer, bool writable)
            {
                var raw = Base58.DecodeFixed(key, 32);
                var id = Bytes.ToHex(raw);
                if (!flags.TryGetValue(id, out var e)) { e = new Entry { Raw = raw }; flags[id] = e; }
                e.Signer = e.Signer || signer;
                e.Writable = e.Writable || writable;
            }
            foreach (var ix in instructions)
            {
                foreach (var meta in ix.Accounts) Add(meta.Pubkey, meta.IsSigner, meta.IsWritable);
                Add(ix.ProgramId, false, false);
            }
            var payerRaw = Base58.DecodeFixed(payer, 32);
            flags.Remove(Bytes.ToHex(payerRaw));

            List<byte[]> Group(bool signer, bool writable)
            {
                var l = new List<byte[]>();
                foreach (var e in flags.Values)
                    if (e.Signer == signer && e.Writable == writable) l.Add(e.Raw);
                l.Sort(new KeyOrder());
                return l;
            }
            var ws = Group(true, true);
            var rs = Group(true, false);
            var wu = Group(false, true);
            var ru = Group(false, false);
            var keys = new List<byte[]> { payerRaw };
            keys.AddRange(ws); keys.AddRange(rs); keys.AddRange(wu); keys.AddRange(ru);
            var index = new Dictionary<string, int>(StringComparer.Ordinal);
            for (int i = 0; i < keys.Count; i++) index[Bytes.ToHex(keys[i])] = i;
            byte Idx(string key) => (byte)index[Bytes.ToHex(Base58.DecodeFixed(key, 32))];

            using (var o = new MemoryStream())
            {
                void W(byte[] b) => o.Write(b, 0, b.Length);
                W(new[] { (byte)(1 + ws.Count + rs.Count), (byte)rs.Count, (byte)ru.Count });
                W(CompactU16(keys.Count));
                foreach (var k in keys) W(k);
                W(Base58.DecodeFixed(recentBlockhash, 32));
                W(CompactU16(instructions.Count));
                foreach (var ix in instructions)
                {
                    o.WriteByte(Idx(ix.ProgramId));
                    W(CompactU16(ix.Accounts.Count));
                    foreach (var m in ix.Accounts) o.WriteByte(Idx(m.Pubkey));
                    W(CompactU16(ix.Data.Length));
                    W(ix.Data);
                }
                return o.ToArray();
            }
        }

        /// <summary>Empty signature slots + message, ready for a wallet to sign.</summary>
        public static byte[] UnsignedTransaction(byte[] message)
        {
            if (message.Length == 0) throw new ValueError("empty message");
            int signers = message[0];
            var head = CompactU16(signers);
            var result = new byte[head.Length + 64 * signers + message.Length];
            Buffer.BlockCopy(head, 0, result, 0, head.Length);
            Buffer.BlockCopy(message, 0, result, head.Length + 64 * signers, message.Length);
            return result;
        }

        /// <summary>Unsigned little-endian integer of <paramref name="size"/> bytes (Python int.to_bytes(size, "little")).</summary>
        private static byte[] UIntLE(BigInteger value, int size)
        {
            if (value.Sign < 0 || value >= BigInteger.One << (8 * size))
                throw new ValueError(value.ToString(CultureInfo.InvariantCulture) + " does not fit in "
                                     + size.ToString(CultureInfo.InvariantCulture) + " bytes");
            var result = new byte[size];
            for (int i = 0; i < size; i++) { result[i] = (byte)(value & 0xFF); value >>= 8; }
            return result;
        }

        private static byte[] Prefixed(byte tag, byte[] rest)
        {
            var r = new byte[rest.Length + 1];
            r[0] = tag;
            Buffer.BlockCopy(rest, 0, r, 1, rest.Length);
            return r;
        }

        public static Instruction SetComputeUnitLimit(BigInteger units) =>
            new Instruction(ComputeBudgetProgramId, new AccountMeta[0], Prefixed(2, UIntLE(units, 4)));

        public static Instruction SetComputeUnitPrice(BigInteger microLamports) =>
            new Instruction(ComputeBudgetProgramId, new AccountMeta[0], Prefixed(3, UIntLE(microLamports, 8)));

        /// <summary>
        /// SPL Token TransferChecked; <paramref name="extra"/> keys (e.g. a Solana Pay reference) are appended
        /// read-only so the payment can be found with getSignaturesForAddress(extra key).
        /// </summary>
        public static Instruction TransferChecked(string source, string mint, string destination, string owner,
            BigInteger amount, int decimals, IEnumerable<string>? extra = null)
        {
            var accounts = new List<AccountMeta>
            {
                new AccountMeta(source, false, true), new AccountMeta(mint, false, false),
                new AccountMeta(destination, false, true), new AccountMeta(owner, true, false),
            };
            if (extra != null) foreach (var k in extra) accounts.Add(new AccountMeta(k, false, false));
            var data = new byte[10];
            data[0] = 12;
            Buffer.BlockCopy(UIntLE(amount, 8), 0, data, 1, 8);
            Buffer.BlockCopy(UIntLE(decimals, 1), 0, data, 9, 1);
            return new Instruction(Pda.TokenProgramId, accounts.ToArray(), data);
        }
    }
}
