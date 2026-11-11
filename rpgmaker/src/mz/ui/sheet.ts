/**
 * The payment sheet: the RPG Maker twin of the `scfg_payment` screen in
 * plugin/game/scfg/scfg_screens.rpy (and of Godot's ui/sheet.gd), shown by the default sheet host
 * for `await SCFG.pay(...)`.
 *
 * One step at a time, as the controller's `step` says: amount (tips) -> network -> method ->
 * browser | qr | manual -> waiting -> done. Every step has the header (the back arrow while the
 * controller `canGoBack`, the step dots, the logo, the title and the price), a scrolling body, the
 * pinned forward button when the step has one, and the footer (the status message, the status
 * line and Cancel / Continue playing / Continue). A test play also shows the developer lines
 * (DEV MODE and the wallet setup warnings).
 *
 * The sheet reads only the CheckoutController's fields and getters and calls its methods; SCFG
 * polls it every frame. Buttons that leave the game go through SCFG (openBrowser, openWalletApp,
 * openExplorer, copy, pasteInto). `closed` resolves "cancel", "keep_playing" or "finish", Ren'Py's
 * _close_checkout hows.
 *
 * Keyboard and gamepad (screen.ts): each step focuses its primary control (the selected option,
 * the forward button, the first Copy, Continue); cancel goes back, or closes as Cancel on the
 * first step.
 */
import { parseUnreadyKey, type CheckoutController } from "../../core/index.ts";
import { PASTE_MAX, TX_CHARS } from "../actions.ts";
import { qrSvg } from "./qr.ts";
import { Screen, type ScreenEnv } from "./screen.ts";

const FINAL = ["closed", "paid", "underpaid", "expired"];

/** 1.00 -> "1", 2.50 -> "2.50" (Ren'Py's `_plain_amount`). */
export function plainAmount(text: string): string {
  return text.endsWith(".00") ? text.slice(0, -3) : text;
}

/** 0x5290...9EE7 (Ren'Py's `_short_address`). */
export function shortAddress(address: string): string {
  return address.slice(0, 6) + "..." + address.slice(-4);
}

/**
 * The amount with its token, as the sheet shows it: the price, or (`exact`) the amount the wallet
 * pays, whose last digits identify the payment ("4.993725 USDC"; the manual step's Amount row).
 */
export function amountLine(c: CheckoutController, exact: boolean): string {
  return `${exact ? c.payAmountText : c.priceText} ${c.spec.token}`;
}

export class PaymentSheet extends Screen {
  readonly ctl: CheckoutController;
  private _synced = new Map<string, string>();
  /** The step the last build showed (a new one slides in). */
  private _shown: string | null = null;

  constructor(ctl: CheckoutController, env: ScreenEnv) {
    super("payment", env);
    this.ctl = ctl;
  }

  private get web(): boolean {
    return this.scfg._platform.isWeb();
  }

  // --- what is shown -------------------------------------------------------------------------------

  protected signature(): unknown[] {
    const c = this.ctl;
    return [c.step, c.state, c.error, c.canGoBack, c.hint_tx, c.confirmations, c.required, c.method, c.chain_key,
      c.priceText, c.sessionId, c.browser_failed, c.chainOptions(), c.methods(), c.result?.tx_hash ?? null, c.message(),
      [...c.unready()].sort(), this.env.logo()];
  }

  protected primary(): string {
    const c = this.ctl;
    if (FINAL.includes(c.state)) return "continue";
    switch (c.step) {
      case "amount": {
        const preset = c.tipPresets().find((p) => c.amount.cmp(p) === 0);
        return preset !== undefined ? `preset_${preset}` : "custom_amount";
      }
      case "network": {
        const options = c.chainOptions();
        if (c.chain_key !== null) return `network_${c.chain_key}`;
        return options.length > 0 ? `network_${options[0]![0]}` : "cancel";
      }
      case "method": {
        const methods = c.methods();
        if (c.method !== null && methods.includes(c.method)) return `method_${c.method}`;
        return methods.length > 0 ? `method_${methods[0]}` : "cancel";
      }
      case "browser":
      case "qr":
        return "forward";
      case "manual":
        return "copy_amount";
      case "waiting":
        for (const key of ["explorer", "reopen", "keep_playing"]) if (this.control(key) !== null) return key;
    }
    return "cancel";
  }

  protected sync(): void {
    for (const key of ["tx_input", "custom_amount"] as const) {
      const f = this.control(key) as HTMLInputElement | null;
      const value = this.ctl[key];
      if (f !== null && this._synced.get(key) !== value) {
        f.value = value;
        f.setSelectionRange(value.length, value.length);
        this._synced.set(key, value);
      }
    }
  }

  protected onCancel(): void {
    const c = this.ctl;
    if (c.canGoBack) c.back();
    else if (!c.txSeen && c.step !== "waiting" && c.step !== "done" && c.stepDots()[0] === 0) this.closeWith("cancel");
  }

  // --- the build -----------------------------------------------------------------------------------

  protected build(): void {
    this._synced.clear();
    this.sheet.setAttribute("aria-label", this.title());
    this.el.setAttribute("data-step", this.ctl.step);
    this.sheet.append(this.header());
    const body = this.div("sheet-body stack");
    if (this._shown !== null && this._shown !== this.ctl.step) body.classList.add(this.ctl.nav_dir < 0 ? "slide-back" : "slide-forward");
    this._shown = this.ctl.step;
    this.body = body;
    this.sheet.append(body);
    const forward = this.step(body);
    if (forward !== null) this.sheet.append(this.button("forward", forward[0], "accent forward", forward[1]));
    this.sheet.append(this.footer());
  }

  /** A tip with the default title shows the translated word; any other title is the game's data. */
  private titleIsText(): boolean {
    const spec = this.ctl.spec;
    return spec.kind === "tip" && spec.title === "Tip";
  }

  private title(): string {
    return this.titleIsText() ? this.text("tip") : this.ctl.spec.title;
  }

  private header(): HTMLElement {
    const c = this.ctl;
    const head = this.div("sheet-head");
    const back = this.button("back", "", "back", () => c.back());
    back.setAttribute("aria-label", this.text("back"));
    back.append(this.arrow());
    back.hidden = !c.canGoBack;
    const nav = this.div("nav", back, this.node("div", "grow"));
    if (c.state !== "closed" && c.step !== "done") {
      const [index, total] = c.stepDots();
      if (total > 1) nav.append(this.dots(index, total));
    }
    head.append(nav, this.logo() ?? this.node("div"));
    if (c.state === "closed") return head;
    if (this.scfg._platform.isTestPlay()) {
      const to = shortAddress(String(c.recipient));
      head.append(this.label(c.config.testnet ? this.text("dev_mode_paying_to_testnet", to) : this.text("dev_mode_paying_to", to), "dev"));
      for (const entry of [...c.unready()].sort()) {
        const [chain, token] = parseUnreadyKey(entry);
        if (this.chainOf(chain)?.family === "solana") head.append(this.label(this.text("dev_solana_cant_receive", token), "dev"));
      }
    }
    const spec = c.spec;
    head.append(this.titleIsText() ? this.label(this.title(), "title", "title") : this.value(spec.title, "title", "title"));
    head.append(this.div("price", this.value(c.priceText, "amount", "price"), this.value(spec.token, "amount-token", "token")));
    const note = this.exactNote();
    if (note !== "") head.append(this.label(note, "note", "note"));
    return head;
  }

  /** Builds the step's body; returns [label, action] of its forward button, or null. */
  private step(body: HTMLElement): [string, () => void] | null {
    const c = this.ctl;
    if (c.state === "closed") {
      body.append(this.label(this.text("payment_screen_inactive")));
      return null;
    }
    switch (c.step) {
      case "amount":
        this.stepAmount(body);
        return [this.text("next"), () => void c.nextStep()];
      case "network":
        this.stepNetwork(body);
        return null;
      case "method":
        this.stepMethod(body);
        return null;
      case "browser":
        this.stepBrowser(body);
        return [this.web ? this.text("open_wallet") : this.text("open_checkout_browser"), () => this.scfg.openBrowser(c)];
      case "qr":
        this.stepQr(body);
        return [this.text("i_sent_it"), () => c.markSent()];
      case "manual":
        this.stepManual(body);
        return [this.text("i_sent_it"), () => c.markSent()];
      case "waiting":
        this.stepWaiting(body);
        return null;
      default:
        this.stepDone(body);
        return null;
    }
  }

  private stepAmount(body: HTMLElement): void {
    const c = this.ctl;
    body.append(this.label(this.text("choose_amount"), "label"));
    const presets = c.tipPresets();
    const grid = this.grid(Math.min(presets.length, 4));
    for (const preset of presets) {
      const text = `${plainAmount(String(preset))} ${c.spec.token}`;
      grid.append(this.option(`preset_${preset}`, text, "", c.amount.cmp(preset) === 0, () => c.setTipAmount(String(preset)),
        { row: "presets", textIsData: true }));
    }
    body.append(grid, this.label(this.text("or_enter_your_own"), "label"));
    const field = this.field("custom_amount", c.custom_amount, 8, "0123456789.", (t) => {
      c.custom_amount = t;
      this._synced.set("custom_amount", t);
    }, () => c.applyCustomAmount(), "amount");
    body.append(this.div("field-row", field, this.button("set_amount", this.text("set"), "small", () => c.applyCustomAmount(), "amount")));
    this._synced.set("custom_amount", c.custom_amount);
  }

  private stepNetwork(body: HTMLElement): void {
    const c = this.ctl;
    body.append(this.label(this.text("which_network", c.spec.token), "label"));
    const options = c.chainOptions();
    const grid = this.grid(Math.min(options.length, 2));
    options.forEach(([key, name, fee], i) => {
      const known = this.feeLabel(fee);
      grid.append(this.option(`network_${key}`, name, known ?? (fee || ""), c.chain_key === key, () => c.chooseNetwork(key),
        { row: `networks:${Math.floor(i / 2)}`, textIsData: true, subIsData: known === null }));
    });
    body.append(grid);
  }

  private stepMethod(body: HTMLElement): void {
    const c = this.ctl;
    body.append(this.label(this.text("how_to_pay"), "label"));
    const list = this.div("stack");
    for (const m of c.methods()) list.append(this.option(`method_${m}`, this.methodLabel(m), this.methodHint(m), c.method === m, () => c.chooseMethod(m)));
    body.append(list);
  }

  private stepBrowser(body: HTMLElement): void {
    const c = this.ctl;
    const solana = c.family === "solana";
    body.append(this.label(this.text("browser_wallet"), "label"));
    let note: string;
    if (this.web) note = solana ? this.text("solana_opens_here_note") : this.text("evm_opens_here_note");
    else note = solana ? this.text("solana_opens_browser_note") : this.text("evm_opens_browser_note");
    body.append(this.label(note), this.label(this.text("approve_in_wallet", this.amountLine(false), this.chainDisplay()), "muted"));
  }

  private stepQr(body: HTMLElement): void {
    const c = this.ctl;
    body.append(this.label(this.text("phone_wallet"), "label"));
    const row = this.div("qr-row");
    let uri: string | null = null;
    try {
      uri = c.paymentUri();
    } catch (e) {
      this.report("no payment link:", e);
    }
    if (uri !== null) {
      const frame = this.div("qr-frame");
      try {
        const svg = new (this.doc.defaultView as any).DOMParser().parseFromString(qrSvg(uri), "image/svg+xml").documentElement;
        svg.setAttribute("aria-label", this.text("qr_code"));
        frame.append(this.doc.importNode(svg, true));
      } catch (e) {
        this.report("cannot draw the QR code:", e);
      }
      row.append(this.reg(frame, "qr"));
    }
    row.append(this.div("stack grow qr-steps",
      this.label(this.text("qr_step_open"), "qr-step"),
      this.label(this.text("qr_step_scan"), "qr-step"),
      this.label(this.text("qr_step_approve", this.amountLine(false)), "qr-step")));
    body.append(row);
    if (this.scfg._platform.isMobile() && c.config.methods["qr"]?.["wallet_button"]) {
      body.append(this.button("wallet_app", this.text("open_wallet_app"), "", () => this.scfg.openWalletApp(c)));
    }
    body.append(this.label(c.family === "solana" ? this.text("wallet_changes_amount") : this.text("wallet_changes_amount_manual"), "muted"));
  }

  private stepManual(body: HTMLElement): void {
    const c = this.ctl;
    const solana = c.family === "solana";
    body.append(this.label(solana ? this.text("send_exact_amount_solana") : this.text("send_exact_amount", this.chainDisplay()), "warning"));
    const rows: [string, string, string][] = [
      ["amount", this.text("amount"), this.amountLine(true)],
      ["recipient", this.text("to_address"), String(c.recipient)],
      ["token", solana ? this.text("token_mint") : this.text("token_contract"), String(c.tokenAddress)],
    ];
    for (const [key, label, value] of rows) {
      const copy = this.button(`copy_${key}`, this.text("copy"), "small", () => this.scfg.copy(value.split(" ")[0]));
      body.append(this.div("stack tight", this.label(label, "label"), this.div("value-row", this.value(value, "mono grow", `value_${key}`), copy)));
    }
    if (!c.config.methods["manual"]?.["allow_tx_paste"]) return;
    body.append(this.label(this.text("sent_from_exchange_paste", this.txWord()), "label"));
    const field = this.field("tx_input", c.tx_input, PASTE_MAX, TX_CHARS, (t) => {
      c.tx_input = t;
      this._synced.set("tx_input", t);
    }, () => c.submitHash(), "tx");
    const row = this.div("field-row", field);
    if (this.web) row.append(this.button("paste", this.text("paste"), "small", () => this.scfg.pasteInto(c), "tx"));
    row.append(this.button("check", this.text("check"), "small", () => c.submitHash(), "tx"));
    body.append(row);
    this._synced.set("tx_input", c.tx_input);
  }

  private stepWaiting(body: HTMLElement): void {
    const c = this.ctl;
    body.append(this.label(this.progressText(), "heading", "heading"));
    if (c.state === "confirming") {
      const fill = this.node("div", "progress-fill");
      fill.style.width = `${Math.min(100, (100 * c.confirmations) / Math.max(1, c.required))}%`;
      body.append(this.reg(this.div("progress", fill), "progress"));
    }
    if (c.hint_tx) {
      body.append(this.label(this.text("your_hash", this.txWord()), "label"));
      body.append(this.value(c.hint_tx, "mono", "hint"));
      const url = c.explorerUrl();
      if (url) body.append(this.div("row", this.button("explorer", this.text("view_on_explorer"), "small", () => this.scfg.openExplorer(url))));
    } else if (c.method === "browser") {
      body.append(this.label(this.web ? this.text("finish_in_wallet_window") : this.text("finish_in_browser"), "muted"));
      const text = this.web ? this.text("reopen_wallet_window") : this.text("reopen_checkout_page");
      body.append(this.div("row", this.button("reopen", text, "small", () => this.scfg.openBrowser(c))));
    }
    body.append(this.node("div", "gap"), this.label(this.text("keep_playing_note"), "muted"));
  }

  private stepDone(body: HTMLElement): void {
    const c = this.ctl;
    body.append(this.label(this.doneTitle(), `heading ${this.statusTone()}`, "heading"));
    const tx = c.result?.tx_hash ?? null;
    if (c.state === "paid" && tx) {
      body.append(this.label(this.text("keep_this_to_restore", this.txWord()), "muted"), this.value(tx, "mono", "tx"));
      const url = c.explorerUrl();
      body.append(this.div("row",
        this.button("copy_tx", c.family === "solana" ? this.text("copy_signature") : this.text("copy_hash"), "small", () => this.scfg.copy(tx), "done"),
        this.button("explorer", this.text("view_on_explorer"), "small", () => this.scfg.openExplorer(url), "done")));
    } else if (c.state === "underpaid") {
      body.append(this.label(this.text("underpaid_note"), "muted"));
    } else if (c.state === "expired") {
      body.append(this.label(this.text("already_paid_note"), "muted"));
    }
  }

  private footer(): HTMLElement {
    const c = this.ctl;
    const foot = this.div("sheet-foot");
    const msg = this.messageText();
    if (msg !== null) foot.append(this.label(msg, "error", "message"));
    foot.append(this.rule());
    const status = c.state !== "closed" ? this.statusText() : "";
    if (status !== "") foot.append(this.statusRow(status, this.statusTone()));
    const actions = this.div("actions");
    if (FINAL.includes(c.state)) {
      actions.append(this.button("continue", this.text("continue"), "primary", () => this.closeWith("finish"), "footer"));
    } else {
      actions.append(this.button("cancel", this.text("cancel"), "", () => this.closeWith("cancel"), "footer"));
      if (c.session) actions.append(this.button("keep_playing", this.text("continue_playing"), "primary", () => this.closeWith("keep_playing"), "footer"));
    }
    foot.append(actions);
    return foot;
  }

  // --- texts (Ren'Py's helpers of the same names) --------------------------------------------------

  /** The controller's message in the player's language: its id is the error key (CHECKOUT_MESSAGES). */
  private messageText(): string | null {
    const c = this.ctl;
    if (c.message() === null) return null;
    return c.error ? this.text(c.error) : this.text("solana_unavailable");
  }

  private chainDisplay(): string {
    return this.ctl.chain?.name ?? "";
  }

  private txWord(): string {
    return this.ctl.family === "solana" ? this.text("word_signature") : this.text("word_tx_hash");
  }

  private methodLabel(m: string): string {
    if (m === "browser") return this.text("browser_wallet");
    if (m === "qr") return this.text("phone_wallet");
    return this.text("manual_transfer");
  }

  private methodHint(m: string): string {
    const solana = this.ctl.family === "solana";
    if (m === "browser") {
      if (solana) return this.web ? this.text("solana_browser_sub") : this.text("solana_extension_sub");
      return this.web ? this.text("evm_browser_sub") : this.text("evm_extension_sub");
    }
    if (m === "qr") return this.text("scan_with_phone_wallet");
    return this.text("send_from_exchange");
  }

  /** The words for a built-in fee hint, or null for a custom chain's own (shown as it is). */
  private feeLabel(fee: string): string | null {
    if (fee === "low") return this.text("fees_low");
    if (fee === "lowest") return this.text("fees_lowest");
    if (fee === "high") return this.text("fees_high");
    return null;
  }

  private statusText(): string {
    const c = this.ctl;
    if (["amount", "waiting", "done"].includes(c.step) || c.state === "choose") return "";
    if (c.state === "waiting" && !c.hint_tx) return "";
    return this.progressText();
  }

  private progressText(): string {
    const c = this.ctl;
    const chain = this.chainDisplay();
    if (c.state === "starting") return this.text("preparing_payment");
    if (c.state === "waiting") return c.hint_tx ? this.text("tx_received_waiting", chain) : this.text("waiting_for_payment", chain);
    if (c.state === "confirming") return this.text("confirming", chain, c.confirmations, c.required);
    return "";
  }

  private doneTitle(): string {
    if (this.ctl.state === "paid") return this.text("paid_title");
    if (this.ctl.state === "underpaid") return this.text("payment_too_small");
    return this.text("payment_window_closed");
  }

  /** Ren'Py's `_status_color`, as a class: ok, error, warn, else the accent. */
  private statusTone(): string {
    return { paid: "ok", underpaid: "error", expired: "warn" }[this.ctl.state] ?? "accent";
  }

  private amountLine(exact: boolean): string {
    return amountLine(this.ctl, exact);
  }

  /** Under the price once a session exists: the wallet asks for slightly more (the last digits identify the payment). */
  private exactNote(): string {
    const c = this.ctl;
    if (!c.session || c.step === "manual" || c.step === "done" || c.state === "closed") return "";
    return this.text("wallet_will_ask", this.amountLine(true));
  }
}
