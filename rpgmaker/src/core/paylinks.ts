/**
 * Payment links (EIP-681), the buy page link and QR code geometry.
 *
 * The QR matrix comes from the vendored Project Nayuki generator (vendor/qrcodegen.js).
 * Rendering is left to the engine: `qrRects` merges each row's dark modules into
 * horizontal runs, so the screen draws a few hundred rectangles instead of thousands
 * of single squares.
 */
import type { Units } from "./amounts.ts";
import { KeyError, ValueError } from "./errors.ts";
import { qrcodegen } from "./vendor/qrcodegen.js";

// The buy page offered to players who have no stablecoin yet (configure({onramp})): a template
// whose {placeholders} are filled in for the network the player picked. The default is thirdweb's
// hosted Buy page, which needs no account or key; the player connects their own wallet there and
// buys with a card, then pays as usual. tokenEditable=false keeps the page on the network and token,
// paymentMethods=card offers card payment only; tokenAddress is not in thirdweb's iframe docs but the
// hosted page honours it (paylinks.py).
export const ONRAMP_URL = "https://thirdweb.com/bridge/buy-widget?chain={chain_id}"
  + "&tokenAddress={token_address}&amount={amount}&tokenEditable=false&paymentMethods=card";
export const ONRAMP_FIELDS: readonly string[] = ["amount", "chain", "chain_id", "token", "token_address"];
/** Python's `re.compile(r"\{([^{}]*)\}")`, global (for matchAll and replace). */
export const ONRAMP_PLACEHOLDER = /\{([^{}]*)\}/g;
// On a chain whose gas coin is the token (Arc), the payment's fee is paid in it too.
const ONRAMP_GAS_UNITS = 10_000n; // 0.01
// Before a session exists: the most the item code and nonce add to the price (0.009999).
export const ONRAMP_EXACT_UNITS = 9_999n;

/** ERC-20 transfer request that mobile wallets understand when scanned or opened. */
export function eip681TransferUri(args: { token_address: string; chain_id: number; recipient: string; units: Units }): string {
  return `ethereum:${args.token_address}@${args.chain_id}/transfer?address=${args.recipient}&uint256=${args.units}`;
}

/**
 * The amount suggested on the buy page: the payment (6-decimal units), plus a cent for the fee
 * where the token is the gas coin, rounded up to whole cents: 4993742 -> "5.00".
 */
export function onrampAmount(units: Units, gasIsToken: boolean): string {
  const total = units + (gasIsToken ? ONRAMP_GAS_UNITS : 0n);
  const q = total / 10_000n; // truncates: one more cent for a remainder (units are never negative)
  const cents = total > q * 10_000n ? q + 1n : q;
  return `${cents / 100n}.${String(cents % 100n).padStart(2, "0")}`;
}

/**
 * Fill a validated onramp template (config.ts's onrampUrlArg). Every value is made of URL-safe
 * characters (a chain key, digits, a 0x address, a token symbol, an amount), so none is escaped.
 */
export function onrampUrl(template: string, args: {
  chain: string; chainId: number; token: string; tokenAddress: string; amount: string;
}): string {
  const values = new Map([
    ["amount", args.amount], ["chain", args.chain], ["chain_id", String(args.chainId)], ["token", args.token],
    ["token_address", args.tokenAddress],
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
