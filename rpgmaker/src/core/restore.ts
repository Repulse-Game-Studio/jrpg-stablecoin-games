/**
 * Restoring purchases from a transaction hash: the chain is the ledger (twin of scfg_core/restore.py).
 *
 * The player pastes a tx hash. We find the chain it lives on, pick out the USDC/USDT transfers to
 * the developer, and work out what each one bought from the code encoded in its amount. Amounts
 * that carry no code (e.g. an exchange sent a round 5.00) are ambiguous, so the player chooses
 * which item it paid for, or keeps it as a tip.
 *
 * Errors mirror Python: only RpcUnavailable/RpcError are swallowed by `lookup`; anything else (a
 * malformed log) rejects the whole lookup, except a transfer of 10**32 units or more, which `decode`
 * cannot price: that one is skipped and reported to `on_error` (classifyOrSkip).
 */
import { decode, unitsToDecimal, type Units } from "./amounts.ts";
import { backendFor } from "./backends/index.ts";
import { parseTransfer, type Chain } from "./chains.ts";
import { acceptedWallets, itemByCode, type Config } from "./config.ts";
import { InvalidOperation, ValueError, printError } from "./errors.ts";
import type { TransferLog } from "./evm.ts";
import { hasOwn, pyTruthy } from "./pystr.ts";
import { addReceipt, makeReceipt, receiptKey, type ReceiptJson } from "./receipts.ts";
import { RpcError, RpcUnavailable } from "./rpc.ts";
import type { Transfer } from "./transfers.ts";
import { asList, dictGet } from "./watcher.ts";

export const TIP_CHOICE = "tip";

/** code: invalid_hash | tx_not_found | tx_reverted | tx_not_a_payment | network_unavailable */
export class RestoreError extends Error {
  readonly code: string;

  constructor(code: string) {
    super(code);
    this.name = "RestoreError";
    this.code = code;
  }
}

export interface RestoreCandidate {
  chain: string;
  token: string;
  transfer: Transfer | TransferLog;
  kind: string | null; // "unlock" | "tip" | null (= the player must choose)
  item: string | null;
  choices: string[]; // item ids (+ "tip") offered when kind is null
  already_restored: boolean;
}


/** Decide what a transfer of `value` base units paid for: [kind, item, choices]. */
export function classify(config: Config, value: Units): [string | null, string | null, string[]] {
  const [, code, nonce] = decode(value);
  const amount = unitsToDecimal(value);
  const affordable = [...config.items.values()].filter((i) => i.min_price.cmp(amount) <= 0).map((i) => i.item_id);
  if (code === 0 && nonce === 0) {
    // A whole-cent amount carries no code at all: could be an exchange-rounded purchase or a plain
    // tip. Let the player say which.
    return [null, null, [...affordable, TIP_CHOICE]];
  }
  if (code === 0) return ["tip", null, []];
  const item = itemByCode(config, code);
  if (item !== null && amount.cmp(item.min_price) >= 0) return ["unlock", item.item_id, []];
  return [null, null, [...affordable, TIP_CHOICE]];
}

/** Told of a contained crash (a transfer that cannot be classified); default: printError. */
export type OnError = ((e: unknown) => void) | undefined;

/**
 * classify(), or null for a value decode() cannot price (10**32 units or more): such a transfer is
 * skipped and reported to `on_error`; the rest of the transaction is still offered.
 */
export function classifyOrSkip(
  config: Config, value: Units, on_error: OnError,
): [string | null, string | null, string[]] | null {
  try {
    return classify(config, value);
  } catch (e) {
    if (!(e instanceof InvalidOperation)) throw e;
    (on_error ?? printError)(e);
    return null;
  }
}

/** Candidates for a normalized tx hash on one EVM chain; null if the chain lacks it. */
export async function lookupEvm(
  tx_hash: string, args: { config: Config; chain: Chain; rpc: any; receipts: Record<string, unknown>; on_error?: OnError },
): Promise<RestoreCandidate[] | null> {
  const { config, chain, rpc, receipts } = args;
  const receipt = await rpc.getTransactionReceipt(tx_hash);
  if (receipt === null || receipt === undefined) return null;
  if (dictGet(receipt, "status") === "0x0") throw new RestoreError("tx_reverted");
  const byAddress = new Map<string, string>();
  for (const [name, t] of Object.entries(chain.tokens)) byAddress.set(t.address, name);
  const wallets = acceptedWallets(config);
  const out: RestoreCandidate[] = [];
  const logs = dictGet(receipt, "logs");
  for (const log of pyTruthy(logs) ? asList(logs) : []) {
    const t = parseTransfer(chain, log as Record<string, any>);
    if (t === null || !byAddress.has(t.token) || t.value <= 0n || !wallets.includes(t.recipient)) continue;
    const classified = classifyOrSkip(config, t.value, args.on_error);
    if (classified === null) continue;
    const [kind, item, choices] = classified;
    out.push({
      chain: chain.key, token: byAddress.get(t.token)!, transfer: t, kind, item, choices,
      already_restored: hasOwn(receipts, receiptKey(chain.key, t.tx_hash, t.log_index)),
    });
  }
  if (out.length === 0) throw new RestoreError("tx_not_a_payment");
  return out;
}

/** Find the chain that knows `tx_hash` and the candidates in it. */
export async function lookup(
  tx_hash: string,
  args: {
    config: Config; chains: ReadonlyMap<string, Chain>; rpc_for: (key: string) => any; receipts: Record<string, unknown>;
    on_error?: OnError;
  },
): Promise<RestoreCandidate[]> {
  const { config, chains, rpc_for, receipts, on_error } = args;
  let networkTrouble = false;
  let understood = false;
  for (const [key, chain] of chains) {
    const backend = backendFor(chain);
    let tx: string;
    try {
      tx = backend.normalizeTx(tx_hash);
    } catch (e) {
      if (e instanceof ValueError) continue;
      throw e;
    }
    understood = true;
    let out: unknown[] | null;
    try {
      out = await backend.lookup(tx, { config, chain, rpc: rpc_for(key), receipts, on_error });
    } catch (e) {
      if (e instanceof RpcUnavailable || e instanceof RpcError) {
        networkTrouble = true;
        continue;
      }
      throw e;
    }
    if (out !== null) return out as RestoreCandidate[];
  }
  if (!understood) throw new RestoreError("invalid_hash");
  throw new RestoreError(networkTrouble ? "network_unavailable" : "tx_not_found");
}

/**
 * Store receipts for the candidates. `choices` maps candidate index -> item id or "tip" for
 * candidates whose kind is null (candidates without a choice are skipped).
 */
export function apply(
  receipts: Record<string, ReceiptJson>, candidates: RestoreCandidate[],
  choices: Record<number | string, string | null> | null = null,
): ReceiptJson[] {
  const picked = choices ?? {};
  const added: ReceiptJson[] = [];
  candidates.forEach((c, i) => {
    let { kind, item } = c;
    if (kind === null) {
      const choice = hasOwn(picked, String(i)) ? picked[i] : null;
      if (choice === null || choice === undefined) return;
      if (!c.choices.includes(choice)) throw new ValueError(`${JSON.stringify(choice)} is not an allowed choice`);
      [kind, item] = choice === TIP_CHOICE ? ["tip", null] : ["unlock", choice];
    }
    const r = makeReceipt({ chain: c.chain, transfer: c.transfer, kind: kind!, item, token: c.token, source: "restore" });
    if (addReceipt(receipts, r)) added.push(r);
  });
  return added;
}
