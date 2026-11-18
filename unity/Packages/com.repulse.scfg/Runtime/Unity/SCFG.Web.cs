// The facade in a WebGL build (Godot's web branches of scfg.gd): see SCFG.cs for the overview.
//
// THE PAGE. The game already runs in a browser and wallet extensions inject into its page, so the browser checkout is
// the shared checkout page shown over the game (an OverlayCheckout over ScfgWebPage), never a localhost server, and
// "Browser wallet" is offered only when the page has a wallet for the chain (ScfgWebPage.HasWallet). Links open with
// window.open (noopener,noreferrer), Copy writes the browser's clipboard (a refusal within CopyWatchS shows the copy_failed
// toast), and Paste asks the browser, whose answer arrives on a later frame (PumpPage): an empty answer from the
// browser is a refusal and sets the screen's paste_blocked error.
//
// THE KEYBOARD. A Unity player takes every key of its page by default (WebGLInput.captureAllKeyboardInput), so over
// the game the overlay could not be used with the keyboard and its keys would reach the sheet underneath. While an
// overlay is shown (polled once per frame while one is open: its close button and a sent payment only hide it) the
// package sets captureAllKeyboardInput to false, so keys go to what has focus in the page, and puts back the game's
// own value when no overlay is shown any more (unless the game set it to true meanwhile).
//
// WEB: A SECOND COPY OF THE RECEIPTS (ScfgWebMirror). Every Unity WebGL build on one origin shares one IndexedDB
// database, so another game there can delete state.json. On the web a save also merges the receipts (not the pending
// sessions) into a copy in localStorage, keeping receipts other tabs of this game saved and dropping the ones this tab
// removed. The first read of the state once Configure ran merges the copy back (state.json wins a conflict) and saves at
// once; merged receipts are re-verified like any other. Receipts it adds raise ReceiptsChanged (unlike Godot, the
// queries can answer before Configure, so a game may already have shown the receipts without them). A missing, blocked or full localStorage only warns (development
// builds, once). After every save the package asks the page to write persistentDataPath to IndexedDB (Unity does so by
// itself only when the page sets config.autoSyncPersistentDataPath).
#nullable enable
using System;
using System.Collections.Generic;
using Scfg.Core;
using UnityEngine;

namespace Scfg
{
    public static partial class SCFG
    {
        /// <summary>After a Copy on the web: how long the page is asked whether the browser refused it.</summary>
        internal const double CopyWatchS = 5.0;

        /// <summary>The page this WebGL player runs in (a test hook replaces it); null off the web.</summary>
        private static IScfgWebJs? Js() => S.Js ??= ScfgBrowserJs.Create();

        /// <summary>The checkout page's bridge in the game's page, installed on first use; null when there is no page.</summary>
        internal static ScfgWebPage? WebPage()
        {
            if (S.Page != null) return S.Page;
            var js = Js();
            if (js == null) return null;
            S.Page = new ScfgWebPage(js, ScfgWebAssets.Load, Warn);
            try
            {
                S.Page.Install();
            }
            catch (Exception e)
            {
                ReportError("cannot load the checkout page into the web page:", e);
            }
            return S.Page;
        }

        /// <summary>
        /// The checkout controllers' browser_wallet on the web: does the page have a wallet for the chain (never, when the
        /// checkout page could not be installed: ScfgWebPage.Usable).
        /// </summary>
        private static Func<Chain, bool>? BrowserWalletFor()
        {
            if (!S.Platform.IsWeb) return null;
            var page = WebPage();
            if (page == null) return null;
            if (!page.Usable) return _ => false;
            return chain =>
            {
                try
                {
                    return page.HasWallet(chain);
                }
                catch (Exception e)
                {
                    ReportError("cannot ask the page for a wallet:", e);
                    return false;
                }
            };
        }

        /// <summary>The overlay a web checkout pays in (Godot's _server_factory on the web); null without a usable page.</summary>
        private static Func<ServerArgs, ICheckoutServer>? OverlayFactory()
        {
            var page = WebPage();
            if (page == null || !page.Usable) return null;
            return args =>
            {
                var logoUrl = LogoDataUrl();
                // The core's session_info says whether the core config has a logo (a path); the glue's logo is a texture.
                var info = (JsonObject)DeepCopy.Of(args.SessionInfo)!;
                info["logo"] = logoUrl != null;
                return new OverlayCheckout(page, NewId("o"), info, args.OnTx, args.GetStatus, logoUrl, args.BuildTx,
                    () => S.Clock());
            };
        }

        /// <summary>The logo as a data: URL for the page, or null without one.</summary>
        private static string? LogoDataUrl()
        {
            var png = LogoPng();
            return png == null ? null : "data:image/png;base64," + Convert.ToBase64String(png);
        }

        /// <summary>Web, once per frame: the keyboard, the browser's answer to a Paste button, and whether a Copy was refused.</summary>
        private static void PumpPage()
        {
            var page = S.Page;
            if (page == null) return;
            PumpKeyboard(page);
            try
            {
                if (S.PasteTarget != null)
                {
                    var text = page.TakePaste();
                    if (text != null) ApplyPaste(S.PasteTarget, text, true);
                }
                if (S.Clock() < S.CopyWatchUntil && page.TakeCopyFailed())
                {
                    S.CopyWatchUntil = double.NegativeInfinity;
                    NotifyPlayer(ScfgUiTexts.Get("copy_failed"));
                }
            }
            catch (Exception e)
            {
                S.PasteTarget = null;
                ReportError("the page's clipboard answer failed:", e);
            }
        }

        /// <summary>The page has the keyboard while an overlay is shown (see the file header).</summary>
        private static void PumpKeyboard(ScfgWebPage page)
        {
            if (page.OpenOverlays == 0 && !S.KeyboardHanded) return;
            var js = S.Js;
            if (js == null) return;
            try
            {
                bool shown = page.OverlayShown();
                if (shown == S.KeyboardHanded) return;
                if (shown)
                {
                    S.GameCapturesKeyboard = js.CaptureAllKeyboardInput;
                    js.CaptureAllKeyboardInput = false;
                    S.KeyboardHanded = true;
                }
                else
                {
                    GiveBackKeyboard();
                }
            }
            catch (Exception e)
            {
                ReportError("cannot hand the keyboard to the checkout overlay:", e);
            }
        }

        /// <summary>The game's own captureAllKeyboardInput back (no overlay shown, or the facade shutting down).</summary>
        private static void GiveBackKeyboard()
        {
            if (!S.KeyboardHanded) return;
            S.KeyboardHanded = false;
            var js = S.Js;
            if (js != null && !js.CaptureAllKeyboardInput) js.CaptureAllKeyboardInput = S.GameCapturesKeyboard;
        }

        /// <summary>
        /// Ren'Py's _take_paste: <paramref name="text"/> into the hash field of the screen <paramref name="id"/>.
        /// <paramref name="fromPage"/>: the browser's answer, where an empty one means it refused (error paste_blocked).
        /// </summary>
        private static void ApplyPaste(string id, string? text, bool fromPage)
        {
            if (S.PasteTarget == id) S.PasteTarget = null;
            var cleaned = CleanPaste(text);
            if (S.Checkouts.TryGetValue(id, out var checkout))
            {
                if (cleaned.Length > 0)
                {
                    checkout.Core.TxInput = cleaned;
                    checkout.Core.Error = null;
                }
                else if (fromPage) checkout.Core.Error = "paste_blocked";
                return;
            }
            if (!S.Restores.TryGetValue(id, out var restore) || restore.Core.State != "input") return; // the player moved on
            if (cleaned.Length > 0)
            {
                restore.Core.TxInput = cleaned;
                restore.Core.Error = null;
            }
            else if (fromPage) restore.Core.Error = "paste_blocked";
        }

        // --- the receipts' copy in localStorage -----------------------------------------------------------

        /// <summary>The copy's key: Godot's scheme over the product name and ScfgOptions.WebStorageId.</summary>
        internal static string MirrorKey() => ScfgWebMirror.KeyFor(Application.productName, S.WebStorageId);

        /// <summary>The receipts' copy (web only), made on first use; null when there is no page.</summary>
        private static ScfgWebMirror? WebMirror()
        {
            if (S.Mirror != null) return S.Mirror;
            var js = Js();
            if (js == null) return null;
            S.Mirror = new ScfgWebMirror(js, MirrorKey());
            return S.Mirror;
        }

        /// <summary>Web, at the first read of the state once configured: merge the copy into it (see the file header).</summary>
        private static void MergeMirror()
        {
            S.MirrorMerged = true;
            var mirror = WebMirror();
            if (mirror == null)
            {
                S.MirrorOff = true;
                return;
            }
            var receipts = S.PersistedReceipts!;
            if (S.ClearMirrorAtMerge)
            {
                // ClearLocalData ran before Configure: the copy is emptied, never merged back
                S.ClearMirrorAtMerge = false;
                WriteMirror(true);
                return;
            }
            var got = mirror.Read();
            if (got.Warning != null) Warn(got.Warning);
            if (!got.Available)
            {
                S.MirrorOff = true;
                return;
            }
            bool added = false;
            foreach (var kv in got.Receipts)
            {
                if (receipts.ContainsKey(kv.Key) || !(kv.Value is JsonObject)) continue;
                receipts[kv.Key] = kv.Value;
                added = true;
            }
            S.OwnPrev = KeySet(receipts);
            if (added)
            {
                S.Dirty = true;
                S.LastSave = S.Clock();
                Save(); // also brings the copy up to date
                // The queries may have answered before Configure (from state.json alone), and the Worker's first
                // snapshot will equal the merged receipts: this is the only notice a receipts list gets.
                EmitReceiptsChanged();
            }
            else
            {
                WriteMirror(false);
            }
        }

        /// <summary>Web: merge the receipts into the copy (<paramref name="clear"/>: empty it). A refusal warns once a run.</summary>
        private static void WriteMirror(bool clear)
        {
            if (!S.Platform.IsWeb || S.MirrorOff || !S.MirrorMerged) return;
            var mirror = WebMirror();
            if (mirror == null) return;
            var receipts = S.PersistedReceipts!;
            string? refused;
            try
            {
                if (clear)
                {
                    refused = mirror.Write(new JsonObject());
                }
                else
                {
                    var dropped = new HashSet<string>(StringComparer.Ordinal);
                    if (S.OwnPrev != null)
                        foreach (var k in S.OwnPrev)
                            if (!receipts.ContainsKey(k)) dropped.Add(k);
                    refused = mirror.Update(receipts, dropped);
                }
            }
            catch (Exception e)
            {
                refused = "the receipts' copy in localStorage could not be written (" + e.GetType().Name + ": " + e.Message + ")";
            }
            if (refused == null)
            {
                S.OwnPrev = KeySet(receipts);
                return;
            }
            if (S.MirrorWarned) return;
            S.MirrorWarned = true;
            Warn(refused);
        }

        /// <summary>Web: ask the page to write persistentDataPath to IndexedDB (after a save or an export).</summary>
        private static void SyncWebFs()
        {
            if (!S.Platform.IsWeb) return;
            try
            {
                Js()?.SyncFs();
            }
            catch (Exception e)
            {
                ReportError("cannot write the saved state to IndexedDB:", e);
            }
        }

        /// <summary>Web, once when payments start: warn when persistentDataPath does not outlive the tab.</summary>
        private static void WarnUnpersistentFs()
        {
            if (!S.Platform.IsWeb) return;
            var js = Js();
            if (js == null || js.FsPersistent()) return;
            Warn("persistentDataPath is not persistent in this browser (IndexedDB is unavailable: private browsing, blocked"
                 + " site data or a restricted iframe). Players' receipts are then kept only in localStorage. See \"WebGL"
                 + " builds\" in the package's documentation.");
        }

        private static HashSet<string> KeySet(JsonObject d)
        {
            var keys = new HashSet<string>(StringComparer.Ordinal);
            foreach (var kv in d) keys.Add(kv.Key);
            return keys;
        }
    }
}
