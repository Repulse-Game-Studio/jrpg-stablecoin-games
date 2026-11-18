// Showcase for the Stablecoin for Games (SCFG) package: one screen that uses the whole public API (the twin of
// godot/showcase/main.gd).
//
// Left column: a small shop (three unlocks, one with pay-what-you-want), a tip jar and the player tools (restore,
// export, clear). Right column: what the package knows (owned items, tip total, pending payments, stored receipts) and a
// log of every result and event as it happens. The screen is built with UI Toolkit in code, on its own panel.
//
// Read it top to bottom: 1. configure, 2. build the shop, 3. pay, 4. listen, 5. show the state.
// Testnets only: no real money moves.
//
// Configure once per run, before payments start: play this scene first. Loaded again later in the same run, it carries
// on with the configuration it made the first time (the Console shows the refused second Configure).
using System.Collections.Generic;
using System.Linq;
using Scfg;
using UnityEngine;
using UnityEngine.UIElements;

namespace Scfg.Samples.Showcase
{
    [AddComponentMenu("SCFG/SCFG Showcase")]
    public sealed class ScfgShowcase : MonoBehaviour
    {
        // --- 1. Configuration ------------------------------------------------------------------------------

        // The wallet that receives the payments. This placeholder is the public example address from the EIP-55 spec:
        // it is not yours, so a real payment to it would be lost. Replace it with your own.
        private const string Wallet = "0x52908400098527886E0F7030069857D2E4169EE7";

        // The accent color of the payment sheet, as "#rrggbb".
        private const string Accent = "#7c5cff";

        // The paid items, passed to SCFG.AddItem. Code (1-99) identifies the item in the payment amount: never change
        // it after release. Pay may charge another amount than the price: anything above it (pay what you want), or down
        // to an optional MinPrice (a sale).
        private static readonly (string Id, ScfgItem Item)[] Items =
        {
            ("chapter2", new ScfgItem { Code = 1, Price = "1.00", Token = "USDC", Title = "Chapter 2: The Fall" }),
            ("soundtrack", new ScfgItem { Code = 2, Price = "0.50", Token = "USDC", Title = "Original soundtrack" }),
            ("supporter", new ScfgItem { Code = 3, Price = "2.00", Token = "USDC", Title = "Supporter pack" }),
        };

        // The supporter pack also offers a bigger, pay-what-you-want amount.
        private const string SupporterGenerousAmount = "5.00";

        // The tip jar's suggested amounts (the player can change the amount in the sheet).
        private static readonly string[] TipAmounts = { "0.50", "1.00", "3.00" };
        private const string TipToken = "USDC";

        private UIDocument _document;
        private VisualElement _itemsBox;
        private VisualElement _tipsBox;
        private Label _summary;
        private Label _receipts;
        private ScrollView _log;
        private readonly Dictionary<string, Label> _itemStates = new Dictionary<string, Label>();
        private readonly Dictionary<string, List<Button>> _itemButtons = new Dictionary<string, List<Button>>();
        private bool _busy;
        private float _timeScale = 1;
        private PanelSettings _panel;

        private void Start()
        {
            BuildScreen();
            var err = Configure();
            if (err == "configure.after_start" && Items.All(i => SCFG.ItemTitle(i.Id) == i.Item.Title))
            {
                Log("SCFG was configured by an earlier load of this scene; carrying on with that configuration.");
            }
            else if (err == "configure.after_start")
            {
                Log("Payments already started with a configuration made elsewhere (another scene, or an SCFG Settings asset"
                    + " with this scene not played first): play this scene first, or remove the SCFG Settings asset.");
                return;
            }
            else if (err != null)
            {
                Log("The package refused the configuration; see the Console.");
                return;
            }
            BuildShop();
            Listen();
            Refresh();
            Log("Ready. Build: " + BuildKind() + ". Pick something on the left.");
            FocusFirstButton();
        }

        // Configures SCFG and declares the items. Configure and AddItem return null on success, or the error code they
        // already logged (with Debug.LogError); so does this. With an SCFG Settings asset in the project, the asset is
        // applied first and this call replaces it, as long as payments have not started (this scene is played first).
        private static string Configure()
        {
            var err = SCFG.Configure(new ScfgOptions
            {
                Wallet = Wallet,
                Testnet = true, // Sepolia, Arbitrum Sepolia, Amoy, Base Sepolia, Arc Testnet
                Accent = Accent,
                // Optional, among others: SolanaWallet, Chains, Tokens, Methods, Logo, RpcUrls.
            });
            if (err != null) return err;
            foreach (var (id, item) in Items)
            {
                err = SCFG.AddItem(id, item);
                if (err != null) return err;
            }
            SCFG.Notify = true; // a "Payment confirmed" toast for payments that confirm in the background
            return null;
        }

        // --- 2. The shop -----------------------------------------------------------------------------------

        private void BuildShop()
        {
            foreach (var (id, item) in Items) AddItemRow(id, item);
            foreach (var amount in TipAmounts) _tipsBox.Add(MakeButton(amount + " " + TipToken, () => Tip(amount)));
        }

        // One row per item: its title, whether it is owned, and the button(s) that buy it.
        private void AddItemRow(string id, ScfgItem item)
        {
            var row = Row();
            var title = new Label(item.Title) { style = { flexGrow = 1 } };
            row.Add(title);
            var state = new Label();
            row.Add(state);
            _itemStates[id] = state;
            var buttons = new List<Button> { MakeButton("Unlock: " + item.Price + " " + item.Token, () => Unlock(id, null)) };
            if (id == "supporter")
                buttons.Add(MakeButton("Pay " + SupporterGenerousAmount + " " + item.Token, () => Unlock(id, SupporterGenerousAmount)));
            foreach (var b in buttons) row.Add(b);
            _itemButtons[id] = buttons;
            _itemsBox.Add(row);
        }

        // --- 3. Paying, restoring, exporting ----------------------------------------------------------------

        // Opens the payment sheet for an item. amount overrides the price (null keeps it).
        private async void Unlock(string id, string amount)
        {
            if (_busy) return;
            var request = new ScfgPayRequest { Item = id, Amount = amount };
            Log("Pay(Unlock, item " + id + (amount != null ? ", amount " + amount : "") + ")");
            _busy = true;
            PauseGame(true);
            ScfgPaymentResult result = await SCFG.Pay(ScfgPayKind.Unlock, request);
            PauseGame(false);
            _busy = false;
            if (this == null) return; // the game quit while the sheet was open
            LogPayment("unlock result", result);
            AfterFlow();
        }

        private async void Tip(string amount)
        {
            if (_busy) return;
            Log("Pay(Tip, " + amount + " " + TipToken + ")");
            _busy = true;
            PauseGame(true);
            ScfgPaymentResult result = await SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = amount, Token = TipToken });
            PauseGame(false);
            _busy = false;
            if (this == null) return;
            LogPayment("tip result", result);
            AfterFlow();
        }

        // Shows the restore screen, where the player pastes the hash of a payment made elsewhere.
        private async void Restore()
        {
            if (_busy) return;
            Log("Restore()");
            _busy = true;
            PauseGame(true);
            ScfgRestoreResult result = await SCFG.Restore();
            PauseGame(false);
            _busy = false;
            if (this == null) return;
            var detail = result.Status == ScfgRestoreStatus.Restored ? " " + result.Restored.Count + " receipt(s) added" : "";
            Log("restore result: " + result.Status + detail + ErrorSuffix(result.Error));
            AfterFlow();
        }

        private void Export()
        {
            var path = SCFG.ExportReceipts();
            Log(path != null ? "ExportReceipts() wrote " + path : "ExportReceipts() failed (see the Console)");
        }

        private void Clear()
        {
            SCFG.ClearLocalData();
            Log("ClearLocalData(): receipts forgotten on this device. Restore brings them back.");
            Refresh(); // ReceiptsChanged comes only when there were receipts; the pending payments go too
        }

        // The sheet takes the keyboard and gamepad, but not your game's own input code: pause the game while a flow
        // runs (the sheet runs on unscaled time, so it keeps working at Time.timeScale = 0), then put back the time
        // scale it had (the game may already have been paused or in slow motion).
        private void PauseGame(bool paused)
        {
            if (paused)
            {
                _timeScale = Time.timeScale;
                Time.timeScale = 0;
            }
            else
            {
                Time.timeScale = _timeScale;
            }
        }

        private void AfterFlow()
        {
            Refresh();
            FocusFirstButton();
        }

        // --- 4. Listening -----------------------------------------------------------------------------------

        private void Listen()
        {
            // A final result with no Pay callback to go to: a payment that confirms in the background or after a
            // restart, and the results of the Pay calls awaited above (so log, don't count).
            SCFG.PaymentResolved += OnPaymentResolved;
            // The stored receipts changed: a payment confirmed, a restore, ClearLocalData.
            SCFG.ReceiptsChanged += OnReceiptsChanged;
        }

        private void OnDestroy()
        {
            SCFG.PaymentResolved -= OnPaymentResolved;
            SCFG.ReceiptsChanged -= OnReceiptsChanged;
            if (_panel != null) Destroy(_panel);
        }

        private void OnPaymentResolved(ScfgPaymentResult result)
        {
            LogPayment("event PaymentResolved", result);
            Refresh();
        }

        private void OnReceiptsChanged()
        {
            Log("event ReceiptsChanged");
            Refresh();
        }

        // --- 5. Showing the state ---------------------------------------------------------------------------

        private void Refresh()
        {
            var owned = new List<string>();
            foreach (var (id, item) in Items)
            {
                bool has = SCFG.Owns(id);
                _itemStates[id].text = has ? "owned" : "";
                foreach (var b in _itemButtons[id]) b.style.display = has ? DisplayStyle.None : DisplayStyle.Flex;
                if (has) owned.Add(item.Title);
            }
            _summary.text = string.Join("\n", new[]
            {
                "Owned: " + (owned.Count > 0 ? string.Join(", ", owned) : "nothing yet"),
                "Tips: " + SCFG.TipTotal() + " " + TipToken + (SCFG.HasTipped() ? " (thank you!)" : ""),
                "Payments still being watched: " + SCFG.Pending().Count,
            });
            _receipts.text = ReceiptsText();
        }

        private static string ReceiptsText()
        {
            var receipts = SCFG.Receipts();
            if (receipts.Count == 0) return "None yet. Pay for something, or restore a payment.";
            return string.Join("\n", receipts.Values.Select(r =>
                (r.Kind == ScfgPayKind.Unlock ? SCFG.ItemTitle(r.Item) : "Tip") + ": " + r.Amount + " " + r.Token + " on "
                + SCFG.ChainName(r.Chain) + ", tx " + Short(r.TxHash ?? "")));
        }

        private void LogPayment(string source, ScfgPaymentResult result)
        {
            var paid = result.Amount != null ? " " + result.Amount + " " + result.Token + " on " + SCFG.ChainName(result.Chain) : "";
            Log(source + ": " + result.Status + paid + ErrorSuffix(result.Error));
        }

        private void Log(string text)
        {
            var line = new Label(System.DateTime.Now.ToString("HH:mm:ss") + "  " + text) { style = { whiteSpace = WhiteSpace.Normal } };
            _log.Add(line);
            _log.schedule.Execute(() => _log.ScrollTo(line));
        }

        private static string ErrorSuffix(string error) => error == null ? "" : " (error: " + error + ")";

        private static string Short(string hash) => hash.Length <= 14 ? hash : hash.Substring(0, 8) + "..." + hash.Substring(hash.Length - 6);

        private static string BuildKind()
        {
            if (SCFG.IsWeb) return "WebGL (the checkout opens inside this page)";
            if (SCFG.IsMobile) return "mobile (best effort, unverified)";
            return "desktop (the checkout opens in your browser)";
        }

        // Keyboard and gamepad players keep a focused button after every flow: the first one shown. Refresh hides a
        // bought item's buttons through style.display, which resolvedStyle only shows after the next layout pass.
        private void FocusFirstButton()
        {
            var first = _document.rootVisualElement.Query<Button>().Where(b => b.style.display.value != DisplayStyle.None).First();
            first?.Focus();
        }

        // --- the screen (plain UI Toolkit) -------------------------------------------------------------------

        private void BuildScreen()
        {
            _document = gameObject.AddComponent<UIDocument>();
            // A copy of the package's panel settings (it carries the text data UI Toolkit needs), below the sheet's panel.
            var asset = Resources.Load<PanelSettings>(ScfgDefaultSheetHost.PanelResource);
            _panel = asset != null ? Instantiate(asset) : ScriptableObject.CreateInstance<PanelSettings>();
            if (_panel.themeStyleSheet == null) _panel.themeStyleSheet = Resources.Load<ThemeStyleSheet>(ScfgDefaultSheetHost.ThemeResource);
            _panel.sortingOrder = 0;
            _document.panelSettings = _panel;
            var root = _document.rootVisualElement;
            root.style.flexDirection = FlexDirection.Row;
            root.style.backgroundColor = new Color(0.09f, 0.11f, 0.15f);
            root.style.paddingLeft = root.style.paddingRight = root.style.paddingTop = root.style.paddingBottom = 24;

            var left = Column(1);
            left.Add(Heading("Unlocks"));
            _itemsBox = new VisualElement();
            left.Add(_itemsBox);
            left.Add(Heading("Tip jar"));
            _tipsBox = Row();
            left.Add(_tipsBox);
            left.Add(Heading("Player tools"));
            var tools = Row();
            tools.Add(MakeButton("Restore purchases", Restore));
            tools.Add(MakeButton("Export receipts", Export));
            tools.Add(MakeButton("Clear local data", Clear));
            left.Add(tools);
            root.Add(left);

            var right = Column(1);
            right.Add(Heading("What SCFG knows"));
            _summary = Text();
            right.Add(_summary);
            right.Add(Heading("Receipts on this device"));
            _receipts = Text();
            right.Add(_receipts);
            right.Add(Heading("Event log"));
            _log = new ScrollView { style = { flexGrow = 1 } };
            right.Add(_log);
            root.Add(right);
        }

        private static VisualElement Column(float grow) => new VisualElement { style = { flexGrow = grow, flexBasis = 0, marginRight = 24 } };

        private static VisualElement Row() => new VisualElement
        {
            style = { flexDirection = FlexDirection.Row, alignItems = Align.Center, flexWrap = Wrap.Wrap, marginBottom = 6 },
        };

        private static Label Heading(string text) => new Label(text)
        {
            style = { unityFontStyleAndWeight = FontStyle.Bold, fontSize = 18, marginTop = 16, marginBottom = 8, color = Color.white },
        };

        private static Label Text() => new Label { style = { whiteSpace = WhiteSpace.Normal, color = new Color(0.85f, 0.87f, 0.9f) } };

        private static Button MakeButton(string text, System.Action onClick) => new Button(onClick) { text = text };
    }
}
