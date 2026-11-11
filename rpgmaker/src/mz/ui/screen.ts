/**
 * What the two screens share (ui/sheet.ts, the payment sheet, and ui/restore.ts, the restore
 * screen): the twin of Godot's ScfgScreenBase. A screen is a `.screen` element in the host's
 * shadow root (ui/dom.ts): the dimmed backdrop over the game (it takes the clicks and does nothing
 * with them) and the light sheet docked on the right, in the Margin look (ui/margin.css).
 *
 * BUILDING. `update()` runs every frame (the default sheet host calls it after SCFG's pump) and
 * rebuilds the sheet when what it shows changed: the subclass's `signature()`. The screen only
 * reads its controller's fields and getters; SCFG polls the controller, a screen never does. Every
 * text it shows goes through `this.text(id, ...)` (SCFG.text: the game's translations); values that
 * are data, not words (amounts, addresses, hashes, chain and item names), carry `data-value`.
 * A text lookup that fails (an unknown id throws in test play) is reported once and shows the id.
 *
 * CONTROLS are registered under a key (`data-key`; the tests find them by it) and, when focusable,
 * join the focus order in the order they are built, which is the visual order. A row of buttons
 * shares a row id. Input (ui/input.ts), as Godot: up and down move along the order, left and right
 * within a row, the ends stay put, Tab wraps; OK presses the focused control (Check in a text
 * field); cancel calls `onCancel()`; page up / down scroll the body. After a rebuild the focus
 * stays on the same key, or goes to the step's primary control when the step changed. In a text
 * field the keyboard is the field's (input.ts stops it at the host): Enter submits, Escape is
 * cancel, and only the real ArrowUp / ArrowDown keys leave it, never letters a game maps to
 * directions; the gamepad still moves the focus out of it through MZ's Input.
 */
import { getChains, type Chain } from "../../core/index.ts";
import type { Scfg } from "../scfg.ts";
import { isTextField, nextFocus, overlayHasFocus, type InputOwner, type Move } from "./input.ts";

export type { Move } from "./input.ts";

/** What a screen gets from its host. */
export interface ScreenEnv {
  scfg: Scfg;
  doc: Document;
  /** The configured logo as a URL once it was read, else null. */
  logo(): string | null;
}

const SVG_NS = "http://www.w3.org/2000/svg";
/** How much of the body's visible height page up / down scroll. */
const PAGE_SCROLL = 0.8;

export abstract class Screen {
  readonly el: HTMLElement;
  /** Resolves once, with how the player closed the screen (sheet_host.ts). */
  readonly closed: Promise<string>;
  ended = false;
  protected readonly scfg: Scfg;
  protected readonly doc: Document;
  protected readonly env: ScreenEnv;
  protected sheet: HTMLElement;
  /** The scrolling part of the current build (page up / down scroll it). */
  protected body: HTMLElement | null = null;
  private _resolve!: (how: string) => void;
  private _controls = new Map<string, HTMLElement>();
  private _order: HTMLElement[] = [];
  private _rows = new Map<HTMLElement, string>();
  private _submits = new WeakMap<HTMLElement, () => void>();
  private _sig: string | null = null;
  private _sigHead: string | null = null;
  /** The web checkout overlay over the game had the keyboard focus on the last frame (web_page.ts). */
  private _overlayHadFocus = false;
  private _reported = new Set<string>();

  constructor(kind: string, env: ScreenEnv) {
    this.env = env;
    this.scfg = env.scfg;
    this.doc = env.doc;
    this.closed = new Promise((resolve) => (this._resolve = resolve));
    this.el = this.doc.createElement("div");
    this.el.className = "screen";
    this.el.setAttribute("data-screen", kind);
    const scrim = this.doc.createElement("div");
    scrim.className = "scrim";
    // the backdrop takes the click (the game behind gets none) and keeps the focus where it is
    scrim.addEventListener("mousedown", (e) => e.preventDefault());
    this.sheet = this.doc.createElement("div");
    this.sheet.className = "sheet";
    this.sheet.setAttribute("role", "dialog");
    this.sheet.setAttribute("aria-modal", "true");
    this.el.append(scrim, this.sheet);
    // the focus ring shows for keys and gamepads, not after a click
    this.el.addEventListener("mousedown", () => this.el.classList.remove("keys"));
  }

  // --- for subclasses --------------------------------------------------------------------------

  /** What the screen shows; it is rebuilt when this changes. Its first element is the step. */
  protected abstract signature(): unknown[];
  protected abstract build(): void;
  /** The key of the control the focus goes to on a new step. */
  protected abstract primary(): string;
  protected abstract onCancel(): void;
  /** Copy the controller's field values into the text fields when they changed from outside (a paste). */
  protected sync(): void {}

  // --- the frame ---------------------------------------------------------------------------------

  /** Rebuild when what the screen shows changed (every frame). */
  update(): void {
    if (this.ended) return;
    // The overlay gave the focus up (hidden or closed: the page's focus fell back to the body):
    // the sheet takes it back, so the next OK or arrow acts on the sheet at once.
    const overlay = overlayHasFocus(this.el.ownerDocument);
    const handedBack = this._overlayHadFocus && !overlay;
    this._overlayHadFocus = overlay;
    let parts: unknown[];
    try {
      this.sync();
      parts = this.signature();
    } catch (e) {
      this.report("the screen cannot read its controller:", e);
      return;
    }
    const sig = JSON.stringify(parts, (_k, v) => (typeof v === "bigint" ? v.toString() : v));
    if (sig === this._sig) {
      if (handedBack && this.focusedKey() === "") this.focus(this.primary());
      return;
    }
    const head = JSON.stringify(parts[0] ?? null);
    const moved = this._sigHead !== head;
    const keep = moved ? "" : this.focusedKey();
    this._sig = sig;
    this._sigHead = head;
    this._controls.clear();
    this._order = [];
    this._rows.clear();
    this.body = null;
    while (this.sheet.firstChild) this.sheet.removeChild(this.sheet.firstChild);
    try {
      this.build();
    } catch (e) {
      this.report("the screen failed to build:", e);
    }
    // the web checkout overlay over the sheet keeps the focus it was given (web_page.ts)
    if (!overlay) this.focus(keep || this.primary());
  }

  /** Force a rebuild on the next update (the logo arrived, the layout changed). */
  invalidate(): void {
    this._sig = null;
  }

  /** Read MZ's input for this frame (the host calls it for the screen on top). */
  input(owner: InputOwner): void {
    if (this.ended) return;
    const moves: [string, Move][] = [["up", "up"], ["down", "down"], ["left", "left"], ["right", "right"]];
    for (const [button, move] of moves) {
      if (owner.repeated(button)) {
        this.keys();
        this.move(move);
        return;
      }
    }
    if (owner.repeated("tab")) {
      this.keys();
      this.move(owner.pressed("shift") ? "previous" : "next");
    } else if (owner.triggered("ok")) {
      this.keys();
      this.press();
    } else if (owner.triggered("cancel")) {
      this.keys();
      this.run(() => this.onCancel());
    } else if (owner.repeated("pageup") || owner.repeated("pagedown")) {
      const body = this.body;
      if (body !== null) body.scrollTop += (owner.repeated("pageup") ? -PAGE_SCROLL : PAGE_SCROLL) * body.clientHeight;
    }
  }

  /** Close the screen once: `closed` resolves `how`. */
  closeWith(how: string): void {
    if (this.ended) return;
    this.ended = true;
    this._resolve(how);
  }

  /** Take the screen off the page. */
  destroy(): void {
    this.ended = true;
    this.el.remove();
  }

  /** The control registered under `key` in the current build, or null. */
  control(key: string): HTMLElement | null {
    return this._controls.get(key) ?? null;
  }

  /** The focusable controls of the current build that can take the focus, in order. */
  focusOrder(): HTMLElement[] {
    return this._order.filter((c) => c.isConnected && c.offsetParent !== null && !(c as HTMLButtonElement).disabled);
  }

  // --- focus -------------------------------------------------------------------------------------

  private activeElement(): Element | null {
    const root = this.el.getRootNode() as Document | ShadowRoot;
    return root.activeElement ?? null;
  }

  protected focusedKey(): string {
    const active = this.activeElement();
    for (const [key, c] of this._controls) if (c === active) return key;
    return "";
  }

  protected focus(key: string): void {
    const order = this.focusOrder();
    let c = key !== "" ? this.control(key) : null;
    if (c === null || !order.includes(c)) c = this.control(this.primary());
    if ((c === null || !order.includes(c)) && order.length > 0) c = order[0]!;
    if (c !== null && c.isConnected) c.focus();
  }

  protected move(move: Move): void {
    const order = this.focusOrder();
    const active = this.activeElement() as HTMLElement | null;
    const here = active !== null ? order.indexOf(active) : -1;
    if (here < 0) {
      this.focus(this.primary());
      return;
    }
    if (isTextField(active) && (move === "left" || move === "right")) return; // the caret's (the keyboard's in a field)
    const there = nextFocus(here, order.length, (i) => this._rows.get(order[i]!) ?? null, move);
    if (there >= 0 && there !== here) order[there]!.focus();
  }

  /** OK: press the focused control (submit a field); nothing focused: focus the primary control. */
  protected press(): void {
    const active = this.activeElement() as HTMLElement | null;
    if (active === null || !this.focusOrder().includes(active)) {
      this.focus(this.primary());
      return;
    }
    const submit = this._submits.get(active);
    if (submit !== undefined) this.run(submit);
    else if (!(active as HTMLButtonElement).disabled) active.click();
  }

  protected keys(): void {
    this.el.classList.add("keys");
  }

  // --- texts and reports -------------------------------------------------------------------------

  /** SCFG.text(id, ...args); a failing lookup is reported once and shows the id. */
  text(id: string, ...args: unknown[]): string {
    try {
      return this.scfg.text(id, ...args);
    } catch (e) {
      this.report(`text ${JSON.stringify(id)}:`, e);
      return id;
    }
  }

  /** An error for the developer, once per message (a screen rebuilds often). */
  protected report(heading: string, e: unknown): void {
    const key = heading + " " + (e instanceof Error ? e.message : String(e));
    if (this._reported.has(key)) return;
    this._reported.add(key);
    this.scfg._reportError(heading, e);
  }

  /** Run a player's action; an error is reported, never thrown into the page. */
  protected run(fn: () => unknown): void {
    if (this.ended) return;
    try {
      fn();
    } catch (e) {
      this.report("screen action failed:", e);
    }
  }

  // --- data the screens show -----------------------------------------------------------------------

  protected chainOf(key: unknown): Chain | null {
    const config = this.scfg._config;
    if (config === null || typeof key !== "string") return null;
    return getChains(config.testnet)[key] ?? config.custom_chains.get(key) ?? null;
  }

  /** A chain's display name (Ren'Py's `_chain_name`), else its key. */
  protected chainName(key: unknown): string {
    return this.chainOf(key)?.name ?? String(key ?? "");
  }

  /** An item's title (Ren'Py's `_item_title`), else its id. */
  protected itemTitle(id: unknown): string {
    const item = typeof id === "string" ? this.scfg._config?.items.get(id) : undefined;
    return item !== undefined ? item.title : String(id ?? "");
  }

  /** `<receipt's what>: <amount> <token> on <chain>` (Ren'Py's `_receipt_line`). */
  protected receiptLine(r: Record<string, any>): string {
    const what = r["kind"] === "unlock" ? this.itemTitle(r["item"]) : this.text("tip");
    return this.text("summary_line", what, r["amount"], r["token"], this.chainName(r["chain"]));
  }

  // --- builders ------------------------------------------------------------------------------------

  protected node<K extends keyof HTMLElementTagNameMap>(tag: K, cls = "", text?: string): HTMLElementTagNameMap[K] {
    const el = this.doc.createElement(tag);
    if (cls !== "") el.className = cls;
    if (text !== undefined) el.textContent = text;
    return el;
  }

  /** Words (from `text()`). */
  protected label(text: string, cls = "", key = ""): HTMLElement {
    const el = this.node("div", cls, text);
    if (key !== "") this.reg(el, key);
    return el;
  }

  /** Data (an amount, an address, a name): shown as it is, never translated. */
  protected value(text: string, cls = "", key = ""): HTMLElement {
    const el = this.node("div", cls, text);
    el.setAttribute("data-value", "");
    if (key !== "") this.reg(el, key);
    return el;
  }

  /** Register `el` under `key`; a focusable one joins the focus order (`row`: left / right move within it). */
  protected reg<T extends HTMLElement>(el: T, key: string, row = ""): T {
    el.setAttribute("data-key", key);
    this._controls.set(key, el);
    if (el.tagName === "BUTTON" || el.tagName === "INPUT") {
      this._order.push(el);
      if (row !== "") this._rows.set(el, row);
    }
    return el;
  }

  protected button(key: string, text: string, cls: string, onPress: () => unknown, row = ""): HTMLButtonElement {
    const b = this.node("button", cls, text);
    b.type = "button";
    b.addEventListener("click", () => this.run(onPress));
    return this.reg(b, key, row);
  }

  /** An option: a label and an optional smaller second line, filled with the accent when selected. */
  protected option(
    key: string, text: string, sub: string, selected: boolean, onPress: () => unknown,
    opts: { row?: string; textIsData?: boolean; subIsData?: boolean } = {},
  ): HTMLButtonElement {
    const b = this.button(key, "", selected ? "option selected" : "option", onPress, opts.row ?? "");
    b.setAttribute("aria-pressed", selected ? "true" : "false");
    const main = this.node("span", "option-text", text);
    if (opts.textIsData) main.setAttribute("data-value", "");
    b.append(main);
    if (sub !== "") {
      const second = this.node("span", "option-sub", sub);
      if (opts.subIsData) second.setAttribute("data-value", "");
      b.append(second);
    }
    return b;
  }

  /** A grid of options, `columns` wide. */
  protected grid(columns: number): HTMLElement {
    const g = this.node("div", "grid");
    g.style.gridTemplateColumns = `repeat(${Math.max(1, columns)}, minmax(0, 1fr))`;
    return g;
  }

  /**
   * A text field: at most `maxLength` characters, only those in `allowed`; `onText(text)` gets
   * the cleaned text, `onSubmit()` Enter (and OK).
   */
  protected field(
    key: string, value: string, maxLength: number, allowed: string, onText: (t: string) => void, onSubmit: () => unknown,
    row = "",
  ): HTMLInputElement {
    const f = this.node("input");
    f.type = "text";
    f.value = value;
    f.maxLength = maxLength;
    f.spellcheck = false;
    f.autocomplete = "off";
    f.setAttribute("autocapitalize", "off");
    f.addEventListener("input", () => {
      const clean = [...f.value].filter((ch) => allowed.includes(ch)).join("").slice(0, maxLength);
      if (clean !== f.value) {
        f.value = clean;
        f.setSelectionRange(clean.length, clean.length);
      }
      this.run(() => onText(clean));
    });
    f.addEventListener("keydown", (e) => {
      if (e.isComposing) return; // an input method's Enter / Escape is its own
      const plain = !e.shiftKey && !e.ctrlKey && !e.altKey && !e.metaKey;
      if (e.key === "Enter") {
        e.preventDefault();
        if (!e.repeat) this.run(onSubmit); // a held Enter checks once
      } else if (plain && (e.key === "ArrowUp" || e.key === "ArrowDown")) {
        // only the arrow keys themselves: letters a game maps to up / down are hash characters
        e.preventDefault();
        this.keys();
        this.move(e.key === "ArrowUp" ? "up" : "down");
      } else if (e.key === "Escape") {
        e.preventDefault();
        this.keys();
        this.run(() => this.onCancel());
      }
    });
    this._submits.set(f, () => onSubmit());
    return this.reg(f, key, row);
  }

  protected div(cls: string, ...children: (Node | null)[]): HTMLElement {
    const el = this.node("div", cls);
    for (const c of children) if (c !== null) el.append(c);
    return el;
  }

  protected rule(): HTMLElement {
    return this.node("div", "rule");
  }

  /** The status line: a dot in `tone` (accent, ok, error, warn) and the text. */
  protected statusRow(text: string, tone: string, key = "status"): HTMLElement {
    const row = this.div("row status-line", this.node("span", `status-dot ${tone}`));
    const t = this.node("span", "status", text);
    row.append(t);
    this.reg(t, key);
    return row;
  }

  /** Step dots: `index` + 1 of `total` filled. */
  protected dots(index: number, total: number): HTMLElement {
    const row = this.node("div", "dots");
    for (let i = 0; i < total; i++) row.append(this.node("span", i <= index ? "dot on" : "dot"));
    return row;
  }

  /** The developer's logo, fitted into its slot, or null. */
  protected logo(): HTMLElement | null {
    const url = this.env.logo();
    if (url === null) return null;
    const img = this.node("img", "logo");
    img.alt = "";
    img.src = url;
    return this.reg(img, "logo");
  }

  /** The back arrow, drawn (no font needs the glyph). */
  protected arrow(): SVGElement {
    const svg = this.doc.createElementNS(SVG_NS, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("aria-hidden", "true");
    const path = this.doc.createElementNS(SVG_NS, "path");
    path.setAttribute("d", "M19 12H5M11 6l-6 6 6 6");
    svg.append(path);
    return svg;
  }
}
