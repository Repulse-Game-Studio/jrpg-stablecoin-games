// The contract between the payment and restore flows (SCFG.Pay, SCFG.Restore) and the screens that show them:
// Godot's sheet_host.gd. SCFG.SheetHost holds one; replace it to show the flows your own way (uGUI, your own UXML).
//
// A host shows a screen for a controller and hands back an IScfgScreen, which raises Closed once when the player is
// done with it:
//   - payment sheet: "cancel" (the player gave up), "keep_playing" (close the sheet but keep the payment and its
//     browser page alive; the final result arrives later) or "finish" (close after a final state: paid, underpaid or
//     expired). Any other word counts as "finish".
//   - restore screen: any word (the restore screen only closes).
// The flows pick a Closed up on the next frame; never raise it from inside OpenPayment / OpenRestore. A screen whose
// Gone turns true without Closed (its GameObject was destroyed) counts as "cancel" and is not handed to Close. When the
// game quits with a screen open, Pay / Restore resolve with what they have and nothing is delivered.
//
// The facade polls the controller every frame, so a screen only reads its fields and calls its methods. Buttons that
// leave the game use the controller wrappers' OpenBrowser, OpenWalletApp, OpenCard, OpenExplorer, Copy and Paste. The flows
// close the controller (stopping its browser page) before they call Close(screen).
#nullable enable
using System;
using System.Collections.Generic;
using Scfg.Core;
using UnityEngine;

namespace Scfg
{
    /// <summary>A screen a host opened (see the file header).</summary>
    public interface IScfgScreen
    {
        /// <summary>Raised once with how the player closed the screen.</summary>
        event Action<string> Closed;

        /// <summary>True once the screen no longer exists (destroyed without raising Closed).</summary>
        bool Gone { get; }
    }

    /// <summary>Shows the payment sheet, the restore screen and short notices (see the file header).</summary>
    public interface IScfgSheetHost
    {
        /// <summary>Show the payment sheet; null when none can be shown (Pay then fails with "sheet.unavailable").</summary>
        IScfgScreen? OpenPayment(ScfgCheckoutController controller);

        /// <summary>Show the restore screen; null when none can be shown.</summary>
        IScfgScreen? OpenRestore(ScfgRestoreController controller);

        /// <summary>Remove a screen, after its controller was closed.</summary>
        void Close(IScfgScreen screen);

        /// <summary>Show a short message to the player (payment confirmations come only when SCFG.Notify is on).</summary>
        void Notify(string text);
    }

    /// <summary>
    /// What a payment sheet binds to: the core's <see cref="CheckoutController"/> (its <see cref="Core"/>; read its
    /// fields, call its methods from buttons) plus the actions that leave the game and a few display helpers.
    /// </summary>
    public sealed class ScfgCheckoutController
    {
        /// <summary>The core's checkout controller: read its fields, call its methods from the sheet's buttons.</summary>
        public CheckoutController Core { get; }
        internal string Cid { get; }
        internal Action<ScfgPaymentResult>? Callback { get; }
        internal string? CallbackSid { get; set; }

        internal ScfgCheckoutController(CheckoutController core, string cid, Action<ScfgPaymentResult>? callback)
        {
            Core = core;
            Cid = cid;
            Callback = callback;
        }

        /// <summary>"Open checkout in browser": starts the localhost page and opens the browser on it.</summary>
        public void OpenBrowser() => SCFG.OpenBrowser(this);

        /// <summary>"Open wallet app" (phones): the payment link (EIP-681 or Solana Pay).</summary>
        public void OpenWalletApp() => SCFG.OpenWalletApp(this);

        /// <summary>
        /// "Open card checkout" on the card step: opens configure's onramp page (thirdweb's card checkout by default)
        /// for this payment and moves the sheet on to waiting (the controller's OpenCard()).
        /// </summary>
        public void OpenCard() => SCFG.OpenCard(this);

        /// <summary>Opens a block explorer link (the controller's explorer URL for a transaction); null does nothing.</summary>
        public void OpenExplorer(string? url) => SCFG.OpenExplorer(url);

        /// <summary>Puts <paramref name="text"/> on the clipboard (an address, an amount, a hash).</summary>
        public void Copy(string text) => SCFG.Copy(text);

        /// <summary>The clipboard's first word, hash characters only, into the hash field.</summary>
        public void Paste() => SCFG.PasteInto(this);

        /// <summary>True on a phone or tablet (SCFG.IsMobile).</summary>
        public bool IsMobile => SCFG.IsMobile;
        /// <summary>True in a WebGL player (SCFG.IsWeb).</summary>
        public bool IsWeb => SCFG.IsWeb;
        /// <summary>The accent color, "#rrggbb" (SCFG.Accent).</summary>
        public string Accent => SCFG.Accent;
        /// <summary>The configured logo, or null.</summary>
        public Texture2D? Logo => SCFG.LogoTexture;
    }

    /// <summary>What a restore screen binds to: the core's <see cref="RestoreController"/> plus Paste and the helpers.</summary>
    public sealed class ScfgRestoreController
    {
        /// <summary>The core's restore controller: read its fields, call its methods from the screen's buttons.</summary>
        public RestoreController Core { get; }
        internal string Rid { get; }
        internal Action<ScfgRestoreResult>? Callback { get; }
        internal bool Delivered { get; set; }

        internal ScfgRestoreController(RestoreController core, string rid, Action<ScfgRestoreResult>? callback)
        {
            Core = core;
            Rid = rid;
            Callback = callback;
        }

        /// <summary>The clipboard's first word, hash characters only, into the hash field.</summary>
        public void Paste() => SCFG.PasteInto(this);
        /// <summary>Puts <paramref name="text"/> on the clipboard (a receipt's hash).</summary>
        public void Copy(string text) => SCFG.Copy(text);

        /// <summary>An item's title (the declared one, else the id).</summary>
        public string ItemTitle(string? itemId) => SCFG.ItemTitle(itemId);

        /// <summary>A chain's display name, else its key.</summary>
        public string ChainName(string? key) => SCFG.ChainName(key);

        /// <summary>The receipts on this device (for the "Purchases on this device" list).</summary>
        public IReadOnlyList<ScfgReceipt> DeviceReceipts() => new List<ScfgReceipt>(SCFG.Receipts().Values);

        /// <summary>Changes each time SCFG.ReceiptsChanged is raised: compare it instead of copying the receipts.</summary>
        public int ReceiptsRevision => SCFG.ReceiptsRevision;

        /// <summary>True in a WebGL player (SCFG.IsWeb).</summary>
        public bool IsWeb => SCFG.IsWeb;
        /// <summary>The accent color, "#rrggbb" (SCFG.Accent).</summary>
        public string Accent => SCFG.Accent;
        /// <summary>The configured logo, or null.</summary>
        public Texture2D? Logo => SCFG.LogoTexture;
    }

    /// <summary>
    /// Where the game runs and how it reaches the world outside: the platform checks, opening a URL and the clipboard.
    /// The facade uses its own; tests replace it with a recording one (that setting is not part of the API).
    /// </summary>
    public interface IScfgPlatform
    {
        /// <summary>True in a WebGL player.</summary>
        bool IsWeb { get; }
        /// <summary>True on a phone or tablet.</summary>
        bool IsMobile { get; }
        /// <summary>Opens <paramref name="url"/> in the browser or the app that handles it.</summary>
        void OpenUrl(string url);
        /// <summary>Puts <paramref name="text"/> on the system clipboard.</summary>
        void ClipboardSet(string text);
        /// <summary>The system clipboard's text ("" when empty).</summary>
        string ClipboardGet();
    }

    internal sealed class UnityPlatform : IScfgPlatform
    {
        public bool IsWeb => Application.platform == RuntimePlatform.WebGLPlayer;
        public bool IsMobile => Application.isMobilePlatform;
        public void OpenUrl(string url) => Application.OpenURL(url);
        public void ClipboardSet(string text) => GUIUtility.systemCopyBuffer = text;
        public string ClipboardGet() => GUIUtility.systemCopyBuffer ?? "";
    }
}
