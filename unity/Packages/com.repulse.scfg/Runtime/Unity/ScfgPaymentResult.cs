// What `await SCFG.Pay(...)`, a Pay callback and SCFG.PaymentResolved give the game: the core's PaymentResult as a
// typed object (Godot's payment_result.gd), plus the stored receipts and pending payments as typed records.
#nullable enable
using System.Collections.Generic;
using System.Globalization;
using Scfg.Core;

namespace Scfg
{
    /// <summary>How a payment ended (ScfgPaymentResult.Status).</summary>
    public enum ScfgPaymentStatus
    {
        /// <summary>The payment confirmed.</summary>
        Paid,
        /// <summary>An unlock the player already owns: no sheet was shown.</summary>
        AlreadyOwned,
        /// <summary>The player closed the sheet before paying.</summary>
        Cancelled,
        /// <summary>The payment window closed without a payment.</summary>
        Expired,
        /// <summary>A payment arrived but was less than the price.</summary>
        Underpaid,
        /// <summary>A misused call (Error is the ConfigError code), or payments could not start.</summary>
        Failed,
        /// <summary>The player chose to keep playing; the final result comes later through the callback or PaymentResolved.</summary>
        Pending,
    }

    /// <summary>A payment's outcome (see the file header). Every field but Status may be null.</summary>
    public sealed class ScfgPaymentResult
    {
        /// <summary>How the payment ended.</summary>
        public ScfgPaymentStatus Status { get; internal set; }
        /// <summary>Unlock or Tip; null for a call refused before it had a kind.</summary>
        public ScfgPayKind? Kind { get; internal set; }
        /// <summary>The item id of an unlock.</summary>
        public string? Item { get; internal set; }
        /// <summary>The amount actually paid, "4.99"; null when nothing was matched on chain.</summary>
        public string? Amount { get; internal set; }
        /// <summary>The token paid, "USDC" or "USDT".</summary>
        public string? Token { get; internal set; }
        /// <summary>The chain key the payment was made on (SCFG.ChainName gives its display name).</summary>
        public string? Chain { get; internal set; }
        /// <summary>The payment's transaction hash (EVM) or signature (Solana).</summary>
        public string? TxHash { get; internal set; }
        /// <summary>The address that paid.</summary>
        public string? Payer { get; internal set; }
        /// <summary>The payment session's id (the pending payment's SessionId while it is watched).</summary>
        public string? SessionId { get; internal set; }
        /// <summary>A ConfigError code for a misused Pay ("item.unknown"), or the core's reason for a failed payment.</summary>
        public string? Error { get; internal set; }

        internal ScfgPaymentResult(ScfgPaymentStatus status)
        {
            Status = status;
        }

        /// <summary>The core's status word as the enum (an unknown word is Failed).</summary>
        internal static ScfgPaymentStatus StatusOf(string? status) => status switch
        {
            "paid" => ScfgPaymentStatus.Paid,
            "already_owned" => ScfgPaymentStatus.AlreadyOwned,
            "cancelled" => ScfgPaymentStatus.Cancelled,
            "expired" => ScfgPaymentStatus.Expired,
            "underpaid" => ScfgPaymentStatus.Underpaid,
            "pending" => ScfgPaymentStatus.Pending,
            _ => ScfgPaymentStatus.Failed,
        };

        internal static ScfgPayKind? KindOf(string? kind) =>
            kind == "unlock" ? ScfgPayKind.Unlock : kind == "tip" ? ScfgPayKind.Tip : (ScfgPayKind?)null;

        internal static ScfgPaymentResult FromCore(PaymentResult r) => new ScfgPaymentResult(StatusOf(r.Status))
        {
            Kind = KindOf(r.Kind),
            Item = r.Item,
            Amount = r.Amount?.ToString(),
            Token = r.Token,
            Chain = r.Chain,
            TxHash = r.TxHash,
            Payer = r.Payer,
            SessionId = r.SessionId,
            Error = r.Error,
        };

        /// <summary>The core's status word ("already_owned").</summary>
        public string StatusText => Status switch
        {
            ScfgPaymentStatus.Paid => "paid",
            ScfgPaymentStatus.AlreadyOwned => "already_owned",
            ScfgPaymentStatus.Cancelled => "cancelled",
            ScfgPaymentStatus.Expired => "expired",
            ScfgPaymentStatus.Underpaid => "underpaid",
            ScfgPaymentStatus.Pending => "pending",
            _ => "failed",
        };

        public override string ToString() =>
            "ScfgPaymentResult(" + StatusText + ", item=" + (Item ?? "null") + ", amount=" + (Amount ?? "null") + ", token="
            + (Token ?? "null") + ", chain=" + (Chain ?? "null") + ", tx=" + (TxHash ?? "null") + ", error=" + (Error ?? "null") + ")";
    }

    /// <summary>A verified payment kept on this device (state.json's receipt record).</summary>
    public sealed class ScfgReceipt
    {
        /// <summary>The receipt key ("chain:tx_hash:log_index").</summary>
        public string Key { get; }
        /// <summary>The chain key (SCFG.ChainName gives its display name).</summary>
        public string? Chain { get; }
        /// <summary>The transaction hash (EVM) or signature (Solana).</summary>
        public string? TxHash { get; }
        /// <summary>The transfer's index in its transaction's logs (EVM; 0 on Solana).</summary>
        public long LogIndex { get; }
        /// <summary>The block (EVM) or slot (Solana) the payment is in.</summary>
        public long Block { get; }
        /// <summary>Unlock or Tip.</summary>
        public ScfgPayKind? Kind { get; }
        /// <summary>The item id of an unlock.</summary>
        public string? Item { get; }
        /// <summary>The amount paid, "4.99".</summary>
        public string? Amount { get; }
        /// <summary>"USDC" or "USDT".</summary>
        public string? Token { get; }
        /// <summary>The address that paid.</summary>
        public string? Payer { get; }
        /// <summary>The wallet that was paid (yours, or one of ScfgOptions.PreviousWallets).</summary>
        public string? Recipient { get; }
        /// <summary>"payment" or "restore".</summary>
        public string? Source { get; }

        internal ScfgReceipt(string key, JsonObject r)
        {
            Key = key;
            Chain = r.Get("chain") as string;
            TxHash = r.Get("tx_hash") as string;
            LogIndex = r.Get("log_index") is long li ? li : 0;
            Block = r.Get("block") is long b ? b : 0;
            Kind = ScfgPaymentResult.KindOf(r.Get("kind") as string);
            Item = r.Get("item") as string;
            Amount = r.Get("amount") as string;
            Token = r.Get("token") as string;
            Payer = r.Get("payer") as string;
            Recipient = r.Get("recipient") as string;
            Source = r.Get("source") as string;
        }

        internal static List<ScfgReceipt> ListOf(IEnumerable<JsonObject> records)
        {
            var output = new List<ScfgReceipt>();
            foreach (var r in records)
            {
                string key = r.Get("chain") is string chain && r.Get("tx_hash") is string tx && r.Get("log_index") is long li
                    ? Receipts.ReceiptKey(chain, tx, li)
                    : "";
                output.Add(new ScfgReceipt(key, r));
            }
            return output;
        }
    }

    /// <summary>A payment still being watched for (state.json's pending record).</summary>
    public sealed class ScfgPendingPayment
    {
        /// <summary>The payment session's id (ScfgPaymentResult.SessionId of its result).</summary>
        public string SessionId { get; }
        /// <summary>Unlock or Tip.</summary>
        public ScfgPayKind? Kind { get; }
        /// <summary>The item id of an unlock.</summary>
        public string? Item { get; }
        /// <summary>What the sheet showed as the payment's title.</summary>
        public string? Title { get; }
        /// <summary>The price asked, "4.99".</summary>
        public string? Price { get; }
        /// <summary>"USDC" or "USDT".</summary>
        public string? Token { get; }
        /// <summary>The chain key the player chose, or null before they chose one.</summary>
        public string? Chain { get; }
        /// <summary>"awaiting" or "confirming".</summary>
        public string? State { get; }
        /// <summary>The transaction the player reported, if any.</summary>
        public string? HintTx { get; }
        /// <summary>Unix seconds.</summary>
        public double CreatedAt { get; }

        internal ScfgPendingPayment(string sid, JsonObject r)
        {
            SessionId = sid;
            Kind = ScfgPaymentResult.KindOf(r.Get("kind") as string);
            Item = r.Get("item") as string;
            Title = r.Get("title") as string;
            Price = r.Get("price") as string;
            Token = r.Get("token") as string;
            Chain = r.Get("chain") as string;
            State = r.Get("state") as string;
            HintTx = r.Get("hint_tx") as string;
            CreatedAt = r.Get("created_at") is long l ? l : r.Get("created_at") is double d ? d : 0;
        }
    }
}
