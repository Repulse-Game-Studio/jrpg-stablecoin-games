/**
 * TEST ONLY. The desktop e2e driver's sheet host (desktop_driver.ts): a SheetHost that shows
 * nothing. `view(screen)` is what the plugin's payment sheet (src/mz/ui/sheet.ts) would show, and
 * `buttons(screen)` its buttons on the controller's current step, under the sheet's own control
 * names (`data-key`); pressing one does exactly what the sheet's button of that name does (the
 * controller's methods, SCFG's actions, or closing the sheet as Cancel / Continue playing /
 * Continue), nothing more. The sheet tests (tests/rpgmaker_ui/test_sheet.py,
 * test_headless_host_buttons_match_the_sheet) load this module into the sheet test page and
 * compare its button names with the real sheet's on every step. DOM- and Node-free.
 */
import type { CheckoutController, RestoreController } from "../../src/core/index.ts";
import type { Scfg } from "../../src/mz/scfg.ts";
import type { SheetHandle, SheetHost } from "../../src/mz/sheet_host.ts";
import { amountLine } from "../../src/mz/ui/sheet.ts";

const FINAL = ["closed", "paid", "underpaid", "expired"];

/** A command the driver cannot carry out (its reply is an error). */
export class DriverError extends Error {}


export interface Button {
  name: string;
  text: string;
  press: () => void;
}

/** One screen the host shows: its controller, its `closed` and its buttons (ui/sheet.ts's names). */
export class HeadlessScreen {
  readonly kind: "payment" | "restore";
  readonly ctl: any;
  readonly handle: SheetHandle;
  ended = false;
  private _resolve!: (how: string) => void;

  constructor(kind: "payment" | "restore", ctl: unknown) {
    this.kind = kind;
    this.ctl = ctl;
    this.handle = { closed: new Promise<string>((resolve) => (this._resolve = resolve)) };
  }

  closeWith(how: string): void {
    if (this.ended) return;
    this.ended = true;
    this._resolve(how);
  }
}

/**
 * A SheetHost that shows nothing: `view(screen)` is what the plugin's sheet would show, and its
 * buttons do exactly what the sheet's buttons of the same name do (ui/sheet.ts: the controller's
 * methods, SCFG's actions, or closing the sheet as Cancel / Continue playing / Continue).
 */
export class HeadlessHost implements SheetHost {
  screens: HeadlessScreen[] = [];
  notes: string[] = [];
  private readonly scfg: Scfg;

  constructor(scfg: Scfg) {
    this.scfg = scfg;
  }

  openPayment(ctl: CheckoutController): SheetHandle {
    return this._open("payment", ctl);
  }

  openRestore(ctl: RestoreController): SheetHandle {
    return this._open("restore", ctl);
  }

  private _open(kind: "payment" | "restore", ctl: unknown): SheetHandle {
    const screen = new HeadlessScreen(kind, ctl);
    this.screens.push(screen);
    return screen.handle;
  }

  close(handle: SheetHandle): void {
    this.screens = this.screens.filter((s) => s.handle !== handle);
  }

  notify(text: string): void {
    this.notes.push(text);
  }

  private t(id: string, ...args: unknown[]): string {
    return this.scfg.text(id, ...args);
  }

  /** The buttons of the screen's current step, in the sheet's order. */
  buttons(screen: HeadlessScreen): Button[] {
    return screen.kind === "payment" ? this._paymentButtons(screen) : this._restoreButtons(screen);
  }

  private _paymentButtons(screen: HeadlessScreen): Button[] {
    const c = screen.ctl;
    const s = this.scfg;
    const out: Button[] = [];
    const add = (name: string, text: string, press: () => void) => out.push({ name, text, press });
    if (c.canGoBack) add("back", this.t("back"), () => c.back());
    if (c.state !== "closed") {
      switch (c.step) {
        case "amount":
          for (const p of c.tipPresets()) add(`preset_${p}`, `${p} ${c.spec.token}`, () => c.setTipAmount(String(p)));
          add("set_amount", this.t("set"), () => c.applyCustomAmount());
          add("forward", this.t("next"), () => void c.nextStep());
          break;
        case "network":
          for (const [key, name] of c.chainOptions()) add(`network_${key}`, name, () => c.chooseNetwork(key));
          break;
        case "method":
          for (const m of c.methods()) add(`method_${m}`, m, () => c.chooseMethod(m));
          break;
        case "browser":
          add("forward", this.t("open_checkout_browser"), () => s.openBrowser(c));
          break;
        case "qr":
          if (s._platform.isMobile() && c.config.methods["qr"]?.["wallet_button"]) add("wallet_app", this.t("open_wallet_app"), () => s.openWalletApp(c));
          add("forward", this.t("i_sent_it"), () => c.markSent());
          break;
        case "manual": {
          const values = this._manualValues(c);
          for (const key of ["amount", "recipient", "token"] as const) {
            add(`copy_${key}`, this.t("copy"), () => s.copy(values[key].split(" ")[0]));
          }
          // (the sheet's Paste button is web only: on desktop the player pastes into the field)
          if (c.config.methods["manual"]?.["allow_tx_paste"]) add("check", this.t("check"), () => c.submitHash());
          add("forward", this.t("i_sent_it"), () => c.markSent());
          break;
        }
        case "waiting":
          if (c.hint_tx) {
            const url = c.explorerUrl();
            if (url) add("explorer", this.t("view_on_explorer"), () => s.openExplorer(url));
          } else if (c.method === "browser") {
            add("reopen", this.t("reopen_checkout_page"), () => s.openBrowser(c));
          }
          break;
        default: {
          const tx = c.result?.tx_hash ?? null;
          if (c.state === "paid" && tx) {
            add("copy_tx", this.t(c.family === "solana" ? "copy_signature" : "copy_hash"), () => s.copy(tx));
            add("explorer", this.t("view_on_explorer"), () => s.openExplorer(c.explorerUrl()));
          }
        }
      }
    }
    if (FINAL.includes(c.state)) {
      add("continue", this.t("continue"), () => screen.closeWith("finish"));
    } else {
      add("cancel", this.t("cancel"), () => screen.closeWith("cancel"));
      if (c.session) add("keep_playing", this.t("continue_playing"), () => screen.closeWith("keep_playing"));
    }
    return out;
  }

  private _restoreButtons(screen: HeadlessScreen): Button[] {
    // (the e2e does not restore: the restore screen only closes)
    return [{ name: "close", text: this.t("close"), press: () => screen.closeWith("close") }];
  }

  /** The manual step's three rows, as the sheet shows them (the amount with its token: the sheet's amountLine). */
  private _manualValues(c: any): { amount: string; recipient: string; token: string } {
    return { amount: amountLine(c, true), recipient: String(c.recipient), token: String(c.tokenAddress) };
  }

  /** The names of the buttons a screen of `kind` for `ctl` shows now (the sheet test compares them). */
  buttonNames(kind: "payment" | "restore", ctl: unknown): string[] {
    return this.buttons(new HeadlessScreen(kind, ctl)).map((b) => b.name);
  }

  /** What the screen shows: the controller's visible values and the step's buttons. */
  view(screen: HeadlessScreen): Record<string, unknown> {
    const c = screen.ctl;
    const buttons = this.buttons(screen).map((b) => ({ name: b.name, text: b.text }));
    if (screen.kind === "restore") return { kind: "restore", state: c.state, message: c.message(), buttons };
    const view: Record<string, unknown> = {
      kind: "payment", step: c.step, state: c.state, title: c.spec.title, price: c.priceText, token: c.spec.token,
      method: c.method, chain: c.chain_key, network: c.chain?.name ?? null, session: c.sessionId ?? null,
      confirmations: c.confirmations, required: c.required, hint: c.hint_tx, tx: c.result?.tx_hash ?? null,
      message: c.message(), buttons,
    };
    if (c.step === "manual" && c.state !== "closed") {
      const m = this._manualValues(c);
      Object.assign(view, { amount: m.amount, recipient: m.recipient, token_address: m.token });
    }
    if (c.step === "done") view["heading"] = c.state === "paid" ? this.t("paid_title") : c.state;
    return view;
  }

  /** Press the button `name` of the newest screen. */
  press(name: string): void {
    const screen = this.screens[this.screens.length - 1];
    if (screen === undefined) throw new DriverError(`no screen is open (to press ${name})`);
    const button = this.buttons(screen).find((b) => b.name === name);
    if (button === undefined) {
      throw new DriverError(`no button ${name} on the ${screen.kind} screen: ${this.buttons(screen).map((b) => b.name).join(", ")}`);
    }
    button.press();
  }
}
