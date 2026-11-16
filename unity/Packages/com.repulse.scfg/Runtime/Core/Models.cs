// Twin of scfg_core/models.py (and of rpgmaker/src/core/models.ts).
// Value objects shared with the developer's script.
using System.Collections.Generic;

namespace Scfg.Core
{
    /// <summary>A paid unlock declared with add_item (a frozen dataclass: immutable here).</summary>
    public sealed class Item
    {
        public string ItemId { get; }
        public int Code { get; }
        public Dec Price { get; }
        public string Token { get; }
        public string Title { get; }
        public Dec MinPrice { get; }

        public Item(string itemId, int code, Dec price, string token, string title, Dec minPrice)
        {
            ItemId = itemId;
            Code = code;
            Price = price;
            Token = token;
            Title = title;
            MinPrice = minPrice;
        }
    }

    /// <summary>
    /// What pay(), callbacks and on_result receive. Status: "paid", "already_owned", "cancelled", "expired",
    /// "underpaid", "failed" or "pending".
    /// </summary>
    public sealed class PaymentResult
    {
        public string Status { get; set; }
        public string Kind { get; set; }
        public string? Item { get; set; }
        public Dec? Amount { get; set; }
        public string? Token { get; set; }
        public string? Chain { get; set; }
        public string? TxHash { get; set; }
        public string? Payer { get; set; }
        public string? SessionId { get; set; }
        public string? Error { get; set; }

        public PaymentResult(string status, string kind)
        {
            Status = status;
            Kind = kind;
        }
    }

    /// <summary>What restore() returns. Status: "restored", "nothing_found", "cancelled" or "failed".</summary>
    public sealed class RestoreResult
    {
        public string Status { get; set; }
        public List<JsonObject> Restored { get; set; } = new List<JsonObject>();
        public string? Error { get; set; }

        public RestoreResult(string status)
        {
            Status = status;
        }
    }
}
