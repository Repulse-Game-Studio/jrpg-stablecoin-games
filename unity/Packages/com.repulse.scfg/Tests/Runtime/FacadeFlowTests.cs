// The facade in play mode: the hidden runner pumping it every frame, `await SCFG.Pay(...)` resolving on the main
// thread, the real localhost checkout page (Scfg.Desktop.CheckoutServer) answering a browser on its URL, quitting and
// the domain-reload reset closing everything (Enter Play Mode Options with domain reload off), and the logo route.
#nullable enable
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using NUnit.Framework;
using Scfg.Core;
using UnityEngine;
using UnityEngine.Networking;
using UnityEngine.TestTools;

namespace Scfg.Tests
{
    public sealed class FacadeFlowTests
    {
        private ScfgRig _rig = null!;

        [SetUp]
        public void Fresh() => _rig = new ScfgRig(fakeServers: false);

        [TearDown]
        public void Clean() => _rig.Dispose();

        private static IEnumerator Frames(int n = 3) => SheetTests.Frames(n);

        private static GameObject? Runner() =>
            Resources.FindObjectsOfTypeAll<GameObject>().FirstOrDefault(g => g.name == "SCFG Runner");

        private static IEnumerator Get(string url, System.Action<UnityWebRequest> done)
        {
            using var r = UnityWebRequest.Get(url);
            r.timeout = 5;
            var op = r.SendWebRequest();
            while (!op.isDone) yield return null;
            done(r);
        }

        private ScfgPaymentResult? _awaited;

        private async Awaitable PayAndRemember()
        {
            _awaited = await SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
        }

        // An asset without a wallet says so only if nothing configured SCFG by the end of the first frame (a game
        // that applies it with a runtime wallet gets no line); it is setup advice, a warning.
        [UnityTest]
        public IEnumerator TheNoWalletLineComesOnlyIfNothingConfiguredInTheFirstFrame()
        {
            ScfgSettingsNotice.Arm("Unconfigured");
            yield return Frames(2);
            Assert.That(_rig.Warnings.Count(w => w.Contains("the SCFG Settings asset \"Unconfigured\" has no wallet, so it was not applied")),
                Is.EqualTo(1), string.Join("; ", _rig.Warnings));
            ScfgSettingsNotice.Arm("Configured");
            _rig.Configure(new ScfgMethods { Browser = false, Qr = false });
            yield return Frames(2);
            Assert.That(_rig.Warnings.Where(w => w.Contains("\"Configured\"")), Is.Empty);
            Assert.That(_rig.Reported, Is.Empty);
        }

        [UnityTest]
        public IEnumerator TheRunnerPumpsAndAwaitResolvesOnTheMainThread()
        {
            _rig.Configure(new ScfgMethods { Browser = false, Qr = false });
            Assert.That(Runner(), Is.Not.Null);
            Assert.That(Runner()!.hideFlags, Is.EqualTo(HideFlags.HideAndDontSave));
            var main = System.Threading.Thread.CurrentThread.ManagedThreadId;
            _ = PayAndRemember();
            yield return Frames(4);
            var screen = _rig.Host.Last;
            Assert.That(screen.Checkout.Core.State, Is.EqualTo("waiting"), "no manual pump: the runner did it");
            int thread = -1;
            SCFG.PaymentResolved += _ => thread = System.Threading.Thread.CurrentThread.ManagedThreadId;
            screen.Close("cancel");
            yield return Frames();
            Assert.That(_awaited, Is.Not.Null);
            Assert.That(_awaited!.Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
            Assert.That(thread, Is.EqualTo(main));
        }

        [UnityTest]
        public IEnumerator TheRunnerKeepsRunInBackgroundOnWhileTheBrowserCheckoutIsOpen()
        {
            _rig.Configure(new ScfgMethods { Qr = false, Manual = false });
            var call = SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames(4);
            Assert.That(_rig.RunInBackground, Is.True, "on while the sheet is open, so the page answers unfocused");
            var screen = _rig.Host.Last;
            screen.Checkout.OpenBrowser();
            screen.Close("keep_playing");
            yield return Frames();
            Assert.That(_rig.RunInBackground, Is.True, "still on for the kept browser page");
            _rig.PaySession(screen.Checkout.Core);
            _rig.Tick();
            yield return Frames();
            Assert.That(SCFG.Current.Background, Is.Empty);
            Assert.That(_rig.RunInBackground, Is.False, "the game's value is back once the payment is final");
            Assert.That(_rig.RunInBackgroundSets, Is.EqualTo(new[] { true, false }));
            Assert.That(call.GetAwaiter().IsCompleted, Is.True);
        }

        [UnityTest]
        public IEnumerator TheLocalhostPageAnswersOnItsUrlAndClosesWithTheSheet()
        {
            _rig.Configure(new ScfgMethods { Qr = false, Manual = false });
            var call = SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames(4);
            var ctl = _rig.Host.Last.Checkout;
            Assert.That(ctl.Core.Step, Is.EqualTo("browser"), ctl.Core.Error);
            ctl.OpenBrowser();
            var url = _rig.Platform.Opened.Single();
            Assert.That(url, Does.StartWith("http://127.0.0.1:"));
            string? session = null;
            long code = 0;
            yield return Get(url + "session.json", r => { session = r.downloadHandler.text; code = r.responseCode; });
            Assert.That(code, Is.EqualTo(200));
            var info = (JsonObject)Json.Loads(session!)!;
            Assert.That(info["recipient"], Is.EqualTo(ScfgRig.Dev));
            Assert.That(info["amount_text"], Is.EqualTo(ctl.Core.PayAmountText));
            Assert.That(info["chain_name"], Is.EqualTo("Arbitrum"));
            string? page = null;
            yield return Get(url, r => page = r.downloadHandler.text);
            Assert.That(page, Does.Contain("<html"), "index.html comes from the package's Resources");
            yield return Get(url + "app.js", r => code = r.responseCode);
            Assert.That(code, Is.EqualTo(200));
            yield return Get(url + "status", r => session = r.downloadHandler.text);
            Assert.That(((JsonObject)Json.Loads(session!)!)["state"], Is.EqualTo("waiting"));
            _rig.Host.Last.Close("cancel");
            yield return Frames();
            Assert.That(call.GetAwaiter().GetResult().Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
            UnityWebRequest.Result result = UnityWebRequest.Result.Success;
            yield return Get(url + "session.json", r => result = r.result);
            Assert.That(result, Is.EqualTo(UnityWebRequest.Result.ConnectionError), "the page stopped with the sheet");
        }

        [UnityTest]
        public IEnumerator TheLogoIsServedAsPng()
        {
            var logo = new Texture2D(4, 4, TextureFormat.RGBA32, false);
            logo.SetPixels(Enumerable.Repeat(Color.red, 16).ToArray());
            logo.Apply();
            SCFG.Configure(new ScfgOptions
            {
                Wallet = ScfgRig.Dev, Chains = new List<string> { "arbitrum" }, Logo = logo,
                Onramp = new ScfgOnramp { Enabled = false }, // one network without a card button: the network step is skipped
                Methods = new ScfgMethods { Qr = false, Manual = false },
            });
            SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" });
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames(4);
            _rig.Host.Last.Checkout.OpenBrowser();
            var url = _rig.Platform.Opened.Single();
            byte[]? png = null;
            string? type = null;
            yield return Get(url + "logo", r => { png = r.downloadHandler.data; type = r.GetResponseHeader("Content-Type"); });
            Assert.That(type, Is.EqualTo("image/png"));
            Assert.That(png!.Take(4), Is.EqualTo(new byte[] { 0x89, (byte)'P', (byte)'N', (byte)'G' }));
            string? session = null;
            yield return Get(url + "session.json", r => session = r.downloadHandler.text);
            Assert.That(((JsonObject)Json.Loads(session!)!)["logo"], Is.EqualTo(true));
            Object.Destroy(logo);
        }

        [UnityTest]
        public IEnumerator AKeptPlayingPaymentKeepsItsPageUntilFinal()
        {
            _rig.Configure(new ScfgMethods { Qr = false, Manual = false });
            var got = new List<ScfgPaymentResult>();
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" }, got.Add);
            yield return Frames(4);
            var screen = _rig.Host.Last;
            screen.Checkout.OpenBrowser();
            var url = _rig.Platform.Opened.Single();
            screen.Close("keep_playing");
            yield return Frames();
            long code = 0;
            yield return Get(url + "status", r => code = r.responseCode);
            Assert.That(code, Is.EqualTo(200), "the background page still answers");
            _rig.PaySession(screen.Checkout.Core);
            _rig.Tick();
            yield return Frames();
            Assert.That(got.Single().Status, Is.EqualTo(ScfgPaymentStatus.Paid));
            Assert.That(SCFG.Current.Background, Is.Empty);
            UnityWebRequest.Result result = UnityWebRequest.Result.Success;
            yield return Get(url + "status", r => result = r.result);
            Assert.That(result, Is.EqualTo(UnityWebRequest.Result.ConnectionError));
        }

        [UnityTest]
        public IEnumerator TheDomainReloadResetClosesPagesAndDropsTheRunner()
        {
            SCFG.SheetHost = new ScfgDefaultSheetHost();
            _rig.Configure(new ScfgMethods { Qr = false, Manual = false });
            var call = SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames(4);
            var view = (ScfgSheetView)SCFG.Current.Waits.Single().Screen;
            view.Controller.OpenBrowser();
            var url = _rig.Platform.Opened.Single();
            int raised = 0;
            SCFG.PaymentResolved += _ => raised++;
            SCFG.ResetStatics(); // what SubsystemRegistration runs when play starts again without a domain reload
            yield return Frames();
            Assert.That(call.GetAwaiter().GetResult().Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
            Assert.That(raised, Is.EqualTo(0));
            Assert.That(Runner(), Is.Null);
            Assert.That(GameObject.Find("SCFG Payment Sheet"), Is.Null);
            UnityWebRequest.Result result = UnityWebRequest.Result.Success;
            yield return Get(url + "status", r => result = r.result);
            Assert.That(result, Is.EqualTo(UnityWebRequest.Result.ConnectionError), "no page keeps its port into the next session");
            Assert.That(SCFG.Current.Config, Is.Null);
            Assert.That(SCFG.Current.PaymentResolved, Is.Null);
        }

        [UnityTest]
        public IEnumerator QuittingEndsRequestsInFlightThenFlushesTheState()
        {
            _rig.Configure(new ScfgMethods { Browser = false, Qr = false });
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            yield return Frames(4);
            var sid = _rig.Host.Last.Checkout.Core.SessionId!;
            long Stored() => (long)((JsonObject)ScfgStore.Load(_rig.StorePath).Pending[sid]!)["cursor"]!;
            var saved = Stored();
            _rig.Node.Head += 40;
            _rig.Pump(2, 4); // scan progress inside the 10 s window: in memory, not on disk
            Assert.That(Stored(), Is.EqualTo(saved));
            // a transport of the facade's own with a request in flight (the engine's is the rig's node)
            using var silent = new CannedServer { Silent = true };
            var own = new UnityWebRequestTransport();
            SCFG.Current.OwnTransport = own;
            var inFlight = own.PostJson(silent.Url, new JsonObject(), 30);
            yield return null;
            SCFG.Shutdown();
            Assert.That(inFlight.Exception?.InnerException, Is.TypeOf<HostGone>());
            Assert.That(SCFG.Current.Worker!.Running, Is.False);
            Assert.That(Stored(), Is.GreaterThan(saved), "quitting writes what was not on disk");
        }

        [UnityTest]
        public IEnumerator ARunnerTheGameDestroysComesBack()
        {
            _rig.Configure(new ScfgMethods { Browser = false, Qr = false });
            yield return Frames();
            Object.DestroyImmediate(Runner());
            Assert.That(Runner(), Is.Null);
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            Assert.That(Runner(), Is.Not.Null);
            yield return Frames(4);
            Assert.That(_rig.Host.Last.Checkout.Core.State, Is.EqualTo("waiting"), "the new runner pumps");
        }
    }
}
