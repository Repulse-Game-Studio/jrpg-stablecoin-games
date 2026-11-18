// Twin of scfg_core/receipts.py (and of rpgmaker/src/core/receipts.ts).
// Receipts: verified on-chain transfers to the developer, stored in `persistent`.
//
// There is deliberately no "owned" flag anywhere. Ownership is always recomputed from receipts, and receipts are
// re-checked against the chain at every launch. A store is a JsonObject (Python's dict), a receipt a JsonObject
// shaped as the frozen receipt schema (conformance/schema/state.json).
#nullable enable
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;

namespace Scfg.Core
{
    /// <summary>Python's <c>(owned, tips)</c> tuple from <c>compute_ownership</c>.</summary>
    public sealed class Ownership
    {
        /// <summary>Owned item ids, sorted by code point.</summary>
        public List<string> Owned { get; }
        /// <summary>The Decimal sum of the tip amounts: the widest fraction digits, never fixed to a scale.</summary>
        public Dec TipTotal { get; }

        public Ownership(List<string> owned, Dec tipTotal)
        {
            Owned = owned;
            TipTotal = tipTotal;
        }
    }

    public static class Receipts
    {
        public static string ReceiptKey(string chain, string txHash, long logIndex)
        {
            // EVM hashes are hex (case-insensitive); Solana signatures are base58 (case matters).
            var tx = txHash.StartsWith("0x", StringComparison.Ordinal) ? txHash.ToLowerInvariant() : txHash;
            return chain + ":" + tx + ":" + logIndex.ToString(CultureInfo.InvariantCulture);
        }

        public static JsonObject MakeReceipt(string chain, Transfer transfer, string kind, string? item, string token, string source) =>
            Make(chain, transfer.TxHash, transfer.LogIndex, transfer.BlockNumber, transfer.Value, transfer.Sender,
                 transfer.Recipient, kind, item, token, source);

        public static JsonObject MakeReceipt(string chain, TransferLog transfer, string kind, string? item, string token, string source) =>
            Make(chain, transfer.TxHash, transfer.LogIndex, transfer.BlockNumber, transfer.Value, transfer.Sender,
                 transfer.Recipient, kind, item, token, source);

        private static JsonObject Make(string chain, string txHash, long logIndex, long block, BigInteger value,
                                       string? sender, string recipient, string kind, string? item, string token, string source)
        {
            if (kind != "unlock" && kind != "tip") throw new ValueError("invalid kind " + Py.Repr(kind));
            if (source != "payment" && source != "restore") throw new ValueError("invalid source " + Py.Repr(source));
            return new JsonObject
            {
                { "chain", chain }, { "tx_hash", txHash }, { "log_index", logIndex },
                { "block", block }, { "kind", kind }, { "item", item },
                { "amount", Amounts.UnitsToDecimal(value).ToString() }, { "token", token }, // culture-ok: Dec.ToString is invariant
                { "payer", sender }, { "recipient", recipient }, { "source", source },
            };
        }

        /// <summary>Stores the receipt unless its key is already there. Returns whether it was added.</summary>
        public static bool AddReceipt(JsonObject store, JsonObject receipt)
        {
            var key = ReceiptKey(
                Py.Get(receipt, "chain") as string ?? throw new TypeError("receipt chain must be a string"),
                Py.Get(receipt, "tx_hash") as string ?? throw new TypeError("receipt tx_hash must be a string"),
                Ints.ToLong(Py.Int(Py.Get(receipt, "log_index")), "log_index"));
            if (store.ContainsKey(key)) return false;
            store[key] = receipt;
            return true;
        }

        public static void RemoveReceipt(JsonObject store, string key) => store.Remove(key);

        /// <summary>
        /// (owned item ids, total tipped). Malformed entries are skipped, never fatal: a missing field, an amount
        /// that is not a number within the stored-amount range (<see cref="Amounts.StoredAmount"/>, so the tip total
        /// stays finite), an unhashable item.
        /// </summary>
        public static Ownership ComputeOwnership(JsonObject store, IReadOnlyDictionary<string, Item> items)
        {
            var owned = new HashSet<string>(StringComparer.Ordinal);
            var tips = Dec.FromInt(0);
            foreach (var kv in store)
            {
                if (!(kv.Value is JsonObject r)) continue; // r["kind"] on a non-dict is a TypeError: skipped
                if (!r.TryGetValue("kind", out var kind) || !r.TryGetValue("amount", out var rawAmount)) continue;
                Dec amount;
                try
                {
                    amount = Amounts.StoredAmount(rawAmount);
                }
                catch (Exception e) when (e is ValueError || e is TypeError)
                {
                    continue;
                }
                if (kind is string k && k == "unlock")
                {
                    var key = r.Get("item");
                    if (key is JsonObject || key is List<object?>) continue; // items.get(<unhashable>) is a TypeError
                    if (key is string s && items.TryGetValue(s, out var item) && amount >= item.MinPrice)
                        owned.Add(item.ItemId);
                }
                else if (kind is string t && t == "tip")
                {
                    tips = tips + amount;
                }
            }
            var list = new List<string>(owned);
            list.Sort(Py.CompareCodePoints);
            return new Ownership(list, tips);
        }
    }
}
