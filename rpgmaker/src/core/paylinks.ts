/**
 * Payment links (EIP-681) and QR code geometry.
 *
 * The QR matrix comes from the vendored Project Nayuki generator (vendor/qrcodegen.js).
 * Rendering is left to the engine: `qrRects` merges each row's dark modules into
 * horizontal runs, so the screen draws a few hundred rectangles instead of thousands
 * of single squares.
 */
import type { Units } from "./amounts.ts";
import { ValueError } from "./errors.ts";
import { qrcodegen } from "./vendor/qrcodegen.js";

/** ERC-20 transfer request that mobile wallets understand when scanned or opened. */
export function eip681TransferUri(args: { token_address: string; chain_id: number; recipient: string; units: Units }): string {
  return `ethereum:${args.token_address}@${args.chain_id}/transfer?address=${args.recipient}&uint256=${args.units}`;
}

/** Square matrix of modules, true = dark. Medium error correction. */
export function qrMatrix(text: string): boolean[][] {
  let qr;
  try {
    qr = qrcodegen.QrCode.encodeText(text, qrcodegen.QrCode.Ecc.MEDIUM);
  } catch (e) {
    // Too long for any version (RangeError) or a lone surrogate (URIError): Python's ValueError.
    if (e instanceof RangeError || e instanceof URIError) throw new ValueError(e.message);
    throw e;
  }
  const n = qr.size;
  const out: boolean[][] = [];
  for (let y = 0; y < n; y++) {
    const row: boolean[] = [];
    for (let x = 0; x < n; x++) row.push(qr.getModule(x, y));
    out.push(row);
  }
  return out;
}

/** (size in modules including the quiet-zone border, [[x, y, width], ...] dark runs). */
export function qrRects(text: string, border = 2): [number, [number, number, number][]] {
  const matrix = qrMatrix(text);
  const n = matrix.length;
  const runs: [number, number, number][] = [];
  matrix.forEach((row, y) => {
    let x = 0;
    while (x < n) {
      if (row[x]) {
        const start = x;
        while (x < n && row[x]) x++;
        runs.push([start + border, y + border, x - start]);
      } else {
        x++;
      }
    }
  });
  return [n + 2 * border, runs];
}
