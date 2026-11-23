// Twin of scfg_core's tests/unit/test_checkout_solana.py, ported from rpgmaker/src/core/checkout_solana.test.ts (the
// Python file's RestoreController and CheckoutServer tests belong to the restore flow and the localhost server).
#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;
using Scfg.Core.Solana;

namespace Scfg.Core.Tests
{
    public class CheckoutSolanaTests
    {
        private const string DevEvm = "0x52908400098527886E0F7030069857D2E4169EE7";
        private const string Dev = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU";
        private const string Payer = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM";
        private static string Mint => Chains.Mainnet["solana"].Tokens["USDC"].Address;
        private static readonly string EvmTx = "0x" + string.Concat(Enumerable.Repeat("ab", 32));
        private static readonly string Sig = SigOf(7);
        private static readonly string Sig2 = SigOf(8);

        /// <summary>solana_fakes.ts's sig(n): a distinct 64-byte signature per n.</summary>
        private static string SigOf(int n)
        {
            var b = new byte[64];
            b[63] = (byte)n;
            return Base58.B58Encode(b);
        }

        private sealed class Rigged
        {
            public CheckoutController Ctl = null!;
            public CheckoutRig Rig = null!;
            public ImmediateSubmitter Submitter = null!;
            public readonly List<FakeServer> Servers = new List<FakeServer>();
            public Engine E => Rig.Engine;
        }

        private static Rigged Setup(params (string Chain, string Token)[] unready)
        {
            var cfg = Config.BuildConfig(new JsonObject { { "wallet", DevEvm }, { "solana_wallet", Dev } });
            Config.AddItem(cfg, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            var r = new Rigged { Rig = new CheckoutRig(cfg, 0) };
            r.Rig.Sol.Accounts.Add(Pda.AssociatedTokenAddress(Dev, Mint));
            r.Rig.Sol.Accounts.Add(Pda.AssociatedTokenAddress(Dev, Chains.Mainnet["solana"].Tokens["USDT"].Address));
            r.Submitter = new ImmediateSubmitter(r.Rig.Engine);
            var spec = Payments.PreparePayment(cfg, "unlock", new JsonObject { { "item", "chapter2" } });
            IReadOnlyCollection<(string Chain, string Token)> blocked = unready.ToList().AsReadOnly();
            r.Ctl = new CheckoutController(cfg, spec, r.Submitter.Submit, serverFactory: args =>
            {
                var s = new FakeServer(args);
                r.Servers.Add(s);
                return s;
            }, unready: () => blocked, gameName: "Ruby Dreams");
            return r;
        }

        private static async Task<Rigged> Started(string chain)
        {
            var r = Setup();
            r.Ctl.SelectChain(chain);
            await r.Submitter.Idle();
            r.Ctl.Poll();
            return r;
        }

        private static List<object?> Triple(JsonObject info) => new List<object?> { info["error"], info["tx_hash"], info["dropped_tx"] };

        [Test]
        public void SolanaIsOfferedAndUnreadyTokensAreHidden()
        {
            var ctl = Setup().Ctl;
            Assert.That(ctl.ChainOptions(), Does.Contain(("solana", "Solana", "lowest")));
            Assert.That(ctl.DevWarnings(), Is.Empty);
            ctl = Setup(("solana", "USDC")).Ctl;
            Assert.That(ctl.ChainOptions().Select(o => o.Key), Does.Not.Contain("solana"));
            Assert.That(ctl.DevWarnings(), Is.EqualTo(new[] { "Your Solana wallet can't receive USDC yet: receive any amount once." }));
        }

        [Test]
        public void DevWarningsAreSortedAndNameOnlySolanaRoutes()
        {
            var ctl = Setup(("solana", "USDT"), ("base", "USDC"), ("solana", "USDC"), ("nowhere", "USDC")).Ctl;
            Assert.That(ctl.DevWarnings(), Is.EqualTo(new[]
            {
                "Your Solana wallet can't receive USDC yet: receive any amount once.",
                "Your Solana wallet can't receive USDT yet: receive any amount once.",
            }));
            Assert.That(ctl.ChainOptions().Select(o => o.Key), Does.Not.Contain("base"));
        }

        [Test]
        public void OnlyUnreadyRoutesShowsTheUnavailableMessage()
        {
            var cfg = Config.BuildConfig(new JsonObject { { "solana_wallet", Dev } });
            Config.AddItem(cfg, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            var spec = Payments.PreparePayment(cfg, "unlock", new JsonObject { { "item", "chapter2" } });
            IReadOnlyCollection<(string Chain, string Token)> blocked = new[] { ("solana", "USDC") };
            var ctl = new CheckoutController(cfg, spec, command => throw new InvalidOperationException("unused"),
                                             unready: () => blocked);
            Assert.That(ctl.ChainOptions(), Is.Empty);
            Assert.That(ctl.Message(), Is.EqualTo(Checkout.Messages["solana_unavailable"]));
            Assert.That(ctl.Step, Is.EqualTo("network"));
        }

        [Test]
        public async Task SolanaRecipientAndPaymentLink()
        {
            var ctl = (await Started("solana")).Ctl;
            Assert.That(ctl.Family, Is.EqualTo("solana"));
            Assert.That(ctl.Recipient, Is.EqualTo(Dev));
            Assert.That(ctl.TokenAddress, Is.EqualTo(Mint));
            var uri = ctl.PaymentUri()!;
            Assert.That(uri, Does.StartWith("solana:" + Dev + "?amount=4.9937"));
            Assert.That(uri, Does.Contain("spl-token=" + Mint));
            Assert.That(uri, Does.Contain("reference=" + (string)ctl.Session!["reference"]!));
            Assert.That(uri, Does.Contain("label=Ruby%20Dreams"));
            Assert.That(uri, Does.Contain("message=Chapter%202"));
            Assert.That(ctl.Session!["dest"], Is.EqualTo(Pda.AssociatedTokenAddress(Dev, Mint)));
        }

        [Test]
        public async Task EvmChainStillUsesEip681()
        {
            var ctl = (await Started("base")).Ctl;
            Assert.That(ctl.Family, Is.EqualTo("evm"));
            Assert.That(ctl.Recipient, Is.EqualTo(DevEvm));
            Assert.That(ctl.PaymentUri(), Does.StartWith("ethereum:"));
        }

        [Test]
        public void TheRecipientBeforeAChainIsChosenIsTheEvmWalletFirst()
        {
            Assert.That(Setup().Ctl.Recipient, Is.EqualTo(DevEvm));
            Assert.That(Setup().Ctl.TokenAddress, Is.Null);
            Assert.That(Setup().Ctl.Family, Is.Null);
        }

        [Test]
        public async Task PastingTheWrongFamilyIsAnInvalidHash()
        {
            var r = await Started("solana");
            Assert.That(r.Ctl.SubmitHash(EvmTx), Is.False);
            Assert.That(r.Ctl.Error, Is.EqualTo("invalid_hash"));
            Assert.That(r.Ctl.SubmitHash(Sig), Is.True);
            Assert.That(((JsonObject)r.E.Pending[r.Ctl.SessionId!]!)["hint_tx"], Is.EqualTo(Sig));
            var ctl = (await Started("base")).Ctl;
            Assert.That(ctl.SubmitHash(Sig), Is.False);
            Assert.That(ctl.Error, Is.EqualTo("invalid_hash"));
            Assert.That(Checkout.Messages["invalid_hash"], Does.Contain("signature"));
        }

        [Test]
        public async Task TransactionThatNeverLandedLetsThePlayerPayAgain()
        {
            var ctl = (await Started("solana")).Ctl;
            Assert.That(ctl.SubmitHash(Sig), Is.True);
            Assert.That(ctl.HintTx, Is.EqualTo(Sig));
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "waiting", error: "tx_not_landed"));
            Assert.That(ctl.HintTx, Is.Null);
            Assert.That(ctl.State, Is.EqualTo("waiting"));
            Assert.That(ctl.Message(), Is.EqualTo("That transaction didn't reach the network. You can pay again."));
            Assert.That(ctl.StatusInfo()["tx_hash"], Is.Null);
            Assert.That(ctl.StatusInfo()["error"], Is.EqualTo("tx_not_landed"));
            Assert.That(ctl.StatusInfo()["dropped_tx"], Is.EqualTo(Sig), "no hint_tx in the outcome: the controller's own");
        }

        [Test]
        public async Task StatusNamesTheSignatureThatNeverLanded()
        {
            var ctl = (await Started("solana")).Ctl;
            Assert.That(ctl.StatusInfo()["dropped_tx"], Is.Null);
            ctl.SubmitHash(Sig);
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "waiting", error: "tx_not_landed", hintTx: Sig));
            Assert.That(Triple(ctl.StatusInfo()), Is.EqualTo(new object?[] { "tx_not_landed", null, Sig }));
        }

        [Test]
        public async Task ANewHintClearsTheOldNotLandedStatus()
        {
            var r = await Started("solana");
            var ctl = r.Ctl;
            ctl.SubmitHash(Sig);
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "waiting", error: "tx_not_landed", hintTx: Sig));
            ctl.ReportHint(Sig2, Dev); // the page's POST /tx for the next send
            Assert.That(Triple(ctl.StatusInfo()), Is.EqualTo(new object?[] { null, Sig2, null }));
            Assert.That(ctl.Message(), Is.Null);
            Assert.That(((JsonObject)r.E.Pending[ctl.SessionId!]!)["hint_tx"], Is.EqualTo(Sig2));
        }

        [TestCase("tx_not_landed")]
        [TestCase("tx_reverted")]
        [TestCase("tx_not_a_payment")]
        public async Task AVerdictOnAnOlderHintDoesNotTouchTheNewerOne(string error)
        {
            // The worker may judge the old hint just before the page reports the next one.
            var ctl = (await Started("solana")).Ctl;
            ctl.SubmitHash(Sig);
            ctl.ReportHint(Sig2, Dev);
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "waiting", error: error, hintTx: Sig));
            Assert.That(Triple(ctl.StatusInfo()), Is.EqualTo(new object?[] { null, Sig2, null }));
        }

        [TestCase("tx_reverted")]
        [TestCase("tx_not_a_payment")]
        public async Task AVerdictOnTheCurrentHintDropsItWithoutADroppedTx(string error)
        {
            var ctl = (await Started("solana")).Ctl;
            ctl.SubmitHash(Sig);
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "waiting", error: error, hintTx: Sig));
            Assert.That(Triple(ctl.StatusInfo()), Is.EqualTo(new object?[] { error, null, null }));
        }

        // --- additions with no Python twin ---

        [Test]
        public async Task SolanaSessionInfoAndBuildTx()
        {
            var r = await Started("solana");
            var ctl = r.Ctl;
            ctl.StartBrowser();
            var s = r.Servers[0];
            var info = s.SessionInfo;
            Assert.That(info.Keys, Is.EqualTo(new[]
            {
                "title", "kind", "token", "family", "decimals", "amount_units", "amount_text", "price_text",
                "recipient", "chain_name", "native_symbol", "explorer_tx", "testnet", "logo", "accent", "on_accent",
                "wallet_chain", "mint", "reference",
            }));
            Assert.That(info["family"], Is.EqualTo("solana"));
            Assert.That(info["mint"], Is.EqualTo(Mint));
            Assert.That(info["reference"], Is.EqualTo(ctl.Session!["reference"]));
            Assert.That(info["wallet_chain"], Is.EqualTo(Chains.Mainnet["solana"].WalletChain));
            Assert.That(info["recipient"], Is.EqualTo(Dev));
            Assert.That(s.Args.BuildTx, Is.Not.Null);
            int before = r.Submitter.Jobs.Count;
            int calls = r.Rig.Sol.Requests.Count;
            var job = s.Args.BuildTx!(DevEvm); // the TS test's account: not a Solana key
            await r.Submitter.Idle();
            Assert.That(job.Done(), Is.True);
            Assert.That(r.Submitter.Jobs, Has.Count.EqualTo(before + 1), "the build went through submit");
            Assert.That(job.Exception(), Is.TypeOf<ValueError>(), "an EVM address is no Solana account");
            Assert.That(r.Rig.Sol.Requests, Has.Count.EqualTo(calls), "refused before any RPC call");
        }

        [Test]
        public async Task BuildTxBuildsTheTransferForThePlayersAccount()
        {
            var r = await Started("solana");
            var ctl = r.Ctl;
            ctl.StartBrowser();
            var source = Pda.AssociatedTokenAddress(Payer, Mint);
            r.Rig.Sol.TokenBalances[source] = 7000000;
            r.Rig.Sol.Lamports[Payer] = 5000;
            r.Rig.Sol.Requests.Clear();
            var job = r.Servers[0].Args.BuildTx!(Payer);
            await r.Submitter.Idle();
            Assert.That(job.Exception(), Is.Null);
            var built = (JsonObject)job.Result()!;
            Assert.That(built.Keys, Is.EqualTo(new[] { "transaction", "token_balance", "sol_balance" }));
            Assert.That(built["token_balance"], Is.EqualTo("7000000"));
            Assert.That(built["sol_balance"], Is.EqualTo("5000"));
            Assert.That(built["transaction"], Is.TypeOf<string>().And.Not.Empty);
            Assert.That(r.Rig.Sol.Requests.Select(q => q.Method), Is.EqualTo(new[]
            {
                "getAccountInfo", "getBalance", "getLatestBlockhash", "getRecentPrioritizationFees",
            }));
            Assert.That(r.Rig.Sol.Requests[0].Params[0], Is.EqualTo(source), "the player's token account");
            Assert.That(r.Rig.Sol.Requests[1].Params[0], Is.EqualTo(Payer), "the player's SOL balance");
            Assert.That(r.Rig.Sol.Requests[3].Params[0], Is.EqualTo(new List<object?> { ctl.Session!["dest"] }));
        }

        // --- Solana has no "Pay with card" button ------------------------------------------------

        private static Rigged SetupWith(JsonObject extra)
        {
            var kwargs = new JsonObject { { "wallet", DevEvm }, { "solana_wallet", Dev } };
            foreach (var kv in extra) kwargs[kv.Key] = kv.Value;
            var cfg = Config.BuildConfig(kwargs);
            Config.AddItem(cfg, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            var r = new Rigged { Rig = new CheckoutRig(cfg, 0) };
            foreach (var chains in new[] { Chains.Mainnet, Chains.Testnet })
                foreach (var token in chains["solana"].Tokens.Values)
                    r.Rig.Sol.Accounts.Add(Pda.AssociatedTokenAddress(Dev, token.Address));
            r.Submitter = new ImmediateSubmitter(r.Rig.Engine);
            var spec = Payments.PreparePayment(cfg, "unlock", new JsonObject { { "item", "chapter2" } });
            IReadOnlyCollection<(string Chain, string Token)> none = new List<(string, string)>().AsReadOnly();
            r.Ctl = new CheckoutController(cfg, spec, r.Submitter.Submit, unready: () => none, gameName: "Ruby Dreams");
            return r;
        }

        [Test]
        public void SolanaIsAMainTileWithoutACardButton()
        {
            var ctl = SetupWith(new JsonObject()).Ctl;
            var layout = ctl.NetworkLayout();
            Assert.That(layout.Main, Is.EqualTo(new[] { "ethereum", "solana" }));
            Assert.That(layout.Card, Does.Not.Contain("solana"));
            Assert.That(layout.Card, Does.Contain("ethereum"));
            ctl.PayWithCard("solana");
            Assert.That(ctl.Step, Is.EqualTo("network"));
            Assert.That(ctl.Session, Is.Null);
        }
    }
}
