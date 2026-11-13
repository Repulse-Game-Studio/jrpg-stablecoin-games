/**
 * The payment QR code as SVG markup: the core's `qrRects` (dark runs, quiet zone of 2 modules)
 * drawn black on white with crisp edges. The markup holds only fixed text and integers, so the
 * encoded text (a payment link, partly from the chain config) can never reach it as markup.
 */
import { qrRects } from "../../core/index.ts";

/** Quiet zone, in modules (the QR standard asks for 4; 2 scans fine on a light sheet). */
export const QUIET_ZONE = 2;

const int = (n: number): string => {
  if (!Number.isSafeInteger(n) || n < 0) throw new RangeError(`scfg: bad QR coordinate ${n}`);
  return String(n);
};

/** `<svg>` markup of the QR code for `text`. A text that does not fit a QR code throws (a ValueError of the core). */
export function qrSvg(text: string): string {
  const [size, runs] = qrRects(text, QUIET_ZONE);
  const parts = [
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${int(size)} ${int(size)}" shape-rendering="crispEdges" role="img" aria-label="QR code">`,
    `<rect x="0" y="0" width="${int(size)}" height="${int(size)}" fill="#ffffff"/>`,
  ];
  for (const [x, y, w] of runs) parts.push(`<rect x="${int(x)}" y="${int(y)}" width="${int(w)}" height="1" fill="#000000"/>`);
  parts.push("</svg>");
  return parts.join("");
}
