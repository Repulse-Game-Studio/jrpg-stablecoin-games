/**
 * Payment links (EIP-681), the Pay with card page link and QR code geometry.
 *
 * The QR matrix comes from the vendored Project Nayuki generator (vendor/qrcodegen.js).
 * Rendering is left to the engine: `qrRects` merges each row's dark modules into
 * horizontal runs, so the screen draws a few hundred rectangles instead of thousands
 * of single squares.
 */
import type { Units } from "./amounts.ts";
import { KeyError, ValueError } from "./errors.ts";
import { qrcodegen } from "./vendor/qrcodegen.js";

// The "Pay with card" page (configure({onramp})): a template whose {placeholders} are filled in
// for the network the player picked and the payment's session. The default is thirdweb's hosted
// Checkout page, which needs no account or key: the player pays by card and thirdweb sends the
// token to `seller`, the developer's wallet, on that network. {amount} is the session's exact
// amount with all 6 decimals, so the game finds the payment as it finds any other (amounts.ts's
// encode). paymentMethods=card offers card payment only (thirdweb's iframe docs).
// NOT YET CHECKED with a real card: that thirdweb delivers the amount exactly, to 6 decimals.
export const ONRAMP_URL = "https://thirdweb.com/bridge/checkout-widget?chain={chain_id}"
  + "&tokenAddress={token_address}&amount={amount}&seller={recipient}&paymentMethods=card";
export const ONRAMP_FIELDS: readonly string[] = ["amount", "chain", "chain_id", "recipient", "token", "token_address"];
/** Python's `re.compile(r"\{([^{}]*)\}")`, global (for matchAll and replace). */
export const ONRAMP_PLACEHOLDER = /\{([^{}]*)\}/g;

/** ERC-20 transfer request that mobile wallets understand when scanned or opened. */
export function eip681TransferUri(args: { token_address: string; chain_id: number; recipient: string; units: Units }): string {
  return `ethereum:${args.token_address}@${args.chain_id}/transfer?address=${args.recipient}&uint256=${args.units}`;
}

/**
 * Fill a validated onramp template (config.ts's onrampUrlArg). Every value is made of URL-safe
 * characters (a chain key, digits, a 0x address, a token symbol, an amount), so none is escaped.
 */
export function onrampUrl(template: string, args: {
  chain: string; chainId: number; token: string; tokenAddress: string; amount: string; recipient: string;
}): string {
  const values = new Map([
    ["amount", args.amount], ["chain", args.chain], ["chain_id", String(args.chainId)], ["token", args.token],
    ["token_address", args.tokenAddress], ["recipient", args.recipient],
  ]);
  return template.replace(ONRAMP_PLACEHOLDER, (_m, name: string) => {
    const v = values.get(name);
    if (v === undefined) throw new KeyError(name); // an unvalidated template, as Python's values[name]
    return v;
  });
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
