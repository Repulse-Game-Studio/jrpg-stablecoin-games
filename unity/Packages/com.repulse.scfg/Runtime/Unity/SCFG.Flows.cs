// The facade's payment and restore flows (Godot's pay / restore / _close_checkout and the buttons that leave the game):
// see SCFG.cs for the overview and ScfgScreens.cs for the screen contract.
#nullable enable
using System;
using System.Linq;
using Scfg.Core;
using UnityEngine;

namespace Scfg
{
    public static partial class SCFG
    {
        /// <summary>
        /// Show the payment sheet and resolve when the player closes it. Status: AlreadyOwned (no sheet is shown),
        /// Cancelled, Pending (the player chose to keep playing while the payment confirms), Paid, Underpaid, Expired,
        /// or Failed (a misused call, logged; Error is the ConfigError code).
        /// <para>
        /// Without a callback a cancel and the later results (such as the Paid result of a payment the player kept
        /// playing through) reach <see cref="PaymentResolved"/>, also when this call returns the same result: a payment
        /// that confirms while the sheet is open is raised then, and returned when the player closes the sheet. A
        /// handler of both must be idempotent (or use the callback overload). The Awaitable can be awaited once.
        /// </para>
        /// </summary>
        public static Awaitable<ScfgPaymentResult> Pay(ScfgPayKind kind, ScfgPayRequest request)
        {
            var source = new AwaitableCompletionSource<ScfgPaymentResult>();
            StartPay(kind, request, null, r => source.TrySetResult(r));
            return source.Awaitable;
        }

        /// <summary>
        /// Pay with a callback: <paramref name="callback"/> gets this call's own result when it is AlreadyOwned, Failed
        /// or Cancelled, and every later result of the payment (Paid, Underpaid, Expired), also one confirmed after the
        /// player kept playing. PaymentResolved is not raised for it. Handlers must be idempotent.
        /// </summary>
        public static void Pay(ScfgPayKind kind, ScfgPayRequest request, Action<ScfgPaymentResult> callback)
        {
            if (callback == null)
            {
                Report("Pay(): callback must not be null (use the overload that returns an Awaitable)");
                return;
            }
            StartPay(kind, request, callback, _ => { });
        }

        private static void StartPay(ScfgPayKind kind, ScfgPayRequest? request, Action<ScfgPaymentResult>? cb,
                                     Action<ScfgPaymentResult> done)
        {
            var coreKind = ScfgPayKinds.Core(kind);
            var req = request ?? new ScfgPayRequest();
            PaymentSpec spec;
            try
            {
                if (request == null) throw new TypeError("Pay(): request must not be null");
                if (coreKind == null) throw new TypeError("Pay(): unknown kind " + ((int)kind).ToString(System.Globalization.CultureInfo.InvariantCulture));
                var cfg = S.Config ?? throw NotConfigured();
                spec = Payments.PreparePayment(cfg, coreKind,
                    new JsonObject { { "item", req.Item }, { "amount", req.Amount }, { "token", req.Token } });
            }
            catch (Exception e) when (IsMisuse(e))
            {
                done(FailedPayment(kind, req, cb, e));
                return;
            }
            if (spec.Kind == "unlock" && spec.Item != null && OwnedItems()?.Contains(spec.Item) == true)
            {
                var owned = new ScfgPaymentResult(ScfgPaymentStatus.AlreadyOwned)
                {
                    Kind = kind, Item = spec.Item, Token = spec.Token,
                };
                Call(cb, owned);
                done(owned);
                return;
            }
            var worker = EnsureStarted();
            if (worker == null)
            {
                done(FailedPayment(kind, req, cb, StartFailure()));
                return;
            }
            CheckoutController core;
            try
            {
                core = new CheckoutController(S.Config!, spec, worker.Submit, S.Platform.IsMobile, ServerFactoryFor(),
                    () => worker.Snapshot.Unready, Application.productName, BrowserWalletFor());
            }
            catch (Exception e) when (IsMisuse(e))
            {
                done(FailedPayment(kind, req, cb, e));
                return;
            }
            var ctl = new ScfgCheckoutController(core, NewId("c"), cb);
            S.Checkouts[ctl.Cid] = ctl;
            HoldRunInBackground(); // before the player can switch to the browser
            IScfgScreen? screen = OpenScreen(host => host.OpenPayment(ctl));
            if (screen == null)
            {
                S.Checkouts.Remove(ctl.Cid);
                HoldRunInBackground();
                core.Cancel(); // discards a session start already submitted, as a cancel does
                done(FailedPayment(kind, req, cb, NoScreen("OpenPayment")));
                return;
            }
            Wait(screen, w => done(CloseCheckout(ctl, w)));
        }

        private static IScfgScreen? OpenScreen(Func<IScfgSheetHost, IScfgScreen?> open)
        {
            var host = SheetHost;
            if (host == null) return null;
            try
            {
                return open(host);
            }
            catch (Exception e)
            {
                ReportError("the sheet host could not open a screen:", e);
                return null;
            }
        }

        private static ConfigError NoScreen(string method) =>
            new ConfigError("no screen to show: SCFG.SheetHost." + method + "() returned null", "sheet.unavailable");

        private static void Wait(IScfgScreen screen, Action<ScreenWait> end)
        {
            var wait = new ScreenWait(screen, end);
            if (S.ShuttingDown) wait.Release();
            else S.Waits.Add(wait);
        }

        /// <summary>Ren'Py's _close_checkout: close the controller (cancel / keep playing / finish), then the sheet.</summary>
        private static ScfgPaymentResult CloseCheckout(ScfgCheckoutController ctl, ScreenWait closed)
        {
            S.Checkouts.Remove(ctl.Cid);
            var core = ctl.Core;
            PaymentResult result;
            if (closed.How == "shutdown")
            {
                // Ren'Py's _shutdown: close the page and deliver nothing; the session stays watched
                result = core.Finish();
                if (!closed.Gone) CloseScreen(closed.Screen);
                return ScfgPaymentResult.FromCore(result);
            }
            RegisterCallback(ctl);
            if (closed.How == "cancel")
            {
                result = core.Cancel();
            }
            else if (closed.How == "keep_playing" && !string.IsNullOrEmpty(core.SessionId))
            {
                result = core.KeepPlaying();
                S.Background[ctl.Cid] = ctl; // keeps the browser page answering until final
            }
            else
            {
                result = core.Finish();
            }
            if (!closed.Gone) CloseScreen(closed.Screen);
            var typed = ScfgPaymentResult.FromCore(result);
            if (closed.How == "cancel") Resolve(ctl.Callback, typed);
            return typed;
        }

        private static void CloseScreen(IScfgScreen screen)
        {
            try
            {
                SheetHost?.Close(screen);
            }
            catch (Exception e)
            {
                ReportError("the sheet host could not close a screen:", e);
            }
        }

        /// <summary>A misused Pay: reported, given to the callback, and returned as a Failed result.</summary>
        private static ScfgPaymentResult FailedPayment(ScfgPayKind kind, ScfgPayRequest req, Action<ScfgPaymentResult>? cb, Exception e)
        {
            if (!ReferenceEquals(e, S.StartError)) Report(e.Message);
            var r = new ScfgPaymentResult(ScfgPaymentStatus.Failed)
            {
                Kind = ScfgPayKinds.Core(kind) == null ? (ScfgPayKind?)null : kind,
                Item = req.Item,
                Token = req.Token,
                Error = ErrCode(e),
            };
            Call(cb, r);
            return r;
        }

        /// <summary>
        /// Show the restore screen (the player pastes a transaction hash) and resolve when it is closed: Restored,
        /// NothingFound, Cancelled (closed before a restore finished) or Failed (a misused call; Error is the code).
        /// <paramref name="callback"/>, if given, gets the result once a restore finishes, while the screen is still open.
        /// </summary>
        public static Awaitable<ScfgRestoreResult> Restore(Action<ScfgRestoreResult>? callback = null)
        {
            var source = new AwaitableCompletionSource<ScfgRestoreResult>();
            StartRestore(callback, r => source.TrySetResult(r));
            return source.Awaitable;
        }

        private static void StartRestore(Action<ScfgRestoreResult>? cb, Action<ScfgRestoreResult> done)
        {
            if (S.Config == null)
            {
                done(FailedRestore(cb, NotConfigured()));
                return;
            }
            var worker = EnsureStarted();
            if (worker == null)
            {
                done(FailedRestore(cb, StartFailure()));
                return;
            }
            var ctl = new ScfgRestoreController(new RestoreController(worker.Submit), NewId("r"), cb);
            S.Restores[ctl.Rid] = ctl;
            HoldRunInBackground(); // the player may switch to a wallet or an explorer to find the transaction
            IScfgScreen? screen = OpenScreen(host => host.OpenRestore(ctl));
            if (screen == null)
            {
                S.Restores.Remove(ctl.Rid);
                HoldRunInBackground();
                done(FailedRestore(cb, NoScreen("OpenRestore")));
                return;
            }
            Wait(screen, w =>
            {
                S.Restores.Remove(ctl.Rid);
                if (!w.Gone) CloseScreen(w.Screen);
                done(ScfgRestoreResult.FromCore(ctl.Core.Result()));
            });
        }

        private static ScfgRestoreResult FailedRestore(Action<ScfgRestoreResult>? cb, Exception e)
        {
            if (!ReferenceEquals(e, S.StartError)) Report(e.Message);
            var r = new ScfgRestoreResult(ScfgRestoreStatus.Failed) { Error = ErrCode(e) };
            Call(cb, r);
            return r;
        }

        // --- the browser checkout and the buttons that leave the game ------------------------------------

        /// <summary>The checkout controllers' server factory: the localhost page on desktop, the in-page overlay on the web.</summary>
        private static Func<ServerArgs, ICheckoutServer>? ServerFactoryFor()
        {
            if (S.ServerFactory != null) return S.ServerFactory;
            if (S.Platform.IsWeb) return OverlayFactory();
#if !UNITY_WEBGL || UNITY_EDITOR
            return ScfgDesktop.CreateServer;
#else
            return null;
#endif
        }

        private static bool IsOpen(ScfgCheckoutController ctl) =>
            S.Checkouts.TryGetValue(ctl.Cid, out var open) && ReferenceEquals(open, ctl);

        private static bool IsOpen(ScfgRestoreController ctl) =>
            S.Restores.TryGetValue(ctl.Rid, out var open) && ReferenceEquals(open, ctl);

        internal static void OpenBrowser(ScfgCheckoutController ctl)
        {
            if (!IsOpen(ctl)) return;
            var url = ctl.Core.OpenBrowser();
            if (!string.IsNullOrEmpty(url) && !S.Platform.IsWeb) OpenUrl(url!);
        }

        internal static void OpenWalletApp(ScfgCheckoutController ctl)
        {
            if (!IsOpen(ctl)) return;
            string? uri;
            try
            {
                uri = ctl.Core.PaymentUri();
            }
            catch (Exception e)
            {
                ReportError("cannot build the payment link:", e);
                return;
            }
            if (!string.IsNullOrEmpty(uri)) OpenUrl(uri!);
        }

        /// <summary>
        /// The sheet's "No USDC yet? Buy some with a card": open configure's onramp page for the chosen network (a new tab
        /// with noopener,noreferrer on WebGL, as every link the sheet opens).
        /// </summary>
        internal static void OpenOnramp(ScfgCheckoutController ctl)
        {
            if (!IsOpen(ctl)) return;
            string? url;
            try
            {
                url = ctl.Core.OnrampUrl();
            }
            catch (Exception e)
            {
                ReportError("cannot build the buy link:", e);
                return;
            }
            if (!string.IsNullOrEmpty(url)) OpenUrl(url!);
        }

        internal static void OpenExplorer(string? url)
        {
            if (!string.IsNullOrEmpty(url)) OpenUrl(url!);
        }

        private static void OpenUrl(string url)
        {
            try
            {
                var page = S.Platform.IsWeb ? WebPage() : null;
                if (page != null) page.OpenUrl(url); // window.open with noopener,noreferrer (Application.OpenURL keeps the opener)
                else S.Platform.OpenUrl(url);
            }
            catch (Exception e)
            {
                ReportError("cannot open " + url + ":", e);
            }
        }

        internal static void Copy(string text)
        {
            try
            {
                var page = S.Platform.IsWeb ? WebPage() : null;
                if (page != null)
                {
                    S.CopyWatchUntil = S.Clock() + CopyWatchS;
                    page.Copy(text ?? ""); // the browser may refuse: PumpPage tells the player
                    return;
                }
                S.Platform.ClipboardSet(text ?? "");
            }
            catch (Exception e)
            {
                ReportError("cannot copy:", e);
            }
        }

        internal static void PasteInto(ScfgCheckoutController ctl)
        {
            if (IsOpen(ctl)) Paste(ctl.Cid);
        }

        internal static void PasteInto(ScfgRestoreController ctl)
        {
            if (IsOpen(ctl)) Paste(ctl.Rid);
        }

        /// <summary>Godot's paste_into: the clipboard into the screen's hash field (on the web the page answers later).</summary>
        private static void Paste(string id)
        {
            var page = S.Platform.IsWeb ? WebPage() : null;
            if (page != null)
            {
                S.PasteTarget = id; // navigator.clipboard is asynchronous: PumpPage applies the answer
                try
                {
                    page.RequestPaste();
                }
                catch (Exception e)
                {
                    S.PasteTarget = null;
                    ReportError("cannot read the clipboard:", e);
                }
                return;
            }
            ApplyPaste(id, ClipboardText(), false);
        }

        private static string ClipboardText()
        {
            try
            {
                return S.Platform.ClipboardGet() ?? "";
            }
            catch (Exception e)
            {
                ReportError("cannot read the clipboard:", e);
                return "";
            }
        }

        /// <summary>
        /// A pasted clipboard as a hash field takes it: the first word, hash characters only, at most TxMaxLength
        /// (Ren'Py's <c>"".join(c for c in words[0] if c in TX_CHARS)[:90]</c>).
        /// </summary>
        public static string CleanPaste(string? text)
        {
            var s = text ?? "";
            int i = 0;
            while (i < s.Length && Py.IsSpace(s[i])) i++;
            var output = new System.Text.StringBuilder();
            while (i < s.Length && output.Length < TxMaxLength && !Py.IsSpace(s[i]))
            {
                if (TxChars.IndexOf(s[i]) >= 0) output.Append(s[i]);
                i++;
            }
            return output.ToString();
        }
    }
}
