// The typed options of SCFG.Configure, SCFG.AddItem and SCFG.AddChain (Ren'Py's keyword arguments, Godot's option
// Dictionaries). Each converts to the core's keyword-argument JsonObject, so the core validates them and reports the
// ConfigError codes the conformance vectors pin. A null field is left out (the core's default applies).
#nullable enable
using System.Collections.Generic;
using Scfg.Core;
using UnityEngine;

namespace Scfg
{
    /// <summary>SCFG.Configure's options: the payout wallet(s) and how players may pay.</summary>
    public sealed class ScfgOptions
    {
        /// <summary>The EVM payout address (0x...). At least one of Wallet and SolanaWallet is required.</summary>
        public string? Wallet { get; set; }
        /// <summary>The Solana payout address (base58).</summary>
        public string? SolanaWallet { get; set; }
        /// <summary>Use the test networks (develop with this on; release with it off).</summary>
        public bool Testnet { get; set; }
        /// <summary>The networks offered, in order (default: every network the wallets can receive on).</summary>
        public IList<string>? Chains { get; set; }
        /// <summary>The tokens accepted: "USDC", "USDT" (default both).</summary>
        public IList<string>? Tokens { get; set; }
        /// <summary>Which payment methods the sheet offers (default: all of them).</summary>
        public ScfgMethods? Methods { get; set; }
        /// <summary>Extra RPC endpoints per chain key, tried before the built-in ones.</summary>
        public IDictionary<string, IList<string>>? RpcUrls { get; set; }
        /// <summary>Confirmations required per chain key.</summary>
        public IDictionary<string, int>? Confirmations { get; set; }
        /// <summary>Wallets you were paid to before (their receipts stay valid).</summary>
        public IList<string>? PreviousWallets { get; set; }
        /// <summary>The accent color of the sheet and the checkout page, "#rrggbb".</summary>
        public string? Accent { get; set; }
        /// <summary>The logo on the sheet and the checkout page (a raster texture).</summary>
        public Texture2D? Logo { get; set; }
        /// <summary>
        /// WebGL builds: tells two games with the same product name on one web origin apart in the receipts' copy in
        /// localStorage (its key is "scfg:&lt;productName&gt;:&lt;sha256 of this, else of the product name&gt;"). Set it once and
        /// never change it: a new value starts a new, empty copy.
        /// </summary>
        public string? WebStorageId { get; set; }

        internal JsonObject ToKwargs()
        {
            var kw = new JsonObject();
            if (Wallet != null) kw["wallet"] = Wallet;
            if (SolanaWallet != null) kw["solana_wallet"] = SolanaWallet;
            kw["testnet"] = Testnet;
            if (Chains != null) kw["chains"] = List(Chains);
            if (Tokens != null) kw["tokens"] = List(Tokens);
            if (Methods != null) kw["methods"] = Methods.ToJson();
            if (RpcUrls != null)
            {
                var urls = new JsonObject();
                foreach (var kv in RpcUrls) urls[kv.Key] = kv.Value == null ? null : List(kv.Value);
                kw["rpc_urls"] = urls;
            }
            if (Confirmations != null)
            {
                var conf = new JsonObject();
                foreach (var kv in Confirmations) conf[kv.Key] = (long)kv.Value;
                kw["confirmations"] = conf;
            }
            if (PreviousWallets != null) kw["previous_wallets"] = List(PreviousWallets);
            if (Accent != null) kw["accent"] = Accent;
            return kw;
        }

        internal static List<object?> List(IEnumerable<string> values)
        {
            var output = new List<object?>();
            foreach (var v in values) output.Add(v);
            return output;
        }
    }

    /// <summary>The payment methods (config.py's methods). A null field keeps the default.</summary>
    public sealed class ScfgMethods
    {
        /// <summary>The checkout page in the player's browser, for a wallet extension.</summary>
        public bool? Browser { get; set; }
        /// <summary>Minutes the checkout page keeps answering (default 30).</summary>
        public double? BrowserTimeoutMinutes { get; set; }
        /// <summary>A QR code for a phone wallet.</summary>
        public bool? Qr { get; set; }
        /// <summary>The "Open wallet app" button on phones.</summary>
        public bool? QrWalletButton { get; set; }
        /// <summary>The chains the QR code is offered on (default: all).</summary>
        public IList<string>? QrChains { get; set; }
        /// <summary>The address and amount to send by hand, from an exchange or any wallet.</summary>
        public bool? Manual { get; set; }
        /// <summary>Whether the player may paste the transaction hash on the manual page.</summary>
        public bool? ManualAllowTxPaste { get; set; }

        internal JsonObject ToJson()
        {
            var m = new JsonObject();
            var browser = new JsonObject();
            if (Browser.HasValue) browser["enabled"] = Browser.Value;
            if (BrowserTimeoutMinutes.HasValue) browser["timeout_min"] = BrowserTimeoutMinutes.Value;
            if (browser.Count > 0) m["browser"] = browser;
            var qr = new JsonObject();
            if (Qr.HasValue) qr["enabled"] = Qr.Value;
            if (QrWalletButton.HasValue) qr["wallet_button"] = QrWalletButton.Value;
            if (QrChains != null) qr["chains"] = ScfgOptions.List(QrChains);
            if (qr.Count > 0) m["qr"] = qr;
            var manual = new JsonObject();
            if (Manual.HasValue) manual["enabled"] = Manual.Value;
            if (ManualAllowTxPaste.HasValue) manual["allow_tx_paste"] = ManualAllowTxPaste.Value;
            if (manual.Count > 0) m["manual"] = manual;
            return m;
        }
    }

    /// <summary>A paid unlock (SCFG.AddItem). Never change Code after release: receipts carry it.</summary>
    public sealed class ScfgItem
    {
        /// <summary>1..99, unique per game.</summary>
        public int Code { get; set; }
        /// <summary>The price as a decimal string, "4.99" (never a float).</summary>
        public string? Price { get; set; }
        /// <summary>"USDC" or "USDT".</summary>
        public string? Token { get; set; }
        /// <summary>What the player sees on the sheet.</summary>
        public string? Title { get; set; }
        /// <summary>The least a payment may be and still unlock (default: the price).</summary>
        public string? MinPrice { get; set; }

        internal JsonObject ToKwargs()
        {
            var kw = new JsonObject { { "code", (long)Code }, { "price", Price }, { "token", Token }, { "title", Title } };
            if (MinPrice != null) kw["min_price"] = MinPrice;
            return kw;
        }
    }

    /// <summary>An EVM chain that is not built in (SCFG.AddChain, before Configure).</summary>
    public sealed class ScfgChain
    {
        public string? Name { get; set; }
        public long ChainId { get; set; }
        /// <summary>https RPC endpoints.</summary>
        public IList<string>? Rpc { get; set; }
        /// <summary>Token symbol to contract address ({"USDC": "0x..."}); 6-decimal tokens only.</summary>
        public IDictionary<string, string>? Tokens { get; set; }
        public int Confirmations { get; set; }
        /// <summary>An https URL with "{}" where the transaction hash goes.</summary>
        public string? ExplorerTx { get; set; }
        public string? NativeSymbol { get; set; }
        /// <summary>"lowest", "low" or "high".</summary>
        public string? FeeHint { get; set; }
        public bool Testnet { get; set; }
        public long? MaxLogRange { get; set; }

        internal JsonObject ToKwargs(string key)
        {
            var tokens = new JsonObject();
            if (Tokens != null)
                foreach (var kv in Tokens) tokens[kv.Key] = kv.Value;
            var kw = new JsonObject
            {
                { "key", key }, { "name", Name }, { "chain_id", ChainId }, { "rpc", Rpc == null ? null : ScfgOptions.List(Rpc) },
                { "tokens", Tokens == null ? null : tokens }, { "confirmations", (long)Confirmations }, { "explorer_tx", ExplorerTx },
                { "testnet", Testnet },
            };
            if (NativeSymbol != null) kw["native_symbol"] = NativeSymbol;
            if (FeeHint != null) kw["fee_hint"] = FeeHint;
            if (MaxLogRange.HasValue) kw["max_log_range"] = MaxLogRange.Value;
            return kw;
        }
    }
}
