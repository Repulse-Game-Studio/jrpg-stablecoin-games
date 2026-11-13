/** Value objects shared with the developer's script. */
import type { Dec } from "./decimal.ts";

/** A paid unlock declared with scfg.add_item(). */
export interface Item {
  item_id: string;
  code: number;
  price: Dec;
  token: string;
  title: string;
  min_price: Dec;
}

/**
 * What pay(), callbacks and on_result receive.
 *
 * status: "paid" | "already_owned" | "cancelled" | "expired" | "underpaid" | "failed" | "pending"
 */
export interface PaymentResult {
  status: string;
  kind: string;
  item: string | null;
  amount: Dec | null;
  token: string | null;
  chain: string | null;
  tx_hash: string | null;
  payer: string | null;
  session_id: string | null;
  error: string | null;
}

/** Python's dataclass defaults: everything but `status` and `kind` is None. */
export function makePaymentResult(partial: Partial<PaymentResult> & Pick<PaymentResult, "status" | "kind">): PaymentResult {
  return {
    item: null, amount: null, token: null, chain: null, tx_hash: null, payer: null, session_id: null, error: null,
    ...partial,
  };
}

/** What restore() returns. status: "restored" | "nothing_found" | "cancelled" | "failed". */
export interface RestoreResult {
  status: string;
  restored: Record<string, unknown>[];
  error: string | null;
}

export function makeRestoreResult(partial: Partial<RestoreResult> & Pick<RestoreResult, "status">): RestoreResult {
  return { restored: [], error: null, ...partial };
}
