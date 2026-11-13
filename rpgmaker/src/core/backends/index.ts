/**
 * Chain-family backends: everything that differs between EVM chains and Solana
 * (twin of scfg_core/backends/__init__.py).
 *
 * The engine, sessions, receipts and screens are family-neutral. Anything that touches a chain's
 * RPC, transaction-id format or payment matching goes through backendFor(chain). One backend
 * instance per family, created on first use. Python registers EVM then Solana; normalizeAnyTx
 * tries the families in that order.
 */
import type { Chain } from "../chains.ts";
import { ValueError } from "../errors.ts";
import { normalizeSignature } from "../solana/base58.ts";
import { normalizeTxHash } from "../sessions.ts";
import type { Backend } from "./base.ts";
import { EvmBackend } from "./evm.ts";
import { SolanaBackend } from "./solana.ts";

// Import cycle (restore.ts/reverify.ts -> here -> evm.ts/solana.ts -> restore.ts/reverify.ts), safe
// only because backends are created lazily. Do NOT create a backend at module top level: with
// backends/evm.ts imported first, this module evaluates before evm.ts and hits EvmBackend's TDZ.
let evm: EvmBackend | null = null;
let solana: SolanaBackend | null = null;

export function backendForFamily(family: string): Backend {
  if (family === "evm") return (evm ??= new EvmBackend());
  if (family === "solana") return (solana ??= new SolanaBackend());
  throw new ValueError(`unknown chain family ${JSON.stringify(family)}`);
}

export function backendFor(chain: Pick<Chain, "family">): Backend {
  return backendForFamily(chain.family);
}

const NORMALIZERS: ((text: unknown) => string)[] = [normalizeTxHash, normalizeSignature];

/** A transaction id in any supported family's format, normalized. */
export function normalizeAnyTx(text: unknown): string {
  for (const normalize of NORMALIZERS) {
    try {
      return normalize(text);
    } catch (e) {
      if (e instanceof ValueError) continue;
      throw e;
    }
  }
  throw new ValueError(`not a transaction hash or signature: ${JSON.stringify(text)}`);
}
