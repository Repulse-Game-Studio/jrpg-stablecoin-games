// Twin of scfg_core/watcher.py (and of rpgmaker/src/core/watcher.ts).
// The payment watcher: advances one pending session per call.
//
//     awaiting --(hinted tx verified | exact amount found by scan)--> confirming
//     confirming --(N confirmations, block hash unchanged)--> paid (receipt stored)
//     confirming --(block hash changed: reorg)--> awaiting
//     awaiting --(hinted tx pays less than min_price)--> underpaid
//     awaiting --(24 h, nothing found)--> closed
//
// Advance is logic over plain JsonObjects plus an RpcClient. State is only mutated after RPC calls succeed, so a
// network outage never corrupts a session. Record fields are read the way Python indexes them (Py.Get): a
// malformed record throws, and the Engine contains it per session.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using System.Threading.Tasks;

namespace Scfg.Core
{
    /// <summary>Python's <c>StepOutcome</c> dataclass.</summary>
    public sealed class StepOutcome
    {
        public string SessionId { get; set; }
        /// <summary>missing, waiting, confirming, reorged, paid, underpaid, closed or expired.</summary>
        public string Status { get; set; }
        public long Confirmations { get; set; }
        public long Required { get; set; }
        public PaymentResult? Result { get; set; }
        /// <summary>
        /// network_unavailable, tx_reverted, tx_not_a_payment, tx_not_landed (Solana), or internal_error (Engine.Tick:
        /// the session's step crashed).
        /// </summary>
        public string? Error { get; set; }
        /// <summary>
        /// The hint a tx_reverted / tx_not_a_payment / tx_not_landed verdict is about (Solana), so the screen can tell
        /// it from a newer hint reported meanwhile.
        /// </summary>
        public string? HintTx { get; set; }

        public StepOutcome(string sessionId, string status, long confirmations = 0, long required = 0,
                           PaymentResult? result = null, string? error = null, string? hintTx = null)
        {
            SessionId = sessionId;
            Status = status;
            Confirmations = confirmations;
            Required = required;
            Result = result;
            Error = error;
            HintTx = hintTx;
        }
    }

    public static class Watcher
    {
        /// <summary>Upper bound on blocks scanned per Advance.</summary>
        public const long ScanStepBlocks = 10_000;
        /// <summary>eth_getLogs calls per Advance, to be gentle with public RPCs.</summary>
        public const long MaxScanCalls = 5;

        private static readonly Dec MinTipUnit = Dec.Parse("0.000001")!;

        // ---- Python's view of a record's fields -------------------------------------------------------------

        /// <summary>A str field or None, as the dataclasses that carry it hold it; anything else is a TypeError.</summary>
        internal static string? StrOrNone(object? v, string what) =>
            v == null ? null : v as string ?? throw new TypeError(what + " must be a str, not " + Py.TypeName(v));

        internal static string Str(object? v, string what) =>
            v as string ?? throw new TypeError(what + " must be a str, not " + Py.TypeName(v));

        /// <summary>
        /// A field used where Python needs an int (hex(), range bounds, a dataclass int): an int, or a bool (an int in
        /// Python). A float or anything else is a TypeError; an int beyond 64 bits a ValueError.
        /// </summary>
        internal static long Int(object? v, string what)
        {
            switch (v)
            {
                case long l: return l;
                case int i: return i;
                case bool b: return b ? 1 : 0;
                case BigInteger _: throw new ValueError(what + " does not fit 64 bits");
                default: throw new TypeError(what + " must be an int, not " + Py.TypeName(v));
            }
        }

        /// <summary>A stored count of base units (<c>match.value</c>): an int.</summary>
        internal static BigInteger Units(object? v, string what)
        {
            switch (v)
            {
                case long l: return l;
                case int i: return i;
                case bool b: return b ? 1 : 0;
                case BigInteger bi: return bi;
                default: throw new TypeError(what + " must be an int, not " + Py.TypeName(v));
            }
        }

        /// <summary>An int as Python keeps it in a JSON-like dict: a long when it fits, else a BigInteger.</summary>
        internal static object IntValue(BigInteger n) => n >= long.MinValue && n <= long.MaxValue ? (object)(long)n : n;

        /// <summary>Python <c>value == x</c> for an int and a JSON value: equal ints, an equal integral float, a bool as 0/1.</summary>
        private static bool IntEquals(BigInteger value, object? x)
        {
            switch (x)
            {
                case long l: return value == l;
                case int i: return value == i;
                case bool b: return value == (b ? 1 : 0);
                case BigInteger bi: return value == bi;
                case double d: return !double.IsNaN(d) && !double.IsInfinity(d) && Math.Floor(d) == d && value == new BigInteger(d);
                default: return false;
            }
        }

        /// <summary>Python <c>min(a, b)</c> of two numbers: <paramref name="b"/> only when it is smaller.</summary>
        private static object? Min(JsonObject rec, string key, object? b, double bValue) =>
            bValue < Sessions.NumField(rec, key) ? b : rec[key];

        /// <summary><c>chain.tokens[rec["token"]]</c>: a KeyError for an unknown token, a TypeError for an unhashable one.</summary>
        internal static Token TokenOf(Chain chain, JsonObject rec)
        {
            var key = Py.Get(rec, "token");
            if (key is JsonObject || key is List<object?>) throw new TypeError("unhashable type: '" + Py.TypeName(key) + "'");
            if (key is string s && chain.Tokens.TryGetValue(s, out var token)) return token;
            throw new KeyError(key as string ?? Py.Repr(key));
        }

        /// <summary>Python <c>log.get(...)</c> on a log: a dict, else AttributeError (a TypeError here).</summary>
        private static JsonObject Log(object? lg) =>
            lg as JsonObject ?? throw new TypeError("'" + Py.TypeName(lg) + "' object has no attribute 'get'");

        // ---- the watcher ------------------------------------------------------------------------------------

        public static Dec MinAmount(JsonObject rec, IReadOnlyDictionary<string, Item> items)
        {
            if (Py.Get(rec, "kind") is string kind && kind == "unlock")
            {
                var key = Py.Get(rec, "item");
                if (key is JsonObject || key is List<object?>) throw new TypeError("unhashable type: '" + Py.TypeName(key) + "'");
                if (key is string s && items.TryGetValue(s, out var item)) return item.MinPrice;
                return Sessions.SessionPrice(rec);
            }
            return MinTipUnit;
        }

        private static JsonObject MatchDict(TransferLog t) => new JsonObject
        {
            { "tx_hash", t.TxHash }, { "log_index", t.LogIndex }, { "block", t.BlockNumber },
            { "block_hash", t.BlockHash }, { "value", IntValue(t.Value) }, { "payer", t.Sender },
            { "recipient", t.Recipient },
        };

        public static PaymentResult ResultFromMatch(JsonObject rec, string status, string chainKey, object? m) =>
            new PaymentResult(status, Str(Py.Get(rec, "kind"), "kind"))
            {
                Item = StrOrNone(Py.Get(rec, "item"), "item"),
                Amount = Amounts.UnitsToDecimal(Units(Py.Get(m, "value"), "value")),
                Token = StrOrNone(Py.Get(rec, "token"), "token"),
                Chain = chainKey,
                TxHash = StrOrNone(Py.Get(m, "tx_hash"), "tx_hash"),
                Payer = StrOrNone(Py.Get(m, "payer"), "payer"),
                SessionId = StrOrNone(Py.Get(rec, "id"), "id"),
            };

        private static bool Qualifies(TransferLog? t, JsonObject rec, Chain chain, string wallet, JsonObject receipts) =>
            t != null
            && t.Token == TokenOf(chain, rec).Address
            && t.Recipient == wallet
            && t.BlockNumber >= Sessions.NumField(rec, "start_block")
            && !receipts.ContainsKey(Receipts.ReceiptKey(chain.Key, t.TxHash, t.LogIndex));

        /// <summary>
        /// Check the tx hash reported by the browser page or pasted by the player. Returns "matched", "underpaid", or
        /// null (keep waiting / hint discarded).
        /// </summary>
        private static async Task<string?> VerifyHint(JsonObject rec, RpcClient rpc, Chain chain, string wallet,
                                                      IReadOnlyDictionary<string, Item> items, JsonObject receipts,
                                                      long now, StepOutcome output)
        {
            var receipt = await rpc.GetTransactionReceipt(Py.Get(rec, "hint_tx"));
            if (receipt == null) // not mined yet (or unknown): give it up to 30 minutes
            {
                if (now - Sessions.NumField(rec, "hint_at") >= Sessions.HintTimeoutS) Sessions.ClearHint(rec);
                return null;
            }
            var body = Log(receipt); // receipt.get(...): a dict
            if (body.Get("status") is string status && status == "0x0")
            {
                output.HintTx = StrOrNone(rec["hint_tx"], "hint_tx"); // the verdict is about this hint
                Sessions.ClearHint(rec);
                output.Error = "tx_reverted";
                return null;
            }
            var logs = body.Get("logs");
            var candidates = new List<TransferLog>();
            if (Py.Truthy(logs))
            {
                foreach (var lg in Coio.Iterate(logs))
                {
                    var t = chain.ParseTransfer(Log(lg));
                    if (Qualifies(t, rec, chain, wallet, receipts) && t!.Value > 0) candidates.Add(t);
                }
            }
            if (candidates.Count == 0)
            {
                output.HintTx = StrOrNone(rec["hint_tx"], "hint_tx"); // the verdict is about this hint
                Sessions.ClearHint(rec);
                output.Error = "tx_not_a_payment";
                return null;
            }
            var best = candidates[0]; // Python max(): the first of the largest values
            foreach (var c in candidates)
                if (c.Value > best.Value) best = c;
            rec["match"] = MatchDict(best);
            if (Amounts.UnitsToDecimal(best.Value) < MinAmount(rec, items)) return "underpaid";
            return "matched";
        }

        /// <summary>Look for a transfer of exactly the session's encoded amount since the cursor.</summary>
        private static async Task<bool> Scan(JsonObject rec, RpcClient rpc, Chain chain, string wallet, JsonObject receipts, long head)
        {
            if (Sessions.NumField(rec, "cursor") > head) return false; // this endpoint is behind the one that set start_block
            long cursor = Int(rec["cursor"], "cursor");
            long span = rpc.LogRange;
            long step = span != 0 ? Math.Min(ScanStepBlocks, span * MaxScanCalls) : ScanStepBlocks;
            long to = Math.Min(head, cursor + step - 1);
            var source = TokenOf(chain, rec).LogSource;
            var logs = await rpc.GetLogs(source, new List<object?> { Evm.TransferTopic, null, Evm.AddressTopic(wallet) }, cursor, to);
            var qualifying = new List<TransferLog>();
            foreach (var lg in logs)
            {
                var t = chain.ParseTransfer(Log(lg));
                // Python reads rec["units"] only for a log that qualifies
                if (Qualifies(t, rec, chain, wallet, receipts) && IntEquals(t!.Value, Py.Get(rec, "units"))) qualifying.Add(t);
            }
            var found = qualifying.OrderBy(t => t.BlockNumber).ThenBy(t => t.LogIndex).ToList(); // stable, as sorted()
            rec["cursor"] = to + 1;
            if (found.Count > 0)
            {
                rec["match"] = MatchDict(found[0]);
                return true;
            }
            return false;
        }

        public static async Task<StepOutcome> Advance(JsonObject pending, JsonObject receipts, string sid, RpcClient rpc,
                                                      Chain chain, string wallet, IReadOnlyDictionary<string, Item> items,
                                                      long now)
        {
            var found = pending.Get(sid);
            var output = new StepOutcome(sid, "missing", required: chain.Confirmations);
            if (found == null) return output;
            try
            {
                long? head = null;
                var state = Py.Get(found, "state"); // a record that is not a dict is a TypeError here, as in Python
                var rec = (JsonObject)found;
                if (state is string s && s == "awaiting")
                {
                    output.Status = "waiting";
                    if (Sessions.IsPastHorizon(rec, now))
                    {
                        Sessions.RemoveSession(pending, sid);
                        output.Status = "closed";
                        return output;
                    }
                    string? outcome = null;
                    if (Py.Truthy(Py.Get(rec, "hint_tx")))
                        outcome = await VerifyHint(rec, rpc, chain, wallet, items, receipts, now, output);
                    if (outcome == null && !Py.Truthy(Py.Get(rec, "hint_tx")))
                    {
                        head = await rpc.BlockNumber();
                        outcome = await Scan(rec, rpc, chain, wallet, receipts, head.Value) ? "matched" : null;
                    }
                    if (outcome == "underpaid")
                    {
                        Sessions.RemoveSession(pending, sid);
                        output.Status = "underpaid";
                        output.Result = ResultFromMatch(rec, "underpaid", chain.Key, rec["match"]);
                        return output;
                    }
                    if (outcome != "matched") return output;
                    rec["state"] = "confirming";
                }

                output.Status = "confirming";
                var m = Py.Get(rec, "match");
                if (head == null) head = await rpc.BlockNumber();
                var rawBlock = Py.Get(m, "block");
                double blockValue = Sessions.NumField(m, "block");
                if (head.Value < blockValue) return output; // lagging endpoint, not a reorg
                long block = Int(rawBlock, "block");
                var current = await rpc.GetBlockHash(block);
                if (current == null || !(Py.Get(m, "block_hash") is string blockHash && current.ToLowerInvariant() == blockHash))
                {
                    rec["state"] = "awaiting";
                    rec["match"] = null;
                    rec["cursor"] = Min(rec, "cursor", rawBlock, blockValue);
                    output.Status = "reorged";
                    return output;
                }
                output.Confirmations = head.Value - block + 1;
                if (output.Confirmations < chain.Confirmations) return output;
                var transfer = new TransferLog(
                    TokenOf(chain, rec).Address, Str(Py.Get(m, "payer"), "payer"), Str(Py.Get(m, "recipient"), "recipient"),
                    Units(Py.Get(m, "value"), "value"), Str(Py.Get(m, "tx_hash"), "tx_hash"),
                    Int(Py.Get(m, "log_index"), "log_index"), block, blockHash);
                Receipts.AddReceipt(receipts, Receipts.MakeReceipt(
                    chain.Key, transfer, Str(Py.Get(rec, "kind"), "kind"), StrOrNone(Py.Get(rec, "item"), "item"),
                    Str(Py.Get(rec, "token"), "token"), "payment"));
                Sessions.RemoveSession(pending, sid);
                output.Status = "paid";
                output.Result = ResultFromMatch(rec, "paid", chain.Key, m);
                return output;
            }
            catch (Exception e) when (e is RpcUnavailable || e is RpcError)
            {
                output.Error = "network_unavailable";
                return output;
            }
        }
    }
}
