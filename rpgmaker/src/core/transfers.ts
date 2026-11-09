/**
 * A token transfer to the developer, in a form shared by every chain family.
 *
 * Receipts and restore candidates are built from it. EVM fills it from an ERC-20
 * Transfer log; Solana from a transaction's token-balance changes.
 */
import type { Units } from "./amounts.ts";

export interface Transfer {
  token: string; // token contract (EVM) or mint (Solana)
  sender: string | null; // payer; None in Python when a Solana transaction names no payer at all
  recipient: string; // developer wallet (the owner, never a token account)
  value: Units; // base units
  tx_hash: string; // EVM: lowercase 0x hash; Solana: base58 signature, case preserved
  log_index: number; // EVM: log index; Solana: the developer token account's accountIndex
  block_number: number; // EVM: block number; Solana: slot
}
