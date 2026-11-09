/**
 * Launch-time re-verification of stored receipts against the chain (twin of scfg_core/reverify.py).
 *
 * Each receipt is checked by its chain family's backend:
 *
 * - EVM: one eth_getLogs call scoped to the receipt's own block. Block-scoped lookups keep working
 *   years later, even on nodes that no longer index old transaction hashes.
 * - Solana: getTransaction on the receipt's signature, asked of each endpoint in turn; a missing
 *   transaction only counts when an endpoint whose history reaches back to the receipt's slot says so.
 *
 * Only a *definitive* mismatch removes a receipt; network trouble never does.
 *
 * Pinned quirks of the Python reference: the removed keys come back in store order (insertion
 * order here; receipt keys start with a chain id, so they are never integer-like and JS keeps
 * insertion order); a stored amount of "Infinity" escapes Python's malformed-entry net as an
 * OverflowError (`int(Decimal("Infinity"))`) and rejects the whole run here too.
 */
import { decimalToUnits, type Units } from "./amounts.ts";
import { backendFor } from "./backends/index.ts";
import { logSource, parseTransfer, type Chain } from "./chains.ts";
import { Dec } from "./decimal.ts";
import { KeyError, ValueError } from "./errors.ts";
import { TRANSFER_TOPIC, addressTopic, parseTransferLog, type RawTransferLog } from "./evm.ts";
import { hasOwn, pyGet, pyIntNumber } from "./pystr.ts";
import { RpcError, RpcUnavailable } from "./rpc.ts";

export const FUTURE_TOLERANCE_BLOCKS = 10_000;

/** `decimal_to_units(Decimal(x))` for a stored amount. */
export function amountUnits(x: unknown): Units {
  let d: Dec | "nan" | "inf";
  if (typeof x === "string") d = Dec.parse(x);
  else if (typeof x === "boolean") d = Dec.fromInt(x ? 1 : 0);
  else if (typeof x === "bigint") d = Dec.fromInt(x);
  else if (typeof x === "number") d = Dec.fromNumber(x);
  else throw new TypeError("conversion to Decimal is not supported");
  if (d === "nan") throw new ValueError("more than 6 decimals"); // NaN != NaN
  if (d === "inf") throw new Error("OverflowError: cannot convert Infinity to integer");
  return decimalToUnits(d);
}

export interface ReverifyArgs {
  rpc_for: (key: string) => any;
  chains: Record<string, Chain>;
  /** a sequence (the developer's wallets, for every family) or family -> sequence */
  wallets: readonly string[] | Record<string, readonly string[]>;
}

/**
 * Remove receipts that the chain contradicts. Returns the removed keys. A family with no wallets
 * keeps its receipts: they cannot be judged.
 */
export async function reverifyReceipts(receipts: Record<string, any>, args: ReverifyArgs): Promise<string[]> {
  const { rpc_for, chains, wallets } = args;
  const removed: string[] = [];
  const drop = (key: string): void => {
    delete receipts[key];
    removed.push(key);
  };
  for (const [key, r] of Object.entries(receipts)) {
    let chain: Chain | undefined;
    let tokenName: unknown;
    try {
      const chainKey = pyGet(r, "chain");
      if (typeof chainKey === "object" && chainKey !== null) throw new TypeError("unhashable chain key");
      chain = typeof chainKey === "string" && hasOwn(chains, chainKey) ? chains[chainKey] : undefined;
      // Malformed receipts are dropped here; the backend then reads the same fields.
      const tx = pyGet(r, "tx_hash");
      if (typeof tx !== "string") throw new TypeError("tx_hash must be a string");
      pyIntNumber(pyGet(r, "log_index"));
      pyIntNumber(pyGet(r, "block"));
      amountUnits(pyGet(r, "amount"));
      tokenName = pyGet(r, "token");
    } catch (e) {
      // KeyError, TypeError, ValueError (AttributeError / InvalidOperation in Python): drop.
      if (e instanceof KeyError || e instanceof TypeError || e instanceof ValueError) {
        drop(key);
        continue;
      }
      throw e;
    }
    if (chain === undefined) continue; // chain unknown to this build: cannot judge, keep
    if (typeof tokenName === "object" && tokenName !== null) throw new TypeError("unhashable token name");
    if (typeof tokenName !== "string" || !hasOwn(chain.tokens, tokenName)) {
      drop(key);
      continue;
    }
    const familyWallets = Array.isArray(wallets)
      ? wallets as readonly string[]
      : (hasOwn(wallets, chain.family) ? (wallets as Record<string, readonly string[]>)[chain.family]! : []);
    if (familyWallets.length === 0) continue; // no wallet of this family configured any more: cannot judge, keep
    const verdict = await backendFor(chain).reverify(r, { rpc: rpc_for(chain.key), chain, wallets: [...familyWallets] });
    if (verdict === "drop") drop(key);
  }
  return removed;
}

/** Judge one stored EVM receipt: "drop" if the chain contradicts it, else "keep". */
export async function checkEvmReceipt(
  r: Record<string, any>, args: { rpc: any; chain: Chain; wallets: readonly string[] },
): Promise<string> {
  const { rpc, chain, wallets } = args;
  const txHash = (pyGet(r, "tx_hash") as string).toLowerCase();
  const logIndex = pyIntNumber(pyGet(r, "log_index"));
  const block = pyIntNumber(pyGet(r, "block"));
  const units = amountUnits(pyGet(r, "amount"));
  const token = pyGet(chain.tokens, pyGet(r, "token"));
  const topics = [TRANSFER_TOPIC, null, wallets.map((w) => addressTopic(w))];

  // Python's any() over a lazy generator: logs after the first match are never parsed. `parse` is
  // parseTransfer (values floored to units) or, for the token contract's own log, parseTransferLog,
  // whose raw value is already in the token's 6 decimals there.
  const matches = (logs: any[], parse: (lg: any) => RawTransferLog | null): boolean => {
    for (const lg of logs) {
      const t = parse(lg);
      if (t !== null && t.tx_hash === txHash && t.log_index === logIndex && t.value === units
        && wallets.includes(t.recipient) && t.token === token.address) return true;
    }
    return false;
  };

  let logs: any[];
  try {
    const head = await rpc.blockNumber();
    if (block > head) {
      // probably a lagging endpoint: keep, unless the block is impossibly far ahead
      return block > head + FUTURE_TOLERANCE_BLOCKS ? "drop" : "keep";
    }
    logs = await rpc.getLogs(logSource(token), topics, block, block);
    if (matches(logs, (lg) => parseTransfer(chain, lg))) return "keep";
    if (!token.log_address) return "drop";
    // A receipt stored by a game that added this chain itself before it was built in
    // (add_chain("arc", ...)) points at the token contract's own log. That log is still a genuine
    // transfer, so it keeps the purchase.
    logs = await rpc.getLogs(token.address, topics, block, block);
  } catch (e) {
    if (e instanceof RpcUnavailable || e instanceof RpcError) return "keep";
    throw e;
  }
  return matches(logs, (lg) => parseTransferLog(lg)) ? "keep" : "drop";
}
