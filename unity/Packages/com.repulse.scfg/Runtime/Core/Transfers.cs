// Twin of scfg_core/transfers.py (and of rpgmaker/src/core/transfers.ts).
// A token transfer to the developer, in a form shared by every chain family. Receipts and restore candidates
// are built from it. EVM fills it from an ERC-20 Transfer log; Solana from a transaction's token-balance changes.
using System;
using System.Numerics;

namespace Scfg.Core
{
    /// <summary>Python's frozen <c>Transfer</c> dataclass: immutable, equal by value.</summary>
    public sealed class Transfer : IEquatable<Transfer>
    {
        /// <summary>Token contract (EVM) or mint (Solana).</summary>
        public string Token { get; }
        /// <summary>Payer.</summary>
        public string Sender { get; }
        /// <summary>Developer wallet (the owner, never a token account).</summary>
        public string Recipient { get; }
        /// <summary>Base units.</summary>
        public BigInteger Value { get; }
        /// <summary>EVM: lowercase 0x hash; Solana: base58 signature, case preserved.</summary>
        public string TxHash { get; }
        /// <summary>EVM: log index; Solana: the developer token account's accountIndex.</summary>
        public long LogIndex { get; }
        /// <summary>EVM: block number; Solana: slot.</summary>
        public long BlockNumber { get; }

        public Transfer(string token, string sender, string recipient, BigInteger value, string txHash,
                        long logIndex, long blockNumber)
        {
            Token = token;
            Sender = sender;
            Recipient = recipient;
            Value = value;
            TxHash = txHash;
            LogIndex = logIndex;
            BlockNumber = blockNumber;
        }

        public bool Equals(Transfer? o) =>
            o != null && Token == o.Token && Sender == o.Sender && Recipient == o.Recipient && Value == o.Value
            && TxHash == o.TxHash && LogIndex == o.LogIndex && BlockNumber == o.BlockNumber;

        public override bool Equals(object? obj) => Equals(obj as Transfer);

        public override int GetHashCode() =>
            (Token, Sender, Recipient, Value, TxHash, LogIndex, BlockNumber).GetHashCode();
    }

    /// <summary>Python's frozen <c>TransferLog</c> dataclass (defined in evm.py; kept here beside <see cref="Transfer"/>).</summary>
    public sealed class TransferLog : IEquatable<TransferLog>
    {
        /// <summary>Checksummed contract address.</summary>
        public string Token { get; }
        /// <summary>Checksummed.</summary>
        public string Sender { get; }
        /// <summary>Checksummed.</summary>
        public string Recipient { get; }
        /// <summary>Base units.</summary>
        public BigInteger Value { get; }
        /// <summary>Lowercase.</summary>
        public string TxHash { get; }
        public long LogIndex { get; }
        public long BlockNumber { get; }
        /// <summary>Lowercase.</summary>
        public string BlockHash { get; }

        public TransferLog(string token, string sender, string recipient, BigInteger value, string txHash,
                           long logIndex, long blockNumber, string blockHash)
        {
            Token = token;
            Sender = sender;
            Recipient = recipient;
            Value = value;
            TxHash = txHash;
            LogIndex = logIndex;
            BlockNumber = blockNumber;
            BlockHash = blockHash;
        }

        public Transfer ToTransfer() => new Transfer(Token, Sender, Recipient, Value, TxHash, LogIndex, BlockNumber);

        public bool Equals(TransferLog? o) =>
            o != null && Token == o.Token && Sender == o.Sender && Recipient == o.Recipient && Value == o.Value
            && TxHash == o.TxHash && LogIndex == o.LogIndex && BlockNumber == o.BlockNumber && BlockHash == o.BlockHash;

        public override bool Equals(object? obj) => Equals(obj as TransferLog);

        public override int GetHashCode() =>
            (Token, Sender, Recipient, Value, TxHash, LogIndex, BlockNumber, BlockHash).GetHashCode();
    }
}
