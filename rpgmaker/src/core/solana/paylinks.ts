/** Solana Pay transfer-request links (https://docs.solanapay.com/spec). */
import { formatUnits, type Units } from "../amounts.ts";
import { urlQuote } from "../pystr.ts";

/** The amount always carries 6 decimals, so the item code in it survives. */
export function solanaPayUri(args: {
  recipient: string; units: Units; mint: string; reference: string; label: string; message: string;
}): string {
  const params: [string, string][] = [
    ["amount", formatUnits(args.units)], ["spl-token", args.mint], ["reference", args.reference],
    ["label", args.label], ["message", args.message],
  ];
  const query = params.filter(([, v]) => v !== "").map(([k, v]) => `${k}=${urlQuote(v)}`).join("&");
  return `solana:${args.recipient}?${query}`;
}
