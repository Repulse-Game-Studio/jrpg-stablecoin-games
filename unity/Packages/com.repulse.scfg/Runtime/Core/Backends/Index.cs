// Twin of scfg_core/backends/__init__.py (and of rpgmaker/src/core/backends/index.ts).
// Chain-family backends: everything that differs between EVM chains and Solana.
//
// The engine, sessions, receipts and screens are family-neutral. Anything that touches a chain's RPC,
// transaction-id format or payment matching goes through BackendFor(chain). One stateless backend per family,
// registered EVM first, then Solana: NormalizeAnyTx tries the families in that order, as Python does.
#nullable enable
using System.Collections.Generic;

namespace Scfg.Core
{
    public static class Backends
    {
        private static readonly IBackend[] All = { new EvmBackend(), new SolanaBackend() };

        /// <summary>The backend of a family; a ValueError for an unknown one.</summary>
        public static IBackend BackendForFamily(string family)
        {
            foreach (var b in All)
                if (b.Family == family) return b;
            throw new ValueError("unknown chain family " + Py.Repr(family));
        }

        public static IBackend BackendFor(Chain chain) => BackendForFamily(chain.Family);

        /// <summary>The backends in registration order (EVM, then Solana).</summary>
        public static IReadOnlyList<IBackend> Registered => All;

        /// <summary>
        /// Python <c>normalize_any_tx</c>: a transaction id in any supported family's format, normalized, trying
        /// each family's <c>normalize_tx</c> in registration order (EVM's normalize_tx_hash, then Solana's
        /// normalize_signature). A ValueError when none reads it; any other exception propagates, as in Python.
        /// </summary>
        public static string NormalizeAnyTx(object? text)
        {
            foreach (var backend in All)
            {
                try
                {
                    return backend.NormalizeTx(text);
                }
                catch (ValueError)
                {
                }
            }
            throw new ValueError("not a transaction hash or signature: " + Py.Repr(text));
        }
    }
}
