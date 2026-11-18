// What SCFG.Pay takes (Godot's pay(kind, {"item", "amount", "token"})).
#nullable enable

namespace Scfg
{
    /// <summary>What a payment is for.</summary>
    public enum ScfgPayKind
    {
        /// <summary>A declared item (SCFG.AddItem): Item is required, Amount and Token default to the item's.</summary>
        Unlock,
        /// <summary>A tip: Amount (the suggested amount, which the player may change) and Token are required.</summary>
        Tip,
    }

    /// <summary>The payment SCFG.Pay shows. Amounts are decimal strings such as "4.99", never floats.</summary>
    public sealed class ScfgPayRequest
    {
        public string? Item { get; set; }
        public string? Amount { get; set; }
        public string? Token { get; set; }
    }

    internal static class ScfgPayKinds
    {
        /// <summary>The core's kind ("unlock", "tip"), or null for a value that is not a member.</summary>
        public static string? Core(ScfgPayKind kind) =>
            kind == ScfgPayKind.Unlock ? "unlock" : kind == ScfgPayKind.Tip ? "tip" : null;
    }
}
