/**
 * Input (rmmz_core.js): keyboard and gamepad, written from MZ's documented behaviour. Every member
 * keeps MZ's name. Keyboard events are read from `document` (`keydown`/`keyup`) and the window's
 * `blur`, the gamepads from `navigator.getGamepads()` once per `update()`.
 *
 * `_shouldPreventDefault` prevents 8 (Backspace), 9 (Tab), 33/34 (Page Up/Down) and 37-40 (arrows),
 * as MZ: Home (36) and End (35) are not prevented (controller ruling, Task 1 fix round 1).
 */
import type { Ctx, GamepadLike } from "./context.ts";

type KeyEventLike = { keyCode: number; preventDefault(): void };

const NUM_LOCK = 144;

/** What Input.clear() resets, with the starting values. */
const startingState = () => ({
  _currentState: {} as Record<string, boolean>, _previousState: {} as Record<string, boolean>, _gamepadStates: [] as boolean[][],
  _latestButton: null as string | null, _pressedTime: 0, _dir4: 0, _dir8: 0, _preferredAxis: "", _date: 0,
  _virtualButton: null as string | null,
});

/** NUMPAD[y + 1][x + 1]: the numpad key of a direction (x, y in -1..1, y down), 0 for none. */
const NUMPAD = [[7, 8, 9], [4, 0, 6], [1, 2, 3]] as const;

export function createInput(ctx: Ctx) {
  const Input = {
    // Input.keyRepeatWait / Input.keyRepeatInterval: frames before the first repeat, then between repeats
    keyRepeatWait: 24,
    keyRepeatInterval: 6,
    // Input.keyMapper: keyCode -> button name
    keyMapper: {
      9: "tab", 13: "ok", 16: "shift", 17: "control", 18: "control", 27: "escape", 32: "ok", 33: "pageup",
      34: "pagedown", 37: "left", 38: "up", 39: "right", 40: "down", 45: "escape", 81: "pageup", 87: "pagedown",
      88: "escape", 90: "ok", 96: "escape", 98: "down", 100: "left", 102: "right", 104: "up", 120: "debug",
    } as Record<number, string>,
    // Input.gamepadMapper: gamepad button index -> button name
    gamepadMapper: {
      0: "ok", 1: "cancel", 2: "shift", 3: "menu", 4: "pageup", 5: "pagedown", 12: "up", 13: "down", 14: "left", 15: "right",
    } as Record<number, string>,

    _currentState: {} as Record<string, boolean>,
    _previousState: {} as Record<string, boolean>,
    _gamepadStates: [] as boolean[][],
    _latestButton: null as string | null,
    _pressedTime: 0,
    _dir4: 0,
    _dir8: 0,
    _preferredAxis: "",
    _date: 0,
    _virtualButton: null as string | null,

    // Input.initialize()
    initialize(): void {
      this.clear();
      this._setupEventHandlers();
    },
    // Input.clear(): every state field back to its starting value (nothing held, no latest button)
    clear(): void {
      Object.assign(this, startingState());
    },
    // Input.update(): once per frame, from SceneManager.updateInputData. Modelled behaviour: the
    // button pressed most recently is "the latest"; its hold counts frames while it stays down
    // and it stops being the latest once released; a button that went down since the last
    // frame becomes the latest with a count of 0 (the last such one, in key order); a touch UI
    // virtual click wins over both.
    update(): void {
      this._pollGamepads();
      const latest = this._latestButton;
      if (latest === null || !this._currentState[latest]) this._latestButton = null;
      else this._pressedTime += 1;
      const wentDown = Object.keys(this._currentState).filter((k) => this._currentState[k] && !this._previousState[k]);
      if (wentDown.length > 0) {
        this._latestButton = wentDown[wentDown.length - 1]!;
        this._pressedTime = 0;
        this._date = Date.now();
      }
      this._previousState = { ...this._previousState, ...this._currentState };
      if (this._virtualButton !== null) {
        [this._latestButton, this._pressedTime, this._virtualButton] = [this._virtualButton, 0, null];
      }
      this._updateDirection();
    },
    // Input.isPressed(keyName): "cancel" and "menu" also answer for "escape"
    isPressed(keyName: string): boolean {
      if (this._isEscapeCompatible(keyName) && this.isPressed("escape")) return true;
      return !!this._currentState[keyName];
    },
    // Input.isTriggered(keyName): pressed on this frame
    isTriggered(keyName: string): boolean {
      if (this._isEscapeCompatible(keyName) && this.isTriggered("escape")) return true;
      return this._latestButton === keyName && this._pressedTime === 0;
    },
    // Input.isRepeated(keyName): triggered, then every keyRepeatInterval frames after keyRepeatWait
    isRepeated(keyName: string): boolean {
      if (this._isEscapeCompatible(keyName) && this.isRepeated("escape")) return true;
      return this._latestButton === keyName && (this._pressedTime === 0
        || (this._pressedTime >= this.keyRepeatWait && this._pressedTime % this.keyRepeatInterval === 0));
    },
    // Input.isLongPressed(keyName): held for keyRepeatWait frames or more
    isLongPressed(keyName: string): boolean {
      if (this._isEscapeCompatible(keyName) && this.isLongPressed("escape")) return true;
      return this._latestButton === keyName && this._pressedTime >= this.keyRepeatWait;
    },
    // Input.dir4: 2/4/6/8 (numpad directions) or 0
    get dir4(): number {
      return this._dir4;
    },
    // Input.dir8: 1-9 or 0
    get dir8(): number {
      return this._dir8;
    },
    // Input.date: when the latest button was pressed (ms)
    get date(): number {
      return this._date;
    },
    // Input.virtualClick(buttonName): the touch UI's buttons
    virtualClick(buttonName: string): void {
      this._virtualButton = buttonName;
    },
    // Input._isEscapeCompatible(keyName)
    _isEscapeCompatible(keyName: string): boolean {
      return keyName === "cancel" || keyName === "menu";
    },
    // Input._setupEventHandlers(): document keydown/keyup, window blur
    _setupEventHandlers(): void {
      ctx.listen(ctx.doc, "keydown", (e) => this._onKeyDown(e));
      ctx.listen(ctx.doc, "keyup", (e) => this._onKeyUp(e));
      ctx.listen(ctx.win, "blur", () => this._onLostFocus());
    },
    // Input._onKeyDown(event). Modelled behaviour: the browser's default is stopped for the keys
    // _shouldPreventDefault names; Num Lock forgets every held key; a mapped key is held.
    _onKeyDown(event: KeyEventLike): void {
      const code = event.keyCode;
      if (this._shouldPreventDefault(code)) event.preventDefault();
      if (code === NUM_LOCK) this.clear();
      this._shimHold(code, true);
    },
    // Input._shouldPreventDefault(keyCode)
    _shouldPreventDefault(keyCode: number): boolean {
      return keyCode === 8 || keyCode === 9 || keyCode === 33 || keyCode === 34 || (keyCode >= 37 && keyCode <= 40);
    },
    // Input._onKeyUp(event): a mapped key is released
    _onKeyUp(event: KeyEventLike): void {
      this._shimHold(event.keyCode, false);
    },
    // (shim) the button a key code maps to, held or not
    _shimHold(keyCode: number, down: boolean): void {
      const name = this.keyMapper[keyCode];
      if (name) this._currentState[name] = down;
    },
    // Input._onLostFocus()
    _onLostFocus(): void {
      this.clear();
    },
    // Input._pollGamepads(): every connected pad from navigator.getGamepads()
    _pollGamepads(): void {
      const pads = typeof ctx.nav.getGamepads === "function" ? ctx.nav.getGamepads() : null;
      for (const pad of Array.from(pads ?? [])) {
        if (pad?.connected) this._updateGamepadState(pad);
      }
    },
    // Input._updateGamepadState(gamepad). Modelled behaviour: buttons 12-15 are the d-pad (up,
    // down, left, right) and the left stick pushed past half way counts as them too; only the
    // slots that changed since this pad's last poll are written into the button state.
    _updateGamepadState(gamepad: GamepadLike): void {
      const before = this._gamepadStates[gamepad.index] ?? [];
      const down: boolean[] = [];
      gamepad.buttons.forEach((b, i) => {
        down[i] = !!b?.pressed;
      });
      const [stickX = 0, stickY = 0] = gamepad.axes;
      const DEAD = 0.5;
      const stick: [number, boolean][] = [[12, stickY < -DEAD], [13, stickY > DEAD], [14, stickX < -DEAD], [15, stickX > DEAD]];
      for (const [slot, pushed] of stick) down[slot] = !!down[slot] || pushed;
      for (let slot = 0; slot < down.length; slot++) {
        if (down[slot] === before[slot]) continue;
        const name = this.gamepadMapper[slot];
        if (name) this._currentState[name] = !!down[slot];
      }
      this._gamepadStates[gamepad.index] = down;
    },
    // Input._updateDirection(). Modelled behaviour: dir8 is the held direction (numpad layout);
    // dir4 drops one axis when both are held, keeping the axis that started being held last.
    _updateDirection(): void {
      const x = this._signX();
      const y = this._signY();
      this._dir8 = this._makeNumpadDirection(x, y);
      const both = x !== 0 && y !== 0;
      if (!both && x !== 0) this._preferredAxis = "y";
      if (!both && y !== 0) this._preferredAxis = "x";
      const keepX = !both || this._preferredAxis === "x";
      this._dir4 = this._makeNumpadDirection(keepX ? x : 0, keepX && both ? 0 : y);
    },
    // Input._signX()
    _signX(): number {
      return Number(this.isPressed("right")) - Number(this.isPressed("left"));
    },
    // Input._signY()
    _signY(): number {
      return Number(this.isPressed("down")) - Number(this.isPressed("up"));
    },
    // Input._makeNumpadDirection(x, y): the numpad key for a direction, 0 for none
    _makeNumpadDirection(x: number, y: number): number {
      return NUMPAD[y + 1]![x + 1]!;
    },
  };
  return Input;
}
