/**
 * EVM chains: the watcher, re-verification and restore code behind the Backend interface
 * (twin of scfg_core/backends/evm.py).
 */
import type { Chain } from "../chains.ts";
import type { Transport } from "../coio.ts";
import type { Config } from "../config.ts";
import type { Item } from "../models.ts";
import { lookupEvm, type RestoreCandidate } from "../restore.ts";
import { checkEvmReceipt } from "../reverify.ts";
import { RpcClient } from "../rpc.ts";
import { normalizeTxHash, type Entropy, type SessionStore } from "../sessions.ts";
import { advance, type EvmRpc, type StepOutcome } from "../watcher.ts";
import type { Backend } from "./base.ts";

export const START_BLOCK_MARGIN = 5;

export class EvmBackend implements Backend {
  readonly family = "evm";

  makeRpc(chain: Chain, transport: Transport, clock?: () => number): RpcClient {
    return new RpcClient(chain.rpc, { transport, log_range: chain.max_log_range, clock });
  }

  normalizeTx(text: unknown): string {
    return normalizeTxHash(text);
  }

  async start(args: { rpc: EvmRpc; chain: Chain; wallet: string | null; token: string; entropy: Entropy }): Promise<Record<string, unknown>> {
    const head = await args.rpc.blockNumber();
    return { start_block: Math.max(0, head - START_BLOCK_MARGIN) };
  }

  advance(
    pending: SessionStore, receipts: SessionStore, sid: string,
    args: { rpc: EvmRpc; chain: Chain; wallet: string; items: Record<string, Item>; now: number },
  ): Promise<StepOutcome> {
    return advance(pending, receipts, sid, args);
  }

  reverify(receipt: Record<string, unknown>, args: { rpc: EvmRpc; chain: Chain; wallets: readonly string[] }): Promise<string> {
    return checkEvmReceipt(receipt, args);
  }

  lookup(tx: string, args: { config: Config; chain: Chain; rpc: EvmRpc; receipts: SessionStore }): Promise<RestoreCandidate[] | null> {
    return lookupEvm(tx, args);
  }

  /** Every token can be received on an EVM chain: no network needed. */
  async checkReady(args: { rpc: EvmRpc; chain: Chain; wallet: string | null }): Promise<Record<string, boolean>> {
    const out: Record<string, boolean> = {};
    for (const name of Object.keys(args.chain.tokens)) out[name] = true;
    return out;
  }
}
