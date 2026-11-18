// Demo game for the Stablecoin for Games (SCFG) package: a title, a locked Chapter 2, a Tip button and a Restore button
// (the twin of godot/demo/main.gd). It uses only the public API: Configure, AddItem, Pay, Owns, Restore and the
// PaymentResolved event. Testnet only: no real money moves.
//
// While a payment or restore screen is open the game is paused (Time.timeScale = 0, then back to what it was): the sheet
// takes the keyboard and gamepad, but your own game code that reads input keeps running unless you stop it.
//
// Configure once per run, before payments start: play this scene first. Loaded again later in the same run, it carries
// on with the configuration it made the first time (the Console shows the refused second Configure).
#nullable enable
using System.Linq;
using Scfg;
using UnityEngine;
using UnityEngine.UIElements;

namespace Scfg.Samples.Demo
{
    [AddComponentMenu("SCFG/SCFG Demo")]
    public sealed class ScfgDemo : MonoBehaviour
    {
        // The wallet that receives the payments: yours. Set it on this component (Wallet, and Solana Wallet to take
        // Solana too), or in the project's SCFG Settings asset (Assets > Create > SCFG Settings), which this scene uses
        // when both fields here are blank. With neither, the scene does not configure SCFG and says so: it never pays a
        // placeholder address.
        [Tooltip("Your EVM payout address (0x...). Blank: the project's SCFG Settings asset is used.")]
        [SerializeField] private string wallet = "";
        [Tooltip("Your Solana payout address (base58). Blank: the project's SCFG Settings asset is used.")]
        [SerializeField] private string solanaWallet = "";

        private const string Item = "chapter2";
        private const string ItemTitle = "Chapter 2: The Fall";

        private Label _chapter = null!;
        private Label _status = null!;
        private Button _unlock = null!;
        private Button _tip = null!;
        private Button _restore = null!;
        private bool _busy;
        private PanelSettings? _panel;

        private void Start()
        {
            BuildScreen();
            // Looked up once: with several SCFG Settings assets in the project, each lookup reports it in the Console.
            var settings = ScfgSettings.Find();
            var (evm, solana) = Wallets(settings);
            if (evm == null && solana == null)
            {
                _status.text = "No wallet is set, so SCFG is not configured and nothing can be paid. Put your wallet"
                               + " address in this component's Wallet field, or in an SCFG Settings asset (Assets >"
                               + " Create > SCFG Settings), then play the scene again.";
                return;
            }
            var err = Configure(settings, evm, solana);
            // configure.after_start with this demo's item declared: this scene was loaded before in this run, and
            // payments started with the configuration it made then, so it carries on
            bool loadedBefore = err == "configure.after_start" && SCFG.ItemTitle(Item) == ItemTitle;
            if (err == "configure.after_start" && !loadedBefore)
            {
                _status.text = "Payments already started with a configuration made elsewhere: play this scene first, or"
                               + " remove the SCFG Settings asset.";
                return;
            }
            if (err != null && !loadedBefore)
            {
                _status.text = "SCFG refused the configuration: see the Console.";
                return;
            }
            SCFG.PaymentResolved += OnPaymentResolved;
            SCFG.ReceiptsChanged += Refresh;
            Refresh();
            FocusFirst();
        }

        private void OnDestroy()
        {
            SCFG.PaymentResolved -= OnPaymentResolved;
            SCFG.ReceiptsChanged -= Refresh;
            if (_panel != null) Destroy(_panel);
        }

        // Configures SCFG. Null, or the error code Configure or AddItem refused it with (and logged).
        private static string? Configure(ScfgSettings? settings, string? evm, string? solana)
        {
            var err = SCFG.Configure(TestnetOptions(settings, evm, solana));
            if (err == null)
                err = SCFG.AddItem(Item, new ScfgItem { Code = 1, Price = "1.00", Token = "USDC", Title = ItemTitle });
            return err;
        }

        // The options: with an SCFG Settings asset in the project, its options are the starting point (accent, logo,
        // buy link ...), and the demo adds only its wallets and testnet (its own item comes after). The demo always
        // runs on the test networks, so it keeps the asset's networks and tokens only when the asset is set to testnet
        // (a release asset's choices, USDT say, may exist on no test network), and of those networks only the ones it
        // has a wallet for ("solana" is the Solana network; every other key is an EVM network).
        private static ScfgOptions TestnetOptions(ScfgSettings? settings, string? evm, string? solana)
        {
            var options = settings != null ? settings.ToOptions() : new ScfgOptions();
            options.Wallet = evm;
            options.SolanaWallet = solana;
            options.Testnet = true; // Sepolia, Arbitrum Sepolia, Amoy, Base Sepolia, Arc Testnet, Solana Devnet: no real money
            if (settings != null && !settings.testnet)
            {
                options.Chains = null;
                options.Tokens = null;
                if (options.Onramp != null) options.Onramp.Chains = null;
                return options;
            }
            if (options.Chains != null)
            {
                var chains = options.Chains.Where(key => key == "solana" ? solana != null : evm != null).ToList();
                options.Chains = chains.Count > 0 ? chains : null;
            }
            if (options.Onramp?.Chains != null)
            {
                var chains = options.Chains;
                var buy = options.Onramp.Chains.Where(key => evm != null && (chains == null || chains.Contains(key))).ToList();
                options.Onramp.Chains = buy.Count > 0 ? buy : null;
            }
            return options;
        }

        // The payout wallets: this component's fields when one is set, else the SCFG Settings asset's, else none.
        private (string? Evm, string? Solana) Wallets(ScfgSettings? settings)
        {
            if (!string.IsNullOrWhiteSpace(wallet) || !string.IsNullOrWhiteSpace(solanaWallet))
                return (Trim(wallet), Trim(solanaWallet));
            if (settings != null && settings.HasWallet) return (Trim(settings.wallet), Trim(settings.solanaWallet));
            return (null, null);
        }

        private static string? Trim(string? s) => string.IsNullOrWhiteSpace(s) ? null : s!.Trim();

        private void Refresh()
        {
            bool owned = SCFG.Owns(Item);
            _chapter.text = owned ? "Chapter 2: unlocked. Thank you for supporting the game!" : "Chapter 2: locked.";
            _unlock.style.display = owned ? DisplayStyle.None : DisplayStyle.Flex;
        }

        // Awaits a payment or restore flow with the game paused and the buttons busy, then puts the time scale back (the
        // game may already have been paused or in slow motion).
        private async Awaitable<T> WhilePaused<T>(Awaitable<T> flow)
        {
            _busy = true;
            float timeScale = Time.timeScale;
            Time.timeScale = 0;
            try
            {
                return await flow;
            }
            finally
            {
                Time.timeScale = timeScale;
                _busy = false;
            }
        }

        private async void OnUnlock()
        {
            if (_busy) return;
            ScfgPaymentResult r = await WhilePaused(SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = Item }));
            if (this == null) return; // the game quit with the sheet open
            _status.text = r.Status switch
            {
                ScfgPaymentStatus.Paid or ScfgPaymentStatus.AlreadyOwned => "Enjoy Chapter 2!",
                ScfgPaymentStatus.Pending => "Your payment is being confirmed; Chapter 2 unlocks when it is.",
                ScfgPaymentStatus.Cancelled => "",
                _ => "Payment: " + r.Status + (r.Error != null ? " (" + r.Error + ")" : ""),
            };
            Refresh();
            FocusFirst();
        }

        private async void OnTip()
        {
            if (_busy) return;
            ScfgPaymentResult r = await WhilePaused(SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "1.00", Token = "USDC" }));
            if (this == null) return;
            if (r.Status == ScfgPaymentStatus.Paid) _status.text = "Thank you for the " + r.Amount + " USDC tip!";
            FocusFirst();
        }

        private async void OnRestore()
        {
            if (_busy) return;
            ScfgRestoreResult r = await WhilePaused(SCFG.Restore());
            if (this == null) return;
            if (r.Status == ScfgRestoreStatus.Restored) _status.text = "Purchases restored.";
            Refresh();
            FocusFirst();
        }

        // A payment that confirmed after its sheet closed (the player chose to keep playing) or after a restart.
        private void OnPaymentResolved(ScfgPaymentResult r)
        {
            if (r.Status == ScfgPaymentStatus.Paid && r.Kind == ScfgPayKind.Unlock) _status.text = "Chapter 2 is unlocked. Thank you!";
            Refresh();
        }

        // Unlock while it is shown (Refresh hides it once Chapter 2 is owned), else Tip.
        private void FocusFirst() => (SCFG.Owns(Item) ? _tip : _unlock).Focus();

        // --- the screen (plain UI Toolkit) -------------------------------------------------------------------

        private void BuildScreen()
        {
            var document = gameObject.AddComponent<UIDocument>();
            // A copy of the package's panel settings (it carries the text data UI Toolkit needs), below the sheet's panel.
            var asset = Resources.Load<PanelSettings>(ScfgDefaultSheetHost.PanelResource);
            _panel = asset != null ? Instantiate(asset) : ScriptableObject.CreateInstance<PanelSettings>();
            if (_panel.themeStyleSheet == null) _panel.themeStyleSheet = Resources.Load<ThemeStyleSheet>(ScfgDefaultSheetHost.ThemeResource);
            _panel.sortingOrder = 0;
            document.panelSettings = _panel;
            var root = document.rootVisualElement;
            root.style.backgroundColor = new Color(0.12f, 0.1f, 0.18f);
            root.style.color = Color.white; // light text on the dark background, whatever the theme's default
            root.style.alignItems = Align.Center;
            root.style.justifyContent = Justify.Center;
            var title = new Label("Ruby Dreams") { style = { fontSize = 40, color = Color.white, marginBottom = 24 } };
            root.Add(title);
            _chapter = new Label { style = { fontSize = 20, color = Color.white, marginBottom = 16 } };
            root.Add(_chapter);
            _unlock = new Button(OnUnlock) { text = "Unlock Chapter 2 (1 USDC)" };
            _tip = new Button(OnTip) { text = "Tip the developer (1 USDC)" };
            _restore = new Button(OnRestore) { text = "Restore purchases" };
            foreach (var b in new[] { _unlock, _tip, _restore })
            {
                b.style.minWidth = 280;
                b.style.marginBottom = 8;
                b.style.color = new Color(0.1f, 0.1f, 0.12f); // dark text on the light button, not the root's white
                root.Add(b);
            }
            _status = new Label { style = { color = new Color(0.8f, 0.8f, 0.9f), marginTop = 16 } };
            root.Add(_status);
        }
    }
}
