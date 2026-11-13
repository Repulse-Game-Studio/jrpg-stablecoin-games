/**
 * TouchInput (rmmz_core.js): mouse and touch, written from MZ's documented behaviour. Events
 * received between frames are collected in `_newState` and become visible on the next `update()`.
 * Positions go through `Graphics.pageToCanvasX/Y` and only count inside the canvas.
 *
 * Simplified: no double-touch, no `pointerType` handling, no `cordova` special cases; touch
 * positions use the first changed touch.
 */
import type { Ctx } from "./context.ts";

type TouchState = {
  triggered: boolean; cancelled: boolean; moved: boolean; hovered: boolean; released: boolean;
  wheelX: number; wheelY: number;
};

export function createTouchInput(ctx: Ctx) {
  const g = ctx.g;
  /** A page position (mouse event or touch) on the canvas, and whether it is inside it. */
  const onCanvas = (p: { pageX: number; pageY: number }): [number, number, boolean] => {
    const x = g.Graphics.pageToCanvasX(p.pageX);
    const y = g.Graphics.pageToCanvasY(p.pageY);
    return [x, y, g.Graphics.isInsideCanvas(x, y)];
  };
  /** The fields clear() resets, with their starting values. */
  const starting = () => ({
    _mousePressed: false, _screenPressed: false, _pressedTime: 0, _clicked: false, _newState: fresh(), _currentState: fresh(),
    _x: 0, _y: 0, _triggerX: 0, _triggerY: 0, _moved: false, _date: 0,
  });
  const fresh = (): TouchState => ({
    triggered: false, cancelled: false, moved: false, hovered: false, released: false, wheelX: 0, wheelY: 0,
  });
  const TouchInput = {
    // TouchInput.keyRepeatWait / keyRepeatInterval / moveThreshold
    keyRepeatWait: 24,
    keyRepeatInterval: 6,
    moveThreshold: 10,

    _mousePressed: false,
    _screenPressed: false,
    _pressedTime: 0,
    _clicked: false,
    _newState: fresh(),
    _currentState: fresh(),
    _x: 0,
    _y: 0,
    _triggerX: 0,
    _triggerY: 0,
    _moved: false,
    _date: 0,

    // TouchInput.initialize()
    initialize(): void {
      this.clear();
      this._setupEventHandlers();
    },
    // TouchInput.clear(): every field back to its starting value (STARTING)
    clear(): void {
      Object.assign(this, starting());
    },
    // TouchInput.update(), once per frame. Modelled behaviour: what the events collected since the
    // last frame becomes this frame's state, and collecting starts afresh; a release that did not
    // move is a click; a held press counts frames.
    update(): void {
      [this._currentState, this._newState] = [this._newState, fresh()];
      this._clicked = this._currentState.released && !this._moved;
      this._pressedTime += this.isPressed() ? 1 : 0;
    },
    // TouchInput.isClicked(): released without moving
    isClicked(): boolean {
      return this._clicked;
    },
    // TouchInput.isPressed()
    isPressed(): boolean {
      return this._mousePressed || this._screenPressed;
    },
    // TouchInput.isTriggered()
    isTriggered(): boolean {
      return this._currentState.triggered;
    },
    // TouchInput.isRepeated()
    isRepeated(): boolean {
      return this.isPressed() && (this._currentState.triggered
        || (this._pressedTime >= this.keyRepeatWait && this._pressedTime % this.keyRepeatInterval === 0));
    },
    // TouchInput.isLongPressed()
    isLongPressed(): boolean {
      return this.isPressed() && this._pressedTime >= this.keyRepeatWait;
    },
    // TouchInput.isCancelled(): right click or two-finger tap
    isCancelled(): boolean {
      return this._currentState.cancelled;
    },
    // TouchInput.isMoved()
    isMoved(): boolean {
      return this._currentState.moved;
    },
    // TouchInput.isHovered()
    isHovered(): boolean {
      return this._currentState.hovered;
    },
    // TouchInput.isReleased()
    isReleased(): boolean {
      return this._currentState.released;
    },
    // TouchInput.wheelX / wheelY / x / y / date
    get wheelX(): number {
      return this._currentState.wheelX;
    },
    get wheelY(): number {
      return this._currentState.wheelY;
    },
    get x(): number {
      return this._x;
    },
    get y(): number {
      return this._y;
    },
    get date(): number {
      return this._date;
    },
    // TouchInput._setupEventHandlers(): mouse, wheel and touch on document, blur on window
    _setupEventHandlers(): void {
      const passive = { passive: false };
      ctx.listen(ctx.doc, "mousedown", (e) => this._onMouseDown(e));
      ctx.listen(ctx.doc, "mousemove", (e) => this._onMouseMove(e));
      ctx.listen(ctx.doc, "mouseup", (e) => this._onMouseUp(e));
      ctx.listen(ctx.doc, "wheel", (e) => this._onWheel(e), passive);
      ctx.listen(ctx.doc, "touchstart", (e) => this._onTouchStart(e), passive);
      ctx.listen(ctx.doc, "touchmove", (e) => this._onTouchMove(e), passive);
      ctx.listen(ctx.doc, "touchend", (e) => this._onTouchEnd(e));
      ctx.listen(ctx.doc, "touchcancel", () => this._onTouchCancel());
      ctx.listen(ctx.win, "blur", () => this._onLostFocus());
    },
    // TouchInput._onMouseDown(event): button 0 left, 1 middle, 2 right
    _onMouseDown(event: MouseEvent): void {
      const byButton = [this._onLeftButtonDown, this._onMiddleButtonDown, this._onRightButtonDown];
      byButton[event.button]?.call(this, event);
    },
    // TouchInput._onLeftButtonDown(event): a press inside the canvas triggers
    _onLeftButtonDown(event: MouseEvent): void {
      const [x, y, inside] = onCanvas(event);
      if (!inside) return;
      this._mousePressed = true;
      this._pressedTime = 0;
      this._onTrigger(x, y);
    },
    // TouchInput._onMiddleButtonDown(event): nothing
    _onMiddleButtonDown(_event: MouseEvent): void {},
    // TouchInput._onRightButtonDown(event): a right click inside the canvas cancels
    _onRightButtonDown(event: MouseEvent): void {
      const [x, y, inside] = onCanvas(event);
      if (inside) this._onCancel(x, y);
    },
    // TouchInput._onMouseMove(event): a drag while pressed, else a hover inside the canvas
    _onMouseMove(event: MouseEvent): void {
      const [x, y, inside] = onCanvas(event);
      if (this._mousePressed) this._onMove(x, y);
      else if (inside) this._onHover(x, y);
    },
    // TouchInput._onMouseUp(event): the left button's release (anywhere)
    _onMouseUp(event: MouseEvent): void {
      if (event.button !== 0) return;
      const [x, y] = onCanvas(event);
      this._mousePressed = false;
      this._onRelease(x, y);
    },
    // TouchInput._onWheel(event): summed until the next update(); the page does not scroll
    _onWheel(event: WheelEvent): void {
      this._newState.wheelX += event.deltaX;
      this._newState.wheelY += event.deltaY;
      event.preventDefault();
    },
    // TouchInput._onTouchStart(event): a touch inside the canvas triggers; with a second finger down it cancels
    _onTouchStart(event: TouchEvent): void {
      const touch = event.changedTouches[0];
      if (!touch) return;
      const [x, y, inside] = onCanvas(touch);
      if (!inside) return;
      this._screenPressed = true;
      this._pressedTime = 0;
      (event.touches.length > 1 ? this._onCancel : this._onTrigger).call(this, x, y);
      event.preventDefault();
    },
    // TouchInput._onTouchMove(event)
    _onTouchMove(event: TouchEvent): void {
      const touch = event.changedTouches[0];
      if (touch) this._onMove(...(onCanvas(touch).slice(0, 2) as [number, number]));
    },
    // TouchInput._onTouchEnd(event)
    _onTouchEnd(event: TouchEvent): void {
      const touch = event.changedTouches[0];
      if (!touch) return;
      this._screenPressed = false;
      this._onRelease(...(onCanvas(touch).slice(0, 2) as [number, number]));
    },
    // TouchInput._onTouchCancel()
    _onTouchCancel(): void {
      this._screenPressed = false;
    },
    // TouchInput._onLostFocus()
    _onLostFocus(): void {
      this.clear();
    },
    // TouchInput._onTrigger(x, y)
    // modelled: a trigger is where a later move is measured from, and has not moved yet
    _onTrigger(x: number, y: number): void {
      this._newState.triggered = true;
      Object.assign(this, { _x: x, _y: y, _triggerX: x, _triggerY: y, _moved: false, _date: Date.now() });
    },
    // TouchInput._onCancel(x, y)
    _onCancel(x: number, y: number): void {
      this._newState.cancelled = true;
      this._x = x;
      this._y = y;
    },
    // TouchInput._onMove(x, y): once the point has gone further than moveThreshold pixels (on
    // either axis) from where it was triggered, every move is reported
    _onMove(x: number, y: number): void {
      const far = Math.max(Math.abs(x - this._triggerX), Math.abs(y - this._triggerY)) > this.moveThreshold;
      this._moved = this._moved || far;
      if (!this._moved) return;
      this._newState.moved = true;
      [this._x, this._y] = [x, y];
    },
    // TouchInput._onHover(x, y)
    _onHover(x: number, y: number): void {
      this._newState.hovered = true;
      this._x = x;
      this._y = y;
    },
    // TouchInput._onRelease(x, y)
    _onRelease(x: number, y: number): void {
      this._newState.released = true;
      this._x = x;
      this._y = y;
    },
  };
  return TouchInput;
}
