// Twin of scfg_core/payments.py (and of rpgmaker/src/core/payments.ts).
// Validation of a pay() request, independent of chain and network. Used synchronously by scfg.pay() (so programmer
// errors raise immediately in the developer's script) and again by Engine.StartSession.
#nullable enable
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace Scfg.Core
{
    /// <summary>Python's frozen <c>PaymentSpec</c> dataclass.</summary>
    public sealed class PaymentSpec
    {
        /// <summary>"unlock" or "tip".</summary>
        public string Kind { get; }
        public string? Item { get; }
        public Dec Price { get; }
        public string Token { get; }
        /// <summary>0 for tips.</summary>
        public int Code { get; }
        public string Title { get; }
        /// <summary>Enabled chains that carry the token, in display order.</summary>
        public IReadOnlyList<string> Chains { get; }

        public PaymentSpec(string kind, string? item, Dec price, string token, int code, string title,
                           IReadOnlyList<string> chains)
        {
            Kind = kind;
            Item = item;
            Price = price;
            Token = token;
            Code = code;
            Title = title;
            Chains = chains;
        }
    }

    public static class Payments
    {
        public static readonly Dec MinTip = Dec.Parse("0.50")!;
        public static readonly IReadOnlyList<Dec> TipPresets =
            new[] { Dec.Parse("1.00")!, Dec.Parse("3.00")!, Dec.Parse("5.00")!, Dec.Parse("10.00")! };

        private static readonly string[] PaymentKeys = { "item", "amount", "token", "title" };

        /// <summary>
        /// prepare_payment(config, kind, *, item=None, amount=None, token=None, title=None): <paramref name="kwargs"/>
        /// holds the keyword arguments, <paramref name="kind"/> is "unlock" or "tip" (anything else is a ConfigError).
        /// </summary>
        public static PaymentSpec PreparePayment(ConfigData config, object? kind, JsonObject kwargs)
        {
            foreach (var k in kwargs.Keys)
                if (!PaymentKeys.Contains(k))
                    throw new TypeError("prepare_payment() got an unexpected keyword argument " + Py.Repr(k));
            var item = kwargs.Get("item");
            var amount = kwargs.Get("amount");
            var token = kwargs.Get("token");
            var title = kwargs.Get("title");

            Dec price;
            int code;
            string? itemId = null;
            if (kind is string k1 && k1 == "unlock")
            {
                if (item is JsonObject) throw new TypeError("unhashable type: 'dict'");
                if (item is IList) throw new TypeError("unhashable type: 'list'");
                if (!(item is string id) || !config.Items.TryGetValue(id, out var it))
                    throw new ConfigError("unknown item " + Py.Repr(item) + "; declare it with scfg.add_item()", "item.unknown");
                itemId = id;
                price = amount == null ? it.Price : Amounts.ParseAmount(amount);
                if (price < it.MinPrice)
                    throw new ConfigError("amount " + price + " is below the item's min_price " + it.MinPrice,
                                          "payment.below_min_price");
                if (!Py.Truthy(token)) token = it.Token;
                code = it.Code;
                if (!Py.Truthy(title)) title = it.Title;
            }
            else if (kind is string k2 && k2 == "tip")
            {
                if (amount == null || token == null)
                    throw new ConfigError("tips need an amount and a token", "tip.needs_amount_and_token");
                price = Amounts.ParseAmount(amount);
                if (price < MinTip) throw new ConfigError("tips must be at least " + MinTip, "tip.below_minimum");
                code = 0;
                itemId = item as string; // a tip carries whatever item it was given
                if (!Py.Truthy(title)) title = "Tip";
            }
            else
            {
                throw new ConfigError("kind must be 'unlock' or 'tip', not " + Py.Repr(kind), "payment.bad_kind");
            }
            if (!(token is string tokenText) || !config.Tokens.Contains(tokenText))
                throw new ConfigError("token " + Py.Repr(token) + " is not enabled", "token.not_enabled");
            var chains = Config.ChainsForToken(config, tokenText);
            if (chains.Count == 0)
                throw new ConfigError(tokenText + " is not available on any enabled chain", "token.unavailable");
            // Python lets any title and item through; the C# spec holds text.
            if (item != null && itemId == null) throw new TypeError("item must be a string, not " + Py.TypeName(item));
            if (!(title is string titleText)) throw new TypeError("title must be a string, not " + Py.TypeName(title));
            return new PaymentSpec((string)kind!, itemId, price, tokenText, code, titleText, chains);
        }
    }
}
