// No Python twin: what the vectors do not pin for chains.py, config.py, payments.py and the base58 they use:
// the keyword-argument contract (an unknown or missing key is a TypeError), the read-only tables and registries,
// the internal registry override and Chain.With, the Arc log source, and the ASCII-only case folding under a
// Turkish culture.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Numerics;
using NUnit.Framework;
using Scfg.Core.Solana;

namespace Scfg.Core.Tests
{
    public class ConfigTests
    {
        private const string Evm1 = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";

        private static JsonObject Kw(params (string, object?)[] pairs)
        {
            var o = new JsonObject();
            foreach (var (k, v) in pairs) o[k] = v;
            return o;
        }

        private static JsonObject ChainKw(params (string, object?)[] over)
        {
            var o = Kw(("key", "avalanche"), ("name", "Avalanche"), ("chain_id", 43114L),
                       ("rpc", "https://api.avax.network/ext/bc/C/rpc"),
                       ("tokens", Kw(("USDC", "0xb97ef9ef8734c71904d8002f8b6bc66dd9c48a6e"))),
                       ("confirmations", 5L), ("explorer_tx", "https://snowtrace.io/tx/{}"));
            foreach (var (k, v) in over) o[k] = v;
            return o;
        }

        private static string CodeOf(System.Action f) => Assert.Throws<ConfigError>(() => f())!.Code;

        // test_config.py's test_onramp_url_needs_a_host: a host must follow https://.
        [TestCase("https://")]
        [TestCase("https:///buy")]
        [TestCase("https://{amount}")]
        [TestCase("https://?a=1")]
        [TestCase("https://#top")]
        [TestCase("https://}")]
        public void OnrampUrlNeedsAHost(string url)
        {
            Assert.That(CodeOf(() => Config.BuildConfig(Kw(("wallet", Evm1), ("onramp", Kw(("url", url)))))), Is.EqualTo("onramp.bad_url"));
            Assert.That(Config.BuildConfig(Kw(("wallet", Evm1), ("onramp", Kw(("url", "https://x"))))).Onramp.Url, Is.EqualTo("https://x"));
        }

        [Test]
        public void UnknownAndMissingKeywordsAreTypeErrors()
        {
            Assert.Throws<TypeError>(() => Config.BuildConfig(Kw(("wallet", Evm1), ("colour", "red"))));
            Assert.Throws<TypeError>(() => Config.BuildChain(Kw(("key", "x"))));
            Assert.Throws<TypeError>(() => Config.BuildChain(ChainKw(("bogus", 1L))));
            var cfg = Config.BuildConfig(Kw(("wallet", Evm1)));
            Assert.Throws<TypeError>(() => Config.AddItem(cfg, "a", Kw(("code", 1L), ("price", "1"), ("token", "USDC"))));
            Assert.Throws<TypeError>(() => Config.AddItem(cfg, "a", Kw(("code", 1L), ("price", "1"), ("token", "USDC"),
                                                                       ("title", "T"), ("extra", 1L))));
            Assert.Throws<TypeError>(() => Payments.PreparePayment(cfg, "tip", Kw(("amout", "1"))));
        }

        [Test]
        public void WrongTypesReachTheCoreChecks()
        {
            // Python's bool is an int; a C# bool is not, and neither is a float.
            Assert.That(CodeOf(() => Config.BuildChain(ChainKw(("confirmations", true)))), Is.EqualTo("int.below_minimum"));
            Assert.That(CodeOf(() => Config.BuildChain(ChainKw(("chain_id", 1.0)))), Is.EqualTo("int.below_minimum"));
            Assert.That(CodeOf(() => Config.BuildConfig(Kw(("wallet", Evm1), ("confirmations", Kw(("base", 1.5)))))),
                        Is.EqualTo("confirmations.invalid"));
            Assert.That(CodeOf(() => Config.BuildConfig(Kw(("wallet", 5L)))), Is.EqualTo("address.invalid"));
            Assert.Throws<TypeError>(() => Config.BuildConfig(Kw(("wallet", Evm1), ("chains", 5L))));
            Assert.Throws<TypeError>(() => Config.BuildConfig(Kw(("wallet", Evm1), ("chains", new List<object?> { new List<object?>() }))));
            // A huge int is a valid Python int; it does not fit the 64-bit field (Python's class for that).
            var big = Assert.Throws<OverflowError>(() => Config.BuildChain(ChainKw(("confirmations", BigInteger.Pow(10, 30)))))!;
            Assert.That(big.Message, Is.EqualTo("confirmations out of range: 1" + new string('0', 30)));
            Assert.Throws<OverflowError>(() => Config.BuildChain(ChainKw(("chain_id", BigInteger.Pow(2, 64)))));
            Assert.Throws<OverflowError>(() => Config.BuildChain(ChainKw(("max_log_range", BigInteger.Pow(2, 63)))));
            Assert.Throws<OverflowError>(() => Config.BuildConfig(Kw(("wallet", Evm1), ("confirmations", Kw(("base", BigInteger.Pow(2, 70)))))));
        }

        // Domain reload off keeps statics alive between play sessions: no table or registry may be writable.
        private static void AssertReadOnlyList<T>(IReadOnlyList<T> list)
        {
            Assert.That(list, Is.Not.InstanceOf<Array>());
            Assert.That(list, Is.Not.InstanceOf<List<T>>());
            Assert.Throws<NotSupportedException>(() => ((IList<T>)list)[0] = list[0]);
            Assert.Throws<NotSupportedException>(() => ((ICollection<T>)list).Clear());
        }

        private static void AssertReadOnlyDict<TK, TV>(IReadOnlyDictionary<TK, TV> dict) where TK : notnull
        {
            Assert.That(dict, Is.Not.InstanceOf<Dictionary<TK, TV>>());
            var first = dict.Keys.First();
            Assert.Throws<NotSupportedException>(() => ((IDictionary<TK, TV>)dict)[first] = dict[first]);
            Assert.Throws<NotSupportedException>(() => ((IDictionary<TK, TV>)dict).Remove(first));
        }

        [Test]
        public void PublicTablesAreReadOnly()
        {
            AssertReadOnlyList(Config.Tokens);
            AssertReadOnlyList(Config.FeeHints);
            AssertReadOnlyList(Chains.DisplayOrder);
            AssertReadOnlyList(Sessions.UiStatuses);
            AssertReadOnlyList(Payments.TipPresets);
            AssertReadOnlyDict(Config.LogoTypes);
            Assert.That(Config.Tokens, Is.EqualTo(new[] { "USDC", "USDT" }));
            Assert.That(Config.FeeHints, Is.EqualTo(new[] { "lowest", "low", "high" }));
            Assert.That(Chains.DisplayOrder, Is.EqualTo(new[] { "arbitrum", "base", "arc", "polygon", "ethereum" }));
        }

        [Test]
        public void RegistriesAndChainsAreReadOnly()
        {
            foreach (var registry in new[] { Chains.Mainnet, Chains.Testnet })
            {
                AssertReadOnlyDict(registry);
                foreach (var chain in registry.Values)
                {
                    AssertReadOnlyDict(chain.Tokens);
                    AssertReadOnlyList(chain.Rpc);
                }
            }
            Assert.That(Chains.Mainnet.Keys, Is.EqualTo(new[] { "ethereum", "arbitrum", "polygon", "base", "arc", "solana" }));
        }

        [Test]
        public void AChainCopiesItsRpcAndTokens()
        {
            var rpc = new[] { "https://a.example" };
            var tokens = new Dictionary<string, Token> { ["USDC"] = Chains.MakeToken("0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48") };
            var chain = new Chain("x", 1, "X", rpc, 1, 10, "https://x/tx/{}", "ETH", "low", tokens);
            rpc[0] = "https://evil.example";
            tokens.Clear();
            Assert.That(chain.Rpc, Is.EqualTo(new[] { "https://a.example" }));
            Assert.That(chain.Tokens.Keys, Is.EqualTo(new[] { "USDC" }));
            var copy = chain.With(confirmations: 3);
            Assert.That(copy.Rpc, Is.EqualTo(chain.Rpc));
            Assert.That(copy.Tokens.Keys, Is.EqualTo(new[] { "USDC" }));
            Assert.That(copy.Confirmations, Is.EqualTo(3));
        }

        [Test]
        public void OverridePointsARegistryEntryAtALocalNodeAndResetRestoresIt()
        {
            var original = Chains.Mainnet["base"];
            var originalRpc = original.Rpc.ToList();
            try
            {
                Chains.Override(false, original.With(rpc: new[] { "http://127.0.0.1:8545" }, confirmations: 1));
                var cfg = Config.BuildConfig(Kw(("wallet", Evm1), ("chains", new List<object?> { "base" })));
                var enabled = Config.EnabledChains(cfg)["base"];
                Assert.That(enabled.Rpc, Is.EqualTo(new[] { "http://127.0.0.1:8545" }));
                Assert.That(enabled.Confirmations, Is.EqualTo(1));
                Assert.That(enabled.ChainId, Is.EqualTo(original.ChainId));
                Assert.That(Chains.Testnet["base"].Rpc, Is.Not.EqualTo(new[] { "http://127.0.0.1:8545" }), "only the network named");
                Assert.That(Chains.Mainnet.Keys, Is.EqualTo(new[] { "ethereum", "arbitrum", "polygon", "base", "arc", "solana" }),
                            "an override keeps the registry's order");
                Assert.Throws<KeyError>(() => Chains.Override(false, Config.BuildChain(ChainKw())), "only an existing entry");
                Assert.Throws<ValueError>(() => Chains.Override(true, original), "a mainnet chain is not a testnet entry");
            }
            finally
            {
                Chains.ResetRegistries();
            }
            Assert.That(Chains.Mainnet["base"].Rpc, Is.EqualTo(originalRpc));
            Assert.That(Chains.Mainnet["base"].Confirmations, Is.EqualTo(original.Confirmations));
            Assert.That(Chains.GetChains(false), Is.SameAs(Chains.Mainnet));
            Assert.That(Chains.GetChains(true), Is.SameAs(Chains.Testnet));
            Assert.That(Chains.MakeToken("0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48").Address,
                        Is.EqualTo("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"));
        }

        [Test]
        public void ExplorerUrlFormatsTheHash()
        {
            Assert.That(Chains.Mainnet["ethereum"].ExplorerUrl("0xabc"), Is.EqualTo("https://etherscan.io/tx/0xabc"));
            Assert.That(Chains.Testnet["solana"].ExplorerUrl("sig"), Is.EqualTo("https://solscan.io/tx/sig?cluster=devnet"));
        }

        [Test]
        public void ArcLogsAreReadFromTheSystemEmitterAndFlooredToSixDecimals()
        {
            var arc = Chains.Mainnet["arc"];
            Assert.That(arc.Tokens["USDC"].LogSource, Is.EqualTo(Chains.ArcSystemEmitter));
            JsonObject Log(string address, string value) => Kw(
                ("address", address),
                ("topics", new List<object?>
                {
                    Evm.TransferTopic, Evm.AddressTopic(Evm1), Evm.AddressTopic("0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238"),
                }),
                ("data", "0x" + value.PadLeft(64, '0')), ("transactionHash", "0x" + new string('a', 64)),
                ("logIndex", "0x1"), ("blockNumber", "0x2"), ("blockHash", "0x" + new string('b', 64)));
            // 1.5 USDC in 18 decimals is 1_500_000 units; 10^11 wei floors to 0 units.
            var t = arc.ParseTransfer(Log(Chains.ArcSystemEmitter, new BigInteger(1.5e18).ToString("x", CultureInfo.InvariantCulture)));
            Assert.That(t!.Token, Is.EqualTo(Chains.ArcUsdc));
            Assert.That(t.Value, Is.EqualTo(new BigInteger(1500000)));
            Assert.That(arc.ParseTransfer(Log(Chains.ArcSystemEmitter, "174876e800")), Is.Null);
            Assert.That(arc.ParseTransfer(Log(Chains.ArcUsdc, "1")), Is.Null, "the 0x3600 interface is not the log source");
        }

        [Test]
        public void LocalHttpTakesUnicodeDigitsLikePythonsBackslashD()
        {
            JsonObject Urls(string u) => Kw(("wallet", Evm1), ("rpc_urls", Kw(("base", u))));
            Assert.DoesNotThrow(() => Config.BuildConfig(Urls("http://localhost:٨٠٨٠/x")));
            Assert.DoesNotThrow(() => Config.BuildConfig(Urls("http://127.0.0.1/")));
            foreach (var bad in new[] { "http://localhost:123456", "http://localhost:", "http://localhost.evil.org", "http://localhost\n" })
                Assert.That(CodeOf(() => Config.BuildConfig(Urls(bad))), Is.EqualTo("rpc.not_https"), bad);
        }

        [Test]
        public void CaseFoldingIsAsciiOnlyUnderTurkishCulture()
        {
            var before = CultureInfo.CurrentCulture;
            try
            {
                CultureInfo.CurrentCulture = new CultureInfo("tr-TR");
                Assert.That(Config.LogoContentType("images/LOGO.PNG"), Is.EqualTo("image/png"));
                Assert.That(Config.LogoContentType("a.JPEG"), Is.EqualTo("image/jpeg"));
                Assert.That(CodeOf(() => Config.LogoContentType("a.PİNG")), Is.EqualTo("logo.bad_type"));
                Assert.That(CodeOf(() => Config.LogoContentType("a.pnıg")), Is.EqualTo("logo.bad_type"));
                var cfg = Config.BuildConfig(Kw(("wallet", Evm1), ("accent", "#ABCDEF")));
                Assert.That(cfg.Accent, Is.EqualTo("#abcdef"));
                Assert.That(Config.BuildConfig(Kw(("wallet", Evm1), ("accent", "#ABC"))).Accent, Is.EqualTo("#aabbcc"));
                Assert.That(CodeOf(() => Config.BuildChain(ChainKw(("key", "AVALANCHE")))), Is.EqualTo("chain.invalid_key"));
                Assert.That(CodeOf(() => Config.BuildChain(ChainKw(("key", "ı")))), Is.EqualTo("chain.invalid_key"));
                Assert.That(CodeOf(() => Config.BuildChain(ChainKw(("fee_hint", "LOW")))), Is.EqualTo("chain.bad_fee_hint"));
            }
            finally
            {
                CultureInfo.CurrentCulture = before;
            }
        }

        [Test]
        public void TextOnPicksTheHigherContrast()
        {
            Assert.That(Config.TextOn("#0e7c74"), Is.EqualTo("#ffffff"));
            Assert.That(Config.TextOn("#ffffff"), Is.EqualTo(Config.Ink));
            Assert.That(Config.TextOn("#000000"), Is.EqualTo("#ffffff"));
        }

        [Test]
        public void FormatOneFollowsStrFormat()
        {
            Assert.That(Py.Format1("x/{}/{{y}}", "0x"), Is.EqualTo("x/0x/{y}"));
            Assert.That(Py.Format1("{0}-{0!r}", "h"), Is.EqualTo("h-'h'"));
            Assert.Throws<ValueError>(() => Py.Format1("{}/{0}", "h"));
            Assert.Throws<ValueError>(() => Py.Format1("{0}/{}", "h"));
            Assert.Throws<IndexError>(() => Py.Format1("{}{}", "h"));
            Assert.Throws<IndexError>(() => Py.Format1("{1}", "h"));
            Assert.Throws<KeyError>(() => Py.Format1("{x}", "h"));
            Assert.Throws<ValueError>(() => Py.Format1("a}", "h"));
            Assert.Throws<ValueError>(() => Py.Format1("{", "h"));
            Assert.Throws<ValueError>(() => Py.Format1("{0!x}", "h"));
        }

        [Test]
        public void Base58RoundTripsAndRefusesBadLengths()
        {
            var bytes = new byte[] { 0, 0, 1, 2, 255 };
            Assert.That(Base58.B58Decode(Base58.B58Encode(bytes)), Is.EqualTo(bytes));
            Assert.Throws<ValueError>(() => Base58.DecodeFixed("1", 1.0));
            Assert.Throws<ValueError>(() => Base58.DecodeFixed("1", true));
            Assert.Throws<ValueError>(() => Base58.DecodeFixed("1", BigInteger.Pow(10, 400)));
            Assert.That(Base58.DecodeFixed("1", 1), Is.EqualTo(new byte[] { 0 }));
        }

        [Test]
        public void ItemsKeepInsertionOrderAndPaymentsUseThem()
        {
            var cfg = Config.BuildConfig(Kw(("wallet", Evm1), ("chains", new List<object?> { "base" })));
            Config.AddItem(cfg, "b", Kw(("code", 2L), ("price", "1.5"), ("token", "USDC"), ("title", "B")));
            Config.AddItem(cfg, "a", Kw(("code", 1L), ("price", 5L), ("token", "USDC"), ("title", "A"), ("min_price", "2")));
            Assert.That(cfg.Items.Keys, Is.EqualTo(new[] { "b", "a" }));
            Assert.That(cfg.ItemByCode(1)!.ItemId, Is.EqualTo("a"));
            var spec = Payments.PreparePayment(cfg, "unlock", Kw(("item", "a"), ("amount", "2.5")));
            Assert.That(spec.Price.ToString(), Is.EqualTo("2.50")); // culture-ok: Dec.ToString takes no culture
            Assert.That(spec.Chains, Is.EqualTo(new[] { "base" }));
            Assert.That(CodeOf(() => Payments.PreparePayment(cfg, "unlock", Kw(("item", "a"), ("amount", "1.99")))),
                        Is.EqualTo("payment.below_min_price"));
            Assert.That(CodeOf(() => Payments.PreparePayment(cfg, 5L, Kw())), Is.EqualTo("payment.bad_kind"));
            Assert.Throws<TypeError>(() => Payments.PreparePayment(cfg, "unlock", Kw(("item", new List<object?>()))));
        }
    }
}
