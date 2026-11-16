// Twin of scfg_core/page_messages.py: what the checkout page may tell the game, checked the same way wherever the
// page runs. A tx hash is only a hint: the watcher verifies it on-chain before anything is unlocked. Also holds
// the twin of backends.normalize_any_tx until the backends are ported.
using System;

namespace Scfg.Core
{
    public static class PageMessages
    {
        /// <summary>How long a page waits for the game to build a Solana transaction, in seconds.</summary>
        public const int BuildTimeoutS = 15;

        /// <summary>
        /// Python <c>backends.normalize_any_tx</c>: a transaction id in any supported family's format, normalized,
        /// trying the families in Python's registration order (EVM's normalize_tx_hash, then Solana's
        /// normalize_signature). ValueError when neither reads it.
        /// </summary>
        internal static string NormalizeAnyTx(object? text)
        {
            try
            {
                return Sessions.NormalizeTxHash(text);
            }
            catch (ValueError)
            {
            }
            try
            {
                return Solana.Base58.NormalizeSignature(text);
            }
            catch (ValueError)
            {
            }
            throw new ValueError("not a transaction hash or signature: " + Py.Repr(text));
        }

        /// <summary>
        /// Python <c>parse_tx_message</c>: <c>{"tx_hash", "from"?}</c> to (the normalized tx hash or signature, the
        /// normalized sender or null). A non-dict, a missing or a malformed hash is a ValueError; a malformed sender
        /// is dropped. A truthy sender whose <c>str()</c> starts with <c>0x</c> is read as an EVM address, any
        /// other as a Solana account.
        /// </summary>
        public static (string TxHash, string? Sender) ParseTxMessage(object? data)
        {
            if (!(data is JsonObject d)) throw new ValueError("not an object");
            if (!d.TryGetValue("tx_hash", out var raw)) throw new ValueError("bad tx hash");
            var txHash = NormalizeAnyTx(raw);
            var from = d.Get("from");
            string? sender = null;
            if (Py.Truthy(from))
            {
                try
                {
                    sender = Coio.Str(from).StartsWith("0x", StringComparison.Ordinal)
                        ? Evm.ToChecksumAddress(from)
                        : Solana.Base58.NormalizePubkey(from);
                }
                catch (Exception e) when (e is ValueError || e is TypeError)
                {
                    sender = null;
                }
            }
            return (txHash, sender);
        }

        /// <summary>Python <c>parse_build_message</c>: <c>{"account"}</c> to the player's Solana account, normalized. Raises ValueError.</summary>
        public static string ParseBuildMessage(object? data)
        {
            if (!(data is JsonObject d)) throw new ValueError("not an object");
            if (!d.TryGetValue("account", out var raw)) throw new ValueError("bad account");
            return Solana.Base58.NormalizePubkey(raw);
        }
    }
}
