/**
 * Receipts: verified on-chain transfers to the developer, stored in `persistent`.
 *
 * There is deliberately no "owned" flag anywhere. Ownership is always recomputed from
 * receipts, and receipts are re-checked against the chain at every launch.
 */
import { unitsToDecimal } from "./amounts.ts";
import { Dec } from "./decimal.ts";
import { ValueError } from "./errors.ts";
import type { TransferLog } from "./evm.ts";
import type { Item } from "./models.ts";
import type { Transfer } from "./transfers.ts";
import { hasOwn } from "./pystr.ts";

/** One stored receipt: the frozen JSON record (conformance/schema/state.json). */
export interface ReceiptJson {
  chain: string;
  tx_hash: string;
  log_index: number;
  block: number;
  kind: string;
  item: string | null;
  amount: string;
  token: string;
  payer: string | null; // the schema says str, but a Solana payment with no payer stores None
  recipient: string;
  source: string;
}


export function receiptKey(chain: string, tx_hash: string, log_index: number): string {
  // EVM hashes are hex (case-insensitive); Solana signatures are base58 (case matters).
  const tx = tx_hash.startsWith("0x") ? tx_hash.toLowerCase() : tx_hash;
  return `${chain}:${tx}:${log_index}`;
}

export function makeReceipt(args: {
  chain: string;
  transfer: Transfer | TransferLog;
  kind: string;
  item: string | null;
  token: string;
  source: string;
}): ReceiptJson {
  const { chain, transfer, kind, item, token, source } = args;
  if (kind !== "unlock" && kind !== "tip") throw new ValueError(`invalid kind ${JSON.stringify(kind)}`);
  if (source !== "payment" && source !== "restore") throw new ValueError(`invalid source ${JSON.stringify(source)}`);
  return {
    chain, tx_hash: transfer.tx_hash, log_index: transfer.log_index, block: transfer.block_number,
    kind, item, amount: String(unitsToDecimal(transfer.value)), token,
    payer: transfer.sender, recipient: transfer.recipient, source,
  };
}

/** Store the receipt unless its key is already there. Returns whether it was added. */
export function addReceipt(store: Record<string, ReceiptJson>, receipt: ReceiptJson): boolean {
  const key = receiptKey(receipt.chain, receipt.tx_hash, receipt.log_index);
  if (hasOwn(store, key)) return false;
  store[key] = receipt;
  return true;
}

export function removeReceipt(store: Record<string, ReceiptJson>, key: string): void {
  delete store[key];
}

/**
 * Python `Decimal(x)` for a stored amount, or null where Python raises TypeError/InvalidOperation.
 * A string takes any exponent libmpdec accepts (the amount is only compared and summed).
 */
function storedAmount(x: unknown): Dec | null {
  if (typeof x === "string") {
    try {
      const d = Dec.parse(x, { anyExponent: true });
      return d instanceof Dec ? d : null; // NaN and Infinity: not finite, skipped
    } catch (e) {
      if (e instanceof ValueError) return null;
      throw e;
    }
  }
  if (typeof x === "boolean") return Dec.fromInt(x ? 1 : 0); // Decimal(True) == 1
  if (typeof x === "bigint") return Dec.fromInt(x);
  if (typeof x === "number") return Number.isFinite(x) ? Dec.fromNumber(x) : null;
  // Python: Decimal([...]) is a ValueError that compute_ownership does not catch; None and dict are TypeErrors.
  if (Array.isArray(x)) throw new ValueError("argument must be a tuple of length 3");
  return null;
}

/**
 * (owned item ids, total tipped). Malformed entries are skipped, never fatal. As in Python, the tip
 * total is a default-context Decimal sum, so a stored tip beyond Emax (999999) is an Overflow.
 */
export function computeOwnership(store: Record<string, unknown>, items: ReadonlyMap<string, Item>): [Set<string>, Dec] {
  const owned = new Set<string>();
  let tips = Dec.fromInt(0);
  for (const r of Object.values(store)) {
    if (typeof r !== "object" || r === null || Array.isArray(r)) continue;
    const rec = r as Record<string, unknown>;
    if (!hasOwn(rec, "kind") || !hasOwn(rec, "amount")) continue;
    const kind = rec["kind"];
    const amount = storedAmount(rec["amount"]);
    if (amount === null) continue;
    if (kind === "unlock") {
      const key = rec["item"];
      if (typeof key === "object" && key !== null) { // Python: items.get([...]) is not caught
        throw new TypeError(`unhashable type: '${Array.isArray(key) ? "list" : "dict"}'`);
      }
      const item = typeof key === "string" ? items.get(key) : undefined;
      if (item !== undefined && amount.cmp(item.min_price) >= 0) owned.add(item.item_id);
    } else if (kind === "tip") {
      tips = tips.addChecked(amount); // Python: `tips += amount`, decimal.Overflow not caught
    }
  }
  return [owned, tips];
}
