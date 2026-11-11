/**
 * The sheet's host: one element on `document.body`, after MZ's canvas and above everything MZ
 * and the usual plugins put on the page, holding a shadow root with the Margin stylesheet
 * (ui/margin.css) and an empty `.scfg-root` the sheet builds into. The shadow root keeps the
 * game's and the plugins' CSS out of the sheet and the sheet's out of the game.
 *
 * Its mode is "open": the shadow DOM is here for style isolation, not secrecy (the sheet shows
 * nothing the player does not already see), and an open root lets the end-to-end tests (Playwright
 * selectors) and the browser's devtools reach the sheet. `hostRoot()` / `hostElement()` are the
 * hooks for the unit tests.
 *
 * Size and position follow the game canvas the way MZ places `#gameCanvas` (Graphics._updateCanvas
 * stretches it to the window and centres it): the host copies the canvas' box on the page
 * (`getBoundingClientRect`) and recomputes on window resize and canvas resize. `--scfg-k` is the
 * scale of the sheet's design sizes (Ren'Py's _k(), min(height / 720, width / 960) of that box)
 * and `--scfg-sheet-w` its docked width. `setAccent` sets the accent and the text on it.
 */
import { DEFAULT_ACCENT, textOn } from "../../core/index.ts";
import { assets } from "../assets.ts";

/** Above MZ's own elements (its error printer is at 9999) and below the test-play notice. */
export const HOST_Z_INDEX = 100000;
export const HOST_ID = "scfg-host";
export const STYLESHEET = "margin.css";

/** The sheet's scale and docked width for a box of `width` x `height` (Ren'Py's `_k()` and `_sheet_width()`). */
export function layoutFor(width: number, height: number): { k: number; sheetWidth: number } {
  const k = width <= 0 || height <= 0 ? 1 : Math.min(height / 720, width / 960);
  const docked = Math.round(520 * k);
  return { k, sheetWidth: width < height || docked * 1.6 > width ? width : docked };
}

/** The Margin stylesheet embedded in the plugin (an Error if the build did not carry it). */
export function sheetCss(): string {
  const css = assets().css[STYLESHEET];
  if (typeof css !== "string") throw new Error(`scfg: stylesheet ${STYLESHEET} is missing from the assets`);
  return css;
}

interface Mounted {
  host: HTMLElement;
  root: ShadowRoot;
  teardown: Array<() => void>;
  last: string;
}

let mounted: Mounted | null = null;
let accent = DEFAULT_ACCENT;

const mz = (): any => globalThis as any;
const HEX = /^#[0-9a-fA-F]{6}$/;

/** The rectangle of the game on the page: the canvas' box, else the size MZ reports at the page's corner. */
function gameBox(): { left: number; top: number; width: number; height: number } {
  const g = mz().Graphics;
  const doc = mz().document as Document;
  const canvas: HTMLElement | null = g?._canvas ?? doc.getElementById("gameCanvas");
  if (canvas && typeof canvas.getBoundingClientRect === "function") {
    const r = canvas.getBoundingClientRect();
    if (r.width > 0 && r.height > 0) return { left: r.left, top: r.top, width: r.width, height: r.height };
  }
  return { left: 0, top: 0, width: Number(g?.width) || 816, height: Number(g?.height) || 624 };
}

/** Put the host over the game canvas again (after a resize; it also runs by itself on window and canvas resizes). */
export function layoutHost(): void {
  if (mounted === null) return;
  const box = gameBox();
  const { k, sheetWidth } = layoutFor(box.width, box.height);
  const key = [box.left, box.top, box.width, box.height].map((n) => n.toFixed(2)).join(",");
  if (key === mounted.last) return;
  mounted.last = key;
  const s = mounted.host.style;
  s.left = `${box.left}px`;
  s.top = `${box.top}px`;
  s.width = `${box.width}px`;
  s.height = `${box.height}px`;
  s.setProperty("--scfg-k", String(k));
  s.setProperty("--scfg-sheet-w", `${sheetWidth}px`);
}

function applyAccent(): void {
  if (mounted === null) return;
  mounted.host.style.setProperty("--scfg-accent", accent);
  mounted.host.style.setProperty("--scfg-on-accent", textOn(accent));
}

/**
 * The developer's accent and the text colour on it (`textOn`). The sheet host passes the
 * configured `accent`, which the core already normalised to lowercase `#rrggbb` (config.ts
 * `accentArg`: `#abc` expanded, an alpha `#rrggbbaa` dropped, none = DEFAULT_ACCENT); anything
 * else (not a `#rrggbb` string) keeps the default here.
 */
export function setAccent(color: string | null | undefined): void {
  accent = typeof color === "string" && HEX.test(color) ? color.toLowerCase() : DEFAULT_ACCENT;
  applyAccent();
}

/** The sheet's shadow root, created on first use; later calls return the same one. */
export function mountHost(): ShadowRoot {
  if (mounted !== null) return mounted.root;
  const win = mz();
  const doc = win.document as Document;
  const host = doc.createElement("div");
  host.id = HOST_ID;
  host.style.cssText = `position:fixed;left:0;top:0;width:0;height:0;z-index:${HOST_Z_INDEX};overflow:hidden;`
    + "margin:0;padding:0;border:0;pointer-events:none;";
  const root = host.attachShadow({ mode: "open" });
  const style = doc.createElement("style");
  style.textContent = sheetCss();
  const base = doc.createElement("div");
  base.className = "scfg-root";
  root.append(style, base);
  doc.body.appendChild(host);
  const teardown: Array<() => void> = [];
  mounted = { host, root, teardown, last: "" };
  applyAccent();
  layoutHost();
  if (typeof win.addEventListener === "function") {
    win.addEventListener("resize", layoutHost);
    teardown.push(() => win.removeEventListener("resize", layoutHost));
  }
  const canvas: HTMLElement | null = win.Graphics?._canvas ?? doc.getElementById("gameCanvas");
  if (canvas && typeof win.ResizeObserver === "function") {
    const ro = new win.ResizeObserver(layoutHost);
    ro.observe(canvas);
    teardown.push(() => ro.disconnect());
  }
  return root;
}

/** The `.scfg-root` element inside the shadow root (what the sheet builds into), or null when not mounted. */
export function hostRoot(): HTMLElement | null {
  return (mounted?.root.querySelector(".scfg-root") as HTMLElement | null) ?? null;
}

/** The host element on the page, or null when not mounted (test hook). */
export function hostElement(): HTMLElement | null {
  return mounted?.host ?? null;
}

/** Remove the host and its listeners. */
export function unmountHost(): void {
  if (mounted === null) return;
  for (const fn of mounted.teardown) fn();
  mounted.host.remove();
  mounted = null;
}
