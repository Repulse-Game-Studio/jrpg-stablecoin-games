/**
 * The interface every chain family implements (twin of scfg_core/backends/base.py; see
 * backends/evm.ts and backends/solana.ts). Methods that are cooperative
 * generators in Python are async here; normalizeTx and makeRpc are plain.
 */
import type { Chain } from "../chains.ts";
import type { Transport } from "../coio.ts";
import type { Config } from "../config.ts";
import type { Item } from "../models.ts";
import type { Entropy, SessionRecord, SessionStore } from "../sessions.ts";
import type { StepOutcome } from "../watcher.ts";

export interface Backend {
  readonly family: string;

  /** The RPC object handed back to every other method for this chain. */
  makeRpc(chain: Chain, transport: Transport, clock?: () => number): any;

  /** Validate a transaction id in this family's format. Throws ValueError. */
  normalizeTx(text: unknown): string;

  /**
   * Fields for a new session: must contain "start_block"; the rest is merged in. `entropy`
   * stands in for Python's `secrets` (Solana draws its reference from it).
   */
  start(args: { rpc: any; chain: Chain; wallet: string | null; token: string; entropy: Entropy }): Promise<Record<string, unknown>>;

  /** Move one pending session forward. */
  advance(
    pending: SessionStore, receipts: SessionStore, sid: string,
    args: { rpc: any; chain: Chain; wallet: string; items: ReadonlyMap<string, Item>; now: number },
  ): Promise<StepOutcome>;

  /** "keep" or "drop". Network trouble must answer "keep". */
  reverify(receipt: Record<string, unknown>, args: { rpc: any; chain: Chain; wallets: readonly string[] }): Promise<string>;

  /**
   * Restore candidates for tx on this chain, or null if the chain doesn't know it.
   * Throws RestoreError("tx_reverted" | "tx_not_a_payment"). A transfer that cannot be classified
   * is skipped and reported to on_error (restore.classifyOrSkip).
   */
  lookup(
    tx: string, args: { config: Config; chain: Chain; rpc: any; receipts: SessionStore; on_error?: ((e: unknown) => void) | undefined },
  ): Promise<unknown[] | null>;

  /** token name -> can the developer receive it on this chain. */
  checkReady(args: { rpc: any; chain: Chain; wallet: string | null }): Promise<Record<string, boolean>>;

  /** Solana only: the unsigned transaction a browser wallet signs for this session. */
  buildPayment?(rec: SessionRecord, args: { rpc: any; chain: Chain; account: string }): Promise<Record<string, unknown>>;
}
