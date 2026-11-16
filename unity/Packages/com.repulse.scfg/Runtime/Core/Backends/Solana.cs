// Twin of scfg_core/backends/solana.py (and of rpgmaker/src/core/backends/solana.ts): a stub until task 11.
// Only the plain members are here (the family, its transaction-id format and its RPC object), so that
// Backends.NormalizeAnyTx and Engine.ReportHint already treat Solana ids as Python does. Every network method
// throws NotImplementedException("solana: task 11").
using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace Scfg.Core
{
    public sealed class SolanaBackend : IBackend
    {
        private const string Todo = "solana: task 11";

        public string Family => "solana";

        public object MakeRpc(Chain chain, ITransport transport) => new Solana.SolanaRpc(new RpcClient(chain.Rpc, transport));

        public string NormalizeTx(object? text) => Solana.Base58.NormalizeSignature(text);

        public Task<JsonObject> Start(object rpc, Chain chain, string? wallet, string token, IEntropy entropy) =>
            throw new NotImplementedException(Todo);

        public Task<StepOutcome> Advance(JsonObject pending, JsonObject receipts, string sid, object rpc, Chain chain,
                                         string wallet, IReadOnlyDictionary<string, Item> items, long now) =>
            throw new NotImplementedException(Todo);

        public Task<string> Reverify(JsonObject receipt, object rpc, Chain chain, IReadOnlyList<string> wallets) =>
            throw new NotImplementedException(Todo);

        public Task<List<RestoreCandidate>?> Lookup(string tx, ConfigData config, Chain chain, object rpc,
                                                    JsonObject receipts, Action<Exception>? onError) =>
            throw new NotImplementedException(Todo);

        public Task<Dictionary<string, bool>> CheckReady(Func<object> rpc, Chain chain, string? wallet) =>
            throw new NotImplementedException(Todo);

        public Task<JsonObject> BuildPayment(JsonObject rec, object rpc, Chain chain, string account) =>
            throw new NotImplementedException(Todo);
    }
}
