// Twin of scfg_core/backends/base.py (and of rpgmaker/src/core/backends/base.ts).
// The interface every chain family implements (see Backends/Evm.cs and Backends/Solana.cs).
//
// Python's cooperative generators (start, advance, reverify, lookup, check_ready, build_payment) are async methods
// here, awaiting the transport of the RPC object they are handed; NormalizeTx and MakeRpc are plain. The RPC object
// is whatever MakeRpc built for the chain (an RpcClient for EVM, a SolanaRpc for Solana), typed object as Python's
// duck-typed `rpc` is: each backend casts it to its own family's client.
#nullable enable
using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace Scfg.Core
{
    public interface IBackend
    {
        /// <summary>"evm" or "solana".</summary>
        string Family { get; }

        /// <summary>The RPC object handed back to every other method for this chain.</summary>
        object MakeRpc(Chain chain, ITransport transport);

        /// <summary>Validate a transaction id in this family's format. Throws ValueError.</summary>
        string NormalizeTx(object? text);

        /// <summary>
        /// Fields for a new session: must contain "start_block"; the rest is merged in. <paramref name="entropy"/>
        /// stands in for Python's <c>secrets</c> (Solana draws its reference from it).
        /// </summary>
        Task<JsonObject> Start(object rpc, Chain chain, string? wallet, string token, IEntropy entropy);

        /// <summary>Move one pending session forward.</summary>
        Task<StepOutcome> Advance(JsonObject pending, JsonObject receipts, string sid, object rpc, Chain chain,
                                  string wallet, IReadOnlyDictionary<string, Item> items, long now);

        /// <summary>"keep" or "drop". Network trouble must answer "keep".</summary>
        Task<string> Reverify(JsonObject receipt, object rpc, Chain chain, IReadOnlyList<string> wallets);

        /// <summary>
        /// Restore candidates for <paramref name="tx"/> on this chain, or null if the chain doesn't know it. Throws
        /// RestoreError("tx_reverted" | "tx_not_a_payment"). A transfer that cannot be classified is skipped and
        /// reported to <paramref name="onError"/>.
        /// </summary>
        Task<List<RestoreCandidate>?> Lookup(string tx, ConfigData config, Chain chain, object rpc, JsonObject receipts,
                                             Action<Exception>? onError);

        /// <summary>
        /// Token name to whether the developer can receive it on this chain. <paramref name="rpc"/> builds the chain's
        /// RPC object on first use (Python's <c>_LazyRpc</c>), so a family that needs no network never builds one.
        /// </summary>
        Task<Dictionary<string, bool>> CheckReady(Func<object> rpc, Chain chain, string? wallet);

        /// <summary>Solana only: the unsigned transaction a browser wallet signs for this session.</summary>
        Task<JsonObject> BuildPayment(JsonObject rec, object rpc, Chain chain, string account);
    }
}
