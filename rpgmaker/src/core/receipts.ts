/**
 * Receipts: verified on-chain transfers to the developer, stored in `persistent`.
 *
 * There is deliberately no "owned" flag anywhere. Ownership is always recomputed from
 * receipts, and receipts are re-checked against the chain at every launch.
 */
import { storedAmount, unitsToDecimal } from "./amounts.ts";
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
  payer: string | null; // null: a Solana payment with no payer (the schema says str|null)
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
 * (owned item ids, total tipped). Malformed entries are skipped, never fatal: a missing field, an
 * amount that is not a number within the stored-amount range (storedAmount, so the tip total, a
 * default-context Decimal sum as in Python, stays finite), an unhashable item.
 */
export function computeOwnership(store: Record<string, unknown>, items: ReadonlyMap<string, Item>): [Set<string>, Dec] {
  const owned = new Set<string>();
  let tips = Dec.fromInt(0);
  for (const r of Object.values(store)) {
    if (typeof r !== "object" || r === null || Array.isArray(r)) continue;
    const rec = r as Record<string, unknown>;
    if (!hasOwn(rec, "kind") || !hasOwn(rec, "amount")) continue;
    const kind = rec["kind"];
    let amount: Dec;
    try {
      amount = storedAmount(rec["amount"]);
    } catch (e) {
      if (e instanceof ValueError || e instanceof TypeError) continue;
      throw e;
    }
    if (kind === "unlock") {
      const key = rec["item"];
      if (typeof key === "object" && key !== null) continue; // Python: items.get([...]) is a TypeError: skipped
      const item = typeof key === "string" ? items.get(key) : undefined;
      if (item !== undefined && amount.cmp(item.min_price) >= 0) owned.add(item.item_id);
    } else if (kind === "tip") {
      tips = tips.addChecked(amount); // Python: `tips += amount` (bounded amounts: no Overflow)
    }
  }
  return [owned, tips];
}
