// Twin of scfg_core's tests/unit/test_checkout.py and test_checkout_abandoned_start.py, ported from
// rpgmaker/src/core/checkout.test.ts and checkout_abandoned_start.test.ts (with the TS file's additions that pin the
// page's session_info and status keys). The engine runs over fake JSON-RPC nodes (CheckoutFakes.cs).
#nullable enable
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    /// <summary>One controller over a CheckoutRig, its submitter and the servers its factory made.</summary>
    public sealed class CheckoutFixture
    {
        public const string Dev = "0x52908400098527886E0F7030069857D2E4169EE7";
        public const string Payer = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
        public static readonly string Tx = "0x" + string.Concat(Enumerable.Repeat("ab", 32));
        public static string UsdcArb => Chains.Mainnet["arbitrum"].Tokens["USDC"].Address;

        public CheckoutRig Rig = null!;
        public ImmediateSubmitter Submitter = null!;
        public CheckoutController Ctl = null!;
        public readonly List<FakeServer> Servers = new List<FakeServer>();
        public Engine Engine => Rig.Engine;

        /// <summary>The TS setup(): a mainnet config paying <see cref="Dev"/>, item chapter2 at 4.99, the clock at 100.</summary>
        public static CheckoutFixture Setup(string kind = "unlock", bool mobile = false, JsonObject? methods = null,
                                            JsonObject? appearance = null, List<object?>? chains = null,
                                            Func<Chain, bool>? browserWallet = null)
        {
            var kwargs = new JsonObject { { "wallet", Dev } };
            if (methods != null) kwargs["methods"] = methods;
            if (chains != null) kwargs["chains"] = chains;
            if (appearance != null) foreach (var kv in appearance) kwargs[kv.Key] = kv.Value;
            var cfg = Config.BuildConfig(kwargs);
            Config.AddItem(cfg, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            var f = new CheckoutFixture { Rig = new CheckoutRig(cfg, 100) };
            f.Submitter = new ImmediateSubmitter(f.Rig.Engine);
            var spec = kind == "unlock"
                ? Payments.PreparePayment(cfg, "unlock", new JsonObject { { "item", "chapter2" } })
                : Payments.PreparePayment(cfg, "tip", new JsonObject { { "amount", "3" }, { "token", "USDC" } });
            f.Ctl = new CheckoutController(cfg, spec, f.Submitter.Submit, mobile, f.Factory, browserWallet: browserWallet);
            return f;
        }

        public ICheckoutServer Factory(ServerArgs args)
        {
            var s = new FakeServer(args);
            Servers.Add(s);
            return s;
        }

        /// <summary>The TS started(): the session on <paramref name="chain"/> is running.</summary>
        public static async Task<CheckoutFixture> Started(string chain = "arbitrum", string kind = "unlock", bool mobile = false,
                                                         JsonObject? methods = null, JsonObject? appearance = null)
        {
            var f = Setup(kind, mobile, methods, appearance);
            f.Ctl.SelectChain(chain);
            await f.Submitter.Idle();
            f.Ctl.Poll();
            return f;
        }

        public JsonObject Pending(string sid) => (JsonObject)Engine.Pending[sid]!;
    }

    public class CheckoutTests
    {
        private const string Dev = CheckoutFixture.Dev;
        private const string Payer = CheckoutFixture.Payer;
        private static readonly string Tx = CheckoutFixture.Tx;
        private static string UsdcArb => CheckoutFixture.UsdcArb;

        private static Task<CheckoutFixture> Started(string chain = "arbitrum", string kind = "unlock", bool mobile = false,
                                                     JsonObject? methods = null, JsonObject? appearance = null) =>
            CheckoutFixture.Started(chain, kind, mobile, methods, appearance);

        private static List<object?> L(params object?[] xs) => xs.ToList();

        [Test]
        public void InitialStateAndChainOptions()
        {
            var ctl = CheckoutFixture.Setup().Ctl;
            Assert.That(ctl.State, Is.EqualTo("choose"));
            Assert.That(ctl.Session, Is.Null);
            Assert.That(ctl.ChainOptions().Select(o => o.Key), Is.EqualTo(new[] { "arbitrum", "base", "arc", "polygon", "ethereum" }));
            Assert.That(ctl.ChainOptions()[0], Is.EqualTo(("arbitrum", "Arbitrum", "low")));
            Assert.That(ctl.PayAmountText, Is.EqualTo("4.99"));
            Assert.That(ctl.TipPresets().Select(d => d.ToString()), Is.EqualTo(new[] { "1.00", "3.00", "5.00", "10.00" }));
        }

        [Test]
        public async Task SelectChainStartsSession()
        {
            var f = await Started();
            var ctl = f.Ctl;
            Assert.That(ctl.State, Is.EqualTo("waiting"));
            Assert.That(f.Engine.Pending.ContainsKey(ctl.SessionId!), Is.True);
            Assert.That(ctl.Method, Is.EqualTo("browser"));
            Assert.That(ctl.Required, Is.EqualTo(20L));
            Assert.That(ctl.PayAmountText, Does.StartWith("4.9937"));
            Assert.That(ctl.PriceText, Is.EqualTo("4.99"));
            Assert.That(ctl.Chain!.Key, Is.EqualTo("arbitrum"));
            Assert.That(ctl.Family, Is.EqualTo("evm"));
        }

        [Test]
        public void SelectUnknownChainRaises()
        {
            var ctl = CheckoutFixture.Setup().Ctl;
            var e = Assert.Throws<ValueError>(() => ctl.SelectChain("solana"))!;
            Assert.That(e.Message, Is.EqualTo("chain 'solana' not available for this payment"));
        }

        [Test]
        public async Task SwitchingChainDiscardsOldSession()
        {
            var f = await Started();
            var old = f.Ctl.SessionId!;
            f.Ctl.SelectChain("base");
            await f.Submitter.Idle();
            f.Ctl.Poll();
            Assert.That(f.Engine.Pending.ContainsKey(old), Is.False);
            Assert.That(f.Engine.Pending.ContainsKey(f.Ctl.SessionId!), Is.True);
            Assert.That(f.Ctl.Session!["chain"], Is.EqualTo("base"));
        }

        [Test]
        public async Task StartFailureReturnsToChooseWithError()
        {
            var f = CheckoutFixture.Setup();
            f.Rig.Evm("arbitrum").Down = true;
            f.Ctl.SelectChain("arbitrum");
            await f.Submitter.Idle();
            f.Ctl.Poll();
            Assert.That(f.Ctl.State, Is.EqualTo("choose"));
            Assert.That(f.Ctl.Error, Is.EqualTo("network_unavailable"));
            Assert.That(f.Ctl.ChainKey, Is.Null);
            Assert.That(f.Ctl.Message(), Is.EqualTo(Checkout.Messages["network_unavailable"]));
        }

        [Test]
        public async Task MethodsOnMobileAndConfig()
        {
            var f = await Started(mobile: true);
            Assert.That(f.Ctl.Methods(), Is.EqualTo(new[] { "qr", "manual" }));
            f = await Started(methods: new JsonObject { { "qr", new JsonObject { { "chains", L("base") } } } });
            Assert.That(f.Ctl.Methods(), Is.EqualTo(new[] { "browser", "manual" })); // QR limited to base, we're on arbitrum
        }

        [Test]
        public async Task PaymentUri()
        {
            var ctl = (await Started()).Ctl;
            Assert.That(ctl.PaymentUri(), Is.EqualTo(
                "ethereum:" + UsdcArb + "@42161/transfer?address=" + Dev + "&uint256=" + Coio.Str(ctl.Session!["units"])));
        }

        [Test]
        public void NoPaymentUriOrExplorerUrlWithoutASession()
        {
            var ctl = CheckoutFixture.Setup().Ctl;
            Assert.That(ctl.PaymentUri(), Is.Null);
            Assert.That(ctl.ExplorerUrl(), Is.Null);
            Assert.That(ctl.StartBrowser(), Is.Null);
            Assert.That(ctl.SubmitHash(Tx), Is.False);
        }

        [Test]
        public async Task SubmitHashValidationAndHint()
        {
            var f = await Started();
            var ctl = f.Ctl;
            Assert.That(ctl.SubmitHash("garbage"), Is.False);
            Assert.That(ctl.Error, Is.EqualTo("invalid_hash"));
            ctl.TxInput = Tx;
            Assert.That(ctl.SubmitHash(), Is.True);
            Assert.That(f.Pending(ctl.SessionId!)["hint_tx"], Is.EqualTo(Tx));
            Assert.That(ctl.HintTx, Is.EqualTo(Tx));
            Assert.That(ctl.TxInput, Is.EqualTo(""));
            Assert.That(ctl.Error, Is.Null);
        }

        [Test]
        public async Task OutcomesDriveStatesToPaid()
        {
            var f = await Started();
            var ctl = f.Ctl;
            f.Rig.Evm("arbitrum").Logs.Add(FakeEvmNode.TransferLog(UsdcArb, Payer, Dev, Ints.IntFrom(ctl.Session!["units"]), 999, Tx));
            await f.Rig.Deliver(ctl);
            Assert.That(ctl.State, Is.EqualTo("confirming"));
            Assert.That(ctl.Confirmations, Is.EqualTo(2L));
            f.Rig.Evm("arbitrum").Head = 1100;
            await f.Rig.Deliver(ctl);
            Assert.That(ctl.State, Is.EqualTo("paid"));
            Assert.That(ctl.Result!.Status, Is.EqualTo("paid"));
            Assert.That(ctl.ExplorerUrl(), Is.EqualTo("https://arbiscan.io/tx/" + Tx));
            Assert.That(ctl.Finish().Status, Is.EqualTo("paid"));
        }

        [Test]
        public async Task OutcomesForOtherSessionsAreIgnored()
        {
            var ctl = (await Started()).Ctl;
            ctl.OnOutcome(new StepOutcome("ps_other", "paid"));
            Assert.That(ctl.State, Is.EqualTo("waiting"));
        }

        [Test]
        public async Task ReorgAndHintErrors()
        {
            var ctl = (await Started()).Ctl;
            ctl.HintTx = Tx;
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "confirming", confirmations: 3, required: 20));
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "reorged"));
            Assert.That(ctl.State, Is.EqualTo("waiting"));
            Assert.That(ctl.Confirmations, Is.EqualTo(0L));
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "waiting", error: "tx_not_a_payment"));
            Assert.That(ctl.Error, Is.EqualTo("tx_not_a_payment"));
            Assert.That(ctl.HintTx, Is.Null);
        }

        [Test]
        public async Task ExpiryShowsExpiredState()
        {
            var f = await Started();
            f.Rig.Clock.Advance(1800);
            await f.Rig.Deliver(f.Ctl);
            Assert.That(f.Ctl.State, Is.EqualTo("expired"));
            Assert.That(f.Ctl.Finish().Status, Is.EqualTo("expired"));
        }

        [Test]
        public async Task CancelMarksSessionAndReturnsResult()
        {
            var f = await Started();
            var result = f.Ctl.Cancel();
            Assert.That(result.Status, Is.EqualTo("cancelled"));
            Assert.That(result.SessionId, Is.EqualTo(f.Ctl.SessionId));
            Assert.That(result.Kind, Is.EqualTo("unlock"));
            Assert.That(result.Item, Is.EqualTo("chapter2"));
            Assert.That(result.Token, Is.EqualTo("USDC"));
            Assert.That(result.Chain, Is.EqualTo("arbitrum"));
            Assert.That(f.Pending(f.Ctl.SessionId!)["ui_status"], Is.EqualTo("cancelled"));
            Assert.That(f.Ctl.Cancel(), Is.SameAs(result), "a second cancel returns the same result");
        }

        [Test]
        public async Task KeepPlaying()
        {
            var f = await Started();
            Assert.That(f.Ctl.KeepPlaying().Status, Is.EqualTo("pending"));
            Assert.That(f.Pending(f.Ctl.SessionId!)["ui_status"], Is.EqualTo("pending"));
        }

        [Test]
        public void CancelBeforeChoosingChain()
        {
            var ctl = CheckoutFixture.Setup().Ctl;
            Assert.That(ctl.Cancel().Status, Is.EqualTo("cancelled"));
        }

        [Test]
        public async Task TipAmountChangesRestartSession()
        {
            var f = await Started(kind: "tip");
            var ctl = f.Ctl;
            var first = ctl.SessionId!;
            Assert.That(ctl.SetTipAmount("5"), Is.True);
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.Amount, Is.EqualTo(Dec.Parse("5.00")));
            Assert.That(ctl.Session!["price"], Is.EqualTo("5.00"));
            Assert.That(f.Engine.Pending.ContainsKey(first), Is.False);
            Assert.That(ctl.SetTipAmount("0.10"), Is.False);
            Assert.That(ctl.Error, Is.EqualTo("invalid_amount"));
            ctl.CustomAmount = " 7.50 ";
            Assert.That(ctl.ApplyCustomAmount(), Is.True);
            Assert.That(ctl.Amount, Is.EqualTo(Dec.Parse("7.50")));
        }

        [Test]
        public void ATipAmountOfTheWrongTypeIsAnInvalidAmount()
        {
            var ctl = CheckoutFixture.Setup(kind: "tip").Ctl;
            Assert.That(ctl.SetTipAmount(5.0), Is.False, "a float is a TypeError in parse_amount");
            Assert.That(ctl.Error, Is.EqualTo("invalid_amount"));
            Assert.That(ctl.SetTipAmount(null), Is.False);
            Assert.That(ctl.SetTipAmount(new JsonObject()), Is.False);
            Assert.That(ctl.SetTipAmount(2L), Is.True);
            Assert.That(ctl.Amount, Is.EqualTo(Dec.Parse("2.00")));
            Assert.That(ctl.Error, Is.Null);
        }

        [Test]
        public async Task TipAmountIgnoredForUnlocks()
        {
            var ctl = (await Started()).Ctl;
            Assert.That(ctl.SetTipAmount("1"), Is.False);
        }

        [Test]
        public async Task BrowserServerLifecycleAndHints()
        {
            var f = await Started();
            var ctl = f.Ctl;
            var url = ctl.StartBrowser();
            Assert.That(url, Is.EqualTo("http://127.0.0.1:1234/c/secret/"));
            Assert.That(ctl.StartBrowser(), Is.EqualTo(url));
            Assert.That(f.Servers, Has.Count.EqualTo(1), "reused");
            var s = f.Servers[0];
            Assert.That(s.Started, Is.True);
            Assert.That(s.SessionInfo["amount_units"], Is.EqualTo(Coio.Str(ctl.Session!["units"])));
            Assert.That(s.SessionInfo["chain_id_hex"], Is.EqualTo("0xa4b1"));
            Assert.That(s.SessionInfo["logo"], Is.EqualTo(false));
            Assert.That(s.SessionInfo["accent"], Is.EqualTo("#0e7c74"));
            Assert.That(s.SessionInfo["on_accent"], Is.EqualTo("#ffffff"));
            Assert.That(s.Args.BuildTx, Is.Null, "EVM: no build_tx");
            s.Args.OnTx(Tx, Payer); // the page reports the tx
            Assert.That(f.Pending(ctl.SessionId!)["hint_tx"], Is.EqualTo(Tx));
            Assert.That(s.Args.GetStatus()["tx_hash"], Is.EqualTo(Tx));
            ctl.Cancel();
            Assert.That(s.Stopped, Is.True);
        }

        [Test]
        public async Task BrowserFailureFallsBack()
        {
            var f = await Started();
            var ctl = f.Ctl;
            ctl.ServerFactory = args => throw new InvalidOperationException("no sockets");
            Assert.That(ctl.StartBrowser(), Is.Null);
            Assert.That(ctl.Error, Is.EqualTo("browser_unavailable"));
            Assert.That(ctl.Methods(), Does.Not.Contain("browser"));
            Assert.That(ctl.Method, Is.EqualTo("qr"));
        }

        [Test]
        public async Task AServerWhoseStartThrowsFallsBackToo()
        {
            var f = await Started();
            var ctl = f.Ctl;
            FailingServer? made = null;
            ctl.ServerFactory = args => made = new FailingServer(args);
            Assert.That(ctl.StartBrowser(), Is.Null);
            Assert.That(made, Is.Not.Null);
            Assert.That(ctl.Error, Is.EqualTo("browser_unavailable"));
            Assert.That(ctl.BrowserFailed, Is.True);
            Assert.That(ctl.Method, Is.EqualTo("qr"));
            ctl.Poll(); // the failed server was dropped: nothing to pump
            Assert.That(made!.Pumps, Is.EqualTo(0));
        }

        [Test]
        public async Task BrowserSessionInfoCarriesAppearance()
        {
            var f = await Started(appearance: new JsonObject { { "logo", "gui/logo.png" }, { "accent", "#ffcc00" } });
            f.Ctl.StartBrowser();
            var info = f.Servers[0].SessionInfo;
            Assert.That(info["logo"], Is.EqualTo(true));
            Assert.That(info["accent"], Is.EqualTo("#ffcc00"));
            Assert.That(info["on_accent"], Is.EqualTo("#17202e"));
        }

        [TestCase("arc", true, "20000000000", "USDC")] // USDC is Arc's gas coin; 20 gwei mempool floor
        [TestCase("base", false, "0", "ETH")]
        public async Task BrowserInfoTellsThePageHowGasIsPaid(string chain, bool gasIsToken, string minFee, string native)
        {
            var f = CheckoutFixture.Setup();
            f.Ctl.SelectChain(chain);
            await f.Submitter.Idle();
            f.Ctl.Poll();
            f.Ctl.StartBrowser();
            var info = f.Servers[0].SessionInfo;
            Assert.That(info["gas_is_token"], Is.EqualTo(gasIsToken));
            Assert.That(info["min_fee_wei"], Is.EqualTo(minFee));
            Assert.That(info["native_symbol"], Is.EqualTo(native));
            Assert.That(info["chain_id"], Is.EqualTo(Chains.Mainnet[chain].ChainId));
        }

        // --- additions with no Python twin: the shared page reads session_info, so pin it exactly ---

        [Test]
        public async Task SessionInfoIsTheExactPythonDict()
        {
            var f = await Started();
            var ctl = f.Ctl;
            ctl.StartBrowser();
            var info = f.Servers[0].SessionInfo;
            var units = ctl.Session!["units"];
            Assert.That(info.Keys, Is.EqualTo(new[]
            {
                "title", "kind", "token", "family", "decimals", "amount_units", "amount_text", "price_text",
                "recipient", "chain_name", "native_symbol", "explorer_tx", "testnet", "logo", "accent", "on_accent",
                "chain_id", "chain_id_hex", "rpc_url", "token_address", "gas_is_token", "min_fee_wei",
            }));
            var expected = new JsonObject
            {
                { "title", "Chapter 2" }, { "kind", "unlock" }, { "token", "USDC" }, { "family", "evm" }, { "decimals", 6L },
                { "amount_units", Coio.Str(units) }, { "amount_text", ctl.PayAmountText }, { "price_text", "4.99" },
                { "recipient", Dev }, { "chain_name", "Arbitrum" }, { "native_symbol", "ETH" },
                { "explorer_tx", "https://arbiscan.io/tx/{}" }, { "testnet", false }, { "logo", false }, { "accent", "#0e7c74" },
                { "on_accent", "#ffffff" }, { "chain_id", 42161L }, { "chain_id_hex", "0xa4b1" },
                { "rpc_url", "https://arbitrum-one-rpc.publicnode.com" }, { "token_address", UsdcArb },
                { "gas_is_token", false }, { "min_fee_wei", "0" },
            };
            Assert.That(Json.Dumps(info), Is.EqualTo(Json.Dumps(expected)));
            foreach (var kv in expected) Assert.That(info[kv.Key]?.GetType(), Is.EqualTo(kv.Value?.GetType()), kv.Key);
            Assert.That(info["amount_units"], Is.TypeOf<string>());
        }

        [Test]
        public async Task TheChainIdHexIsLowercaseUnderAnyCulture()
        {
            var saved = System.Globalization.CultureInfo.CurrentCulture;
            try
            {
                System.Globalization.CultureInfo.CurrentCulture = new System.Globalization.CultureInfo("tr-TR");
                var f = await Started("polygon");
                f.Ctl.StartBrowser();
                Assert.That(f.Servers[0].SessionInfo["chain_id_hex"], Is.EqualTo("0x89"));
                Assert.That(f.Servers[0].SessionInfo["chain_id"], Is.EqualTo(137L));
            }
            finally
            {
                System.Globalization.CultureInfo.CurrentCulture = saved;
            }
        }

        [Test]
        public async Task StatusInfoKeys()
        {
            var ctl = (await Started()).Ctl;
            var expected = new JsonObject
            {
                { "state", "waiting" }, { "confirmations", 0L }, { "required", 20L }, { "tx_hash", null }, { "error", null },
                { "dropped_tx", null },
            };
            Assert.That(Json.Dumps(ctl.StatusInfo()), Is.EqualTo(Json.Dumps(expected)));
            Assert.That(ctl.StatusInfo().Keys, Is.EqualTo(new[] { "state", "confirmations", "required", "tx_hash", "error", "dropped_tx" }));
            Assert.That(ctl.StatusInfo()["confirmations"], Is.TypeOf<long>());
            Assert.That(ctl.StatusInfo()["required"], Is.TypeOf<long>());
        }

        [Test]
        public void PollTreatsACancelledJobAsAFailedStart()
        {
            // a submit whose job was cancelled before it ran (a stopped Worker)
            var f = CheckoutFixture.Setup();
            var ctl = new CheckoutController(f.Rig.Config, f.Ctl.Spec, command =>
            {
                var job = new Job();
                job.Cancel();
                return job;
            });
            ctl.SelectChain("arbitrum");
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("choose"));
            Assert.That(ctl.Error, Is.EqualTo("start_failed"));
            Assert.That(ctl.ChainKey, Is.Null);
        }

        [Test]
        public void AStartThatFailsForAnyOtherReasonIsStartFailed()
        {
            var f = CheckoutFixture.Setup();
            var ctl = new CheckoutController(f.Rig.Config, f.Ctl.Spec, command =>
            {
                var job = new Job();
                job.SetRunningOrNotifyCancel();
                job.SetException(new KeyError("boom"));
                return job;
            });
            ctl.ChooseNetwork("base");
            ctl.Poll();
            Assert.That(ctl.Error, Is.EqualTo("start_failed"));
            Assert.That(ctl.Step, Is.EqualTo("network"));
        }

        [Test]
        public void AStartThatSucceedsWithoutASessionIsStartFailed()
        {
            // checkout.gd's rule (Python would raise on the value later): a stand-in submit that answers null
            var f = CheckoutFixture.Setup();
            var ctl = new CheckoutController(f.Rig.Config, f.Ctl.Spec, command =>
            {
                var job = new Job();
                job.SetRunningOrNotifyCancel();
                job.SetResult(null);
                return job;
            });
            ctl.SelectChain("base");
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("choose"));
            Assert.That(ctl.Error, Is.EqualTo("start_failed"));
            Assert.That(ctl.Session, Is.Null);
            ctl.Cancel(); // nothing to discard either
        }

        [Test]
        public void PollWaitsForAJobStillRunning()
        {
            var f = CheckoutFixture.Setup();
            var job = new Job();
            var ctl = new CheckoutController(f.Rig.Config, f.Ctl.Spec, command => job);
            ctl.SelectChain("base");
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("starting"));
            job.SetRunningOrNotifyCancel();
            job.SetResult(new JsonObject { { "id", "ps_1" }, { "units", 4993742L }, { "chain", "base" } });
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("waiting"));
            Assert.That(ctl.SessionId, Is.EqualTo("ps_1"));
            Assert.That(ctl.Required, Is.EqualTo(Chains.Mainnet["base"].Confirmations));
            Assert.That(ctl.PayAmountText, Is.EqualTo("4.993742"));
        }

        [Test]
        public void MessagesMatchCatalogue()
        {
            var path = Path.Combine(Vectors.Root, "messages.json");
            var file = (JsonObject)Json.Loads(File.ReadAllText(path, new UTF8Encoding(false)))!;
            Assert.That(file.Keys, Is.EqualTo(new[] { "messages" }));
            var catalogue = (JsonObject)file["messages"]!;
            var ours = Checkout.Messages.OrderBy(kv => kv.Key, StringComparer.Ordinal).ToList();
            var theirs = catalogue.OrderBy(kv => kv.Key, StringComparer.Ordinal).Select(kv => (kv.Key, kv.Value as string)).ToList();
            Assert.That(ours.Select(kv => (kv.Key, (string?)kv.Value)), Is.EqualTo(theirs));
            Assert.That(Checkout.Messages["invalid_amount"], Does.Contain(Payments.MinTip.ToString()));
        }

        [Test]
        public async Task AnErrorWithNoMessageShowsNone()
        {
            var ctl = (await Started()).Ctl;
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "waiting", error: "something_new"));
            Assert.That(ctl.Error, Is.EqualTo("something_new"));
            Assert.That(ctl.Message(), Is.Null);
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "waiting", error: "internal_error"));
            Assert.That(ctl.Message(), Is.EqualTo(Checkout.Messages["internal_error"]));
        }

        [Test]
        public void OutcomesBeforeASessionAreIgnored()
        {
            var ctl = CheckoutFixture.Setup().Ctl;
            ctl.OnOutcome(new StepOutcome("ps_x", "waiting", error: "network_unavailable"));
            Assert.That(ctl.Error, Is.Null);
        }

        [Test]
        public void Constants()
        {
            Assert.That(Checkout.FinalStates, Is.EqualTo(new[] { "paid", "underpaid", "expired" }));
            Assert.That(Checkout.HintVerdicts, Is.EqualTo(new[] { "tx_reverted", "tx_not_a_payment", "tx_not_landed" }));
            Assert.That(Checkout.StepOrder.Select(kv => (kv.Key, kv.Value)), Is.EqualTo(new[]
            {
                ("amount", 0), ("network", 1), ("method", 2), ("browser", 3), ("qr", 3), ("manual", 3), ("waiting", 4), ("done", 5),
            }));
        }

        [Test]
        public void TheTablesAreReadOnly()
        {
            Assert.That(Checkout.FinalStates, Is.Not.InstanceOf<string[]>());
            Assert.Throws<NotSupportedException>(() => ((IList<string>)Checkout.FinalStates)[0] = "x");
            Assert.Throws<NotSupportedException>(() => ((IList<string>)Checkout.HintVerdicts)[0] = "x");
            Assert.Throws<NotSupportedException>(() => ((IDictionary<string, int>)Checkout.StepOrder)["done"] = 9);
            Assert.Throws<NotSupportedException>(() => ((IDictionary<string, string>)Checkout.Messages)["x"] = "y");
        }

        [Test]
        public void TheRequiredArgumentsAreChecked()
        {
            var f = CheckoutFixture.Setup();
            Assert.Throws<TypeError>(() => new CheckoutController(null!, f.Ctl.Spec, f.Submitter.Submit));
            Assert.Throws<TypeError>(() => new CheckoutController(f.Rig.Config, null!, f.Submitter.Submit));
            Assert.Throws<TypeError>(() => new CheckoutController(f.Rig.Config, f.Ctl.Spec, null!));
            Assert.That(new CheckoutController(f.Rig.Config, f.Ctl.Spec, f.Submitter.Submit, gameName: "").GameName, Is.EqualTo("Game"));
        }
    }

    /// <summary>A start the player left must not leave a session behind (test_checkout_abandoned_start.py).</summary>
    public class CheckoutAbandonedStartTests
    {
        private static (CheckoutController Ctl, Engine Engine, DeferredSubmitter Submit, FakeClock Clock) Setup(string kind = "unlock")
        {
            var cfg = Config.BuildConfig(new JsonObject { { "wallet", CheckoutFixture.Dev } });
            Config.AddItem(cfg, "chapter2", new JsonObject { { "code", 37L }, { "price", "4.99" }, { "token", "USDC" }, { "title", "Chapter 2" } });
            var rig = new CheckoutRig(cfg, 100);
            var spec = kind == "unlock"
                ? Payments.PreparePayment(cfg, "unlock", new JsonObject { { "item", "chapter2" } })
                : Payments.PreparePayment(cfg, "tip", new JsonObject { { "amount", "3" }, { "token", "USDC" } });
            var submit = new DeferredSubmitter(rig.Engine);
            var ctl = new CheckoutController(cfg, spec, submit.Call);
            return (ctl, rig.Engine, submit, rig.Clock);
        }

        private static List<object?> Chains(Engine e) => e.Pending.Values.Select(r => ((JsonObject)r!)["chain"]).ToList();

        [Test]
        public async Task SwitchingNetworkWhileStartingDiscardsTheFirstSession()
        {
            var (ctl, engine, submit, _) = Setup();
            ctl.ChooseNetwork("arbitrum");
            ctl.ChooseNetwork("base");
            await submit.Run();
            ctl.Poll();
            Assert.That(ctl.State, Is.EqualTo("waiting"));
            Assert.That(ctl.ChainKey, Is.EqualTo("base"));
            await submit.Run(); // the discard queued once the abandoned start finished
            Assert.That(Chains(engine), Is.EqualTo(new[] { "base" }));
        }

        [Test]
        public async Task ChangingTheTipWhileStartingDiscardsTheFirstSession()
        {
            var (ctl, engine, submit, _) = Setup("tip");
            ctl.NextStep();
            ctl.ChooseNetwork("arbitrum");
            Assert.That(ctl.SetTipAmount("5"), Is.True);
            await submit.Run();
            ctl.Poll();
            await submit.Run();
            Assert.That(engine.Pending.Count, Is.EqualTo(1));
            Assert.That(((JsonObject)engine.Pending.Values[0]!)["price"], Is.EqualTo("5.00"));
        }

        [Test]
        public async Task CancelWhileStartingLeavesNoSessionToExpire()
        {
            var (ctl, engine, submit, clock) = Setup();
            ctl.ChooseNetwork("arbitrum");
            Assert.That(ctl.Cancel().Status, Is.EqualTo("cancelled"));
            await submit.Run();
            await submit.Run();
            Assert.That(engine.Pending.Count, Is.EqualTo(0));
            clock.Advance(31 * 60);
            Assert.That((await engine.Tick()).Where(o => o.Status == "expired"), Is.Empty);
        }

        [Test]
        public async Task AFailedAbandonedStartNeedsNoDiscard()
        {
            var (ctl, engine, submit, _) = Setup();
            ctl.ChooseNetwork("arbitrum");
            ctl.Cancel();
            var job = submit.Queue[0].Job;
            submit.Queue.RemoveAt(0);
            job.SetRunningOrNotifyCancel();
            job.SetException(new RpcUnavailable("rpc down"));
            await submit.Run();
            Assert.That(submit.Queue, Is.Empty, "no discard was submitted");
            Assert.That(submit.CallbackErrors, Is.Empty, "the failed start was not read as a session");
            Assert.That(engine.Pending.Count, Is.EqualTo(0));
        }

        [Test]
        public async Task ACancelledAbandonedStartNeedsNoDiscard()
        {
            var (ctl, engine, submit, _) = Setup();
            ctl.ChooseNetwork("arbitrum");
            var job = submit.Queue[0].Job;
            ctl.Cancel();
            Assert.That(job.Cancel(), Is.True); // a stopped Worker cancels the queued start
            await submit.Run();
            Assert.That(submit.Queue, Is.Empty);
            Assert.That(submit.CallbackErrors, Is.Empty);
            Assert.That(engine.Pending.Count, Is.EqualTo(0));
        }

        [Test]
        public async Task AStartAbandonedAfterItFinishedIsDiscardedAtOnce()
        {
            var (ctl, engine, submit, _) = Setup();
            ctl.ChooseNetwork("arbitrum");
            await submit.Run(); // the start is done, but the controller has not polled it yet
            Assert.That(engine.Pending.Count, Is.EqualTo(1));
            ctl.Cancel(); // the done callback runs at once and queues the discard
            Assert.That(submit.Queue, Has.Count.EqualTo(1));
            await submit.Run();
            Assert.That(engine.Pending.Count, Is.EqualTo(0));
        }
    }
}
