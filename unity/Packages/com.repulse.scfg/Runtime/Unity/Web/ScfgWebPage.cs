// Twin of godot/addons/scfg/web_page.gd (and scfg_core/web_page.py): the game's side of a WebGL build, every call into
// the page's JavaScript (the shared checkout page's bridge.js) behind a small method.
//
// The page is an IScfgWebJs (the real one is ScfgBrowserJs, over ScfgWeb.jslib; the tests pass a fake). Arguments
// cross as one JSON array and answers come back as JSON text, read with the core's Json (ints stay ints), so game data
// is never spliced into JavaScript code.
//
// ScfgWebPage is the OverlayCheckout's ui (IOverlayUi) plus the clipboard, links and wallet detection. Every call is
// synchronous: the page answers a paste or a build later, and the facade polls for it once per frame.
#nullable enable
using System;
using System.Collections.Generic;
using Scfg.Core;

namespace Scfg
{
    /// <summary>The checkout page's bridge in the game's own web page (see the file header).</summary>
    public sealed class ScfgWebPage : IOverlayUi
    {
        /// <summary>The page's scripts, bridge.js first (it marks the page as embedded).</summary>
        public static readonly IReadOnlyList<string> Scripts = new[] { "bridge.js", "evm.js", "solana.js", "app.js" };

        private readonly IScfgWebJs _js;
        private readonly Func<string, byte[]?> _loadAsset;
        private readonly Action<string>? _warn;

        public bool Installed { get; private set; }

        /// <summary>
        /// Installed with every page file present and every script run: the browser checkout can work. False when a file
        /// is missing from the build or a script threw in the page (each warned about), so "Browser wallet" is not offered.
        /// </summary>
        public bool Usable { get; private set; }

        /// <param name="js">The page.</param>
        /// <param name="loadAsset">A file of the shared page (Resources/ScfgWeb), or null when the build lacks it.</param>
        /// <param name="warn">Developer advice (the facade's Warn: development builds only).</param>
        public ScfgWebPage(IScfgWebJs js, Func<string, byte[]?> loadAsset, Action<string>? warn = null)
        {
            _js = js ?? throw new TypeError("a ScfgWebPage needs the page");
            _loadAsset = loadAsset ?? throw new TypeError("a ScfgWebPage needs load_asset");
            _warn = warn;
        }

        /// <summary>The text of the page file <paramref name="name"/>, or null (after a warning) when the build lacks it.</summary>
        private string? Text(string name)
        {
            byte[]? bytes = null;
            try
            {
                bytes = _loadAsset(name);
            }
            catch (Exception)
            {
                // a broken loader is a missing file
            }
            if (bytes != null) return new System.Text.UTF8Encoding(false).GetString(bytes);
            _warn?.Invoke("the checkout page file Resources/ScfgWeb/" + name + ".bytes is missing from this build, so the browser"
                          + " checkout cannot work. Reinstall the package (see \"WebGL builds\" in its documentation).");
            return null;
        }

        /// <summary>
        /// Load bridge.js and the checkout page's scripts into the page, once. A file the build lacks is skipped with a
        /// warning; scripts that threw in the page are warned about together (most often a Content-Security-Policy that
        /// forbids eval). Either leaves the page not <see cref="Usable"/>.
        /// </summary>
        public void Install()
        {
            if (Installed) return;
            bool usable = true;
            var failed = new List<string>();
            foreach (var name in Scripts)
            {
                var code = Text(name);
                if (code == null)
                {
                    usable = false;
                    continue;
                }
                bool ran;
                try
                {
                    ran = _js.Load(code);
                }
                catch (Exception)
                {
                    ran = false;
                }
                if (!ran) failed.Add(name);
            }
            if (failed.Count > 0)
            {
                usable = false;
                _warn?.Invoke("the checkout page script" + (failed.Count > 1 ? "s " : " ") + string.Join(", ", failed)
                              + " could not be run in the web page (the"
                              + " browser console says why; most often a Content-Security-Policy whose script-src lacks"
                              + " 'unsafe-eval'), so the browser checkout is off and \"Browser wallet\" is not offered."
                              + " See \"WebGL builds\" in the package's documentation.");
            }
            var page = Text("index.html");
            if (page == null) usable = false;
            CallRaw("setPage", page ?? "");
            Installed = true;
            Usable = usable;
        }

        private string? CallRaw(string function, params object?[] args) =>
            _js.Call(function, Json.Dumps(new List<object?>(args)));

        /// <summary>A call whose answer is JSON text of JSON text (the bridge's take / takePaste): the inner value.</summary>
        private object? CallParsed(string function, params object?[] args) => Parse(Parse(CallRaw(function, args)) as string);

        // --- wallets ---------------------------------------------------------------------------------------

        /// <summary>Is there a wallet in the page that can pay on this chain?</summary>
        public bool HasWallet(Chain chain) => Truthy(Parse(CallRaw("hasWallet", chain.Family, chain.WalletChain)));

        // --- checkout overlay (OverlayCheckout's ui) --------------------------------------------------------

        private readonly HashSet<string> _overlays = new HashSet<string>(StringComparer.Ordinal);

        /// <summary>How many overlays are open in the page (shown or hidden), so that nobody polls the page without one.</summary>
        public int OpenOverlays => _overlays.Count;

        public void Open(string key, JsonObject sessionInfo, string? logoUrl)
        {
            _overlays.Add(key);
            CallRaw("openCheckout", key, sessionInfo, logoUrl);
        }

        public void Show(string key) => CallRaw("show", key);

        public void Close(string key)
        {
            _overlays.Remove(key);
            CallRaw("closeCheckout", key);
        }

        /// <summary>Is one of the overlays shown (not hidden by its close button or after a payment)?</summary>
        public bool OverlayShown() => _overlays.Count > 0 && _js.OverlayShown();

        /// <summary>The messages the overlay's page posted since the last call (a list; an empty one for anything else).</summary>
        public object? Take(string key) => CallParsed("take", key) is List<object?> list ? list : new List<object?>();

        public void SetStatus(string key, JsonObject status) => CallRaw("setStatus", key, status);

        public void Reply(string key, long id, int status, JsonObject body) => CallRaw("reply", key, id, (long)status, body);

        // --- clipboard and links ---------------------------------------------------------------------------

        public void Copy(string text) => CallRaw("copy", text);

        public void RequestPaste() => CallRaw("requestPaste");

        /// <summary>null until the browser answers; "" if it refused; else the clipboard text.</summary>
        public string? TakePaste()
        {
            var answer = Parse(CallRaw("takePaste"));
            if (!(answer is string text)) return null; // the page gave no answer at all
            var parsed = Parse(text);
            if (parsed == null && text.Trim() == "null") return null;
            return parsed as string ?? "";
        }

        /// <summary>True once after a Copy the browser refused; false otherwise.</summary>
        public bool TakeCopyFailed() => Truthy(Parse(CallRaw("takeCopyFailed")));

        /// <summary>window.open with noopener,noreferrer (Application.OpenURL keeps an opener).</summary>
        public void OpenUrl(string url) => CallRaw("openUrl", url);

        // --- values ----------------------------------------------------------------------------------------

        /// <summary>JSON text from the page as a value; null for anything that is not JSON text.</summary>
        internal static object? Parse(string? text)
        {
            if (text == null) return null;
            try
            {
                return Json.Loads(text);
            }
            catch (Exception)
            {
                return null; // not JSON: no answer
            }
        }

        /// <summary>A number or boolean the page answered.</summary>
        internal static bool Truthy(object? answer)
        {
            switch (answer)
            {
                case bool b: return b;
                case long l: return l != 0;
                case double d: return d != 0 && !double.IsNaN(d);
                case System.Numerics.BigInteger n: return !n.IsZero;
                default: return false;
            }
        }
    }
}
