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

// Import cycle, safe only because backends are created lazily: restore.ts and reverify.ts import
// backendFor from here, while evm.ts and solana.ts import from restore.ts and reverify.ts. ES
// modules allow the cycle because no module in it touches another's bindings while it evaluates;
// they only meet inside function bodies, after every module has loaded. Do NOT create a backend at
// module top level (`const evm = new EvmBackend()`): with backends/evm.ts imported first, this
// module would evaluate before evm.ts and hit EvmBackend in its temporal dead zone.
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
