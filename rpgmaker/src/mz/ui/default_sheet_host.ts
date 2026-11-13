/**
 * The sheet host SCFG uses by default (`SCFG.sheetHost`, sheet_host.ts has the contract): the
 * plugin's own payment sheet (ui/sheet.ts) and restore screen (ui/restore.ts) in the shadow root
 * over the game canvas (ui/dom.ts), toasts for notify() (ui/toast.ts), and MZ's input owned by
 * the screens while one is open (ui/input.ts). The twin of Godot's ScfgDefaultSheetHost.
 *
 * Customising: `SCFG.configure({ accent, logo })` (the accent colours the selected options, the
 * forward button and the dots; the logo sits in the header), `SCFG.css` (a stylesheet appended to
 * the sheet's own; the class names are listed in ui/margin.css), or replace `SCFG.sheetHost`.
 *
 * Every frame (`update()`, called by the plugin after SCFG's pump, also while the game is paused
 * or its window unfocused): the screen on top reads MZ's input, then every screen rebuilds when
 * its controller changed. A screen taken off the page by someone else (its element removed)
 * counts as closed by Cancel, as Godot's freed sheet. Without a page (`document.body`) nothing can
 * be shown: openPayment() / openRestore() return null (pay() then resolves "failed", "sheet.unavailable").
 */
import type { CheckoutController, RestoreController } from "../../core/index.ts";
import type { Scfg } from "../scfg.ts";
import type { SheetHandle, SheetHost } from "../sheet_host.ts";
import { hostElement, hostRoot, mountHost, setAccent, unmountHost } from "./dom.ts";
import { InputOwner, guardHost } from "./input.ts";
import { RestoreScreen } from "./restore.ts";
import { PaymentSheet } from "./sheet.ts";
import type { Screen, ScreenEnv } from "./screen.ts";
import { showToast } from "./toast.ts";

const USER_CSS = "scfg-user-css";

export class DefaultSheetHost implements SheetHost {
  /** Who gets MZ's input (the screens while one is open). */
  readonly input: InputOwner;
  private readonly _scfg: Scfg;
  private _screens: Screen[] = [];
  private readonly _handles = new WeakMap<SheetHandle, Screen>();
  private _guarded: HTMLElement | null = null;
  private _unguard: (() => void) | null = null;
  private _returnFocus: HTMLElement | null = null;
  private _logo: { file: string | null; url: string | null } = { file: null, url: null };
  private _css: string | null = null;

  constructor(scfg: Scfg) {
    this._scfg = scfg;
    this.input = new InputOwner();
  }

  openPayment(ctl: CheckoutController): SheetHandle | null {
    return this._open((env) => new PaymentSheet(ctl, env));
  }

  openRestore(ctl: RestoreController): SheetHandle | null {
    return this._open((env) => new RestoreScreen(ctl, env));
  }

  close(handle: SheetHandle): void {
    const screen = this._handles.get(handle);
    if (screen === undefined) return;
    this._handles.delete(handle);
    const i = this._screens.indexOf(screen);
    if (i < 0) return;
    this._screens.splice(i, 1);
    screen.destroy();
    this.input.release();
    if (this._screens.length === 0) {
      const back = this._returnFocus;
      this._returnFocus = null;
      if (back !== null && back.isConnected && typeof back.focus === "function") back.focus();
    }
  }

  notify(text: string): void {
    const doc = this._doc();
    if (doc === null) return;
    showToast(this._mount(), text);
  }

  /** Once a frame, after SCFG's pump (see the header). */
  update(): void {
    this.input.frame();
    if (this._screens.length === 0) return;
    for (const s of [...this._screens]) if (!s.ended && !s.el.isConnected) s.closeWith("cancel");
    this._applyCss();
    const top = [...this._screens].reverse().find((s) => !s.ended);
    if (top !== undefined) top.input(this.input);
    for (const s of this._screens) s.update();
  }

  /** The screens shown and not closed yet, oldest first (tests and drivers find them here). */
  screens(): readonly Screen[] {
    return this._screens;
  }

  // --- internals ---------------------------------------------------------------------------------

  private _doc(): Document | null {
    const doc = (globalThis as any).document as Document | undefined;
    return doc && doc.body && typeof doc.createElement === "function" ? doc : null;
  }

  /** The host's `.scfg-root`, mounted again when something took the host off the page. */
  private _mount(): HTMLElement {
    const el = hostElement();
    if (el !== null && !el.isConnected) unmountHost();
    mountHost();
    const host = hostElement()!;
    if (this._guarded !== host) {
      this._unguard?.();
      this._unguard = guardHost(host, this.input);
      this._guarded = host;
      this._css = null;
    }
    return hostRoot()!;
  }

  private _open(make: (env: ScreenEnv) => Screen): SheetHandle | null {
    const doc = this._doc();
    if (doc === null) return null;
    const root = this._mount();
    setAccent(this._scfg._config?.accent);
    this._applyCss();
    this._loadLogo();
    const screen = make({ scfg: this._scfg, doc, logo: () => this._logo.url });
    if (this._screens.length === 0) {
      const active = doc.activeElement as HTMLElement | null;
      this._returnFocus = active !== null && active !== doc.body && active !== hostElement() ? active : null;
    }
    const toasts = root.querySelector(":scope > .toasts");
    root.insertBefore(screen.el, toasts);
    this._screens.push(screen);
    this.input.acquire();
    screen.update();
    const handle: SheetHandle = { closed: screen.closed };
    this._handles.set(handle, screen);
    return handle;
  }

  /** `SCFG.css` after the sheet's own stylesheet (updated when it changes). */
  private _applyCss(): void {
    const el = hostElement();
    const root = el?.shadowRoot;
    if (!root) return;
    const css = typeof this._scfg.css === "string" ? this._scfg.css : "";
    if (css === this._css) return;
    this._css = css;
    let style = root.getElementById(USER_CSS);
    if (style === null) {
      style = el!.ownerDocument.createElement("style");
      style.id = USER_CSS;
      root.insertBefore(style, root.querySelector(".scfg-root"));
    }
    style.textContent = css;
  }

  /** The configured logo, read once per configuration (SCFG.logoDataUrl); the screens show it when it is there. */
  private _loadLogo(): void {
    const file = this._scfg._config?.logo ?? null;
    if (file === this._logo.file) return;
    this._logo = { file, url: null };
    if (file === null) return;
    const wanted = this._logo;
    void this._scfg.logoDataUrl().then((url) => {
      if (this._logo !== wanted) return;
      wanted.url = url;
      for (const s of this._screens) s.invalidate();
    }).catch((e: unknown) => this._scfg._reportError("cannot show the logo:", e)); // never an unhandled rejection (MZ's onReject)
  }
}
