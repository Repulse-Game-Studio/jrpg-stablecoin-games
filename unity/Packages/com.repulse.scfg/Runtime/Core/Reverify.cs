// Twin of scfg_core/reverify.py (and of rpgmaker/src/core/reverify.ts).
// Launch-time re-verification of stored receipts against the chain.
//
// Each receipt is checked by its chain family's backend:
//
// - EVM: one eth_getLogs call scoped to the receipt's own block. Block-scoped lookups keep working years later, even
//   on nodes that no longer index old transaction hashes.
// - Solana: getTransaction on the receipt's signature, asked of each endpoint in turn; a missing transaction only
//   counts when an endpoint whose history reaches back to the receipt's slot says so.
//
// Only a *definitive* mismatch removes a receipt; network trouble never does. Python's generators are async methods
// that only await the RPC objects they are handed.
#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Threading.Tasks;

namespace Scfg.Core
{
    public static class Reverify
    {
        /// <summary>A receipt this far past the node's head (EVM block, Solana finalized slot) is dropped; nearer, it is kept.</summary>
        public const long FutureToleranceBlocks = 10_000;

        /// <summary>
        /// Python <c>reverify_receipts</c>: remove the receipts that the chain contradicts and return the removed keys,
        /// in the store's order. <paramref name="wallets"/> maps a family to the developer's wallets; a family with no
        /// wallets keeps its receipts, which cannot be judged.
        ///
        /// A stored entry whose own fields are invalid is removed and the reason reported to <paramref name="onError"/>
        /// (default <see cref="Errors.PrintError"/>): a forged receipt cannot escape verification by making its check
        /// fail. The fields are checked before any RPC: a missing field; a chain, token or item that cannot be a key; a
        /// tx hash that is not a str; a log index or block that is not an int &gt;= 0 as <see cref="Ints.IntFrom"/> reads
        /// it; an amount that is not a number of base units within <see cref="Amounts.StoredAmount"/>'s range; and, on a
        /// chain this build knows, a tx id that is not its family's or has surrounding whitespace. A receipt naming a
        /// chain this build does not know is kept, its family fields unchecked: a configuration change must not erase
        /// purchases.
        ///
        /// A check that fails because of what a node returned (any exception, as Python's <c>except Exception</c>) is not
        /// the receipt's fault: that receipt is kept as unverifiable, the failure is reported, and the receipts after it
        /// are still checked.
        /// </summary>
        public static Task<List<string>> ReverifyReceipts(JsonObject receipts, Func<string, object> rpcFor,
                                                         IReadOnlyDictionary<string, Chain> chains,
                                                         IReadOnlyDictionary<string, IReadOnlyList<string>> wallets,
                                                         Action<Exception>? onError = null) =>
            Run(receipts, rpcFor, chains, family => wallets.TryGetValue(family, out var w) ? w : Array.Empty<string>(), onError);

        /// <summary><see cref="ReverifyReceipts(JsonObject, Func{string, object}, IReadOnlyDictionary{string, Chain}, IReadOnlyDictionary{string, IReadOnlyList{string}}, Action{Exception})"/> with the same wallets for every family.</summary>
        public static Task<List<string>> ReverifyReceipts(JsonObject receipts, Func<string, object> rpcFor,
                                                         IReadOnlyDictionary<string, Chain> chains, IReadOnlyList<string> wallets,
                                                         Action<Exception>? onError = null) =>
            Run(receipts, rpcFor, chains, _ => wallets, onError);

        private static async Task<List<string>> Run(JsonObject receipts, Func<string, object> rpcFor,
                                                    IReadOnlyDictionary<string, Chain> chains,
                                                    Func<string, IReadOnlyList<string>> walletsOf, Action<Exception>? onError)
        {
            var removed = new List<string>();
            var report = onError ?? Errors.PrintError;
            foreach (var kv in receipts.ToList()) // list(receipts.items())
            {
                var problem = InvalidFields(kv.Value, chains);
                if (problem != null)
                {
                    receipts.Remove(kv.Key);
                    removed.Add(kv.Key);
                    report(new ValueError("stored receipt " + Py.Repr(kv.Key) + " is invalid and was removed: " + problem.Message, problem));
                    continue;
                }
                string verdict;
                try
                {
                    verdict = await Judge((JsonObject)kv.Value!, rpcFor, chains, walletsOf);
                }
                catch (Exception e)
                {
                    report(e); // node data the check could not read: unverifiable, kept
                    continue;
                }
                if (verdict == "drop")
                {
                    receipts.Remove(kv.Key);
                    removed.Add(kv.Key);
                }
            }
            return removed;
        }

        /// <summary>
        /// Python <c>_invalid_fields</c>: why the stored entry is not a receipt the checks can read (a KeyError, TypeError,
        /// ValueError or ArithmeticError; Python's AttributeError is a TypeError here), or null. The backends then read
        /// the same fields.
        /// </summary>
        private static Exception? InvalidFields(object? r, IReadOnlyDictionary<string, Chain> chains)
        {
            try
            {
                var chainKey = Py.Get(r, "chain");
                Py.CheckHashable(chainKey);
                Py.CheckHashable(Py.Get(r, "token"));
                Py.CheckHashable(((JsonObject)r!).Get("item")); // r["chain"] worked: r is a dict
                var txValue = Py.Get(r, "tx_hash");
                if (!(txValue is string tx)) throw new TypeError("'" + Py.TypeName(txValue) + "' object has no attribute 'lower'");
                if (Ints.IntFrom(Py.Get(r, "log_index")) < 0 || Ints.IntFrom(Py.Get(r, "block")) < 0)
                    throw new ValueError("log index and block must be >= 0");
                Amounts.DecimalToUnits(Amounts.StoredAmount(Py.Get(r, "amount")));
                if (chainKey is string key && chains.TryGetValue(key, out var chain))
                {
                    if (tx != Py.Strip(tx)) throw new ValueError("tx id with surrounding whitespace: " + Py.Repr(tx));
                    Backends.BackendFor(chain).NormalizeTx(tx);
                }
            }
            catch (Exception e) when (e is KeyError || e is TypeError || e is ValueError || e is ArithmeticError)
            {
                return e;
            }
            return null;
        }

        /// <summary>Python <c>_judge</c>: a well-formed receipt's verdict, "drop" or "keep".</summary>
        private static async Task<string> Judge(JsonObject r, Func<string, object> rpcFor, IReadOnlyDictionary<string, Chain> chains,
                                                Func<string, IReadOnlyList<string>> walletsOf)
        {
            if (!(r["chain"] is string key) || !chains.TryGetValue(key, out var chain))
                return "keep"; // chain unknown to this build: cannot judge, keep
            if (!(r["token"] is string token) || !chain.Tokens.ContainsKey(token)) return "drop";
            var familyWallets = walletsOf(chain.Family);
            if (familyWallets.Count == 0) return "keep"; // no wallet of this family configured any more: cannot judge, keep
            return await Backends.BackendFor(chain).Reverify(r, rpcFor(chain.Key), chain, familyWallets.ToArray());
        }

        /// <summary>Python <c>check_evm_receipt</c>: judge one stored EVM receipt, "drop" if the chain contradicts it, else "keep".</summary>
        public static async Task<string> CheckEvmReceipt(JsonObject r, RpcClient rpc, Chain chain, IReadOnlyList<string> wallets)
        {
            var txHash = Watcher.Str(Py.Get(r, "tx_hash"), "tx_hash").ToLowerInvariant(); // an EVM hash: ASCII hex
            var logIndex = Ints.IntFrom(Py.Get(r, "log_index"));
            var block = Ints.IntFrom(Py.Get(r, "block"));
            var units = Amounts.DecimalToUnits(Amounts.StoredAmount(Py.Get(r, "amount")));
            var token = Watcher.TokenOf(chain, r);
            var topics = new List<object?>
            {
                Evm.TransferTopic, null, wallets.Select(w => (object?)Evm.AddressTopic(w)).ToList(),
            };

            // Python's any() over a lazy generator: the logs after the first match are never parsed.
            bool Matches(List<object?> logs, Func<JsonObject, TransferLog?> parse)
            {
                foreach (var lg in logs)
                {
                    var log = lg as JsonObject ?? throw new TypeError("'" + Py.TypeName(lg) + "' object has no attribute 'get'");
                    var t = parse(log);
                    if (t != null && t.TxHash == txHash && t.LogIndex == logIndex && t.Value == units
                        && wallets.Contains(t.Recipient) && t.Token == token.Address)
                        return true;
                }
                return false;
            }

            List<object?> found;
            try
            {
                long head = await rpc.BlockNumber();
                if (block > head)
                {
                    // probably a lagging endpoint: keep, unless the block is impossibly far ahead
                    return block > (BigInteger)head + FutureToleranceBlocks ? "drop" : "keep";
                }
                long at = Ints.ToLong(block, "block"); // block <= head: only a block below a long overflows
                found = await rpc.GetLogs(token.LogSource, topics, at, at);
                if (Matches(found, chain.ParseTransfer)) return "keep";
                if (token.LogAddress.Length == 0) return "drop";
                // A receipt stored by a game that added this chain itself before it was built in (add_chain("arc", ...))
                // points at the token contract's own log. That log is still a genuine transfer, so it keeps the purchase.
                found = await rpc.GetLogs(token.Address, topics, at, at);
            }
            catch (Exception e) when (e is RpcUnavailable || e is RpcError)
            {
                return "keep";
            }
            return Matches(found, Evm.ParseTransferLog) ? "keep" : "drop";
        }
    }
}
