// The payment sheet, the UI Toolkit twin of Godot's ui/sheet.gd (and of the scfg_payment screen in
// plugin/game/scfg/scfg_screens.rpy), shown by ScfgDefaultSheetHost for SCFG.Pay.
//
// One step at a time, as the controller's Step says: amount (tips) -> network -> method -> browser | qr | manual ->
// waiting -> done, or network -> card -> waiting -> done through a network's "Pay with card" link. Every step has the
// header (the back arrow while the controller CanGoBack, the step dots, the logo, the title and the price), a
// scrolling body, the pinned forward button when the step has one, and the footer (the status message, the status line
// and Cancel / Continue playing / Continue).
// Development builds also show the developer lines (DEV MODE and the controller's DevWarnings()).
//
// Keyboard and gamepad: each step focuses its primary control (the selected option, the forward button, the first
// Copy, Continue); cancel (Escape, gamepad B) goes back, or closes as "cancel" on the first step.
#nullable enable
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Scfg.Core;
using UnityEngine.UIElements;

namespace Scfg
{
    /// <summary>The package's payment sheet (see the file header).</summary>
    public sealed class ScfgSheetView : ScfgScreenView
    {
        private static readonly string[] EndedStates = { "closed", "paid", "underpaid", "expired" };
        private const int AmountMaxLength = 8;
        private const string AmountChars = "0123456789.";

        /// <summary>The checkout this sheet shows.</summary>
        public ScfgCheckoutController Controller { get; }
        private CheckoutController Ctl => Controller.Core;

        /// <summary>The step the body shows now.</summary>
        public string ShownStep { get; private set; } = "";

        private readonly Dictionary<string, string> _synced = new Dictionary<string, string>(StringComparer.Ordinal);

        /// <summary>A sheet for <paramref name="controller"/>; add its <see cref="ScfgScreenView.Root"/> to a panel.</summary>
        public ScfgSheetView(ScfgCheckoutController controller)
        {
            Controller = controller ?? throw new ArgumentNullException(nameof(controller));
            Root.AddToClassList("scfg-screen--payment");
        }

        /// <summary>The step dots shown (index, count), or null when there are none.</summary>
        public (int Index, int Count)? Dots()
        {
            if (Ctl.State == "closed" || Ctl.Step == "done") return null;
            var d = Ctl.StepDots();
            return d.Count > 1 ? d : ((int, int)?)null;
        }

        protected override string Signature()
        {
            var c = Ctl;
            var sb = new StringBuilder();
            void Add(object? v) => AddField(sb, v);
            Add(c.Step); Add(c.State); Add(c.Error); Add(c.CanGoBack); Add(c.HintTx); Add(c.Confirmations); Add(c.Required);
            Add(c.Method); Add(c.ChainKey); Add(c.PriceText); Add(c.SessionId); Add(c.BrowserFailed);
            Add(string.Join(",", c.ChainOptions().Select(o => o.Key)));
            Add(string.Join(",", c.Methods()));
            Add(c.Result?.TxHash); Add(c.Message()); Add(c.MoreNetworks);
            return sb.ToString();
        }

        protected override string Primary()
        {
            var c = Ctl;
            if (EndedStates.Contains(c.State)) return "continue";
            switch (c.Step)
            {
                case "amount":
                    foreach (var preset in c.TipPresets())
                        if (Dec.Cmp(c.Amount, preset) == 0) return "preset_" + preset;
                    return "custom_amount";
                case "network":
                {
                    var main = c.NetworkLayout().Main;
                    if (c.ChainKey != null && Control("network_" + c.ChainKey) != null)
                        return (c.Method == "card" ? "card_" : "network_") + c.ChainKey;
                    return main.Count > 0 ? "network_" + main[0] : "cancel";
                }
                case "method":
                {
                    var methods = c.Methods();
                    if (c.Method != null && methods.Contains(c.Method)) return "method_" + c.Method;
                    return methods.Count > 0 ? "method_" + methods[0] : "cancel";
                }
                case "browser":
                case "qr":
                case "card":
                    return "forward";
                case "manual":
                    return "copy_amount";
                case "waiting":
                    foreach (var key in new[] { "explorer", "reopen", "keep_playing" })
                        if (Control(key) != null) return key;
                    break;
            }
            return "cancel";
        }

        protected override void SyncFields()
        {
            Sync("tx_input", Ctl.TxInput);
            Sync("custom_amount", Ctl.CustomAmount);
        }

        private void Sync(string key, string value)
        {
            if (Control(key) is TextField f && (!_synced.TryGetValue(key, out var last) || last != value))
            {
                f.SetValueWithoutNotify(value);
                _synced[key] = value;
            }
        }

        protected override void OnCancel()
        {
            var c = Ctl;
            if (c.CanGoBack) c.Back();
            else if (!c.TxSeen && c.Step != "waiting" && c.Step != "done" && c.StepDots().Index == 0) CloseWith("cancel");
        }

        // --- the build ------------------------------------------------------------------------------

        protected override void Build()
        {
            ShownStep = Ctl.Step;
            _synced.Clear();
            BuildHeader();
            var step = Box("scfg-step");
            Layout.Add(step);
            var body = ScrollingBody(step);
            var forward = BuildStep(body);
            if (forward == null)
            {
                var none = new Button { text = "" };
                none.style.display = DisplayStyle.None;
                none.focusable = false;
                step.Add(Reg(none, "forward"));
            }
            else
            {
                step.Add(Button("forward", forward.Value.Text, "scfg-button--accent", forward.Value.Action));
            }
            BuildFooter();
        }

        private void BuildHeader()
        {
            var c = Ctl;
            var header = Box("scfg-header");
            Layout.Add(header);
            var nav = Row("scfg-nav");
            header.Add(nav);
            var back = Button("back", "←", "scfg-button--back", () => Ctl.Back());
            back.tooltip = ScfgUiTexts.Get("back");
            if (!c.CanGoBack)
            {
                back.style.visibility = Visibility.Hidden;
                Unfocus(back);
            }
            nav.Add(back);
            nav.Add(Box("scfg-grow"));
            var d = Dots();
            if (d != null)
            {
                var row = Row("scfg-dots");
                row.name = "dots";
                for (int i = 0; i < d.Value.Count; i++) row.Add(Dot(i <= d.Value.Index));
                nav.Add(Reg(row, "dots"));
            }
            var mark = Logo();
            if (mark != null) header.Add(mark);
            if (c.State == "closed") return;
            if (SCFG.IsDevBuild)
            {
                var dev = ScfgUiTexts.Format(c.Config.Testnet ? "dev_mode_paying_to_testnet" : "dev_mode_paying_to", Short(c.Recipient ?? ""));
                header.Add(Reg(Text(dev, "scfg-dev"), "dev"));
                foreach (var w in c.DevWarnings()) header.Add(Text(w, "scfg-dev"));
            }
            header.Add(Reg(Text(c.Spec.Title, "scfg-title"), "title"));
            var price = Row("scfg-price");
            price.Add(Reg(Text(c.PriceText, "scfg-amount"), "price"));
            price.Add(Text(c.Spec.Token, "scfg-amount-token"));
            header.Add(price);
            var note = ExactNote();
            if (note.Length > 0) header.Add(Reg(Text(note, "scfg-note"), "exact_note"));
        }

        private (string Text, Action Action)? BuildStep(VisualElement body)
        {
            var c = Ctl;
            if (c.State == "closed")
            {
                body.Add(Text(ScfgUiTexts.Get("payment_screen_inactive")));
                return null;
            }
            switch (c.Step)
            {
                case "amount":
                    StepAmount(body);
                    return (ScfgUiTexts.Get("next"), () => Ctl.NextStep());
                case "network":
                    StepNetwork(body);
                    return null;
                case "method":
                    StepMethod(body);
                    return null;
                case "browser":
                    StepBrowser(body);
                    return (ScfgUiTexts.Get(Web ? "open_wallet" : "open_checkout_browser"), () => Controller.OpenBrowser());
                case "qr":
                    StepQr(body);
                    return (ScfgUiTexts.Get("i_sent_it"), () => Ctl.MarkSent());
                case "manual":
                    StepManual(body);
                    return (ScfgUiTexts.Get("i_sent_it"), () => Ctl.MarkSent());
                case "card":
                    StepCard(body);
                    return (ScfgUiTexts.Get("open_card_checkout"), () => Controller.OpenCard());
                case "waiting":
                    StepWaiting(body);
                    return null;
                default:
                    StepDone(body);
                    return null;
            }
        }

        private void StepAmount(VisualElement body)
        {
            var c = Ctl;
            body.Add(Text(ScfgUiTexts.Get("choose_amount"), "scfg-label"));
            var presets = c.TipPresets();
            var grid = Row("scfg-grid");
            foreach (var preset in presets)
            {
                var value = preset.ToString();
                grid.Add(Option("preset_" + value, PlainAmount(value) + " " + c.Spec.Token, "", Dec.Cmp(c.Amount, preset) == 0,
                    () => Ctl.SetTipAmount(value), "presets"));
            }
            body.Add(grid);
            body.Add(Text(ScfgUiTexts.Get("or_enter_your_own"), "scfg-label"));
            var row = Row();
            var field = Field("custom_amount", c.CustomAmount, AmountMaxLength, AmountChars, t => { Ctl.CustomAmount = t; _synced["custom_amount"] = t; },
                () => Ctl.ApplyCustomAmount(), "amount");
            field.AddToClassList("scfg-grow");
            row.Add(field);
            row.Add(Button("set_amount", ScfgUiTexts.Get("set"), "scfg-button--small", () => Ctl.ApplyCustomAmount(), "amount"));
            body.Add(row);
            _synced["custom_amount"] = c.CustomAmount;
        }

        /// <summary>
        /// The networks (Ren'Py's scfg_step_network): the main ones (Ethereum, Solana) as tiles, the rest behind "More
        /// networks" (the controller's NetworkLayout()). A network with a card button has a small "Pay with card" link
        /// under its tile, which skips the method step for the card page. Every tile has the same width: half the row,
        /// or all of it when there is a single network.
        /// </summary>
        private void StepNetwork(VisualElement body)
        {
            var c = Ctl;
            body.Add(Text(ScfgUiTexts.Format("which_network", c.Spec.Token), "scfg-label"));
            var options = c.ChainOptions().ToDictionary(o => o.Key, o => o, StringComparer.Ordinal);
            var layout = c.NetworkLayout();
            bool full = options.Count == 1;
            body.Add(NetworkTiles(layout.Main, options, layout.Card, full, "networks"));
            if (layout.More.Count == 0) return;
            var more = Button("more_networks", ScfgUiTexts.Get(layout.Expanded ? "fewer_networks" : "more_networks"),
                              "scfg-button--small", () => Ctl.ToggleMoreNetworks());
            more.AddToClassList("scfg-more-networks");
            body.Add(more);
            if (layout.Expanded) body.Add(NetworkTiles(layout.More, options, layout.Card, full, "more"));
        }

        /// <summary>
        /// The network tiles, two to a row, and under each row the "Pay with card" links of the tiles that
        /// <paramref name="card"/> lists, each below its tile. The tiles of a row share their height; the focus goes
        /// through a row's tiles, then that row's links.
        /// </summary>
        private VisualElement NetworkTiles(IReadOnlyList<string> keys, Dictionary<string, (string Key, string Name, string FeeHint)> options,
                                           IReadOnlyList<string> card, bool full, string rows)
        {
            var c = Ctl;
            var grid = Box("scfg-network-grid");
            for (int start = 0; start < keys.Count; start += 2)
            {
                var row = keys.Skip(start).Take(2).ToList();
                var tiles = Row("scfg-network-row");
                var links = Row("scfg-network-row");
                foreach (var key in row)
                {
                    var o = options[key];
                    var tile = Option("network_" + key, o.Name, FeeLabel(o.FeeHint), c.ChainKey == key && c.Method != "card",
                                      () => Ctl.ChooseNetwork(key), rows + ":" + Int(start / 2));
                    tile.AddToClassList("scfg-grow");
                    var cell = Box("scfg-network-cell");
                    if (full) cell.AddToClassList("scfg-network-cell--full");
                    cell.Add(tile);
                    tiles.Add(cell);
                }
                grid.Add(tiles);
                if (!row.Any(card.Contains)) continue;
                foreach (var key in row)
                {
                    var cell = Box("scfg-network-cell");
                    if (full) cell.AddToClassList("scfg-network-cell--full");
                    if (card.Contains(key))
                    {
                        var link = Button("card_" + key, ScfgUiTexts.Get("pay_with_card"), "scfg-card-link", () => Ctl.PayWithCard(key),
                                          rows + "_card:" + Int(start / 2));
                        if (c.ChainKey == key && c.Method == "card") link.AddToClassList("scfg-card-link--selected");
                        cell.Add(link);
                    }
                    links.Add(cell);
                }
                grid.Add(links);
            }
            return grid;
        }

        private void StepMethod(VisualElement body)
        {
            var c = Ctl;
            body.Add(Text(ScfgUiTexts.Get("how_to_pay"), "scfg-label"));
            var list = Box("scfg-list");
            foreach (var m in c.Methods())
            {
                var method = m;
                list.Add(Option("method_" + m, MethodLabel(m), MethodHint(m), c.Method == m, () => Ctl.ChooseMethod(method)));
            }
            body.Add(list);
        }

        private void StepBrowser(VisualElement body)
        {
            bool solana = Ctl.Family == "solana";
            body.Add(Text(ScfgUiTexts.Get("browser_wallet"), "scfg-label"));
            body.Add(Text(ScfgUiTexts.Get(Web ? (solana ? "solana_opens_here_note" : "evm_opens_here_note")
                                              : (solana ? "solana_opens_browser_note" : "evm_opens_browser_note"))));
            body.Add(Text(ScfgUiTexts.Format("approve_in_wallet", AmountLine(false), ChainDisplay()), "scfg-muted"));
        }

        /// <summary>
        /// The card page (Ren'Py's scfg_step_card): what "Open card checkout" does, before it opens configure's onramp
        /// page (thirdweb's card checkout by default) in the player's browser.
        /// </summary>
        private void StepCard(VisualElement body)
        {
            body.Add(Text(ScfgUiTexts.Get("pay_with_card"), "scfg-label"));
            body.Add(Text(ScfgUiTexts.Get("card_opens_browser_note")));
            body.Add(Text(ScfgUiTexts.Format("card_pays_on", AmountLine(true), ChainDisplay()), "scfg-muted"));
        }

        private void StepQr(VisualElement body)
        {
            var c = Ctl;
            body.Add(Text(ScfgUiTexts.Get("phone_wallet"), "scfg-label"));
            var row = Row("scfg-qr-row");
            string? uri = null;
            try
            {
                uri = c.PaymentUri();
            }
            catch (Exception e)
            {
                SCFG.ReportError("cannot build the payment link:", e);
            }
            if (uri != null)
            {
                var frame = Box("scfg-qr-frame");
                var qr = new ScfgQrElement();
                qr.SetText(uri);
                frame.Add(Reg(qr, "qr"));
                row.Add(frame);
            }
            var steps = Box("scfg-qr-steps");
            steps.Add(Text(ScfgUiTexts.Get("qr_step_open"), "scfg-qr-step"));
            steps.Add(Text(ScfgUiTexts.Get("qr_step_scan"), "scfg-qr-step"));
            steps.Add(Text(ScfgUiTexts.Format("qr_step_approve", AmountLine(false)), "scfg-qr-step"));
            row.Add(steps);
            body.Add(row);
            if (Controller.IsMobile && Py.Truthy(Py.Get(Py.Get(c.Config.Methods, "qr"), "wallet_button")))
                body.Add(Button("wallet_app", ScfgUiTexts.Get("open_wallet_app"), "", () => Controller.OpenWalletApp()));
            body.Add(Text(ScfgUiTexts.Get(c.Family == "solana" ? "wallet_changes_amount" : "wallet_changes_amount_manual"), "scfg-muted"));
        }

        private void StepManual(VisualElement body)
        {
            var c = Ctl;
            bool solana = c.Family == "solana";
            body.Add(Text(solana ? ScfgUiTexts.Get("send_exact_amount_solana") : ScfgUiTexts.Format("send_exact_amount", ChainDisplay()),
                "scfg-warning"));
            var rows = new[]
            {
                ("amount", ScfgUiTexts.Get("amount"), AmountLine(true)),
                ("recipient", ScfgUiTexts.Get("to_address"), c.Recipient ?? ""),
                ("token", ScfgUiTexts.Get(solana ? "token_mint" : "token_contract"), c.TokenAddress ?? ""),
            };
            foreach (var (key, title, value) in rows)
            {
                var group = Box("scfg-group");
                group.Add(Text(title, "scfg-label"));
                var line = Row();
                var mono = Mono(value);
                mono.AddToClassList("scfg-grow");
                line.Add(Reg(mono, "value_" + key));
                var copyText = value.Split(' ')[0];
                line.Add(Button("copy_" + key, ScfgUiTexts.Get("copy"), "scfg-button--small", () => Controller.Copy(copyText)));
                group.Add(line);
                body.Add(group);
            }
            if (Py.Truthy(Py.Get(Py.Get(c.Config.Methods, "manual"), "allow_tx_paste")))
            {
                body.Add(Text(ScfgUiTexts.Format("sent_from_exchange_paste", TxWord()), "scfg-label"));
                var row = Row();
                var field = Field("tx_input", c.TxInput, SCFG.TxMaxLength, SCFG.TxChars, t => { Ctl.TxInput = t; _synced["tx_input"] = t; },
                    () => Ctl.SubmitHash(), "tx");
                field.AddToClassList("scfg-grow");
                row.Add(field);
                if (Web) row.Add(Button("paste", ScfgUiTexts.Get("paste"), "scfg-button--small", () => Controller.Paste(), "tx"));
                row.Add(Button("check", ScfgUiTexts.Get("check"), "scfg-button--small", () => Ctl.SubmitHash(), "tx"));
                body.Add(row);
                _synced["tx_input"] = c.TxInput;
            }
        }

        private void StepWaiting(VisualElement body)
        {
            var c = Ctl;
            body.Add(Reg(Text(ProgressText(), "scfg-heading"), "progress_text"));
            if (c.State == "confirming")
            {
                var bar = Box("scfg-progress");
                var fill = Box("scfg-progress__fill");
                fill.AddToClassList("scfg-tone--accent");
                fill.style.width = Length.Percent(100f * Math.Min(c.Confirmations, Math.Max(1, c.Required)) / Math.Max(1, c.Required));
                bar.Add(fill);
                body.Add(Reg(bar, "progress"));
            }
            if (Py.Truthy(c.HintTx))
            {
                body.Add(Text(ScfgUiTexts.Format("your_hash", TxWord()), "scfg-label"));
                body.Add(Reg(Mono(c.HintTx!), "hint_tx"));
                var url = ExplorerUrl();
                if (url != null)
                    body.Add(Button("explorer", ScfgUiTexts.Get("view_on_explorer"), "scfg-button--small", () => Controller.OpenExplorer(url)));
            }
            else if (c.Method == "browser")
            {
                body.Add(Text(ScfgUiTexts.Get(Web ? "finish_in_wallet_window" : "finish_in_browser"), "scfg-muted"));
                body.Add(Button("reopen", ScfgUiTexts.Get(Web ? "reopen_wallet_window" : "reopen_checkout_page"), "scfg-button--small",
                    () => Controller.OpenBrowser()));
            }
            body.Add(Text(ScfgUiTexts.Get("keep_playing_note"), "scfg-muted"));
        }

        private void StepDone(VisualElement body)
        {
            var c = Ctl;
            var title = Text(DoneTitle(), "scfg-heading");
            title.AddToClassList("scfg-tone-text--" + StatusTone());
            body.Add(Reg(title, "done_title"));
            var tx = c.Result?.TxHash;
            if (c.State == "paid" && Py.Truthy(tx))
            {
                body.Add(Text(ScfgUiTexts.Format("keep_this_to_restore", TxWord()), "scfg-muted"));
                body.Add(Reg(Mono(tx!), "result_tx"));
                var row = Row();
                row.Add(Button("copy_tx", ScfgUiTexts.Get(c.Family == "solana" ? "copy_signature" : "copy_hash"), "scfg-button--small",
                    () => Controller.Copy(tx!), "done"));
                var url = ExplorerUrl();
                row.Add(Button("explorer", ScfgUiTexts.Get("view_on_explorer"), "scfg-button--small", () => Controller.OpenExplorer(url), "done"));
                body.Add(row);
            }
            else if (c.State == "underpaid")
            {
                body.Add(Text(ScfgUiTexts.Get("underpaid_note"), "scfg-muted"));
            }
            else if (c.State == "expired")
            {
                body.Add(Text(ScfgUiTexts.Get("already_paid_note"), "scfg-muted"));
            }
        }

        private void BuildFooter()
        {
            var c = Ctl;
            var footer = Box("scfg-footer");
            var msg = c.Message();
            if (Py.Truthy(msg)) footer.Add(Reg(Text(msg!, "scfg-error"), "message"));
            footer.Add(Rule());
            var status = c.State != "closed" ? StatusText() : "";
            if (status.Length > 0) footer.Add(StatusRow(status, StatusTone(), "status"));
            var actions = Row("scfg-actions");
            if (EndedStates.Contains(c.State))
            {
                actions.Add(Button("continue", ScfgUiTexts.Get("continue"), "scfg-button--primary", () => CloseWith("finish"), "footer"));
            }
            else
            {
                actions.Add(Button("cancel", ScfgUiTexts.Get("cancel"), "", () => CloseWith("cancel"), "footer"));
                if (Py.Truthy(c.Session))
                    actions.Add(Button("keep_playing", ScfgUiTexts.Get("continue_playing"), "scfg-button--primary", () => CloseWith("keep_playing"),
                        "footer"));
            }
            footer.Add(actions);
            Layout.Add(footer);
        }

        // --- texts (Ren'Py's helpers of the same names) ------------------------------------------------

        private bool Web => Controller.IsWeb;

        private string? ExplorerUrl()
        {
            try
            {
                return Ctl.ExplorerUrl();
            }
            catch (Exception e)
            {
                SCFG.ReportError("cannot build the explorer link:", e);
                return null;
            }
        }

        private string ChainDisplay() => Ctl.Chain?.Name ?? "";

        private string TxWord() => ScfgUiTexts.Get(Ctl.Family == "solana" ? "word_signature" : "word_tx_hash");

        private static string MethodLabel(string m) =>
            ScfgUiTexts.Get(m == "browser" ? "browser_wallet" : m == "qr" ? "phone_wallet" : "manual_transfer");

        private string MethodHint(string m)
        {
            bool solana = Ctl.Family == "solana";
            if (m == "browser")
                return ScfgUiTexts.Get(solana ? (Web ? "solana_browser_sub" : "solana_extension_sub") : (Web ? "evm_browser_sub" : "evm_extension_sub"));
            return ScfgUiTexts.Get(m == "qr" ? "scan_with_phone_wallet" : "send_from_exchange");
        }

        private static string FeeLabel(string? fee) => fee switch
        {
            "low" => ScfgUiTexts.Get("fees_low"),
            "lowest" => ScfgUiTexts.Get("fees_lowest"),
            "high" => ScfgUiTexts.Get("fees_high"),
            _ => fee ?? "",
        };

        private string StatusText()
        {
            var c = Ctl;
            if (c.Step == "amount" || c.Step == "waiting" || c.Step == "done" || c.State == "choose") return "";
            if (c.State == "waiting" && !Py.Truthy(c.HintTx)) return "";
            return ProgressText();
        }

        private string ProgressText()
        {
            var c = Ctl;
            var chain = ChainDisplay();
            if (c.State == "starting") return ScfgUiTexts.Get("preparing_payment");
            if (c.State == "waiting")
                return Py.Truthy(c.HintTx) ? ScfgUiTexts.Format("tx_received_waiting", chain) : ScfgUiTexts.Format("waiting_for_payment", chain);
            if (c.State == "confirming") return ScfgUiTexts.Format("confirming", chain, Int(c.Confirmations), Int(c.Required));
            return "";
        }

        private string DoneTitle() =>
            ScfgUiTexts.Get(Ctl.State == "paid" ? "paid_title" : Ctl.State == "underpaid" ? "payment_too_small" : "payment_window_closed");

        private string StatusTone() => Ctl.State switch
        {
            "paid" => "ok",
            "underpaid" => "error",
            "expired" => "warn",
            _ => "accent",
        };

        private string AmountLine(bool exact) => (exact ? Ctl.PayAmountText : Ctl.PriceText) + " " + Ctl.Spec.Token;

        private string ExactNote()
        {
            var c = Ctl;
            if (!Py.Truthy(c.Session) || c.Step == "manual" || c.Step == "card" || c.Step == "done" || c.State == "closed") return "";
            return ScfgUiTexts.Format("wallet_will_ask", AmountLine(true));
        }

        /// <summary>1.00 -> "1", 2.50 -> "2.50".</summary>
        internal static string PlainAmount(string text) => text.EndsWith(".00", StringComparison.Ordinal) ? text.Substring(0, text.Length - 3) : text;

        internal static string Short(string address) =>
            address.Length <= 10 ? address : address.Substring(0, 6) + "..." + address.Substring(address.Length - 4);
    }
}
