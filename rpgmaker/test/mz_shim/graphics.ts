/**
 * Graphics (rmmz_core.js): the screen size, the game canvas (`#gameCanvas`), the frame counter and
 * page-to-canvas coordinates. No rendering.
 *
 * In a page the canvas is the existing `#gameCanvas` or a new one centred on the page, as MZ
 * creates it; its scale is 1 (MZ stretches the canvas to the window; unverified details). In Node
 * there is no canvas and `pageToCanvasX/Y` return page coordinates unchanged (MZ returns 0).
 *
 * The game loop: `stopGameLoop()` (SceneManager.stop, after an error) stops it for good here;
 * `printError()` only records what MZ's error screen would show (no screen is drawn).
 */
import type { Ctx } from "./context.ts";

export function createGraphics(ctx: Ctx) {
  const g = ctx.g;
  return {
    _width: ctx.opts.width ?? 816,
    _height: ctx.opts.height ?? 624,
    _realScale: 1,
    _canvas: null as HTMLCanvasElement | null,
    // Graphics.frameCount: SceneManager.updateMain counts it up
    frameCount: 0,
    /** (shim) the game loop was stopped (stopGameLoop): no more frames run. */
    _shimStopped: false,
    /** (shim) what printError was asked to show, or null. */
    _shimPrinted: null as { name: string; message: string } | null,

    // Graphics.initialize()
    initialize(): void {
      if (!ctx.dom) return;
      const doc = g.document as Document;
      let canvas = doc.getElementById("gameCanvas") as HTMLCanvasElement | null;
      if (canvas === null) {
        canvas = doc.createElement("canvas");
        canvas.id = "gameCanvas";
        canvas.style.position = "absolute";
        canvas.style.margin = "auto";
        canvas.style.inset = "0";
        doc.body.appendChild(canvas);
      }
      canvas.width = this._width;
      canvas.height = this._height;
      this._canvas = canvas;
    },
    // Graphics.startGameLoop() / stopGameLoop(). Modelled behaviour: once stopped (SceneManager.stop
    // after an error), the loop calls SceneManager.update no more; the shim never restarts it.
    startGameLoop(): void {},
    stopGameLoop(): void {
      this._shimStopped = true;
    },
    // Graphics.printError(name, message, error): MZ draws its error screen over the game; recorded here
    printError(name: string, message: string, _error?: unknown): void {
      this._shimPrinted = { name: String(name), message: String(message) };
    },
    // Graphics.width / Graphics.height
    get width(): number {
      return this._width;
    },
    set width(value: number) {
      this._width = value;
    },
    get height(): number {
      return this._height;
    },
    set height(value: number) {
      this._height = value;
    },
    // Graphics.pageToCanvasX(x) / pageToCanvasY(y)
    pageToCanvasX(x: number): number {
      if (this._canvas === null) return x;
      return Math.round((x - this._canvas.offsetLeft) / this._realScale);
    },
    pageToCanvasY(y: number): number {
      if (this._canvas === null) return y;
      return Math.round((y - this._canvas.offsetTop) / this._realScale);
    },
    // Graphics.isInsideCanvas(x, y)
    isInsideCanvas(x: number, y: number): boolean {
      return x >= 0 && x < this._width && y >= 0 && y < this._height;
    },
  };
}
