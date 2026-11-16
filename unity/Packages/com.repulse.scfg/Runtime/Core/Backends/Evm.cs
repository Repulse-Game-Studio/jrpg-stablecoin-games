// Twin of scfg_core/backends/evm.py (and of rpgmaker/src/core/backends/evm.ts).
// EVM chains: the watcher, re-verification and restore code behind the IBackend interface.
#nullable enable
using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace Scfg.Core
{
    public sealed class EvmBackend : IBackend
    {
        /// <summary>Blocks before the head a new session starts scanning from.</summary>
        public const long StartBlockMargin = 5;

        public string Family => "evm";

        public object MakeRpc(Chain chain, ITransport transport) =>
            new RpcClient(chain.Rpc, transport, logRange: chain.MaxLogRange);

        public string NormalizeTx(object? text) => Sessions.NormalizeTxHash(text);

        /// <summary>The <see cref="RpcClient"/> MakeRpc built (Python calls whatever it is handed).</summary>
        internal static RpcClient Client(object rpc) =>
            rpc as RpcClient ?? throw new TypeError("an EVM chain needs an RpcClient, not " + Py.TypeName(rpc));

        public async Task<JsonObject> Start(object rpc, Chain chain, string? wallet, string token, IEntropy entropy)
        {
            long head = await Client(rpc).BlockNumber();
            return new JsonObject { { "start_block", Math.Max(0, head - StartBlockMargin) } };
        }

        public Task<StepOutcome> Advance(JsonObject pending, JsonObject receipts, string sid, object rpc, Chain chain,
                                         string wallet, IReadOnlyDictionary<string, Item> items, long now) =>
            Watcher.Advance(pending, receipts, sid, Client(rpc), chain, wallet, items, now);

        public Task<string> Reverify(JsonObject receipt, object rpc, Chain chain, IReadOnlyList<string> wallets) =>
            Scfg.Core.Reverify.CheckEvmReceipt(receipt, Client(rpc), chain, wallets);

        public Task<List<RestoreCandidate>?> Lookup(string tx, ConfigData config, Chain chain, object rpc,
                                                    JsonObject receipts, Action<Exception>? onError) =>
            Restore.LookupEvm(tx, config, chain, Client(rpc), receipts, onError);

        /// <summary>Every token can be received on an EVM chain: no network needed (the RPC is never built).</summary>
        public Task<Dictionary<string, bool>> CheckReady(Func<object> rpc, Chain chain, string? wallet)
        {
            var ready = new Dictionary<string, bool>();
            foreach (var name in chain.Tokens.Keys) ready[name] = true;
            return Task.FromResult(ready);
        }

        public Task<JsonObject> BuildPayment(JsonObject rec, object rpc, Chain chain, string account) =>
            throw new NotImplementedException("build_payment is Solana only");
    }
}
