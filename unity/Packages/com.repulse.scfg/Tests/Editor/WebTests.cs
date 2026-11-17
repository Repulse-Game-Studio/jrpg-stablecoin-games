// Twin of godot/tests/test_web_page.gd, test_web_mirror.gd and test_scfg_web.gd: the WebGL side of the package without a
// browser. ScfgWebPage and ScfgWebMirror run on a FakeWebJs; the facade runs as a WebGL build (ScfgRig.Web) on one. The
// real page (ScfgWeb.jslib, bridge.js) is exercised by tools/run_unity_web_e2e.py.
#nullable enable
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using NUnit.Framework;
using Scfg.Core;
using UnityEngine;

namespace Scfg.Tests
{
    public sealed class WebPageTests
    {
        private static readonly Dictionary<string, string> Files = new Dictionary<string, string>
        {
            { "bridge.js", "/*bridge*/" }, { "evm.js", "/*evm*/" }, { "solana.js", "/*sol*/" }, { "app.js", "/*app*/" },
            { "index.html", "<html><style>s</style><main>m</main></html>" },
        };

        private static byte[]? Asset(string name) => Files.TryGetValue(name, out var t) ? Encoding.UTF8.GetBytes(t) : null;

        private static ScfgWebPage Page(FakeWebJs js, List<string>? warnings = null) =>
            new ScfgWebPage(js, Asset, warnings == null ? (System.Action<string>?)null : warnings.Add);

        [Test]
        public void InstallLoadsBridgeFirstThenThePageOnce()
        {
            var js = new FakeWebJs();
            var page = Page(js);
            page.Install();
            page.Install();
            Assert.That(js.Loaded, Is.EqualTo(new[] { "/*bridge*/", "/*evm*/", "/*sol*/", "/*app*/" }));
            Assert.That(js.CallsTo("setPage").Single(), Is.EqualTo(new List<object?> { Files["index.html"] }));
            Assert.That(page.Installed, Is.True);
        }

        [Test]
        public void InstallReadsTheSharedPageFilesFromResources()
        {
            var js = new FakeWebJs();
            var warnings = new List<string>();
            new ScfgWebPage(js, ScfgWebAssets.Load, warnings.Add).Install();
            Assert.That(warnings, Is.Empty);
            Assert.That(js.Loaded.Count, Is.EqualTo(4));
            Assert.That(js.Loaded[0], Does.Contain("window.scfgBridge"), "bridge.js first");
            Assert.That(js.Loaded[3], Does.Contain("scfgCheckout"), "app.js last");
            Assert.That((string)js.CallsTo("setPage").Single()[0]!, Does.Contain("<main"));
        }

        [Test]
        public void InstallWarnsAboutAMissingPageFile()
        {
            var js = new FakeWebJs();
            var warnings = new List<string>();
            new ScfgWebPage(js, name => name == "evm.js" || name == "index.html" ? null : Asset(name), warnings.Add).Install();
            Assert.That(warnings.Count, Is.EqualTo(2));
            Assert.That(warnings[0], Does.Contain("ScfgWeb/evm.js.bytes"));
            Assert.That(warnings[1], Does.Contain("ScfgWeb/index.html.bytes"));
            Assert.That(js.Loaded, Is.EqualTo(new[] { "/*bridge*/", "/*sol*/", "/*app*/" }), "the missing file is skipped");
            Assert.That(js.CallsTo("setPage").Single(), Is.EqualTo(new List<object?> { "" }));
        }

        [Test]
        public void ValuesReachThePageAsJsonArguments()
        {
            var js = new FakeWebJs();
            var page = Page(js);
            const string hostile = "\"); alert(1); (\"\u2028</script>";
            var info = new JsonObject { { "title", hostile }, { "chain_name", "Arb\nitrum" }, { "units", 4990000L } };
            page.Open("o1", info, null);
            var args = js.CallsTo("openCheckout").Single();
            Assert.That(args[0], Is.EqualTo("o1"));
            Assert.That(Json.Equal(args[1], info), Is.True, "the session info arrives as data, ints intact");
            Assert.That(args[2], Is.Null);
            Assert.That(js.Calls.Single().Args, Does.Not.Contain("\u2028"), "escaped (ensure_ascii)");
        }

        [Test]
        public void HasWalletAsksForTheChainFamily()
        {
            var js = new FakeWebJs();
            var page = Page(js);
            var solana = Chains.Testnet["solana"];
            var arbitrum = Chains.Mainnet["arbitrum"];
            js.Answer("hasWallet", 1L, 0L, true, "1");
            Assert.That(page.HasWallet(solana), Is.True);
            Assert.That(page.HasWallet(arbitrum), Is.False);
            Assert.That(page.HasWallet(arbitrum), Is.True, "a boolean answer");
            Assert.That(page.HasWallet(arbitrum), Is.False, "a string is no answer");
            Assert.That(js.CallsTo("hasWallet")[0], Is.EqualTo(new List<object?> { "solana", solana.WalletChain }));
            Assert.That(js.CallsTo("hasWallet")[1], Is.EqualTo(new List<object?> { "evm", arbitrum.WalletChain }));
            js.Answers["hasWallet"].Enqueue(null);
            Assert.That(page.HasWallet(arbitrum), Is.False, "no answer (the call threw)");
        }

        [Test]
        public void TheOverlayCalls()
        {
            var js = new FakeWebJs();
            var page = Page(js);
            page.Open("o1", new JsonObject(), "data:image/png;base64,AA==");
            page.Show("o1");
            page.SetStatus("o1", new JsonObject { { "state", "waiting" } });
            page.Reply("o1", 7, 200, new JsonObject { { "tx", "x" } });
            page.Close("o1");
            Assert.That(js.Calls.Select(c => c.Function), Is.EqualTo(new[] { "openCheckout", "show", "setStatus", "reply", "closeCheckout" }));
            Assert.That(Json.Equal(js.CallsTo("reply").Single(), new List<object?> { "o1", 7L, 200L, new JsonObject { { "tx", "x" } } }), Is.True);
            Assert.That(js.CallsTo("openCheckout").Single()[2], Is.EqualTo("data:image/png;base64,AA=="));
        }

        [Test]
        public void TakeReadsTheMessagesWithIntsKept()
        {
            var js = new FakeWebJs();
            var page = Page(js);
            js.Answer("take", "[{\"type\": \"tx\", \"body\": {\"tx\": \"0xab\"}}, {\"type\": \"build\", \"id\": 7, \"body\": {}}]");
            var got = (List<object?>)page.Take("o1")!;
            Assert.That(got.Count, Is.EqualTo(2));
            Assert.That(((JsonObject)got[1]!)["id"], Is.EqualTo(7L));
            js.Answer("take", "{\"type\": \"tx\"}", "not json", null);
            Assert.That(page.Take("o1"), Is.Empty, "an object is no messages");
            Assert.That(page.Take("o1"), Is.Empty, "text that is not JSON");
            Assert.That(page.Take("o1"), Is.Empty, "no answer");
            js.Answers["take"].Enqueue(null);
            Assert.That(page.Take("o1"), Is.Empty, "the call threw");
        }

        [Test]
        public void TheClipboardAndLinks()
        {
            var js = new FakeWebJs();
            var page = Page(js);
            page.Copy("0xabc");
            page.OpenUrl("https://arbiscan.io/tx/0x1");
            page.RequestPaste();
            Assert.That(js.Calls.Select(c => c.Function), Is.EqualTo(new[] { "copy", "openUrl", "requestPaste" }));
            Assert.That(js.CallsTo("copy").Single(), Is.EqualTo(new List<object?> { "0xabc" }));
            js.Answer("takePaste", "null", "\"\"", "\"0xfeed\"", "42");
            Assert.That(page.TakePaste(), Is.Null, "no answer yet");
            Assert.That(page.TakePaste(), Is.EqualTo(""), "the browser refused");
            Assert.That(page.TakePaste(), Is.EqualTo("0xfeed"));
            Assert.That(page.TakePaste(), Is.EqualTo(""), "anything else is a refusal");
            js.Answers["takePaste"].Enqueue(null);
            Assert.That(page.TakePaste(), Is.Null, "the page gave no answer at all");
        }

        [Test]
        public void TakeCopyFailedReadsTheOneShotFlag()
        {
            var js = new FakeWebJs();
            var page = Page(js);
            js.Answer("takeCopyFailed", 1L, 0L);
            Assert.That(page.TakeCopyFailed(), Is.True);
            Assert.That(page.TakeCopyFailed(), Is.False);
            Assert.That(page.TakeCopyFailed(), Is.False, "the default");
        }
    }

    public sealed class WebMirrorTests
    {
        private const string Key = "scfg:Game:k";

        [Test]
        public void TheKeyFollowsTheProductNameAndStorageId()
        {
            // sha256("Ruby Dreams") and sha256("ruby-dreams-2"), as Godot's String.sha256_text
            Assert.That(ScfgWebMirror.KeyFor("Ruby Dreams"),
                Is.EqualTo("scfg:Ruby Dreams:" + Bytes.ToHex(Sha256.Hash(Encoding.UTF8.GetBytes("Ruby Dreams")))));
            Assert.That(ScfgWebMirror.KeyFor("Ruby Dreams", ""), Is.EqualTo(ScfgWebMirror.KeyFor("Ruby Dreams")));
            Assert.That(ScfgWebMirror.KeyFor("Ruby Dreams", "ruby-dreams-2"),
                Is.EqualTo("scfg:Ruby Dreams:" + Bytes.ToHex(Sha256.Hash(Encoding.UTF8.GetBytes("ruby-dreams-2")))));
            Assert.That(ScfgWebMirror.KeyFor("Ruby Dreams", "x"), Does.Match("^scfg:Ruby Dreams:[0-9a-f]{64}$"));
            Assert.That(ScfgWebMirror.KeyFor("abc"),
                Is.EqualTo("scfg:abc:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        }

        [Test]
        public void WriteThenReadRoundTripsReceiptsWithInts()
        {
            var js = new FakeWebJs();
            var mirror = new ScfgWebMirror(js, Key);
            var receipts = new JsonObject { { "arbitrum:0xab:3", new JsonObject { { "block", 990L }, { "amount", "4.99" } } } };
            Assert.That(mirror.Write(receipts), Is.Null);
            Assert.That(js.LocalStorage![Key], Is.EqualTo("{\"version\": 1, \"receipts\": {\"arbitrum:0xab:3\": {\"block\": 990, \"amount\": \"4.99\"}}}"));
            var got = mirror.Read();
            Assert.That(got.Warning, Is.Null);
            Assert.That(got.Available, Is.True);
            Assert.That(Json.Equal(got.Receipts, receipts), Is.True);
            Assert.That(((JsonObject)got.Receipts["arbitrum:0xab:3"]!)["block"], Is.EqualTo(990L));
        }

        [Test]
        public void NothingStoredIsNoReceiptsAndNoWarning()
        {
            var got = new ScfgWebMirror(new FakeWebJs(), Key).Read();
            Assert.That(got.Receipts.Count, Is.EqualTo(0));
            Assert.That(got.Warning, Is.Null);
            Assert.That(got.Available, Is.True);
        }

        [TestCase("{\"version\": 1, \"receipts\": ", "is not valid JSON")]
        [TestCase("[1]", "is not a JSON object")]
        [TestCase("{\"version\": 2, \"receipts\": {}}", "has an unknown version 2")]
        [TestCase("{\"version\": true, \"receipts\": {}}", "has an unknown version True")]
        [TestCase("{\"version\": 1, \"receipts\": []}", "has no receipts object")]
        public void AValueThatIsNotTheDocumentIsIgnoredWithAWarning(string stored, string why)
        {
            var js = new FakeWebJs();
            js.LocalStorage![Key] = stored;
            var got = new ScfgWebMirror(js, Key).Read();
            Assert.That(got.Receipts.Count, Is.EqualTo(0));
            Assert.That(got.Available, Is.True);
            Assert.That(got.Warning, Does.Contain(why).And.Contain("ignored and replaced").And.Contain(Key));
        }

        [Test]
        public void AnUnusableLocalStorageIsAWarningNeverAnError()
        {
            var none = new FakeWebJs { LocalStorage = null };
            var got = new ScfgWebMirror(none, Key).Read();
            Assert.That(got.Available, Is.False);
            Assert.That(got.Warning, Does.Contain("cannot be read (no localStorage)"));
            Assert.That(new ScfgWebMirror(none, Key).Write(new JsonObject()), Does.Contain("did not keep"));
            var blocked = new FakeWebJs { StorageThrows = "SecurityError: denied" };
            Assert.That(new ScfgWebMirror(blocked, Key).Read().Warning, Does.Contain("SecurityError: denied"));
            var full = new FakeWebJs { WriteThrows = "QuotaExceededError: full" };
            Assert.That(new ScfgWebMirror(full, Key).Write(new JsonObject()), Does.Contain("QuotaExceededError: full"));
            Assert.That(new ScfgWebMirror(full, Key).Read().Available, Is.True, "a full localStorage still reads");
            Assert.That(new ScfgWebMirror(new BrokenJs(), Key).Read().Warning, Does.Contain("the page gave no answer"));
        }

        private sealed class BrokenJs : IScfgWebJs
        {
            public bool Load(string code) => false;
            public string? Call(string function, string argsJson) => null;
            public string? Storage(string op, string key, string? value) => op == "get" ? "not json" : null;
            public void SyncFs() { }
            public bool FsPersistent() => true;
        }

        [Test]
        public void UpdateMergesIntoTheCopy()
        {
            var js = new FakeWebJs();
            var mirror = new ScfgWebMirror(js, Key);
            mirror.Write(new JsonObject { { "a", new JsonObject { { "v", 1L } } }, { "b", new JsonObject { { "v", 1L } } } });
            var dropped = new HashSet<string> { "a" };
            Assert.That(mirror.Update(new JsonObject { { "b", new JsonObject { { "v", 2L } } }, { "c", new JsonObject() } }, dropped), Is.Null);
            var got = mirror.Read().Receipts;
            Assert.That(got.Keys, Is.EqualTo(new[] { "b", "c" }), "a dropped, b replaced in place, c added");
            Assert.That(((JsonObject)got["b"]!)["v"], Is.EqualTo(2L));
            int writes = js.StorageOps.Count(o => o.StartsWith("set "));
            Assert.That(mirror.Update(new JsonObject { { "b", new JsonObject { { "v", 2L } } } }, new HashSet<string>()), Is.Null);
            Assert.That(js.StorageOps.Count(o => o.StartsWith("set ")), Is.EqualTo(writes), "nothing changed: no write");
            js.LocalStorage![Key] = "broken";
            Assert.That(mirror.Update(new JsonObject(), new HashSet<string>()), Is.Null);
            Assert.That(js.LocalStorage[Key], Is.EqualTo("{\"version\": 1, \"receipts\": {}}"), "a broken copy is written over");
        }
    }

    /// <summary>The facade as a WebGL build (Godot's test_scfg_web.gd).</summary>
    public sealed class FacadeWebTests
    {
        private ScfgRig _rig = null!;
        private FakeWebJs _js = null!;

        [SetUp]
        public void Fresh()
        {
            _rig = new ScfgRig();
            _js = _rig.Web();
        }

        [TearDown]
        public void Clean() => _rig.Dispose();

        private static T Result<T>(Awaitable<T> a)
        {
            Assert.That(a.GetAwaiter().IsCompleted, Is.True, "the call has not resolved");
            return a.GetAwaiter().GetResult();
        }

        private string MirrorKey => SCFG.MirrorKey();

        private JsonObject? Mirror(FakeWebJs? js = null) =>
            (js ?? _js).LocalStorage!.TryGetValue(MirrorKey, out var text) ? (JsonObject)Json.Loads(text)! : null;

        private int MirrorWrites(FakeWebJs? js = null) => (js ?? _js).StorageOps.Count(o => o.StartsWith("set "));

        private static JsonObject Receipt(string tx) => new JsonObject
        {
            { "chain", "arbitrum" }, { "tx_hash", tx }, { "log_index", 3L }, { "block", 990L }, { "kind", "unlock" },
            { "item", "chapter2" }, { "amount", "4.99" }, { "token", "USDC" }, { "payer", ScfgRig.Payer.ToLowerInvariant() },
            { "recipient", ScfgRig.Dev.ToLowerInvariant() }, { "source", "payment" },
        };

        private static string RKey(string tx) => "arbitrum:" + tx + ":3";

        private static string Tx(string pair) => "0x" + string.Concat(Enumerable.Repeat(pair, 32));

        /// <summary>A paid unlock of chapter2 by manual transfer (tx), its sheet closed.</summary>
        private void PayManual(string tx)
        {
            var call = SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "2", Token = "USDC" });
            var screen = _rig.Host.Last;
            _rig.Settle();
            var ctl = screen.Checkout.Core;
            if (ctl.Step == "amount") ctl.NextStep();
            if (ctl.Step == "method") ctl.ChooseMethod("manual");
            _rig.Settle();
            Assert.That(ctl.State, Is.EqualTo("waiting"), string.Join("; ", _rig.Reported));
            _rig.PaySession(ctl, tx: tx);
            _rig.Tick();
            Assert.That(ctl.State, Is.EqualTo("paid"), string.Join("; ", _rig.Reported));
            screen.Close("finish");
            _rig.Pump();
            Assert.That(Result(call).Status, Is.EqualTo(ScfgPaymentStatus.Paid));
        }

        // --- the page --------------------------------------------------------------------------------------

        [Test]
        public void TheWebCheckoutIsTheOverlayNotAServer()
        {
            _rig.Configure(new ScfgMethods { Qr = false, Manual = false });
            var call = SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            var screen = _rig.Host.Last;
            _rig.Settle();
            var ctl = screen.Checkout;
            Assert.That(ctl.Core.Methods(), Is.EqualTo(new[] { "browser" }));
            Assert.That(ctl.Core.Step, Is.EqualTo("browser"));
            Assert.That(_js.Loaded.Count, Is.EqualTo(4), "the page's scripts are in");
            ctl.OpenBrowser();
            Assert.That(ctl.Core.Step, Is.EqualTo("waiting"));
            Assert.That(_rig.Platform.Opened, Is.Empty, "no browser is opened: the overlay shows in the page");
            var open = _js.CallsTo("openCheckout").Single();
            Assert.That(open[0], Does.Match("^o[0-9]+$"));
            Assert.That(((JsonObject)open[1]!)["logo"], Is.EqualTo(false));
            Assert.That(((JsonObject)open[1]!)["family"], Is.EqualTo("evm"));
            Assert.That(open[2], Is.Null);
            _rig.Pump();
            Assert.That(_js.CallsTo("take").Count, Is.GreaterThan(0), "the controller's Poll pumps the overlay");
            Assert.That(_js.CallsTo("setStatus").Count, Is.GreaterThan(0));
            screen.Close("cancel");
            _rig.Pump();
            Assert.That(_js.CallsTo("closeCheckout").Single()[0], Is.EqualTo(open[0]));
            Assert.That(Result(call).Status, Is.EqualTo(ScfgPaymentStatus.Cancelled));
        }

        [Test]
        public void TheOverlayGetsTheLogoAsADataUrl()
        {
            var logo = new Texture2D(2, 2, TextureFormat.RGBA32, false);
            try
            {
                Assert.That(SCFG.Configure(new ScfgOptions
                {
                    Wallet = ScfgRig.Dev, Chains = new List<string> { "arbitrum" }, Logo = logo,
                    Methods = new ScfgMethods { Qr = false, Manual = false },
                }), Is.Null);
                SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" });
                SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
                _rig.Settle();
                _rig.Host.Last.Checkout.OpenBrowser();
                var open = _js.CallsTo("openCheckout").Single();
                Assert.That(((JsonObject)open[1]!)["logo"], Is.EqualTo(true));
                Assert.That((string)open[2]!, Does.StartWith("data:image/png;base64,iVBOR"));
            }
            finally
            {
                Object.DestroyImmediate(logo);
            }
        }

        [Test]
        public void NoWalletInThePageMeansNoBrowserCheckout()
        {
            _js.Wallet = 0;
            _rig.Configure(new ScfgMethods { Qr = false });
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            _rig.Settle();
            Assert.That(_rig.Host.Last.Checkout.Core.Methods(), Is.EqualTo(new[] { "manual" }));
            Assert.That(_js.CallsTo("hasWallet").First(), Is.EqualTo(new List<object?> { "evm", Chains.Mainnet["arbitrum"].WalletChain }));
        }

        [Test]
        public void TheDesktopNeverTouchesThePage()
        {
            var js = new FakeWebJs();
            _rig.WebJs = null;
            _rig.Platform.IsWeb = false;
            _rig.Restart();
            SCFG.Current.Js = js;
            _rig.Configure();
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            _rig.Settle();
            var ctl = _rig.Host.Last.Checkout;
            ctl.Copy("x");
            ctl.Paste();
            ctl.OpenExplorer("https://arbiscan.io/tx/0x1");
            Assert.That(js.Loaded.Concat(js.Calls.Select(c => c.Function)).Concat(js.StorageOps), Is.Empty);
            Assert.That(_rig.Platform.Clipboard, Is.EqualTo("x"));
            Assert.That(_rig.Platform.Opened.Single(), Is.EqualTo("https://arbiscan.io/tx/0x1"));
        }

        [Test]
        public void LinksAndCopyGoThroughThePage()
        {
            _rig.Configure();
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            _rig.Settle();
            var ctl = _rig.Host.Last.Checkout;
            ctl.OpenExplorer("https://arbiscan.io/tx/0x1");
            ctl.Copy("0xabc");
            Assert.That(_js.CallsTo("openUrl").Single(), Is.EqualTo(new List<object?> { "https://arbiscan.io/tx/0x1" }));
            Assert.That(_js.CallsTo("copy").Single(), Is.EqualTo(new List<object?> { "0xabc" }));
            Assert.That(_rig.Platform.Opened, Is.Empty);
            Assert.That(_rig.Platform.Clipboard, Is.EqualTo(""));
            _rig.Pump();
            Assert.That(_rig.Host.Notes, Is.Empty, "the browser kept it");
            _js.Answer("takeCopyFailed", 1L);
            _rig.Pump();
            Assert.That(_rig.Host.Notes.Single(), Is.EqualTo(ScfgUiTexts.Get("copy_failed")));
            _js.Answer("takeCopyFailed", 1L);
            _rig.Pump();
            Assert.That(_rig.Host.Notes.Count, Is.EqualTo(1), "told once per copy");
            ctl.Copy("0xdef");
            int asked = _js.CallsTo("takeCopyFailed").Count;
            _rig.Pump(1, CopyWatch + 0.1);
            Assert.That(_js.CallsTo("takeCopyFailed").Count, Is.EqualTo(asked), "not asked once the watch is over");
        }

        private const double CopyWatch = 5.0;

        [Test]
        public void ThePasteComesFromThePageOnALaterFrame()
        {
            _rig.Configure(new ScfgMethods { Browser = false, Qr = false });
            SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
            _rig.Settle();
            var ctl = _rig.Host.Last.Checkout;
            ctl.Paste();
            Assert.That(_js.CallsTo("requestPaste").Count, Is.EqualTo(1));
            _rig.Pump();
            Assert.That(ctl.Core.TxInput, Is.EqualTo(""), "no answer yet");
            _js.Answer("takePaste", Json.Dumps(" 0xAB!cd\n more"));
            _rig.Pump();
            Assert.That(ctl.Core.TxInput, Is.EqualTo("0xABcd"), "the first word, hash characters only");
            Assert.That(ctl.Core.Error, Is.Null);
            int asked = _js.CallsTo("takePaste").Count;
            _rig.Pump();
            Assert.That(_js.CallsTo("takePaste").Count, Is.EqualTo(asked), "answered: not asked again");
            ctl.Paste();
            _js.Answer("takePaste", Json.Dumps(""));
            _rig.Pump();
            Assert.That(ctl.Core.Error, Is.EqualTo("paste_blocked"), "an empty answer is a refusal");
            Assert.That(ctl.Core.TxInput, Is.EqualTo("0xABcd"), "the field keeps what it had");
        }

        [Test]
        public void ARestoreScreenThatMovedOnDropsThePaste()
        {
            _rig.Configure();
            SCFG.Restore();
            var ctl = _rig.Host.Last.Restore;
            ctl.Paste();
            ctl.Core.TxInput = ScfgRig.Tx;
            ctl.Core.Check();
            Assert.That(ctl.Core.State, Is.Not.EqualTo("input"), "the screen moved on before the answer");
            Assert.That(SCFG.Current.PasteTarget, Is.EqualTo(ctl.Rid), "still waiting for the page");
            _js.Answer("takePaste", Json.Dumps(Tx("cd")));
            int asked = _js.CallsTo("takePaste").Count;
            _rig.Pump();
            Assert.That(_js.CallsTo("takePaste").Count, Is.EqualTo(asked + 1), "the page was asked");
            Assert.That(_js.Answers["takePaste"], Is.Empty, "and its answer taken");
            Assert.That(SCFG.Current.PasteTarget, Is.Null, "the answer ends the wait");
            Assert.That(ctl.Core.TxInput, Is.EqualTo(ScfgRig.Tx), "but is dropped");
            _rig.Pump(2);
            Assert.That(_js.CallsTo("takePaste").Count, Is.EqualTo(asked + 1), "not asked again");
        }

        [Test]
        public void WarnsWhenPersistentDataPathIsNotPersistent()
        {
            _js.Persistent = false;
            _rig.Configure();
            _rig.Pump();
            Assert.That(_rig.Warnings.Count(w => w.Contains("IndexedDB is unavailable")), Is.EqualTo(1));
            _rig.Pump(3);
            Assert.That(_rig.Warnings.Count(w => w.Contains("IndexedDB is unavailable")), Is.EqualTo(1), "once");
        }

        // --- the receipts' copy in localStorage ------------------------------------------------------------

        [Test]
        public void EveryWebSaveMirrorsTheReceiptsAndWritesIndexedDb()
        {
            _rig.Configure(new ScfgMethods { Browser = false, Qr = false });
            _rig.Pump();
            Assert.That(Mirror(), Is.Null, "no receipts yet: nothing to copy");
            Assert.That(_js.StorageOps, Is.Not.Empty.And.All.EqualTo("get " + MirrorKey), "the first read of the state reads the copy");
            int syncs = _js.Syncs;
            PayManual(ScfgRig.Tx);
            Assert.That(_js.Syncs, Is.GreaterThan(syncs), "each save asks the page to write IndexedDB");
            var copy = Mirror()!;
            Assert.That(copy.Keys, Is.EqualTo(new[] { "version", "receipts" }), "the receipts only");
            Assert.That(Json.Equal(copy["receipts"], ScfgStore.Load(_rig.StorePath).Receipts), Is.True);
            Assert.That(((JsonObject)copy["receipts"]!).Count, Is.EqualTo(1));
            int writes = MirrorWrites();
            _rig.Tick();
            _rig.Pump(3, 11);
            Assert.That(MirrorWrites(), Is.EqualTo(writes), "saves that change no receipt do not write the copy again");
            Assert.That(_js.StorageOps.All(o => o == "get " + MirrorKey || o == "set " + MirrorKey), Is.True);
            SCFG.ClearLocalData();
            Assert.That(Json.Equal(Mirror(), new JsonObject { { "version", 1L }, { "receipts", new JsonObject() } }), Is.True,
                "ClearLocalData empties the copy too");
            Assert.That(_rig.Warnings, Is.Empty);
        }

        [Test]
        public void ReceiptsLostFromIndexedDbComeBackFromTheCopy()
        {
            var a = Receipt(ScfgRig.Tx);
            var b = Receipt(Tx("cd"));
            var bCopy = Receipt(Tx("cd"));
            bCopy["block"] = 991L;
            Assert.That(ScfgStore.Save(_rig.StorePath, new JsonObject(), new JsonObject { { RKey(Tx("cd")), b } }), Is.Null);
            _js.LocalStorage![MirrorKey] = Json.Dumps(new JsonObject
            {
                { "version", 1L }, { "receipts", new JsonObject { { RKey(ScfgRig.Tx), a }, { RKey(Tx("cd")), bCopy } } },
            });
            _rig.Configure();
            var want = new JsonObject { { RKey(Tx("cd")), b }, { RKey(ScfgRig.Tx), a } };
            Assert.That(SCFG.Owns("chapter2"), Is.True, "owned before the Worker starts");
            var disk = ScfgStore.Load(_rig.StorePath).Receipts;
            Assert.That(Json.Equal(disk, want), Is.True, "saved back to state.json at once; state.json's wins a conflict");
            Assert.That(disk.Keys, Is.EqualTo(want.Keys), "state.json's first, then the copy's");
            Assert.That(Json.Equal(Mirror()!["receipts"], want), Is.True, "the copy now holds state.json's version");
            Assert.That(_js.Syncs, Is.GreaterThan(0));
            Assert.That(_rig.Warnings, Is.Empty);
            // state.json gone altogether
            File.Delete(_rig.StorePath);
            var js2 = new FakeWebJs();
            js2.LocalStorage![MirrorKey] = Json.Dumps(new JsonObject { { "version", 1L }, { "receipts", new JsonObject { { RKey(ScfgRig.Tx), a } } } });
            _js = _rig.Web(js2);
            _rig.Configure();
            Assert.That(SCFG.Receipts().Keys, Is.EqualTo(new[] { RKey(ScfgRig.Tx) }), "a deleted state.json: the receipts come back");
            Assert.That(SCFG.Pending(), Is.Empty);
            Assert.That(ScfgStore.Load(_rig.StorePath).Receipts.Keys, Is.EqualTo(new[] { RKey(ScfgRig.Tx) }), "saved to a new state.json");
        }

        [Test]
        public void TheCopyIsMergedOnlyOnceConfigured()
        {
            _js.LocalStorage![MirrorKey] = Json.Dumps(new JsonObject { { "version", 1L }, { "receipts", new JsonObject { { RKey(ScfgRig.Tx), Receipt(ScfgRig.Tx) } } } });
            Assert.That(SCFG.Receipts(), Is.Empty, "before Configure: what state.json holds");
            Assert.That(_js.StorageOps, Is.Empty);
            _rig.Configure();
            Assert.That(SCFG.Receipts().Count, Is.EqualTo(1));
        }

        [Test]
        public void ReceiptsMergedFromTheCopyRaiseReceiptsChanged()
        {
            int changed = 0;
            SCFG.ReceiptsChanged += () => changed++;
            _js.LocalStorage![MirrorKey] = Json.Dumps(new JsonObject { { "version", 1L }, { "receipts", new JsonObject { { RKey(ScfgRig.Tx), Receipt(ScfgRig.Tx) } } } });
            Assert.That(SCFG.Receipts(), Is.Empty, "a title screen lists the receipts before Configure");
            int rev = SCFG.ReceiptsRevision;
            _rig.Configure();
            Assert.That(SCFG.Receipts().Count, Is.EqualTo(1));
            Assert.That(changed, Is.EqualTo(1), "the list is told about the merged receipt");
            Assert.That(SCFG.ReceiptsRevision, Is.EqualTo(rev + 1));
            SCFG.Receipts();
            Assert.That(changed, Is.EqualTo(1), "merged once");
        }

        [Test]
        public void ACopyThatAddsNothingRaisesNoReceiptsChanged()
        {
            Assert.That(ScfgStore.Save(_rig.StorePath, new JsonObject(), new JsonObject { { RKey(ScfgRig.Tx), Receipt(ScfgRig.Tx) } }), Is.Null);
            _js.LocalStorage![MirrorKey] = Json.Dumps(new JsonObject { { "version", 1L }, { "receipts", new JsonObject { { RKey(ScfgRig.Tx), Receipt(ScfgRig.Tx) } } } });
            int changed = 0;
            SCFG.ReceiptsChanged += () => changed++;
            _rig.Configure();
            Assert.That(SCFG.Receipts().Count, Is.EqualTo(1));
            Assert.That(changed, Is.EqualTo(0));
        }

        [Test]
        public void TheStorageIdPicksTheCopy()
        {
            var other = ScfgWebMirror.KeyFor(Application.productName, "game-two");
            _js.LocalStorage![other] = Json.Dumps(new JsonObject { { "version", 1L }, { "receipts", new JsonObject { { RKey(ScfgRig.Tx), Receipt(ScfgRig.Tx) } } } });
            Assert.That(SCFG.Configure(new ScfgOptions { Wallet = ScfgRig.Dev, WebStorageId = "game-two" }), Is.Null);
            Assert.That(SCFG.MirrorKey(), Is.EqualTo(other));
            Assert.That(SCFG.Receipts().Count, Is.EqualTo(1));
            Assert.That(_js.StorageOps.All(o => o.EndsWith(other)), Is.True);
        }

        [Test]
        public void MergedReceiptsAreReverifiedLikeStoredOnes()
        {
            var bad = Receipt(Tx("cd"));
            bad["log_index"] = "three";
            _js.LocalStorage![MirrorKey] = Json.Dumps(new JsonObject
            {
                { "version", 1L }, { "receipts", new JsonObject { { RKey(ScfgRig.Tx), Receipt(ScfgRig.Tx) }, { "bad", bad } } },
            });
            _rig.Configure();
            _rig.Settle();
            _rig.Tick();
            Assert.That(SCFG.Receipts(), Is.Empty, "both removed: the chain does not have the one, the other is malformed");
            Assert.That(((JsonObject)Mirror()!["receipts"]!).Count, Is.EqualTo(0), "and the copy follows");
            Assert.That(ScfgStore.Load(_rig.StorePath).Receipts.Count, Is.EqualTo(0));
        }

        [TestCase("none")]
        [TestCase("blocked")]
        [TestCase("full")]
        public void LocalStorageTroubleNeverBreaksSaving(string trouble)
        {
            var js = new FakeWebJs();
            if (trouble == "none") js.LocalStorage = null;
            if (trouble == "blocked") js.StorageThrows = "SecurityError: denied";
            if (trouble == "full") js.WriteThrows = "QuotaExceededError: full";
            _js = _rig.Web(js);
            _rig.Configure(new ScfgMethods { Browser = false, Qr = false });
            PayManual(ScfgRig.Tx);
            PayManual(Tx("cd"));
            Assert.That(ScfgStore.Load(_rig.StorePath).Receipts.Count, Is.EqualTo(2), "state.json is saved");
            Assert.That(SCFG.Current.Dirty, Is.False);
            Assert.That(_rig.Warnings.Count(w => w.Contains("localStorage")), Is.EqualTo(1), string.Join("; ", _rig.Warnings));
            Assert.That(_rig.Warnings.Count, Is.EqualTo(1));
            Assert.That(_rig.Reported, Is.Empty, "never an error");
            if (trouble != "full") Assert.That(MirrorWrites(js), Is.EqualTo(0), "no write is tried once the read failed");
        }

        [Test]
        public void ABrokenCopyIsIgnoredWithAWarningAndReplaced()
        {
            Assert.That(ScfgStore.Save(_rig.StorePath, new JsonObject(), new JsonObject { { RKey(ScfgRig.Tx), Receipt(ScfgRig.Tx) } }), Is.Null);
            _js.LocalStorage![MirrorKey] = "{\"version\": 1, \"receipts\": ";
            _rig.Configure();
            Assert.That(SCFG.Receipts().Count, Is.EqualTo(1));
            Assert.That(_rig.Warnings.Single(), Does.Contain("ignored"));
            Assert.That(Json.Equal(Mirror()!["receipts"], ScfgStore.Load(_rig.StorePath).Receipts), Is.True, "written again from state.json");
        }

        [Test]
        public void AnUnreadableStateFileLeavesTheCopyAlone()
        {
            Directory.CreateDirectory(_rig.StorePath); // a folder where the file should be: cannot be read, never saved over
            var copy = Json.Dumps(new JsonObject { { "version", 1L }, { "receipts", new JsonObject { { RKey(ScfgRig.Tx), Receipt(ScfgRig.Tx) } } } });
            _js.LocalStorage![MirrorKey] = copy;
            _rig.Configure();
            _rig.Pump();
            Assert.That(SCFG.Current.Worker, Is.Null, "payments do not start");
            SCFG.ClearLocalData();
            Assert.That(_js.StorageOps, Is.Empty, "localStorage was not touched");
            Assert.That(_js.LocalStorage[MirrorKey], Is.EqualTo(copy));
        }

        [Test]
        public void ClearLocalDataBeforeConfigureEmptiesTheCopyAtTheMerge()
        {
            _js.LocalStorage![MirrorKey] = Json.Dumps(new JsonObject { { "version", 1L }, { "receipts", new JsonObject { { RKey(ScfgRig.Tx), Receipt(ScfgRig.Tx) } } } });
            SCFG.ClearLocalData();
            _rig.Configure();
            Assert.That(SCFG.Receipts(), Is.Empty);
            Assert.That(((JsonObject)Mirror()!["receipts"]!).Count, Is.EqualTo(0));
        }

        [Test]
        public void TwoTabsOfOneGameKeepEachOthersReceiptsInTheCopy()
        {
            // Tab B: a second facade can't run beside the first, so B's saves are replayed through its own mirror
            // with what B's facade would pass (its receipts and what it dropped), on the same localStorage.
            _rig.Configure(new ScfgMethods { Browser = false, Qr = false });
            _rig.Pump();
            var b = new ScfgWebMirror(new FakeWebJs { LocalStorage = _js.LocalStorage }, MirrorKey);
            PayManual(Tx("11"));
            var r1 = SCFG.Receipts().Keys.Single();
            Assert.That(((JsonObject)Mirror()!["receipts"]!).Keys, Is.EqualTo(new[] { r1 }), "tab A saved R1");
            Assert.That(b.Update(new JsonObject { { "r2", Receipt(Tx("22")) } }, new HashSet<string>()), Is.Null);
            Assert.That(((JsonObject)Mirror()!["receipts"]!).Keys, Is.EqualTo(new[] { r1, "r2" }), "tab B keeps R1 while adding R2");
            PayManual(Tx("33"));
            var mine = SCFG.Receipts().Keys.ToList();
            Assert.That(((JsonObject)Mirror()!["receipts"]!).Keys, Is.EqualTo(new[] { r1, "r2", mine[1] }), "A's next save keeps B's R2");
            SCFG.ClearLocalData();
            Assert.That(((JsonObject)Mirror()!["receipts"]!).Count, Is.EqualTo(0), "ClearLocalData empties the copy");
        }
    }
}
