// Twin of scfg_core's tests/unit/test_checkout_steps.py, ported from rpgmaker/src/core/checkout_steps.test.ts: the
// stepped payment sheet and its navigation.
#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using NUnit.Framework;
using Scfg.Core;

namespace Scfg.Core.Tests
{
    public class CheckoutStepsTests
    {
        private const string Dev = CheckoutFixture.Dev;
        private const string Payer = CheckoutFixture.Payer;
        private static readonly string Tx = CheckoutFixture.Tx;
        private static string UsdcArb => CheckoutFixture.UsdcArb;

        private static JsonObject OnlyManual() => new JsonObject
        {
            { "browser", new JsonObject { { "enabled", false } } }, { "qr", new JsonObject { { "enabled", false } } },
        };

        private static List<object?> L(params object?[] xs) => xs.ToList();

        private static CheckoutFixture Setup(string kind = "unlock", List<object?>? chains = null, JsonObject? methods = null,
                                             Func<Chain, bool>? browserWallet = null) =>
            CheckoutFixture.Setup(kind, methods: methods, chains: chains, browserWallet: browserWallet);

        private static async Task<CheckoutFixture> OnMethodStep(string kind = "unlock", JsonObject? methods = null,
                                                                Func<Chain, bool>? browserWallet = null)
        {
            var f = Setup(kind, methods: methods, browserWallet: browserWallet);
            f.Ctl.ChooseNetwork("arbitrum");
            await f.Submitter.Idle();
            f.Ctl.Poll();
            return f;
        }

        private static void PayOnArbitrum(CheckoutFixture f) =>
            f.Rig.Evm("arbitrum").Logs.Add(FakeEvmNode.TransferLog(UsdcArb, Payer, Dev, Ints.IntFrom(f.Ctl.Session!["units"]), 999, Tx));

        // --- where the sheet starts ---------------------------------------------------------

        [Test]
        public void UnlockStartsOnNetworkAndTipOnAmount()
        {
            var ctl = Setup().Ctl;
            Assert.That(ctl.Step, Is.EqualTo("network"));
            Assert.That(ctl.CanGoBack, Is.False);
            ctl = Setup(kind: "tip").Ctl;
            Assert.That(ctl.Step, Is.EqualTo("amount"));
            Assert.That(ctl.CanGoBack, Is.False);
        }

        [Test]
        public void StepPathCountsTheStepsShown()
        {
            var ctl = Setup().Ctl;
            Assert.That(ctl.StepPath(), Is.EqualTo(new[] { "network", "method", "browser", "waiting" }));
            Assert.That(ctl.StepDots(), Is.EqualTo((0, 4)));
            ctl = Setup(kind: "tip").Ctl;
            Assert.That(ctl.StepPath(), Is.EqualTo(new[] { "amount", "network", "method", "browser", "waiting" }));
        }

        // --- forward ------------------------------------------------------------------------

        [Test]
        public async Task NetworkWaitsForTheSessionThenMovesToMethod()
        {
            var f = Setup();
            var ctl = f.Ctl;
            ctl.ChooseNetwork("arbitrum");
            Assert.That(ctl.Step, Is.EqualTo("network"));
            Assert.That(ctl.State, Is.EqualTo("starting"));
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.Step, Is.EqualTo("method"));
            Assert.That(ctl.NavDir, Is.EqualTo(1));
            Assert.That(f.Engine.Pending.ContainsKey(ctl.SessionId!), Is.True);
            Assert.That(ctl.StepDots(), Is.EqualTo((1, 4)));
        }

        [Test]
        public async Task ChoosingAMethodOpensItsPage()
        {
            foreach (var name in new[] { "browser", "qr", "manual" })
            {
                var ctl = (await OnMethodStep()).Ctl;
                ctl.ChooseMethod(name);
                Assert.That(ctl.Step, Is.EqualTo(name));
                Assert.That(ctl.Method, Is.EqualTo(name));
            }
        }

        [Test]
        public async Task UnknownMethodIsIgnored()
        {
            var ctl = (await OnMethodStep(methods: new JsonObject { { "qr", new JsonObject { { "enabled", false } } } })).Ctl;
            ctl.ChooseMethod("qr");
            Assert.That(ctl.Step, Is.EqualTo("method"));
        }

        [Test]
        public async Task OpeningTheBrowserMovesToWaiting()
        {
            var ctl = (await OnMethodStep()).Ctl;
            ctl.ChooseMethod("browser");
            Assert.That(ctl.OpenBrowser(), Is.EqualTo(FakeServer.PageUrl));
            Assert.That(ctl.Step, Is.EqualTo("waiting"));
            Assert.That(ctl.CanGoBack, Is.True);
        }

        [Test]
        public async Task BrowserFailureReturnsToMethod()
        {
            var ctl = (await OnMethodStep()).Ctl;
            ctl.ChooseMethod("browser");
            ctl.ServerFactory = args => throw new InvalidOperationException("no sockets");
            Assert.That(ctl.OpenBrowser(), Is.Null);
            Assert.That(ctl.Step, Is.EqualTo("method"));
            Assert.That(ctl.NavDir, Is.EqualTo(-1));
            Assert.That(ctl.Error, Is.EqualTo("browser_unavailable"));
            Assert.That(ctl.Method, Is.EqualTo("qr"));
        }

        [Test]
        public async Task BrowserFailureWithOneMethodLeftGoesToIt()
        {
            var methods = new JsonObject { { "qr", new JsonObject { { "enabled", false } } } };
            var ctl = (await OnMethodStep(methods: methods)).Ctl;
            ctl.ChooseMethod("browser");
            ctl.ServerFactory = args => new FailingServer(args);
            Assert.That(ctl.OpenBrowser(), Is.Null);
            Assert.That(ctl.Step, Is.EqualTo("manual"));
            Assert.That(ctl.NavDir, Is.EqualTo(-1));
            Assert.That(ctl.Method, Is.EqualTo("manual"));
        }

        [Test]
        public async Task IveSentItMovesQrAndManualToWaiting()
        {
            foreach (var name in new[] { "qr", "manual" })
            {
                var ctl = (await OnMethodStep()).Ctl;
                ctl.ChooseMethod(name);
                ctl.MarkSent();
                Assert.That(ctl.Step, Is.EqualTo("waiting"));
                Assert.That(ctl.CanGoBack, Is.True);
            }
        }

        [Test]
        public async Task IveSentItIsIgnoredOnTheBrowserPage()
        {
            var ctl = (await OnMethodStep()).Ctl;
            ctl.ChooseMethod("browser");
            ctl.MarkSent();
            Assert.That(ctl.Step, Is.EqualTo("browser"));
        }

        [Test]
        public async Task IveSentItClearsABadHashError()
        {
            var ctl = (await OnMethodStep()).Ctl;
            ctl.ChooseMethod("manual");
            ctl.SubmitHash("garbage");
            Assert.That(ctl.Error, Is.EqualTo("invalid_hash"));
            ctl.MarkSent();
            Assert.That(ctl.Step, Is.EqualTo("waiting"));
            Assert.That(ctl.Error, Is.Null);
        }

        [Test]
        public void TipAmountStepAppliesATypedAmount()
        {
            var ctl = Setup(kind: "tip").Ctl;
            ctl.CustomAmount = "0.10";
            Assert.That(ctl.NextStep(), Is.False);
            Assert.That(ctl.Step, Is.EqualTo("amount"));
            Assert.That(ctl.Error, Is.EqualTo("invalid_amount"));
            ctl.CustomAmount = "7.50";
            Assert.That(ctl.NextStep(), Is.True);
            Assert.That(ctl.Step, Is.EqualTo("network"));
            Assert.That(ctl.Amount.ToString(), Is.EqualTo("7.50"));
            Assert.That(ctl.NextStep(), Is.False, "only the amount step moves on");
        }

        [Test]
        public void ATypedAmountIsStrippedAsStrStripDoes()
        {
            // Python's str.strip(): U+0085 and U+001C are whitespace, U+FEFF is not
            var ctl = Setup(kind: "tip").Ctl;
            ctl.CustomAmount = "\u0085\u001c";
            Assert.That(ctl.NextStep(), Is.True);
            Assert.That(ctl.Step, Is.EqualTo("network"));
            ctl = Setup(kind: "tip").Ctl;
            ctl.CustomAmount = "﻿7.50";
            Assert.That(ctl.NextStep(), Is.False);
            Assert.That(ctl.Error, Is.EqualTo("invalid_amount"));
            Assert.That(ctl.ApplyCustomAmount(), Is.False);
            ctl.CustomAmount = "\u00857.50\u001c";
            Assert.That(ctl.ApplyCustomAmount(), Is.True);
            Assert.That(ctl.Amount.ToString(), Is.EqualTo("7.50"));
        }

        // --- skipping single-option steps -------------------------------------------------------

        [Test]
        public async Task SingleNetworkIsSkippedForUnlocks()
        {
            var f = Setup(chains: L("base"));
            var ctl = f.Ctl;
            Assert.That(ctl.State, Is.EqualTo("starting"));
            Assert.That(ctl.ChainKey, Is.EqualTo("base"));
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.Step, Is.EqualTo("method"));
            Assert.That(ctl.CanGoBack, Is.False);
            Assert.That(ctl.StepPath(), Is.EqualTo(new[] { "method", "browser", "waiting" }));
        }

        [Test]
        public async Task SingleNetworkIsSkippedForTipsBothWays()
        {
            var f = Setup(kind: "tip", chains: L("base"));
            var ctl = f.Ctl;
            ctl.NextStep();
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.Step, Is.EqualTo("method"));
            ctl.Back();
            Assert.That(ctl.Step, Is.EqualTo("amount"));
            Assert.That(ctl.NavDir, Is.EqualTo(-1));
        }

        [Test]
        public async Task SingleMethodIsSkippedBothWays()
        {
            var ctl = (await OnMethodStep(methods: OnlyManual())).Ctl;
            Assert.That(ctl.Step, Is.EqualTo("manual"));
            ctl.Back();
            Assert.That(ctl.Step, Is.EqualTo("network"));
        }

        // --- back -------------------------------------------------------------------------------

        [Test]
        public async Task BackWalksThePathAndKeepsTheSession()
        {
            var f = await OnMethodStep();
            var ctl = f.Ctl;
            var sid = ctl.SessionId!;
            ctl.ChooseMethod("qr");
            ctl.MarkSent();
            ctl.Back();
            Assert.That(ctl.Step, Is.EqualTo("qr"));
            Assert.That(ctl.NavDir, Is.EqualTo(-1));
            ctl.Back();
            Assert.That(ctl.Step, Is.EqualTo("method"));
            ctl.Back();
            Assert.That(ctl.Step, Is.EqualTo("network"));
            Assert.That(ctl.CanGoBack, Is.False);
            ctl.Back(); // nothing before network on an unlock
            Assert.That(ctl.Step, Is.EqualTo("network"));
            Assert.That(ctl.SessionId, Is.EqualTo(sid));
            Assert.That(f.Engine.Pending.ContainsKey(sid), Is.True);
        }

        [Test]
        public async Task ChoosingTheSameNetworkAgainKeepsTheSession()
        {
            var ctl = (await OnMethodStep()).Ctl;
            var sid = ctl.SessionId;
            ctl.Back();
            ctl.ChooseNetwork("arbitrum");
            Assert.That(ctl.Step, Is.EqualTo("method"));
            Assert.That(ctl.SessionId, Is.EqualTo(sid));
            Assert.That(ctl.NavDir, Is.EqualTo(1));
        }

        [Test]
        public async Task ChoosingAnotherNetworkRestartsTheSession()
        {
            var f = await OnMethodStep();
            var ctl = f.Ctl;
            var sid = ctl.SessionId!;
            ctl.Back();
            ctl.ChooseNetwork("base");
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.Step, Is.EqualTo("method"));
            Assert.That(ctl.SessionId, Is.Not.EqualTo(sid));
            Assert.That(f.Engine.Pending.ContainsKey(sid), Is.False);
        }

        [Test]
        public async Task BackToAmountAndSameAmountKeepsTheSession()
        {
            var f = await OnMethodStep(kind: "tip");
            var ctl = f.Ctl;
            var sid = ctl.SessionId;
            ctl.Back();
            ctl.Back();
            Assert.That(ctl.Step, Is.EqualTo("amount"));
            Assert.That(ctl.SetTipAmount("3"), Is.True);
            Assert.That(ctl.SessionId, Is.EqualTo(sid));
            Assert.That(ctl.SetTipAmount("5"), Is.True);
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.SessionId, Is.Not.EqualTo(sid));
            Assert.That(ctl.Step, Is.EqualTo("amount"));
            ctl.NextStep();
            ctl.ChooseNetwork("arbitrum");
            Assert.That(ctl.Step, Is.EqualTo("method"));
        }

        [Test]
        public async Task BackClearsTheMessage()
        {
            var ctl = (await OnMethodStep()).Ctl;
            ctl.ChooseMethod("manual");
            ctl.SubmitHash("garbage");
            Assert.That(ctl.Error, Is.EqualTo("invalid_hash"));
            ctl.Back();
            Assert.That(ctl.Error, Is.Null);
        }

        // --- automatic moves ----------------------------------------------------------------------

        [Test]
        public async Task ASeenPaymentMovesToWaitingAndHidesBack()
        {
            var f = await OnMethodStep();
            var ctl = f.Ctl;
            ctl.ChooseMethod("qr");
            PayOnArbitrum(f);
            await f.Rig.Deliver(ctl);
            Assert.That(ctl.State, Is.EqualTo("confirming"));
            Assert.That(ctl.Step, Is.EqualTo("waiting"));
            Assert.That(ctl.NavDir, Is.EqualTo(1));
            Assert.That(ctl.CanGoBack, Is.False);
        }

        [Test]
        public async Task APastedHashMovesToWaitingAndHidesBack()
        {
            var ctl = (await OnMethodStep()).Ctl;
            ctl.ChooseMethod("manual");
            Assert.That(ctl.SubmitHash(Tx), Is.True);
            Assert.That(ctl.Step, Is.EqualTo("waiting"));
            Assert.That(ctl.CanGoBack, Is.False);
        }

        [Test]
        public async Task AHashFromTheBrowserPageMovesToWaiting()
        {
            var f = await OnMethodStep();
            var ctl = f.Ctl;
            ctl.ChooseMethod("browser");
            ctl.OpenBrowser();
            f.Servers[0].Args.OnTx(Tx, Payer);
            Assert.That(ctl.Step, Is.EqualTo("waiting"));
            Assert.That(ctl.CanGoBack, Is.False);
        }

        [Test]
        public async Task ADroppedTransactionGivesBackTheArrow()
        {
            var ctl = (await OnMethodStep()).Ctl;
            ctl.ChooseMethod("manual");
            ctl.SubmitHash(Tx);
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "waiting", error: "tx_not_landed", hintTx: Tx));
            Assert.That(ctl.Step, Is.EqualTo("waiting"));
            Assert.That(ctl.CanGoBack, Is.True);
            ctl.Back();
            Assert.That(ctl.Step, Is.EqualTo("manual"));
        }

        [Test]
        public async Task FinalStatesMoveToDone()
        {
            var f = await OnMethodStep();
            f.Ctl.ChooseMethod("qr");
            PayOnArbitrum(f);
            await f.Rig.Deliver(f.Ctl);
            f.Rig.Evm("arbitrum").Head = 1100;
            await f.Rig.Deliver(f.Ctl);
            Assert.That(f.Ctl.State, Is.EqualTo("paid"));
            Assert.That(f.Ctl.Step, Is.EqualTo("done"));
            Assert.That(f.Ctl.CanGoBack, Is.False);

            f = await OnMethodStep();
            f.Ctl.ChooseMethod("browser");
            f.Rig.Clock.Advance(1800);
            await f.Rig.Deliver(f.Ctl);
            Assert.That(f.Ctl.State, Is.EqualTo("expired"));
            Assert.That(f.Ctl.Step, Is.EqualTo("done"));
        }

        [Test]
        public async Task ALateExpiryDoesNotUndoAPayment()
        {
            // The controller's point, not the chain's: hand-built outcomes.
            var ctl = (await OnMethodStep()).Ctl;
            var paid = new PaymentResult("paid", "unlock") { SessionId = ctl.SessionId };
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "paid", result: paid));
            ctl.OnOutcome(new StepOutcome(ctl.SessionId!, "expired", result: new PaymentResult("expired", "unlock")));
            Assert.That(ctl.State, Is.EqualTo("paid"));
            Assert.That(ctl.Result, Is.SameAs(paid));
            Assert.That(ctl.Cancel(), Is.SameAs(paid), "cancel after a final state keeps its result");
        }

        [Test]
        public async Task StartFailureReturnsToNetworkWithTheMessage()
        {
            var f = Setup();
            f.Rig.Evm("arbitrum").Down = true;
            f.Ctl.ChooseNetwork("arbitrum");
            await f.Submitter.Idle();
            f.Ctl.Poll();
            Assert.That(f.Ctl.Step, Is.EqualTo("network"));
            Assert.That(f.Ctl.Error, Is.EqualTo("network_unavailable"));
        }

        [Test]
        public async Task StartFailureOnASkippedNetworkShowsTheNetworkStep()
        {
            var f = Setup(kind: "tip", chains: L("base"));
            var ctl = f.Ctl;
            f.Rig.Evm("base").Down = true;
            ctl.NextStep();
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.Step, Is.EqualTo("network"));
            Assert.That(ctl.Error, Is.EqualTo("network_unavailable"));
            Assert.That(ctl.CanGoBack, Is.True);
            f.Rig.Evm("base").Down = false;
            ctl.ChooseNetwork("base");
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.Step, Is.EqualTo("method"));
        }

        [Test]
        public async Task ASessionRestartedOnTheAmountStepWaitsForANetworkPick()
        {
            var f = Setup(kind: "tip");
            var ctl = f.Ctl;
            ctl.NextStep();
            ctl.ChooseNetwork("arbitrum");
            await f.Submitter.Idle();
            ctl.Poll();
            ctl.Back();
            ctl.Back();
            Assert.That(ctl.Step, Is.EqualTo("amount"));
            ctl.SetTipAmount("5"); // restarts the session on arbitrum
            ctl.NextStep();
            await f.Submitter.Idle();
            ctl.Poll(); // ... which is ready now
            Assert.That(ctl.Step, Is.EqualTo("network")); // but the player hasn't picked yet
            ctl.ChooseNetwork("arbitrum");
            Assert.That(ctl.Step, Is.EqualTo("method"));
        }

        [Test]
        public async Task LeavingTheNetworkStepWhileStartingStaysPut()
        {
            var f = Setup(kind: "tip");
            var ctl = f.Ctl;
            ctl.NextStep();
            ctl.ChooseNetwork("arbitrum");
            ctl.Back();
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.Step, Is.EqualTo("amount"));
            Assert.That(ctl.State, Is.EqualTo("waiting"));
        }

        // --- web build ----------------------------------------------------------------------

        [Test]
        public async Task WebOffersBrowserWalletOnlyWhenThePageHasOne()
        {
            var ctl = (await OnMethodStep(browserWallet: c => false)).Ctl;
            Assert.That(ctl.Methods(), Is.EqualTo(new[] { "qr", "manual" }));
            ctl = (await OnMethodStep(browserWallet: c => c.Key == "arbitrum")).Ctl;
            Assert.That(ctl.Methods(), Is.EqualTo(new[] { "browser", "qr", "manual" }));
        }

        [Test]
        public async Task WebBrowserWalletIgnoresTheMobileFlag()
        {
            var f = Setup(browserWallet: c => true);
            var ctl = f.Ctl;
            ctl.Mobile = true;
            ctl.ChooseNetwork("arbitrum");
            await f.Submitter.Idle();
            ctl.Poll();
            Assert.That(ctl.Methods(), Does.Contain("browser"));
        }

        [Test]
        public async Task TheControllerPumpsAndReopensAnOverlayPage()
        {
            var f = await OnMethodStep();
            var ctl = f.Ctl;
            ctl.ChooseMethod("browser");
            ctl.OpenBrowser();
            ctl.Poll();
            Assert.That(f.Servers[0].Pumps, Is.EqualTo(1));
            ctl.OpenBrowser();
            Assert.That(f.Servers[0].Reopens, Is.EqualTo(1));
            Assert.That(f.Servers, Has.Count.EqualTo(1));
        }

        [Test]
        public async Task SwitchingNetworkStopsThePage()
        {
            var f = await OnMethodStep();
            f.Ctl.ChooseMethod("browser");
            f.Ctl.OpenBrowser();
            f.Ctl.Back();
            f.Ctl.Back();
            f.Ctl.Back();
            Assert.That(f.Ctl.Step, Is.EqualTo("network"));
            f.Ctl.ChooseNetwork("base");
            Assert.That(f.Servers[0].Stopped, Is.True);
            await f.Submitter.Idle();
            f.Ctl.Poll();
            f.Ctl.ChooseMethod("browser");
            f.Ctl.OpenBrowser();
            Assert.That(f.Servers, Has.Count.EqualTo(2), "a new page for the new session");
            Assert.That(f.Servers[1].SessionInfo["chain_name"], Is.EqualTo("Base"));
        }
    }
}
