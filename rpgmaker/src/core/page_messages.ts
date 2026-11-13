/**
 * What the checkout page may tell the game, checked the same way wherever the page runs.
 * A tx hash is only a hint: the watcher verifies it on-chain before anything is unlocked.
 */
import { normalizeAnyTx } from "./backends/index.ts";
import { ValueError } from "./errors.ts";
import { toChecksumAddress } from "./evm.ts";
import { normalizePubkey } from "./solana/base58.ts";
import { hasOwn } from "./pystr.ts";

export const BUILD_TIMEOUT_S = 15; // how long a page waits for the game to build a Solana transaction

function asObject(data: unknown): Record<string, unknown> {
  if (typeof data !== "object" || data === null || Array.isArray(data)) throw new ValueError("not an object");
  return data as Record<string, unknown>;
}

const own = (o: Record<string, unknown>, key: string): unknown =>
  hasOwn(o, key) ? o[key] : undefined;

/**
 * {"tx_hash", "from"?} -> (normalized tx hash or signature, normalized sender or null).
 * Raises ValueError for a missing or malformed hash; a malformed sender is dropped.
 */
export function parseTxMessage(data: unknown): [string, string | null] {
  const obj = asObject(data);
  const raw = own(obj, "tx_hash");
  if (raw === undefined) throw new ValueError("bad tx hash");
  const txHash = normalizeAnyTx(raw);
  const from = own(obj, "from");
  // Python truthiness: only a non-empty string can be a sender; any other truthy value fails
  // normalization (str(x) never starts with "0x" and normalize_pubkey rejects non-strings).
  let sender: string | null = null;
  if (typeof from === "string" && from !== "") {
    try {
      sender = from.startsWith("0x") ? toChecksumAddress(from) : normalizePubkey(from);
    } catch (e) {
      if (!(e instanceof ValueError || e instanceof TypeError)) throw e;
    }
  }
  return [txHash, sender];
}

/** {"account"} -> the player's Solana account, normalized. Raises ValueError. */
export function parseBuildMessage(data: unknown): string {
  const obj = asObject(data);
  const raw = own(obj, "account");
  if (raw === undefined) throw new ValueError("bad account");
  return normalizePubkey(raw);
}
