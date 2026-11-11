/**
 * Who gets RPG Maker MZ's input while the payment sheet or the restore screen is open: the
 * screen, never the game (the twin of Godot's ScfgScreenBase swallowing every event and Ren'Py's
 * `modal True`).
 *
 * THE GATE. MZ's game code asks `Input.isPressed/isTriggered/isRepeated/isLongPressed(name)`,
 * `Input.dir4/dir8` and `TouchInput.isPressed/isTriggered/...`, `wheelX/wheelY`. The first time a
 * screen opens, `InputOwner` wraps each of these on the Input and TouchInput objects with a gate
 * (an alias, the way plugins alias MZ). While a screen owns the input the gate answers "nothing"
 * (false, 0); otherwise it calls through with the same `this` and arguments and returns exactly what
 * the function below it returns.
 *
 * HOW IT COMPOSES WITH OTHER PLUGINS. The gate is put on top of whatever is there at that moment,
 * once all plugins have loaded and aliased Input, and it is never taken off: closing the last
 * screen only turns it into a pass-through. Taking it off would mean writing the old functions
 * back, which would throw away an alias another plugin put on top of the gate in the meantime (or
 * leave it calling a gate that is gone). So:
 *   - a plugin's alias below the gate (loaded before the first screen opened) runs whenever the
 *     gate passes through, and the screen reads input through it;
 *   - a plugin's alias above the gate (made later) runs as always and gets the gate's answer from
 *     its call to the original;
 *   - only code that bypasses these functions (reading `Input._currentState` directly, or an alias
 *     above the gate that answers without calling its original) is not blocked.
 * A fresh Input object (a game or a test rebuilding it) gets its own gate on the next acquire().
 *
 * THE SCREEN'S READS (`pressed`, `triggered`, `repeated`) call the functions that were below the
 * gate, with the gate opened for the duration (MZ's isTriggered("cancel") asks
 * `this.isTriggered("escape")` itself). A button already held when a screen opens is the game's:
 * the screen ignores it until it is released (the OK that talked to the NPC must not press the
 * sheet's first button). The other way round, a button still held when the last screen closes
 * stays hidden from the game until it is released (the OK that pressed Continue must not talk to
 * the NPC again). `frame()` lets go of released buttons; the host calls it every frame.
 *
 * KEYS IN A TEXT FIELD never reach MZ: `guardHost` stops `keydown`/`keyup` at the shadow host
 * while a field inside it has the focus, so MZ's document listener (`Input._onKeyDown`) neither
 * calls preventDefault on Backspace and the arrows nor turns letters a game mapped (WASD) into
 * directions. A keyup is stopped only when the keydown of that key was (wherever the focus is by
 * then) and nothing made that moot since: the key released outside the host (MZ saw that release),
 * the window losing the focus (the release may never reach the page; MZ clears its own keys then)
 * or the screens' input session ending (the last screen closed, or a new one began). So MZ sees
 * every release of a key it saw go down: an arrow held into a field, the OK still held when the
 * screen opened, or a key held in a field while the sheet was closed by mouse never stays stuck
 * in MZ's Input. An auto-repeat in a field is stopped but never claims its key's release: that
 * key went down outside the field, so MZ saw it go down.
 * Elsewhere on a screen the keys do reach MZ (the screen reads them through Input, as the gamepad),
 * and Enter and Space do not also press the focused button by the browser's default. Mouse,
 * touch and wheel events on the screens are stopped at the host too, so TouchInput never sees a
 * click that pressed a button (nor calls preventDefault on the sheet's scrolling). The one
 * exception is a mouse release (mouseup, pointerup) while MZ's TouchInput holds a press it saw on
 * the canvas before the screen opened under it: MZ gets that release, or it would stay pressed
 * after the screen closed (the player walking toward the pointer). A mouse release goes to the
 * element under the pointer, so a press MZ saw can end over the host. A touch cannot: the browser
 * sends touchend and touchcancel to the element the touch started on, so a touch release at the
 * host is always one of a touch that began there, which MZ never saw (the release of a touch MZ
 * saw goes to the canvas, past the host). Those stay stopped, like every release of a press MZ
 * never saw: MZ would count it as a click once the screen closed, and a menu window under the
 * sheet (the Tip Jar's) would run the command under the pointer.
 */

const INPUT_QUERIES = ["isPressed", "isTriggered", "isRepeated", "isLongPressed"] as const;
const INPUT_GETTERS = ["dir4", "dir8"] as const;
const TOUCH_QUERIES = [
  "isPressed", "isTriggered", "isRepeated", "isLongPressed", "isClicked", "isCancelled", "isMoved", "isHovered", "isReleased",
] as const;
const TOUCH_GETTERS = ["wheelX", "wheelY"] as const;
const DIRECTIONS = ["up", "down", "left", "right"];
/** Input._isEscapeCompatible: these also answer for "escape". */
const ESCAPE_LIKE = ["cancel", "menu"];
/** The button names MZ itself uses (the mappers may add more). */
const BUTTONS = ["tab", "ok", "shift", "control", "escape", "cancel", "menu", "pageup", "pagedown", "up", "down", "left", "right", "debug"];

type Fn = (this: unknown, ...args: unknown[]) => unknown;

interface Originals {
  input: Record<string, Fn>;
  touch: Record<string, Fn>;
  /** Getters of dir4 / dir8 / wheelX / wheelY below the gate. */
  inputGet: Record<string, () => unknown>;
  touchGet: Record<string, () => unknown>;
}

/** The getter of `name` on `obj` or its prototype chain, called on `obj`. */
function getterOf(obj: any, name: string): (() => unknown) | null {
  for (let o = obj; o !== null && o !== undefined; o = Object.getPrototypeOf(o)) {
    const d = Object.getOwnPropertyDescriptor(o, name);
    if (d === undefined) continue;
    if (typeof d.get === "function") return d.get;
    return () => obj[name] === undefined ? undefined : d.value;
  }
  return null;
}

export class InputOwner {
  private _owners = 0;
  private _epoch = 0;
  /** > 0 while the screen reads (the gate passes through for MZ's own nested calls). */
  private _reading = 0;
  /** Buttons held when the last screen closed: hidden from the game until released. */
  private _held = new Set<string>();
  private _touchHeld = false;
  /** Buttons held when the first screen opened: ignored by the screen until released. */
  private _stale = new Set<string>();
  /** Graphics.frameCount when `_held` / `_stale` were taken. */
  private _heldFrame = -1;
  private _staleFrame = -1;
  private _input: any = null;
  private _touch: any = null;
  /** The functions below the gate (the input.test.ts suite compares the gate with them). */
  _originals: Originals = { input: {}, touch: {}, inputGet: {}, touchGet: {} };

  private readonly _g: any;

  constructor(target: any = globalThis) {
    this._g = target;
  }

  /** A screen owns the input now. */
  get active(): boolean {
    return this._owners > 0;
  }

  /** Bumped whenever the screens take the input or give it back (guardHost forgets older key claims). */
  get epoch(): number {
    return this._epoch;
  }

  /** A screen opened: the game sees no input until every screen released it. */
  acquire(): void {
    this._install();
    if (this._owners === 0) {
      this._epoch++;
      this._stale = new Set(this._pressedNow());
      this._staleFrame = this._frameCount();
      this._held.clear();
      this._touchHeld = false;
    }
    this._owners++;
  }

  /** MZ's TouchInput holds a press (its `isPressed()` below the gate): the canvas saw it go down. */
  touchPressed(): boolean {
    return this._touch !== null && this._read(() => this._call(this._originals.touch, "isPressed", this._touch) === true);
  }

  /** A screen closed; the last one gives the input back (held buttons once released). */
  release(): void {
    if (this._owners === 0) return;
    this._owners--;
    if (this._owners > 0) return;
    this._epoch++;
    this._held = new Set(this._pressedNow());
    this._heldFrame = this._frameCount();
    this._touchHeld = this.touchPressed();
    this._stale.clear();
  }

  /** Once a frame: forget the held buttons that were released. */
  frame(): void {
    if (this._input === null) return;
    if (this._g.Input !== this._input) return;
    for (const name of [...this._held]) this._stillHeld(this._held, name);
    for (const name of [...this._stale]) this._stillHeld(this._stale, name);
    if (this._touchHeld && !this.touchPressed()) this._touchHeld = false;
  }

  // --- the screen's reads ------------------------------------------------------------------

  pressed(name: string): boolean {
    return this._own(name) && this._rawPressed(name);
  }

  triggered(name: string): boolean {
    return this._own(name) && this._rawTriggered(name);
  }

  repeated(name: string): boolean {
    return this._own(name) && this._read(() => this._call(this._originals.input, "isRepeated", this._input, name) === true);
  }

  // --- internals ---------------------------------------------------------------------------

  /** The screen may use `name` (not a button the game was holding when the screen opened). */
  private _own(name: string): boolean {
    if (!this.active || this._input === null) return false;
    return !this._stillHeld(this._stale, name);
  }

  /**
   * `name` (or "escape" for cancel / menu) is in `set` and still held since then. A released
   * button leaves the set; so does one pressed again (triggered on a later frame than the one the
   * set was taken on: it went down anew without a frame seeing it up).
   */
  private _stillHeld(set: Set<string>, name: string): boolean {
    const since = set === this._stale ? this._staleFrame : this._heldFrame;
    for (const n of ESCAPE_LIKE.includes(name) ? [name, "escape"] : [name]) {
      if (!set.has(n)) continue;
      if (this._rawPressed(n) && !(this._rawTriggered(n) && this._frameCount() !== since)) return true;
      set.delete(n);
    }
    return false;
  }

  private _frameCount(): number {
    const n = Number(this._g.Graphics?.frameCount);
    return Number.isFinite(n) ? n : -1;
  }

  private _rawTriggered(name: string): boolean {
    return this._read(() => this._call(this._originals.input, "isTriggered", this._input, name) === true);
  }

  private _rawPressed(name: string): boolean {
    return this._read(() => this._call(this._originals.input, "isPressed", this._input, name) === true);
  }

  private _read<T>(fn: () => T): T {
    this._reading++;
    try {
      return fn();
    } finally {
      this._reading--;
    }
  }

  private _call(table: Record<string, Fn>, name: string, self: unknown, ...args: unknown[]): unknown {
    const fn = table[name];
    return typeof fn === "function" ? fn.apply(self, args) : undefined;
  }

  /** Every button name MZ knows that is held now. */
  private _pressedNow(): string[] {
    const I = this._input;
    if (I === null) return [];
    const names = new Set(BUTTONS);
    for (const map of [I.keyMapper, I.gamepadMapper]) {
      if (map && typeof map === "object") for (const v of Object.values(map)) if (typeof v === "string") names.add(v);
    }
    return [...names].filter((n) => this._rawPressed(n));
  }

  /** The game is blocked from `name` (a query's first argument; undefined for TouchInput and the getters). */
  private _blocks(name: unknown): boolean {
    if (this._reading > 0) return false;
    if (this._owners > 0) return true;
    if (typeof name !== "string" || this._held.size === 0) return false;
    return this._stillHeld(this._held, name);
  }

  private _blocksDirection(): boolean {
    if (this._reading > 0) return false;
    if (this._owners > 0) return true;
    // every direction is asked, not just up to the first held one: _stillHeld also forgets the released ones
    let held = false;
    for (const d of DIRECTIONS) if (this._stillHeld(this._held, d)) held = true;
    return held;
  }

  private _blocksTouch(): boolean {
    return this._reading === 0 && (this._owners > 0 || this._touchHeld);
  }

  /** Put the gate on the current Input / TouchInput (once per object). */
  private _install(): void {
    const I = this._g.Input;
    const T = this._g.TouchInput;
    if (I === this._input && T === this._touch) return;
    const originals: Originals = { input: {}, touch: {}, inputGet: {}, touchGet: {} };
    const owner = this;
    if (I && typeof I === "object") {
      for (const name of INPUT_QUERIES) {
        const below = I[name];
        if (typeof below !== "function") continue;
        originals.input[name] = below;
        I[name] = function (this: unknown, ...args: unknown[]) {
          if (owner._blocks(args[0])) return false;
          return below.apply(this, args);
        };
      }
      for (const name of INPUT_GETTERS) this._gateGetter(I, name, originals.inputGet, () => owner._blocksDirection(), 0);
    }
    if (T && typeof T === "object") {
      for (const name of TOUCH_QUERIES) {
        const below = T[name];
        if (typeof below !== "function") continue;
        originals.touch[name] = below;
        T[name] = function (this: unknown, ...args: unknown[]) {
          if (owner._blocksTouch()) return false;
          return below.apply(this, args);
        };
      }
      for (const name of TOUCH_GETTERS) this._gateGetter(T, name, originals.touchGet, () => owner._blocksTouch(), 0);
    }
    this._originals = originals;
    this._input = I ?? null;
    this._touch = T ?? null;
    this._held.clear();
    this._stale.clear();
    this._touchHeld = false;
  }

  private _gateGetter(obj: any, name: string, table: Record<string, () => unknown>, blocked: () => boolean, none: unknown): void {
    const below = getterOf(obj, name);
    if (below === null) return;
    const own = Object.getOwnPropertyDescriptor(obj, name);
    if (own !== undefined && !own.configurable) return; // cannot be wrapped: left to the game
    table[name] = below;
    Object.defineProperty(obj, name, {
      configurable: true,
      enumerable: own?.enumerable ?? false,
      get(this: unknown) {
        return blocked() ? none : below.call(this);
      },
    });
  }
}

/** A focus move on a screen: along the focus order (up / down, Tab's next / previous) or within a row (left / right). */
export type Move = "up" | "down" | "left" | "right" | "next" | "previous";

/** Where the focus goes from control `index` of `count` (-1 when there is none or nothing to move). */
export function nextFocus(index: number, count: number, rowOf: (i: number) => string | null, move: Move): number {
  if (count <= 0 || index < 0 || index >= count) return -1;
  switch (move) {
    case "up":
      return Math.max(0, index - 1);
    case "down":
      return Math.min(count - 1, index + 1);
    case "next":
      return (index + 1) % count;
    case "previous":
      return (index - 1 + count) % count;
    case "left": {
      const row = rowOf(index);
      return row !== null && index > 0 && rowOf(index - 1) === row ? index - 1 : index;
    }
    case "right": {
      const row = rowOf(index);
      return row !== null && index < count - 1 && rowOf(index + 1) === row ? index + 1 : index;
    }
  }
}

/** The element an event started on, inside the shadow root (events are retargeted to the host outside it). */
function origin(e: Event): EventTarget | null {
  const path = typeof e.composedPath === "function" ? e.composedPath() : [];
  return path.length > 0 ? path[0]! : e.target;
}

/** A text field the player types into. */
export function isTextField(t: unknown): t is HTMLInputElement {
  return typeof t === "object" && t !== null && (t as Element).tagName === "INPUT";
}

const POINTER_EVENTS = [
  "mousedown", "mouseup", "mousemove", "click", "dblclick", "contextmenu", "wheel",
  "touchstart", "touchmove", "touchend", "touchcancel", "pointerdown", "pointerup", "pointermove",
];
/**
 * The releases MZ's TouchInput needs to see wherever they happen (it saw the press on the canvas):
 * the mouse's only. A touchend or touchcancel goes to the element the touch started on, so one at
 * a host is of a touch MZ never saw (see the header).
 */
const POINTER_RELEASES = ["mouseup", "pointerup"];
const ENTER = 13;
const SPACE = 32;

/**
 * Keys whose keydown was kept from MZ at `host`: their keyup is kept from MZ too, unless that is
 * moot by then (see the header): a release outside the host, the window's blur, or a new `epoch()`.
 */
class KeyClaims {
  private readonly _codes = new Map<number, number>(); // keyCode -> epoch it was claimed in
  private readonly _epoch: () => number;
  private readonly _detach: () => void;

  constructor(host: HTMLElement, epoch: () => number) {
    this._epoch = epoch;
    const win = host.ownerDocument?.defaultView ?? null;
    const outside = (e: KeyboardEvent) => {
      const path = typeof e.composedPath === "function" ? e.composedPath() : [];
      if (!path.includes(host)) this._codes.delete(e.keyCode); // MZ sees this release
    };
    const blur = () => this._codes.clear();
    win?.addEventListener("keyup", outside, true);
    win?.addEventListener("blur", blur);
    this._detach = () => {
      win?.removeEventListener("keyup", outside, true);
      win?.removeEventListener("blur", blur);
    };
  }

  /** The keydown `e` was kept from MZ; an auto-repeat claims nothing (MZ saw that key go down). */
  claim(e: KeyboardEvent): void {
    if (!e.repeat) this._codes.set(e.keyCode, this._epoch());
  }

  /** The keyup `e`: true when its keydown was claimed in this epoch (keep it from MZ too). */
  release(e: KeyboardEvent): boolean {
    const claimed = this._codes.get(e.keyCode);
    if (claimed === undefined) return false;
    this._codes.delete(e.keyCode);
    return claimed === this._epoch();
  }

  detach(): void {
    this._codes.clear();
    this._detach();
  }
}

/**
 * The DOM half of the ownership (see the header), on the shadow host: while `owner.active`,
 * keys in a text field are stopped there, Enter and Space do not click the focused button, and
 * pointer events do not reach MZ's TouchInput but the release of a press TouchInput holds
 * (`owner.touchPressed()`, see the header). Returns the function that removes the listeners.
 */
export function guardHost(
  host: HTMLElement, owner: { readonly active: boolean; readonly epoch?: number; touchPressed?(): boolean },
): () => void {
  const claims = new KeyClaims(host, () => owner.epoch ?? 0);
  const onKeyDown = (e: KeyboardEvent) => {
    if (!owner.active) return;
    if (isTextField(origin(e))) {
      e.stopPropagation();
      claims.claim(e); // its release is the field's too, wherever the focus is by then
      return;
    }
    // the screen presses the focused button from MZ's "ok", once; not also by the browser's default
    if (e.keyCode === ENTER || e.keyCode === SPACE) e.preventDefault();
  };
  const onKeyUp = (e: KeyboardEvent) => {
    // only the releases of keys whose keydown was kept from MZ: any other release is MZ's, or its
    // key would stay held (an arrow that moved the focus into a field, the OK that opened the screen)
    if (claims.release(e)) {
      e.stopPropagation();
      return;
    }
    if (owner.active && (e.keyCode === ENTER || e.keyCode === SPACE)) e.preventDefault();
  };
  const onPointer = (e: Event) => {
    if (!owner.active) return;
    // MZ saw this press go down on the canvas: it must see it go up, wherever that happens
    if (POINTER_RELEASES.includes(e.type) && typeof owner.touchPressed === "function" && owner.touchPressed()) return;
    e.stopPropagation();
  };
  host.addEventListener("keydown", onKeyDown);
  host.addEventListener("keyup", onKeyUp);
  for (const type of POINTER_EVENTS) host.addEventListener(type, onPointer, { passive: true });
  return () => {
    host.removeEventListener("keydown", onKeyDown);
    host.removeEventListener("keyup", onKeyUp);
    for (const type of POINTER_EVENTS) host.removeEventListener(type, onPointer);
    claims.detach();
  };
}

/**
 * The web checkout overlay's host (the element bridge.js adds over the game): everything typed
 * into it is the overlay's, so its keys are stopped there (MZ would preventDefault Tab and the
 * arrows, and its "ok" would press the payment sheet's focused button under the overlay), with
 * their releases as in `guardHost`; so are its pointer events but the mouse releases (MZ's wheel
 * handler would keep the overlay from scrolling, its touchstart from being tapped; a touch's own
 * release at the overlay is of a touch that began there, see the header). The host is marked
 * with OVERLAY_ATTR (see `overlayHasFocus`): bridge.js tags it with its key, which is kept; a host
 * without it is marked here and unmarked again. Returns the function that removes the listeners.
 */
export function guardOverlay(host: HTMLElement): () => void {
  const marked = typeof host.hasAttribute === "function" && host.hasAttribute(OVERLAY_ATTR);
  if (!marked && typeof host.setAttribute === "function") host.setAttribute(OVERLAY_ATTR, "");
  const claims = new KeyClaims(host, () => 0);
  const onKeyDown = (e: KeyboardEvent) => {
    e.stopPropagation();
    claims.claim(e);
  };
  const onKeyUp = (e: KeyboardEvent) => {
    if (claims.release(e)) e.stopPropagation();
  };
  const onPointer = (e: Event) => e.stopPropagation();
  const pointer = POINTER_EVENTS.filter((t) => !POINTER_RELEASES.includes(t));
  host.addEventListener("keydown", onKeyDown);
  host.addEventListener("keyup", onKeyUp);
  for (const type of pointer) host.addEventListener(type, onPointer, { passive: true });
  return () => {
    host.removeEventListener("keydown", onKeyDown);
    host.removeEventListener("keyup", onKeyUp);
    for (const type of pointer) host.removeEventListener(type, onPointer);
    claims.detach();
    if (!marked && typeof host.removeAttribute === "function") host.removeAttribute(OVERLAY_ATTR);
  };
}

/** Marks a guarded overlay host (guardOverlay), so the sheet can tell the focus is in the overlay. */
export const OVERLAY_ATTR = "data-scfg-overlay";

/**
 * The keyboard focus is in a shown checkout overlay (web_page.ts gives it the focus when it
 * opens): the payment sheet under it must not take the focus back when it rebuilds.
 */
export function overlayHasFocus(doc: Document | null | undefined): boolean {
  const active = doc?.activeElement as HTMLElement | null | undefined;
  return !!active && typeof active.hasAttribute === "function" && active.hasAttribute(OVERLAY_ATTR)
    && active.style?.display !== "none";
}
