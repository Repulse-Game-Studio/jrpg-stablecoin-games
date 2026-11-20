// Twin of scfg_core/restore.py (and of rpgmaker/src/core/restore.ts). RestoreError is in Errors.cs.
// Restoring purchases from a transaction hash: the chain is the ledger.
//
// The player pastes a tx hash. We find the chain it lives on, pick out the USDC/USDT transfers to the developer, and
// work out what each one bought from the code encoded in its amount. Amounts that carry no code (e.g. an exchange sent
// a round 5.00) are ambiguous, so the player chooses which item it paid for, or keeps it as a tip.
//
// Errors mirror Python: only RpcUnavailable and RpcError are swallowed by Lookup; anything else (a malformed log)
// fails the whole lookup, except a transfer of 10**32 units or more, which Amounts.Decode cannot price: that one is
// skipped and reported to on_error (ClassifyOrSkip).
#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Threading.Tasks;

namespace Scfg.Core
{
    /// <summary>Python's <c>RestoreCandidate</c> dataclass: one transfer to the developer a restore found. Equal by value.</summary>
    public sealed class RestoreCandidate : IEquatable<RestoreCandidate>
    {
        public string Chain { get; set; }
        /// <summary>The token's name on the chain ("USDC", "USDT").</summary>
        public string Token { get; set; }
        /// <summary>A <see cref="Scfg.Core.Transfer"/> (Solana) or a <see cref="TransferLog"/> (EVM).</summary>
        public object Transfer { get; set; }
        /// <summary>"unlock", "tip", or null when the player must choose.</summary>
        public string? Kind { get; set; }
        public string? Item { get; set; }
        /// <summary>Item ids (and "tip") offered when <see cref="Kind"/> is null; empty otherwise. A copy (Python's tuple).</summary>
        public IReadOnlyList<string> Choices { get; set; }
        public bool AlreadyRestored { get; set; }

        public RestoreCandidate(string chain, string token, object transfer, string? kind, string? item,
                                IEnumerable<string> choices, bool alreadyRestored)
        {
            if (!(transfer is Transfer) && !(transfer is TransferLog))
                throw new TypeError("a restore candidate's transfer is a Transfer or a TransferLog, not " + Py.TypeName(transfer));
            Chain = chain;
            Token = token;
            Transfer = transfer;
            Kind = kind;
            Item = item;
            Choices = choices.ToArray();
            AlreadyRestored = alreadyRestored;
        }

        public bool Equals(RestoreCandidate? o) =>
            o != null && Chain == o.Chain && Token == o.Token && Equals(Transfer, o.Transfer) && Kind == o.Kind
            && Item == o.Item && Choices.SequenceEqual(o.Choices) && AlreadyRestored == o.AlreadyRestored;

        public override bool Equals(object? obj) => Equals(obj as RestoreCandidate);

        /// <summary>From the fields Equals compares (Python's mutable dataclass is unhashable: don't key on one you change).</summary>
        public override int GetHashCode() => (Chain, Token, Transfer, Kind, Item, Choices.Count, AlreadyRestored).GetHashCode();
    }

    public static class Restore
    {
        /// <summary>The choice that keeps an ambiguous transfer as a tip.</summary>
        public const string TipChoice = "tip";

        /// <summary>
        /// Python <c>classify</c>: what a transfer of <paramref name="value"/> base units paid for, as (kind, item,
        /// choices). InvalidOperation from 10**32 units (see <see cref="Amounts.Decode"/>).
        /// </summary>
        public static (string? Kind, string? Item, IReadOnlyList<string> Choices) Classify(ConfigData config, BigInteger value)
        {
            var (_, code, nonce) = Amounts.Decode(value);
            var amount = Amounts.UnitsToDecimal(value);
            var affordable = config.Items.Values.Where(i => i.MinPrice <= amount).Select(i => i.ItemId).ToList();
            if (code == 0 && nonce == 0)
            {
                // A whole-cent amount carries no code at all: could be an exchange-rounded purchase or a plain tip. Let
                // the player say which.
                affordable.Add(TipChoice);
                return (null, null, affordable);
            }
            if (code == 0) return ("tip", null, Array.Empty<string>());
            var item = config.ItemByCode(code);
            if (item != null && amount >= item.MinPrice) return ("unlock", item.ItemId, Array.Empty<string>());
            affordable.Add(TipChoice);
            return (null, null, affordable);
        }

        /// <summary>
        /// Python <c>classify_or_skip</c>: <see cref="Classify"/>, or null for a value Decode cannot price (10**32 units
        /// or more). Such a transfer is skipped and reported to <paramref name="onError"/> (default
        /// <see cref="Errors.PrintError"/>); the rest of the transaction is still offered.
        /// </summary>
        public static (string? Kind, string? Item, IReadOnlyList<string> Choices)? ClassifyOrSkip(ConfigData config, BigInteger value,
                                                                                                 Action<Exception>? onError)
        {
            try
            {
                return Classify(config, value);
            }
            catch (InvalidOperation e)
            {
                (onError ?? Errors.PrintError)(e);
                return null;
            }
        }

        /// <summary>A candidate's already_restored: its receipt key is in the store.</summary>
        internal static bool Restored(JsonObject receipts, string chain, string txHash, long logIndex) =>
            receipts.ContainsKey(Receipts.ReceiptKey(chain, txHash, logIndex));

        /// <summary>
        /// Python <c>lookup_evm</c>: candidates for a normalized tx hash on one EVM chain; null if the chain lacks it.
        /// RestoreError("tx_reverted" | "tx_not_a_payment").
        /// </summary>
        public static async Task<List<RestoreCandidate>?> LookupEvm(string txHash, ConfigData config, Chain chain, RpcClient rpc,
                                                                    JsonObject receipts, Action<Exception>? onError = null)
        {
            var receipt = await rpc.GetTransactionReceipt(txHash);
            if (receipt == null) return null;
            if (!(receipt is JsonObject rec))
                throw new TypeError("'" + Py.TypeName(receipt) + "' object has no attribute 'get'");
            if (rec.Get("status") is string status && status == "0x0") throw new RestoreError("tx_reverted");
            var byAddress = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var kv in chain.Tokens) byAddress[kv.Value.Address] = kv.Key; // a later name wins, as in a dict
            var wallets = config.AcceptedWallets;
            var output = new List<RestoreCandidate>();
            var logs = rec.Get("logs");
            foreach (var log in Py.Truthy(logs) ? Coio.Iterate(logs) : new List<object?>())
            {
                var t = chain.ParseTransfer(log as JsonObject ?? throw new TypeError("'" + Py.TypeName(log) + "' object has no attribute 'get'"));
                if (t == null || !byAddress.ContainsKey(t.Token) || t.Value <= 0 || !wallets.Contains(t.Recipient)) continue;
                var classified = ClassifyOrSkip(config, t.Value, onError);
                if (classified == null) continue;
                var (kind, item, choices) = classified.Value;
                output.Add(new RestoreCandidate(chain.Key, byAddress[t.Token], t, kind, item, choices,
                                                Restored(receipts, chain.Key, t.TxHash, t.LogIndex)));
            }
            if (output.Count == 0) throw new RestoreError("tx_not_a_payment");
            return output;
        }

        /// <summary>
        /// Python <c>lookup</c>: the candidates of the first chain, in <paramref name="chains"/>' order, that knows
        /// <paramref name="txHash"/>. A transfer that cannot be classified is skipped and reported to
        /// <paramref name="onError"/> (see <see cref="ClassifyOrSkip"/>). RestoreError("invalid_hash") when no family
        /// reads the id, else "network_unavailable" when a chain could not be asked, else "tx_not_found".
        /// </summary>
        public static async Task<List<RestoreCandidate>> Lookup(object? txHash, ConfigData config,
                                                                IEnumerable<KeyValuePair<string, Chain>> chains,
                                                                Func<string, object> rpcFor, JsonObject receipts,
                                                                Action<Exception>? onError = null)
        {
            bool networkTrouble = false;
            bool understood = false;
            foreach (var kv in chains)
            {
                var backend = Backends.BackendFor(kv.Value);
                string tx;
                try
                {
                    tx = backend.NormalizeTx(txHash);
                }
                catch (ValueError)
                {
                    continue;
                }
                understood = true;
                List<RestoreCandidate>? output;
                try
                {
                    output = await backend.Lookup(tx, config, kv.Value, rpcFor(kv.Key), receipts, onError);
                }
                catch (Exception e) when (e is RpcUnavailable || e is RpcError)
                {
                    networkTrouble = true;
                    continue;
                }
                if (output != null) return output;
            }
            if (!understood) throw new RestoreError("invalid_hash");
            throw new RestoreError(networkTrouble ? "network_unavailable" : "tx_not_found");
        }

        /// <summary>
        /// Python <c>apply</c>: store receipts for the candidates and return the receipts added. <paramref name="choices"/>
        /// maps a candidate's index to an item id or "tip" for the candidates whose kind is null (one without a choice
        /// is skipped; a choice it does not offer is a ValueError).
        /// </summary>
        public static List<JsonObject> Apply(JsonObject receipts, IReadOnlyList<RestoreCandidate> candidates,
                                             IReadOnlyDictionary<int, object?>? choices = null)
        {
            var added = new List<JsonObject>();
            for (int i = 0; i < candidates.Count; i++)
            {
                var c = candidates[i];
                var kind = c.Kind;
                var item = c.Item;
                if (kind == null)
                {
                    object? choice = null;
                    if (choices != null) choices.TryGetValue(i, out choice);
                    if (choice == null) continue;
                    if (!(choice is string picked) || !c.Choices.Contains(picked))
                        throw new ValueError(Py.Repr(choice) + " is not an allowed choice");
                    if (picked == TipChoice)
                    {
                        kind = "tip";
                        item = null;
                    }
                    else
                    {
                        kind = "unlock";
                        item = picked;
                    }
                }
                var r = c.Transfer is TransferLog log
                    ? Receipts.MakeReceipt(c.Chain, log, kind, item, c.Token, "restore")
                    : Receipts.MakeReceipt(c.Chain, (Transfer)c.Transfer, kind, item, c.Token, "restore");
                if (Receipts.AddReceipt(receipts, r)) added.Add(r);
            }
            return added;
        }
    }
}
