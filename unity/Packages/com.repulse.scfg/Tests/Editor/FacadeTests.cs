// Twin of godot/tests/test_scfg_api.gd and test_scfg_flows.gd: the SCFG facade's configuration, queries, persistence
// and payment / restore flows. The facade is reset and pointed at fakes by ScfgRig; the test pumps the frames itself
// (SCFG.Pump) with a manual clock, so no player loop is needed.
#nullable enable
using System.Collections.Generic;
using System.IO;
using System.Linq;
using NUnit.Framework;
using Scfg.Core;
using UnityEngine;

namespace Scfg.Tests
{
    public sealed class FacadeTests
    {
        private ScfgRig _rig = null!;

        [SetUp]
        public void Fresh() => _rig = new ScfgRig();

        [TearDown]
        public void Clean() => _rig.Dispose();

        private static bool Done<T>(Awaitable<T> a) => a.GetAwaiter().IsCompleted;

        private static T Result<T>(Awaitable<T> a)
        {
            Assert.That(a.GetAwaiter().IsCompleted, Is.True, "the call has not resolved");
            return a.GetAwaiter().GetResult();
        }

        private static ScfgMethods ManualOnly() => new ScfgMethods { Browser = false, Qr = false };

        /// <summary>Pay for chapter2 on the manual page and run until its session started.</summary>
        private (Awaitable<ScfgPaymentResult> Call, FakeScreen Screen, CheckoutController Ctl) OpenManualPayment(
            System.Action<ScfgPaymentResult>? callback = null)
        {
            _rig.Configure(ManualOnly());
            Awaitable<ScfgPaymentResult>? call = null;
            if (callback == null) call = SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            else SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" }, callback);
            var screen = _rig.Host.Last;
            _rig.Settle();
            var ctl = screen.Checkout.Core;
            Assert.That(ctl.State, Is.EqualTo("waiting"), string.Join("; ", _rig.Reported));
            Assert.That(ctl.Step, Is.EqualTo("manual"));
            return (call!, screen, ctl);
        }

        // --- configuration ---------------------------------------------------------------------------

        [Test]
        public void ConfigureReportsTheCoresCodes()
        {
            Assert.That(SCFG.Configure(new ScfgOptions()), Is.EqualTo("wallet.missing"));
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = "0x123" }), Is.EqualTo("address.invalid"));
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, Tokens = new List<string>() }), Is.EqualTo("tokens.empty"));
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, Accent = "red" }), Is.EqualTo("accent.invalid"));
            Assert.That(_rig.Reported.Count, Is.EqualTo(4));
            Assert.That(_rig.Reported.All(r => r.StartsWith("scfg: ")), Is.True);
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev }), Is.Null);
        }

        [Test]
        public void TheOnrampOptionReachesTheCore()
        {
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev }), Is.Null);
            Assert.That(SCFG.Current.Config!.Onramp.Enabled, Is.True, "on by default on mainnet");
            Assert.That(SCFG.Current.Config!.Onramp.Chains, Does.Contain("arbitrum"));
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, Onramp = new ScfgOnramp { Enabled = false } }), Is.Null);
            Assert.That(SCFG.Current.Config!.Onramp.Enabled, Is.False);
            Assert.That(SCFG.Configure(new ScfgOptions
            {
                Wallet = ScfgRig.Dev, Onramp = new ScfgOnramp { Chains = new List<string> { "base" }, Url = "https://buy.example/{chain}" },
            }), Is.Null);
            Assert.That(SCFG.Current.Config!.Onramp.Chains, Is.EqualTo(new[] { "base" }));
            Assert.That(SCFG.Current.Config!.Onramp.Url, Is.EqualTo("https://buy.example/{chain}"));
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, Onramp = new ScfgOnramp { Url = "https://" } }),
                        Is.EqualTo("onramp.bad_url"));
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, Onramp = new ScfgOnramp { Chains = new List<string> { "solana" } } }),
                        Is.EqualTo("onramp.disabled_chain"));
        }

        [Test]
        public void MisuseNeverThrows()
        {
            Assert.That(SCFG.Configure(null!), Is.EqualTo("TypeError"));
            Assert.That(SCFG.AddItem("x", new ScfgItem()), Is.EqualTo("configure.not_called"));
            Assert.That(SCFG.Owns("x"), Is.False);
            Assert.That(SCFG.TipTotal(), Is.EqualTo("0"));
            Assert.That(SCFG.HasTipped(), Is.False);
            _rig.Configure();
            Assert.That(SCFG.AddItem("chapter2", new ScfgItem { Code = 2, Price = "1", Token = "USDC", Title = "T" }),
                Is.EqualTo("item.duplicate_id"));
            Assert.That(SCFG.AddItem("ch3", new ScfgItem { Code = 1, Price = "1", Token = "USDC", Title = "T" }),
                Is.EqualTo("item.duplicate_code"));
            Assert.That(SCFG.AddItem("ch3", new ScfgItem { Code = 3, Price = "1.5.0", Token = "USDC", Title = "T" }),
                Is.EqualTo("item.invalid_price"));
            Assert.That(SCFG.AddItem("ch3", null!), Is.EqualTo("TypeError"));
            Assert.That(SCFG.Owns("nope"), Is.False);
            Assert.That(_rig.Reported.Last(), Does.Contain("unknown item 'nope'"));
        }

        [Test]
        public void AddChainAfterConfigureIsAnError()
        {
            var chain = new ScfgChain
            {
                Name = "My L2", ChainId = 4242, Rpc = new List<string> { "https://rpc.example.org" },
                Tokens = new Dictionary<string, string> { { "USDC", "0x0000000000000000000000000000000000000001" } },
                Confirmations = 3, ExplorerTx = "https://explorer.example.org/tx/{}",
            };
            Assert.That(SCFG.AddChain("myl2", chain), Is.Null);
            Assert.That(SCFG.AddChain("Bad Key", chain), Is.EqualTo("chain.invalid_key"));
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, Chains = new List<string> { "myl2" } }), Is.Null);
            Assert.That(SCFG.ChainName("myl2"), Is.EqualTo("My L2"));
            Assert.That(SCFG.AddChain("other", chain), Is.EqualTo("configure.after_add_chain"));
        }

        [Test]
        public void ConfigureAfterStartIsAnError()
        {
            _rig.Configure();
            _rig.Pump();
            Assert.That(SCFG.Current.Worker, Is.Not.Null);
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev }), Is.EqualTo("configure.after_start"));
        }

        // --- queries and persistence -----------------------------------------------------------------

        private static JsonObject Receipt(string item = "chapter2", string kind = "unlock", string amount = "4.99") => new JsonObject
        {
            { "amount", amount }, { "block", 999L }, { "chain", "arbitrum" }, { "item", kind == "tip" ? null : item }, { "kind", kind },
            { "log_index", 0L }, { "payer", ScfgRig.Payer }, { "recipient", ScfgRig.Dev }, { "source", "payment" },
            { "token", "USDC" }, { "tx_hash", ScfgRig.Tx },
        };

        private void StoreReceipt(JsonObject receipt)
        {
            var key = Receipts.ReceiptKey("arbitrum", ScfgRig.Tx, 0);
            Assert.That(ScfgStore.Save(_rig.StorePath, new JsonObject(), new JsonObject { { key, receipt } }), Is.Null);
        }

        [Test]
        public void OwnsFromTheStoredReceiptsBeforeAndAfterStart()
        {
            StoreReceipt(Receipt());
            _rig.Configure();
            Assert.That(SCFG.Owns("chapter2"), Is.True);
            Assert.That(SCFG.Receipts().Count, Is.EqualTo(1));
            Assert.That(SCFG.Receipts().Values.First().Item, Is.EqualTo("chapter2"));
            OnChain();
            _rig.Settle();
            Assert.That(SCFG.Owns("chapter2"), Is.True, string.Join("; ", _rig.Reported));
        }

        /// <summary>The stored receipt's payment on chain, so the worker's reverify keeps it.</summary>
        private void OnChain()
        {
            _rig.Node.Logs.Add(FakeEvmNode.TransferLog(ScfgRig.UsdcArb, ScfgRig.Payer, ScfgRig.Dev, 4990000, 999, ScfgRig.Tx));
            _rig.Node.Receipts[ScfgRig.Tx] = new JsonObject
            {
                { "status", "0x1" }, { "blockNumber", "0x3e7" }, { "transactionHash", ScfgRig.Tx },
                { "logs", new List<object?> { _rig.Node.Logs[0] } },
            };
        }

        [Test]
        public void TipsCountTowardsTipTotal()
        {
            StoreReceipt(Receipt(kind: "tip", amount: "2.50"));
            _rig.Configure();
            Assert.That(SCFG.HasTipped(), Is.True);
            Assert.That(SCFG.TipTotal(), Is.EqualTo("2.50"));
            Assert.That(SCFG.Owns("chapter2"), Is.False);
        }

        [Test]
        public void ACorruptStateFileStartsEmptyAndWarns()
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_rig.StorePath)!);
            File.WriteAllText(_rig.StorePath, "{broken");
            _rig.Configure();
            Assert.That(SCFG.Receipts().Count, Is.EqualTo(0));
            Assert.That(_rig.Warnings.Single(), Does.Contain("is not valid JSON"));
            Assert.That(File.Exists(_rig.StorePath + ".corrupt"), Is.True);
        }

        [Test]
        public void ANewSessionIsSavedAtOnceAndSurvivesARestart()
        {
            var (_, _, ctl) = OpenManualPayment();
            var sid = ctl.SessionId!;
            Assert.That(ScfgStore.Load(_rig.StorePath).Pending.ContainsKey(sid), Is.True);
            _rig.Restart();
            _rig.Configure(ManualOnly());
            Assert.That(SCFG.Pending().ContainsKey(sid), Is.True);
        }

        [Test]
        public void ScanProgressIsSavedAtMostEveryTenSeconds()
        {
            var (_, _, ctl) = OpenManualPayment();
            var sid = ctl.SessionId!;
            long Cursor() => (long)((JsonObject)ScfgStore.Load(_rig.StorePath).Pending[sid]!)["cursor"]!;
            var saved = Cursor();
            _rig.Node.Head += 50;
            _rig.Pump(2, 4); // a 3 s pass moves the cursor; less than 10 s since the last save
            Assert.That((long)((JsonObject)SCFG.Current.Worker!.Snapshot.Pending[sid]!)["cursor"]!, Is.GreaterThan(saved));
            Assert.That(Cursor(), Is.EqualTo(saved));
            _rig.Pump(3, 4); // past 10 s: written even without a new snapshot
            Assert.That(Cursor(), Is.GreaterThan(saved));
        }

        [Test]
        public void AFailedSaveIsRetriedAndReportedOnce()
        {
            _rig.Configure(ManualOnly());
            Directory.CreateDirectory(_rig.StorePath + ".tmp"); // every save fails to write its .tmp
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            _rig.Settle();
            _rig.Pump(6, 11);
            Assert.That(_rig.Reported.Count(r => r.Contains("cannot save the payment state")), Is.EqualTo(1));
            Assert.That(SCFG.Current.Dirty, Is.True);
            Directory.Delete(_rig.StorePath + ".tmp");
            _rig.Pump(2, 11);
            Assert.That(SCFG.Current.Dirty, Is.False);
            Assert.That(ScfgStore.Load(_rig.StorePath).Pending.Count, Is.EqualTo(1));
        }

        [Test]
        public void AnUnreadableStateFileStartsNoPaymentsAndIsNeverSavedOver()
        {
            Directory.CreateDirectory(_rig.StorePath); // a directory where the file belongs: it exists and cannot be read
            _rig.Configure();
            var r = SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            var result = Result(r);
            Assert.That(result.Status, Is.EqualTo(ScfgPaymentStatus.Failed));
            Assert.That(_rig.Reported.Count(x => x.Contains("cannot read the stored receipts")), Is.EqualTo(1));
            SCFG.ClearLocalData();
            Assert.That(Directory.Exists(_rig.StorePath), Is.True);
            Assert.That(_rig.Host.Screens, Is.Empty);
        }

        [Test]
        public void ClearLocalDataForgetsEverythingAndRaisesReceiptsChanged()
        {
            StoreReceipt(Receipt());
            _rig.Configure();
            int changed = 0;
            SCFG.ReceiptsChanged += () => changed++;
            SCFG.ClearLocalData();
            Assert.That(changed, Is.EqualTo(1));
            Assert.That(SCFG.Owns("chapter2"), Is.False);
            Assert.That(ScfgStore.Load(_rig.StorePath).Receipts.Count, Is.EqualTo(0));
        }

        [Test]
        public void ClearLocalDataWhileTheWorkerRunsSkipsTheSnapshotsMadeBeforeTheWipe()
        {
            StoreReceipt(Receipt());
            OnChain();
            _rig.Configure(ManualOnly());
            _rig.Settle();
            Assert.That(SCFG.Current.Worker!.Snapshot.Receipts.Count, Is.EqualTo(1));
            int changed = 0;
            SCFG.ReceiptsChanged += () => changed++;
            SCFG.ClearLocalData();
            Assert.That(SCFG.Owns("chapter2"), Is.False, "readers see the wipe at once");
            Assert.That(SCFG.Current.Wipe, Is.Not.Null);
            _rig.Settle();
            Assert.That(SCFG.Current.Wipe, Is.Null);
            Assert.That(SCFG.Current.Worker!.Snapshot.Receipts.Count, Is.EqualTo(0));
            Assert.That(ScfgStore.Load(_rig.StorePath).Receipts.Count, Is.EqualTo(0), "no old snapshot was saved back");
            Assert.That(changed, Is.EqualTo(1));
        }

        [Test]
        public void ANullSheetHostFailsWithSheetUnavailable()
        {
            _rig.Configure();
            SCFG.SheetHost = null;
            Assert.That(SCFG.SheetHost, Is.Null);
            Assert.That(Result(SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" })).Error, Is.EqualTo("sheet.unavailable"));
        }

        [Test]
        public void ExportReceiptsWritesASortedList()
        {
            StoreReceipt(Receipt());
            _rig.Configure();
            var path = SCFG.ExportReceipts(Path.Combine(_rig.Dir, "out", "receipts.json"));
            Assert.That(path, Is.Not.Null);
            var list = (List<object?>)Json.Loads(File.ReadAllText(path!))!;
            Assert.That(list.Count, Is.EqualTo(1));
            Assert.That(File.ReadAllText(path!), Does.StartWith("[\n  {\n    \"amount\": \"4.99\""));
            File.WriteAllText(Path.Combine(_rig.Dir, "a-file"), "");
            Assert.That(SCFG.ExportReceipts(Path.Combine(_rig.Dir, "a-file", "receipts.json")), Is.Null);
            Assert.That(_rig.Reported.Single(), Does.StartWith("scfg: cannot write"));
        }

        // --- the payment flow ------------------------------------------------------------------------

        [Test]
        public void AnOwnedItemShortCircuitsWithoutASheet()
        {
            StoreReceipt(Receipt());
            _rig.Configure();
            var got = new List<ScfgPaymentResult>();
            var r = Result(SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" }));
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" }, got.Add);
            Assert.That(r.Status, Is.EqualTo(ScfgPaymentStatus.AlreadyOwned));
            Assert.That(r.Item, Is.EqualTo("chapter2"));
            Assert.That(got.Single().Status, Is.EqualTo(ScfgPaymentStatus.AlreadyOwned));
            Assert.That(_rig.Host.Screens, Is.Empty);
        }

        [Test]
        public void MisusedPayResolvesFailedWithTheCode()
        {
            var r = Result(SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" }));
            Assert.That(r.Status, Is.EqualTo(ScfgPaymentStatus.Failed));
            Assert.That(r.Error, Is.EqualTo("configure.not_called"));
            _rig.Configure();
            var got = new List<ScfgPaymentResult>();
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "nope" }, got.Add);
            Assert.That(got.Single().Error, Is.EqualTo("item.unknown"));
            Assert.That(got.Single().Item, Is.EqualTo("nope"));
            Assert.That(Result(SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Token = "USDC" })).Status, Is.EqualTo(ScfgPaymentStatus.Failed));
            Assert.That(Result(SCFG.Pay((ScfgPayKind)7, new ScfgPayRequest())).Error, Is.EqualTo("TypeError"));
            Assert.That(Result(SCFG.Pay(ScfgPayKind.Unlock, null!)).Error, Is.EqualTo("TypeError"));
            Assert.That(_rig.Reported.Count, Is.EqualTo(5));
            Assert.That(_rig.Host.Screens, Is.Empty);
        }

        [Test]
        public void NoSheetFailsClearly()
        {
            _rig.Configure();
            _rig.Host.Refuse = true;
            var r = Result(SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" }));
            Assert.That(r.Error, Is.EqualTo("sheet.unavailable"));
            Assert.That(Result(SCFG.Restore()).Error, Is.EqualTo("sheet.unavailable"));
        }

        [Test]
        public void CancelReturnsCancelledAndCallsTheCallbackOnce()
        {
            var got = new List<ScfgPaymentResult>();
            var (_, screen, ctl) = OpenManualPayment(got.Add);
            var sid = ctl.SessionId!;
            screen.Close("cancel");
            Assert.That(got, Is.Empty, "a close is picked up on the next frame");
            _rig.Pump();
            Assert.That(got.Single().Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
            Assert.That(_rig.Host.ClosedScreens.Single(), Is.SameAs(screen));
            _rig.Settle();
            Assert.That(((JsonObject)SCFG.Current.Worker!.Snapshot.Pending[sid]!)["ui_status"], Is.EqualTo("cancelled"));
        }

        [Test]
        public void ACancelWithoutCallbackReachesPaymentResolvedAndTheAwaitable()
        {
            var events = new List<ScfgPaymentResult>();
            SCFG.PaymentResolved += events.Add;
            var (call, screen, _) = OpenManualPayment();
            Assert.That(Done(call), Is.False);
            screen.Close("cancel");
            _rig.Pump();
            Assert.That(Result(call).Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
            Assert.That(events.Single().Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
        }

        [Test]
        public void APaymentWhileTheSheetIsOpenIsPaidAndRaisedOnce()
        {
            var events = new List<ScfgPaymentResult>();
            SCFG.PaymentResolved += events.Add;
            int changed = 0;
            SCFG.ReceiptsChanged += () => changed++;
            var (call, screen, ctl) = OpenManualPayment();
            _rig.PaySession(ctl);
            _rig.Tick();
            Assert.That(ctl.State, Is.EqualTo("paid"), string.Join("; ", _rig.Reported));
            Assert.That(events.Single().Status, Is.EqualTo(ScfgPaymentStatus.Paid));
            Assert.That(changed, Is.EqualTo(1));
            Assert.That(SCFG.Owns("chapter2"), Is.True);
            Assert.That(ScfgStore.Load(_rig.StorePath).Receipts.Count, Is.EqualTo(1));
            screen.Close("finish");
            _rig.Pump();
            var r = Result(call);
            Assert.That(r.Status, Is.EqualTo(ScfgPaymentStatus.Paid));
            Assert.That(r.TxHash, Is.EqualTo(ScfgRig.Tx));
            Assert.That(r.Amount, Is.EqualTo(ctl.PayAmountText));
            Assert.That(events.Count, Is.EqualTo(1));
        }

        [Test]
        public void KeepPlayingResolvesPendingAndDeliversTheLaterResultToTheCallback()
        {
            var got = new List<ScfgPaymentResult>();
            var events = new List<ScfgPaymentResult>();
            SCFG.PaymentResolved += events.Add;
            var (_, screen, ctl) = OpenManualPayment(got.Add);
            screen.Close("keep_playing");
            _rig.Pump();
            Assert.That(got, Is.Empty, "the pending result goes to the awaiter only");
            Assert.That(SCFG.Current.Background.Count, Is.EqualTo(1));
            Assert.That(SCFG.Pending().Values.Single().State, Is.EqualTo("awaiting"));
            _rig.PaySession(ctl);
            _rig.Pump(3, 16);
            Assert.That(got.Single().Status, Is.EqualTo(ScfgPaymentStatus.Paid), string.Join("; ", _rig.Reported));
            Assert.That(events, Is.Empty);
            Assert.That(SCFG.Current.Background.Count, Is.EqualTo(0), "a final session closes its background controller");
            Assert.That(SCFG.Current.Callbacks.Count, Is.EqualTo(0));
        }

        [Test]
        public void KeepPlayingWithoutCallbackRaisesPaymentResolvedLater()
        {
            var events = new List<ScfgPaymentResult>();
            SCFG.PaymentResolved += events.Add;
            var (call, screen, ctl) = OpenManualPayment();
            screen.Close("keep_playing");
            _rig.Pump();
            Assert.That(Result(call).Status, Is.EqualTo(ScfgPaymentStatus.Pending));
            _rig.PaySession(ctl);
            _rig.Pump(3, 16);
            Assert.That(events.Single().Status, Is.EqualTo(ScfgPaymentStatus.Paid));
        }

        [Test]
        public void APaidResultIsAnnouncedWhenNotifyIsOn()
        {
            SCFG.Notify = true;
            var (_, _, ctl) = OpenManualPayment();
            _rig.PaySession(ctl);
            _rig.Tick();
            Assert.That(_rig.Host.Notes.Single(), Is.EqualTo("Payment confirmed: Chapter 2. Thank you!"));
        }

        [Test]
        public void AScreenGoneWithoutClosingCountsAsCancelAndIsNotClosedAgain()
        {
            var got = new List<ScfgPaymentResult>();
            var (_, screen, _) = OpenManualPayment(got.Add);
            screen.Destroy();
            _rig.Pump();
            Assert.That(got.Single().Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
            Assert.That(_rig.Host.ClosedScreens, Is.Empty);
        }

        [Test]
        public void TheIntervalIsThreeSecondsWhileASheetIsOpen()
        {
            var (_, screen, _) = OpenManualPayment();
            Assert.That(SCFG.Current.Worker!.Interval(), Is.EqualTo(3.0));
            screen.Close("cancel");
            _rig.Pump();
            Assert.That(SCFG.Current.Worker!.Interval(), Is.EqualTo(15.0));
        }

        // --- running unfocused (desktop) ----------------------------------------------------------------

        [Test]
        public void RunInBackgroundIsOnWhileTheSheetIsOpenAndPutBackWhenItCloses()
        {
            var (_, screen, _) = OpenManualPayment();
            Assert.That(_rig.RunInBackground, Is.True, "set when the sheet opened");
            Assert.That(_rig.RunInBackgroundSets, Is.EqualTo(new[] { true }));
            _rig.Pump(3, 1);
            Assert.That(_rig.RunInBackgroundSets, Is.EqualTo(new[] { true }), "set once, not every frame");
            screen.Close("cancel");
            _rig.Pump();
            Assert.That(_rig.RunInBackground, Is.False, "the game's value is back");
            Assert.That(_rig.RunInBackgroundSets, Is.EqualTo(new[] { true, false }));
        }

        [Test]
        public void AGameThatRunsInBackgroundKeepsItsSetting()
        {
            _rig.RunInBackground = true;
            var (_, screen, _) = OpenManualPayment();
            screen.Close("cancel");
            _rig.Pump();
            Assert.That(_rig.RunInBackground, Is.True);
            Assert.That(_rig.RunInBackgroundSets.Distinct(), Is.EqualTo(new[] { true }));
        }

        [Test]
        public void AKeptPlayingCheckoutHoldsRunInBackgroundUntilItIsFinal()
        {
            var (_, screen, ctl) = OpenManualPayment();
            screen.Close("keep_playing");
            _rig.Pump();
            Assert.That(SCFG.Current.Background.Count, Is.EqualTo(1));
            Assert.That(_rig.RunInBackground, Is.True, "its browser page still has to answer");
            _rig.PaySession(ctl);
            _rig.Pump(3, 16);
            Assert.That(SCFG.Current.Background.Count, Is.EqualTo(0));
            Assert.That(_rig.RunInBackground, Is.False);
        }

        [Test]
        public void TwoSheetsHoldRunInBackgroundUntilTheLastCloses()
        {
            var (_, a, _) = OpenManualPayment();
            SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "3", Token = "USDC" });
            var b = _rig.Host.Last;
            a.Close("cancel");
            _rig.Pump();
            Assert.That(_rig.RunInBackground, Is.True);
            b.Close("cancel");
            _rig.Pump();
            Assert.That(_rig.RunInBackground, Is.False);
            Assert.That(_rig.RunInBackgroundSets, Is.EqualTo(new[] { true, false }));
        }

        [Test]
        public void ARestoreHoldsRunInBackgroundAndTheResetPutsItBack()
        {
            _rig.Configure();
            SCFG.Restore();
            Assert.That(_rig.RunInBackground, Is.True);
            _rig.Host.Last.Close("close");
            _rig.Pump();
            Assert.That(_rig.RunInBackground, Is.False, "closing the restore screen puts it back");
            SCFG.Restore();
            Assert.That(_rig.RunInBackground, Is.True);
            SCFG.ResetStatics(); // the domain-reload reset, with only a restore open
            Assert.That(_rig.RunInBackground, Is.False);
            Assert.That(SCFG.Current.HoldingBackground, Is.False);
        }

        [Test]
        public void QuittingPutsRunInBackgroundBack()
        {
            OpenManualPayment();
            Assert.That(_rig.RunInBackground, Is.True);
            SCFG.Shutdown();
            Assert.That(_rig.RunInBackground, Is.False);
        }

        [Test]
        public void ARefusedSheetLeavesRunInBackgroundAlone()
        {
            _rig.Configure(ManualOnly());
            _rig.Host.Refuse = true;
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            Assert.That(_rig.RunInBackground, Is.False);
        }

        [Test]
        public void MobileNeverTouchesRunInBackground()
        {
            _rig.Platform.IsMobile = true;
            var (_, screen, _) = OpenManualPayment();
            screen.Close("cancel");
            _rig.Pump();
            Assert.That(_rig.RunInBackgroundSets, Is.Empty, "mobile ignores the setting");
        }

        [Test]
        public void WebGLNeverTouchesRunInBackground()
        {
            _rig.Web();
            _rig.Configure(ManualOnly());
            SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "3", Token = "USDC" });
            _rig.Settle();
            _rig.Host.Last.Close("cancel");
            _rig.Pump();
            Assert.That(_rig.RunInBackgroundSets, Is.Empty, "the checkout page is in the game's own tab");
        }

        [Test]
        public void QuittingWithTheSheetOpenResolvesWithoutDelivering()
        {
            var got = new List<ScfgPaymentResult>();
            var events = new List<ScfgPaymentResult>();
            SCFG.PaymentResolved += events.Add;
            var (call, screen, ctl) = OpenManualPayment();
            SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "3", Token = "USDC" }, got.Add);
            var sid = ctl.SessionId!;
            SCFG.Shutdown();
            var r = Result(call);
            Assert.That(r.Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
            Assert.That(got, Is.Empty);
            Assert.That(events, Is.Empty);
            Assert.That(_rig.Host.ClosedScreens.Count, Is.EqualTo(2));
            // the session is not marked cancelled: the next launch keeps watching it
            var stored = ScfgStore.Load(_rig.StorePath).Pending;
            Assert.That(((JsonObject)stored[sid]!)["ui_status"], Is.EqualTo("open"));
        }

        [Test]
        public void ClosingTheSheetClosesItsPageFirst()
        {
            _rig.Configure(new ScfgMethods { Qr = false, Manual = false });
            var call = SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            var screen = _rig.Host.Last;
            _rig.Settle();
            var ctl = screen.Checkout;
            Assert.That(ctl.Core.Step, Is.EqualTo("browser"));
            ctl.OpenBrowser();
            Assert.That(_rig.Platform.Opened.Single(), Is.EqualTo(RecordingServer.PageUrl));
            Assert.That(ctl.Core.Step, Is.EqualTo("waiting"));
            _rig.Pump();
            Assert.That(_rig.Servers.Single().Pumps, Is.GreaterThan(0), "the controller's Poll pumps its page");
            screen.Close("cancel");
            _rig.Pump();
            Assert.That(_rig.Journal, Is.EqualTo(new[] { "server start", "server stop" }));
            Assert.That(_rig.Host.Journal.Last(), Is.EqualTo("close screen"));
            Assert.That(Result(call).Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
        }

        [Test]
        public void TwoPayCallsResolveIndependently()
        {
            _rig.Configure(ManualOnly());
            var a = SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            var sa = _rig.Host.Last;
            var b = SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "3", Token = "USDC" });
            var sb = _rig.Host.Last;
            _rig.Settle();
            sb.Close("cancel");
            _rig.Pump();
            Assert.That(Done(a), Is.False);
            Assert.That(Result(b).Kind, Is.EqualTo(ScfgPayKind.Tip));
            sa.Close("cancel");
            _rig.Pump();
            Assert.That(Result(a).Kind, Is.EqualTo(ScfgPayKind.Unlock));
        }

        [Test]
        public void CopyAndPasteUseTheClipboard()
        {
            var (_, screen, ctl) = OpenManualPayment();
            screen.Checkout.Copy("hello");
            Assert.That(_rig.Platform.Clipboard, Is.EqualTo("hello"));
            _rig.Platform.Clipboard = "  " + ScfgRig.Tx + " extra words";
            screen.Checkout.Paste();
            Assert.That(ctl.TxInput, Is.EqualTo(ScfgRig.Tx));
            _rig.Platform.Clipboard = "";
            screen.Checkout.Paste();
            Assert.That(ctl.TxInput, Is.EqualTo(ScfgRig.Tx), "an empty clipboard pastes nothing");
        }

        [Test]
        public void CleanPasteMatchesRenpyAndStopsAtNinety()
        {
            Assert.That(SCFG.CleanPaste("  0xAB-cd ef"), Is.EqualTo("0xABcd"));
            Assert.That(SCFG.CleanPaste(null), Is.EqualTo(""));
            Assert.That(SCFG.CleanPaste(" x"), Is.EqualTo("x"));
            Assert.That(SCFG.CleanPaste(new string('a', 500)).Length, Is.EqualTo(90));
            Assert.That(SCFG.CleanPaste("é" + new string('b', 95)), Is.EqualTo(new string('b', 90)));
        }

        [Test]
        public void TheManualHashIsReportedToTheEngine()
        {
            var (_, screen, ctl) = OpenManualPayment();
            ctl.TxInput = ScfgRig.Tx;
            Assert.That(ctl.SubmitHash(), Is.True);
            _rig.Settle();
            Assert.That(((JsonObject)SCFG.Current.Worker!.Snapshot.Pending[ctl.SessionId!]!)["hint_tx"], Is.EqualTo(ScfgRig.Tx));
            Assert.That(((JsonObject)ScfgStore.Load(_rig.StorePath).Pending[ctl.SessionId!]!)["hint_tx"], Is.EqualTo(ScfgRig.Tx),
                "a session's hint is saved at once");
        }

        [Test]
        public void MobileOffersTheWalletAppLink()
        {
            _rig.Platform.IsMobile = true;
            _rig.Configure(new ScfgMethods { Browser = false, Manual = false });
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            var screen = _rig.Host.Last;
            _rig.Settle();
            Assert.That(screen.Checkout.Core.Step, Is.EqualTo("qr"));
            screen.Checkout.OpenWalletApp();
            Assert.That(_rig.Platform.Opened.Single(), Does.StartWith("ethereum:"));
        }

        [Test]
        public void ADiscardedSessionDropsItsCallback()
        {
            var got = new List<ScfgPaymentResult>();
            var (_, screen, ctl) = OpenManualPayment(got.Add);
            _rig.Pump();
            Assert.That(SCFG.Current.Callbacks.ContainsKey(ctl.SessionId!), Is.True);
            var sid = ctl.SessionId!;
            screen.Close("keep_playing");
            _rig.Pump();
            SCFG.Current.Worker!.Submit(Commands.Sync(e => e.DiscardSession(sid)));
            _rig.Settle();
            _rig.Settle();
            Assert.That(SCFG.Current.Callbacks.ContainsKey(sid), Is.False);
        }

        // --- the restore flow ------------------------------------------------------------------------

        [Test]
        public void RestoreBeforeConfigureFails()
        {
            Assert.That(Result(SCFG.Restore()).Error, Is.EqualTo("configure.not_called"));
        }

        [Test]
        public void RestoreFindsAPaymentAndNotifies()
        {
            SCFG.Notify = true;
            _rig.Configure();
            var callbacks = new List<ScfgRestoreResult>();
            int changed = 0;
            SCFG.ReceiptsChanged += () => changed++;
            var call = SCFG.Restore(callbacks.Add);
            var screen = _rig.Host.Last;
            Assert.That(screen.Kind, Is.EqualTo("restore"));
            // a 4.99 USDC payment to Dev: chapter2 or a tip, the player chooses
            var log = FakeEvmNode.TransferLog(ScfgRig.UsdcArb, ScfgRig.Payer, ScfgRig.Dev, 4990000, 999, ScfgRig.Tx);
            _rig.Node.Receipts[ScfgRig.Tx] = new JsonObject
            {
                { "status", "0x1" }, { "blockNumber", "0x3e7" }, { "transactionHash", ScfgRig.Tx }, { "logs", new List<object?> { log } },
            };
            var ctl = screen.Restore;
            ctl.Core.TxInput = ScfgRig.Tx;
            Assert.That(ctl.Core.Check(), Is.True);
            _rig.Settle();
            _rig.Settle();
            Assert.That(ctl.Core.State, Is.EqualTo("done").Or.EqualTo("choose"), ctl.Core.Error + " " + string.Join("; ", _rig.Reported));
            if (ctl.Core.State == "choose")
            {
                foreach (var i in ctl.Core.PendingChoices()) ctl.Core.Choose(i, "chapter2");
                ctl.Core.Confirm();
                _rig.Settle();
            }
            Assert.That(ctl.Core.State, Is.EqualTo("done"));
            Assert.That(callbacks.Single().Status, Is.EqualTo(ScfgRestoreStatus.Restored));
            Assert.That(_rig.Host.Notes.Single(), Is.EqualTo("Purchases restored."));
            Assert.That(changed, Is.EqualTo(1));
            Assert.That(SCFG.Owns("chapter2"), Is.True);
            // a host that checks again after done: the callback is not called twice
            ctl.Core.TxInput = ScfgRig.Tx;
            ctl.Core.Check();
            _rig.Settle();
            _rig.Settle();
            Assert.That(callbacks.Count, Is.EqualTo(1));
            screen.Close("close");
            _rig.Pump();
            var r = Result(call);
            Assert.That(r.Status, Is.EqualTo(ScfgRestoreStatus.Restored));
            Assert.That(r.Restored.Single().TxHash, Is.EqualTo(ScfgRig.Tx));
        }

        [Test]
        public void RestoreClosedBeforeAnythingIsCancelled()
        {
            _rig.Configure();
            var call = SCFG.Restore();
            var screen = _rig.Host.Last;
            screen.Restore.Core.TxInput = "nonsense";
            Assert.That(screen.Restore.Core.Check(), Is.False);
            screen.Close("close");
            _rig.Pump();
            var r = Result(call);
            Assert.That(r.Status, Is.EqualTo(ScfgRestoreStatus.Cancelled));
            Assert.That(r.Error, Is.EqualTo("invalid_hash"));
        }

        [Test]
        public void ResetStaticsForgetsTheLastSession()
        {
            var (call, _, _) = OpenManualPayment();
            int raised = 0;
            SCFG.PaymentResolved += _ => raised++;
            SCFG.ResetStatics();
            Assert.That(Result(call).Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
            Assert.That(raised, Is.EqualTo(0));
            Assert.That(SCFG.Current.Worker, Is.Null);
            Assert.That(SCFG.Current.Config, Is.Null);
            Assert.That(SCFG.Current.Checkouts.Count + SCFG.Current.Waits.Count, Is.EqualTo(0));
            Assert.That(SCFG.Current.PaymentResolved, Is.Null);
        }
    }
}
