// Twin of godot/tests/test_sheet.gd and test_restore_screen.gd: the package's UI Toolkit screens on a real panel
// (ScfgDefaultSheetHost's UIDocument), driven by the facade's runner, with navigation events as UI Toolkit's input and
// the Input System's UI module send them (NavigationMoveEvent / NavigationSubmitEvent / NavigationCancelEvent).
#nullable enable
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using NUnit.Framework;
using Scfg.Core;
using UnityEngine;
using UnityEngine.TestTools;
using UnityEngine.UIElements;

namespace Scfg.Tests
{
    public sealed class SheetTests
    {
        private ScfgRig _rig = null!;

        [SetUp]
        public void Fresh()
        {
            _rig = new ScfgRig();
            SCFG.SheetHost = new ScfgDefaultSheetHost();
        }

        [TearDown]
        public void Clean()
        {
            Time.timeScale = 1;
            _rig.Dispose();
        }

        private static ScfgSheetView OpenSheet() => (ScfgSheetView)SCFG.Current.Waits.Last().Screen;
        private static ScfgRestoreView OpenRestore() => (ScfgRestoreView)SCFG.Current.Waits.Last().Screen;

        internal static IEnumerator Frames(int n = 3)
        {
            for (int i = 0; i < n; i++) yield return null;
        }

        internal static VisualElement? Focused(ScfgScreenView v) => v.Root.focusController?.focusedElement as VisualElement;

        internal static void Nav(ScfgScreenView v, NavigationMoveEvent.Direction d)
        {
            var target = Focused(v) ?? v.Root;
            using var e = NavigationMoveEvent.GetPooled(d);
            target.SendEvent(e);
        }

        internal static void Submit(ScfgScreenView v)
        {
            var target = Focused(v) ?? v.Root;
            using var e = NavigationSubmitEvent.GetPooled();
            target.SendEvent(e);
        }

        internal static void Cancel(ScfgScreenView v)
        {
            var target = Focused(v) ?? v.Root;
            using var e = NavigationCancelEvent.GetPooled();
            target.SendEvent(e);
        }

        [UnityTest]
        public IEnumerator StepsFollowTheController()
        {
            _rig.Configure();
            SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "3", Token = "USDC" });
            yield return Frames();
            var v = OpenSheet();
            Assert.That(v.ShownStep, Is.EqualTo("amount"));
            Assert.That(v.Control("preset_3.00"), Is.Not.Null);
            Assert.That(v.Focused, Is.EqualTo("preset_3.00"), "the selected preset has the focus");
            Assert.That(v.TextOf("price"), Is.EqualTo("3.00"));
            Assert.That(v.Control("back")!.focusable, Is.False);
            Assert.That(v.Press("forward"), Is.True);
            yield return Frames();
            // one network: the network step is skipped; three methods to choose from
            Assert.That(v.ShownStep, Is.EqualTo("method"), v.Controller.Core.Error);
            Assert.That(v.Keys, Does.Contain("method_browser").And.Contain("method_qr").And.Contain("method_manual"));
            Assert.That(v.Control("back")!.focusable, Is.True);
            Assert.That(v.Press("method_manual"), Is.True);
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("manual"));
            Assert.That(v.TextOf("value_recipient"), Is.EqualTo(ScfgRig.Dev));
            Assert.That(v.Focused, Is.EqualTo("copy_amount"));
            Assert.That(v.Press("copy_recipient"), Is.True);
            Assert.That(_rig.Platform.Clipboard, Is.EqualTo(ScfgRig.Dev));
            Assert.That(v.TextOf("exact_note"), Is.Null, "the manual page shows the exact amount itself");
            Assert.That(v.Press("forward"), Is.True);
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("waiting"));
            Assert.That(v.TextOf("progress_text"), Is.EqualTo("Waiting for your payment on Arbitrum..."));
            Assert.That(v.Control("keep_playing"), Is.Not.Null);
            _rig.PaySession(v.Controller.Core);
            _rig.Tick();
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("done"));
            Assert.That(v.TextOf("done_title"), Is.EqualTo("Payment confirmed. Thank you!"));
            Assert.That(v.TextOf("result_tx"), Is.EqualTo(ScfgRig.Tx));
            Assert.That(v.Focused, Is.EqualTo("continue"));
            Submit(v);
            yield return Frames();
            Assert.That(SCFG.Current.Waits, Is.Empty);
            Assert.That(GameObject.Find("SCFG Payment Sheet"), Is.Null, "the host destroyed the sheet");
        }

        // The network step (Ren'Py's scfg_step_network): Ethereum as a tile, the L2s behind "More networks", a "Pay with
        // card" link under each tile.
        [UnityTest]
        public IEnumerator TheNetworkStepShowsTheMainNetworksAndTheRestBehindMoreNetworks()
        {
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev }), Is.Null);
            Assert.That(SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" }), Is.Null);
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames();
            var v = OpenSheet();
            Assert.That(v.ShownStep, Is.EqualTo("network"));
            Assert.That(v.Keys.Where(k => k.StartsWith("network_")), Is.EqualTo(new[] { "network_ethereum" }));
            Assert.That(v.Focused, Is.EqualTo("network_ethereum"));
            Assert.That(v.TextOf("card_ethereum"), Is.EqualTo("Pay with card"));
            Assert.That(v.TextOf("more_networks"), Is.EqualTo("More networks"));
            Nav(v, NavigationMoveEvent.Direction.Down);
            Assert.That(v.Focused, Is.EqualTo("card_ethereum"), "the link under the tile");
            Nav(v, NavigationMoveEvent.Direction.Down);
            Assert.That(v.Focused, Is.EqualTo("more_networks"));
            Submit(v);
            yield return Frames();
            Assert.That(v.TextOf("more_networks"), Is.EqualTo("Fewer networks"));
            Assert.That(v.Focused, Is.EqualTo("more_networks"), "the focus stays on the toggle");
            Assert.That(v.FocusOrder.Select(c => v.Keys.First(k => v.Control(k) == c)).TakeWhile(k => k != "cancel"), Is.EqualTo(new[]
            {
                "network_ethereum", "card_ethereum", "more_networks", "network_arc", "network_base", "card_arc", "card_base",
                "network_arbitrum", "network_polygon", "card_arbitrum", "card_polygon",
            }));
            var arc = v.Control("network_arc")!;
            var ethereum = v.Control("network_ethereum")!;
            Assert.That(arc.layout.width, Is.EqualTo(ethereum.layout.width).Within(0.5f), "every tile has the same width");
            Assert.That(arc.layout.height, Is.EqualTo(ethereum.layout.height).Within(0.5f), "and the same height");
            Assert.That(v.Press("network_base"), Is.True);
            yield return Frames(6);
            Assert.That(v.ShownStep, Is.EqualTo("method"), v.Controller.Core.Error);
            Assert.That(v.Press("back"), Is.True);
            yield return Frames();
            Assert.That(v.Control("network_base"), Is.Not.Null, "More networks stays open while Base is chosen");
            Assert.That(v.Focused, Is.EqualTo("network_base"));
        }

        // A network's "Pay with card" (Ren'Py's scfg_step_card): the card page, whose button opens the card checkout.
        [UnityTest]
        public IEnumerator PayWithCardOpensTheCardPageAndTheCardCheckout()
        {
            _rig.Configure(card: true);
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames();
            var v = OpenSheet();
            Assert.That(v.ShownStep, Is.EqualTo("network"), "a single network with a card button keeps the network step");
            Assert.That(v.Control("more_networks"), Is.Null);
            Assert.That(v.Press("card_arbitrum"), Is.True);
            yield return Frames(6);
            Assert.That(v.ShownStep, Is.EqualTo("card"), v.Controller.Core.Error);
            Assert.That(v.Focused, Is.EqualTo("forward"));
            Assert.That(v.TextOf("forward"), Is.EqualTo("Open card checkout"));
            Assert.That(v.TextOf("exact_note"), Is.Null, "the card page names the exact amount itself");
            var url = v.Controller.Core.CardUrl();
            Assert.That(url, Does.StartWith("https://thirdweb.com/bridge/checkout-widget?chain=42161&")
                                 .And.Contain("&amount=" + v.Controller.Core.PayAmountText + "&seller=" + ScfgRig.Dev + "&"));
            Assert.That(v.Press("forward"), Is.True);
            Assert.That(_rig.Platform.Opened.Last(), Is.EqualTo(url), "opens the card checkout through the platform");
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("waiting"));
            Assert.That(v.Press("back"), Is.True);
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("card"));
            Assert.That(v.Press("back"), Is.True);
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("network"));
            Assert.That(v.Focused, Is.EqualTo("card_arbitrum"), "the chosen card link has the focus");
            Assert.That(v.Control("card_arbitrum")!.ClassListContains("scfg-card-link--selected"), Is.True);
            Assert.That(v.Control("network_arbitrum")!.ClassListContains("scfg-option--selected"), Is.False);
        }

        // Solana is a main tile without a "Pay with card" link.
        [UnityTest]
        public IEnumerator SolanaHasNoPayWithCardLink()
        {
            Assert.That(SCFG.Configure(new ScfgOptions
            {
                Wallet = ScfgRig.Dev, SolanaWallet = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU",
                Chains = new List<string> { "solana", "arbitrum" },
            }), Is.Null);
            Assert.That(SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" }), Is.Null);
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames();
            var v = OpenSheet();
            Assert.That(v.ShownStep, Is.EqualTo("network"));
            Assert.That(v.Control("network_solana"), Is.Not.Null);
            Assert.That(v.Control("card_solana"), Is.Null);
            Assert.That(v.Control("network_arbitrum"), Is.Null, "behind More networks");
            Assert.That(v.Press("more_networks"), Is.True);
            yield return Frames();
            Assert.That(v.Control("card_arbitrum"), Is.Not.Null);
        }

        [UnityTest]
        public IEnumerator KeyboardAndGamepadOnlyManualPayment()
        {
            _rig.Configure();
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames();
            var v = OpenSheet();
            Assert.That(v.ShownStep, Is.EqualTo("method"));
            Assert.That(v.Focused, Is.EqualTo("method_browser"));
            Nav(v, NavigationMoveEvent.Direction.Down);
            Assert.That(v.Focused, Is.EqualTo("method_qr"));
            Nav(v, NavigationMoveEvent.Direction.Down);
            Assert.That(v.Focused, Is.EqualTo("method_manual"));
            Submit(v);
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("manual"));
            Assert.That(v.Focused, Is.EqualTo("copy_amount"));
            for (int i = 0; i < 3; i++) Nav(v, NavigationMoveEvent.Direction.Down);
            Assert.That(v.Focused, Is.EqualTo("tx_input"));
            var field = (TextField)v.Control("tx_input")!;
            field.value = ScfgRig.Tx + "!!"; // a typed character that is not a hash character is dropped
            Assert.That(v.Controller.Core.TxInput, Is.EqualTo(ScfgRig.Tx));
            Nav(v, NavigationMoveEvent.Direction.Right);
            Assert.That(v.Focused, Is.EqualTo("tx_input"), "left and right move the caret in a field");
            Nav(v, NavigationMoveEvent.Direction.Down);
            Assert.That(v.Focused, Is.EqualTo("check"));
            Submit(v);
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("waiting"));
            Assert.That(v.Controller.Core.HintTx, Is.EqualTo(ScfgRig.Tx));
            Assert.That(v.Focused, Is.EqualTo("explorer"), "the explorer link of the reported hash comes first, as Godot");
            Cancel(v); // a seen transaction: no back, no cancel
            yield return Frames();
            Assert.That(SCFG.Current.Waits.Count, Is.EqualTo(1));
        }

        [UnityTest]
        public IEnumerator TheFocusNeverLeavesTheSheet()
        {
            _rig.Configure();
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames();
            var v = OpenSheet();
            var order = v.FocusOrder;
            order[0].Focus();
            Nav(v, NavigationMoveEvent.Direction.Up);
            Assert.That(Focused(v), Is.SameAs(order[0]));
            order[order.Count - 1].Focus();
            Nav(v, NavigationMoveEvent.Direction.Down);
            Assert.That(Focused(v), Is.SameAs(order[order.Count - 1]));
            Nav(v, NavigationMoveEvent.Direction.Right);
            Assert.That(Focused(v), Is.SameAs(order[order.Count - 1]), "Cancel is alone in its row before a session");
            Assert.That(order.All(c => v.Root.Contains(c)), Is.True);
        }

        [UnityTest]
        public IEnumerator CancelGoesBackThenClosesTheSheet()
        {
            _rig.Configure();
            var call = SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames();
            var v = OpenSheet();
            v.Press("method_qr");
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("qr"));
            Cancel(v);
            yield return Frames();
            Assert.That(v.ShownStep, Is.EqualTo("method"));
            Cancel(v);
            yield return Frames();
            Assert.That(call.GetAwaiter().IsCompleted, Is.True);
            Assert.That(call.GetAwaiter().GetResult().Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
        }

        [UnityTest]
        public IEnumerator TheQrCodeMatchesTheCoresMatrix()
        {
            _rig.Configure(new ScfgMethods { Browser = false, Manual = false });
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames();
            var v = OpenSheet();
            Assert.That(v.ShownStep, Is.EqualTo("qr"));
            var qr = (ScfgQrElement)v.Control("qr")!;
            var uri = v.Controller.Core.PaymentUri()!;
            Assert.That(uri, Does.StartWith("ethereum:"));
            var (size, runs) = Paylinks.QrRects(uri, ScfgQrElement.Border);
            Assert.That(qr.Text, Is.EqualTo(uri));
            Assert.That(qr.Modules, Is.EqualTo(size));
            Assert.That(qr.Runs.Select(r => (r.X, r.Y, r.Width)), Is.EqualTo(runs.Select(r => (r.X, r.Y, r.Width))));
            Assert.That(v.TextOf("forward"), Is.EqualTo("I've sent it"));
        }

        [UnityTest]
        public IEnumerator TheSheetWorksAtTimeScaleZero()
        {
            // the facade's own clocks (unscaled real time), not the rig's manual one
            var s = SCFG.Current;
            s.Clock = () => Time.realtimeSinceStartupAsDouble;
            s.OwnScheduler = new UnityScheduler(null, e => SCFG.ReportError("worker error:", e));
            s.Scheduler = s.OwnScheduler;
            Time.timeScale = 0;
            _rig.Configure(new ScfgMethods { Browser = false, Qr = false });
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames(5);
            var v = OpenSheet();
            Assert.That(v.Controller.Core.State, Is.EqualTo("waiting"), "the runner pumps the worker while the game is paused");
            Assert.That(v.ShownStep, Is.EqualTo("manual"));
            Assert.That(v.Builds, Is.GreaterThan(1), "the sheet rebuilds while the game is paused");
        }

        [UnityTest]
        public IEnumerator TheAccentColorsTheSelectedOptionAndThePinnedButton()
        {
            SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, Chains = new List<string> { "arbitrum" }, Accent = "#7a5cff" });
            SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "3", Token = "USDC" });
            yield return Frames();
            var v = OpenSheet();
            var want = ScfgScreenView.ParseColor("#7a5cff");
            Assert.That(v.Control("forward")!.style.backgroundColor.value, Is.EqualTo(want));
            Assert.That(v.Control("preset_3.00")!.style.borderTopColor.value, Is.EqualTo(want));
        }

        [UnityTest]
        public IEnumerator ASheetDestroyedFromOutsideCountsAsCancel()
        {
            _rig.Configure();
            var got = new List<ScfgPaymentResult>();
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" }, got.Add);
            yield return Frames();
            Object.Destroy(GameObject.Find("SCFG Payment Sheet"));
            yield return Frames();
            Assert.That(got.Single().Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
        }

        [UnityTest]
        public IEnumerator RestoreChooseAndConfirmWithTheGamepad()
        {
            _rig.Configure();
            var call = SCFG.Restore();
            yield return Frames();
            var v = OpenRestore();
            Assert.That(v.Focused, Is.EqualTo("tx_input"));
            ((TextField)v.Control("tx_input")!).value = "-!? ";
            Assert.That(v.Controller.Core.TxInput, Is.EqualTo(""), "only hash characters are taken");
            ((TextField)v.Control("tx_input")!).value = "0x12";
            Nav(v, NavigationMoveEvent.Direction.Down);
            Assert.That(v.Focused, Is.EqualTo("check"));
            Submit(v);
            yield return Frames();
            Assert.That(v.TextOf("message"), Is.EqualTo(RestoreFlow.Messages["invalid_hash"]));
            var log = FakeEvmNode.TransferLog(ScfgRig.UsdcArb, ScfgRig.Payer, ScfgRig.Dev, 4990000, 999, ScfgRig.Tx);
            _rig.Node.Receipts[ScfgRig.Tx] = new JsonObject
            {
                { "status", "0x1" }, { "blockNumber", "0x3e7" }, { "transactionHash", ScfgRig.Tx }, { "logs", new List<object?> { log } },
            };
            ((TextField)v.Control("tx_input")!).value = ScfgRig.Tx;
            v.Press("check");
            yield return Frames(6);
            Assert.That(v.Controller.Core.State, Is.EqualTo("choose"), v.Controller.Core.Error);
            Assert.That(v.TextOf("candidate_0"), Is.EqualTo("Payment of 4.990000 USDC on Arbitrum. What was it for?"));
            Assert.That(v.Focused, Is.EqualTo("choice_0_chapter2"));
            Assert.That(v.Control("restore")!.enabledSelf, Is.False);
            Submit(v);
            yield return Frames();
            Assert.That(v.Control("restore")!.enabledSelf, Is.True);
            Assert.That(v.Focused, Is.EqualTo("choice_0_chapter2"), "a choice keeps the focus");
            v.Control("restore")!.Focus();
            Submit(v);
            yield return Frames(6);
            Assert.That(v.Controller.Core.State, Is.EqualTo("done"));
            Assert.That(v.TextOf("added_0"), Is.EqualTo("Chapter 2: 4.990000 USDC on Arbitrum"));
            Assert.That(v.TextOf("owned_0"), Is.EqualTo("Chapter 2: 4.990000 USDC on Arbitrum"));
            Assert.That(v.Focused, Is.EqualTo("close"));
            Cancel(v);
            yield return Frames();
            Assert.That(call.GetAwaiter().IsCompleted, Is.True);
            Assert.That(call.GetAwaiter().GetResult().Status, Is.EqualTo(ScfgRestoreStatus.Restored));
            Assert.That(GameObject.Find("SCFG Restore"), Is.Null);
        }

        [UnityTest]
        public IEnumerator ANoticeShowsAndGoesAway()
        {
            _rig.Configure();
            SCFG.SheetHost!.Notify("Purchases restored.");
            yield return null;
            var toast = Object.FindAnyObjectByType<ScfgToast>();
            Assert.That(toast, Is.Not.Null);
            Assert.That(toast!.Text, Is.EqualTo("Purchases restored."));
            var until = Time.realtimeSinceStartup + 5f;
            while (toast != null && Time.realtimeSinceStartup < until) yield return null;
            Assert.That(toast == null, Is.True, "the notice is destroyed after its time");
        }
    }
}
