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
 * As in Python, the removed keys come back in store order (insertion order here; receipt keys start
 * with a chain id, so they are never integer-like and JS keeps insertion order). A stored entry whose
 * own fields are invalid (a missing field, a chain, token or item that cannot be a key, a tx hash that
 * is not a str, a log index or block that is not an int, an amount that is not a number of base units
 * within the stored-amount range) is removed whatever chain it names, with the reason reported to
 * `on_error`: a forged receipt cannot escape verification by making its check fail. A check that fails
 * on what a node returned keeps that receipt as unverifiable, reports the failure, and the run goes on.
 */
import { decimalToUnits, storedAmount, type Units } from "./amounts.ts";
import { backendFor } from "./backends/index.ts";
import { logSource, parseTransfer, type Chain } from "./chains.ts";
import { ArithmeticError, KeyError, ValueError, printError } from "./errors.ts";
import { TRANSFER_TOPIC, addressTopic, parseTransferLog, type RawTransferLog } from "./evm.ts";
import { hasOwn, pyGet, pyIntNumber, pyStrip } from "./pystr.ts";
import { RpcError, RpcUnavailable } from "./rpc.ts";

export const FUTURE_TOLERANCE_BLOCKS = 10_000;

/** `decimal_to_units(stored_amount(x))` for a stored amount: ValueError / TypeError when it is invalid. */
export function amountUnits(x: unknown): Units {
  return decimalToUnits(storedAmount(x));
}

export interface ReverifyArgs {
  rpc_for: (key: string) => any;
  chains: Record<string, Chain>;
  /** a sequence (the developer's wallets, for every family) or family -> sequence */
  wallets: readonly string[] | Record<string, readonly string[]>;
  /** told of each receipt whose check crashed (kept as unverifiable); default: printError */
  on_error?: ((e: unknown) => void) | undefined;
}

/**
 * Remove receipts that the chain contradicts. Returns the removed keys. A family with no wallets
 * keeps its receipts: they cannot be judged.
 */
export async function reverifyReceipts(receipts: Record<string, any>, args: ReverifyArgs): Promise<string[]> {
  const report = args.on_error ?? printError;
  const removed: string[] = [];
  for (const [key, r] of Object.entries(receipts)) {
    const problem = invalidFields(r, args.chains);
    if (problem !== null) {
      delete receipts[key];
      removed.push(key);
      report(new ValueError(`stored receipt ${JSON.stringify(key)} is invalid and was removed: ${String(problem)}`));
      continue;
    }
    let verdict: string;
    try {
      verdict = await judge(r, args);
    } catch (e) {
      report(e); // node data the check could not read: unverifiable, kept, the rest still checked
      continue;
    }
    if (verdict === "drop") {
      delete receipts[key];
      removed.push(key);
    }
  }
  return removed;
}

/** A dict key or set member: Python's `hash(v)` fails for a list or dict. */
function hashable(v: unknown, name: string): void {
  if (typeof v === "object" && v !== null) throw new TypeError(`unhashable ${name}`);
}

/**
 * Why the stored entry `r` is not a receipt the checks can read (Python's KeyError, TypeError,
 * ValueError, AttributeError, ArithmeticError), or null. The backends then read the same fields.
 */
function invalidFields(r: unknown, chains: Record<string, Chain>): unknown {
  try {
    const chainKey = pyGet(r, "chain");
    hashable(chainKey, "chain");
    hashable(pyGet(r, "token"), "token name");
    hashable(isObject(r) && hasOwn(r, "item") ? r["item"] : null, "item");
    const tx = pyGet(r, "tx_hash");
    if (typeof tx !== "string") throw new TypeError("tx_hash must be a string");
    if (pyIntNumber(pyGet(r, "log_index")) < 0 || pyIntNumber(pyGet(r, "block")) < 0) {
      throw new ValueError("log index and block must be >= 0");
    }
    amountUnits(pyGet(r, "amount"));
    // On a chain this build knows, the tx id must be one of its family's, as stored (no whitespace
    // that Python's str.strip() removes: JS trim() keeps \x1c-\x1f and \x85, and removes U+FEFF).
    if (typeof chainKey === "string" && hasOwn(chains, chainKey)) {
      if (tx !== pyStrip(tx)) throw new ValueError(`tx id with surrounding whitespace: ${JSON.stringify(tx)}`);
      backendFor(chains[chainKey]!).normalizeTx(tx);
    }
  } catch (e) {
    if (e instanceof KeyError || e instanceof TypeError || e instanceof ValueError || e instanceof ArithmeticError) return e;
    throw e;
  }
  return null;
}

const isObject = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);

/** A well-formed receipt's verdict: "drop" or "keep". */
async function judge(r: Record<string, any>, args: ReverifyArgs): Promise<string> {
  const { rpc_for, chains, wallets } = args;
  const chainKey = pyGet(r, "chain");
  const chain = typeof chainKey === "string" && hasOwn(chains, chainKey) ? chains[chainKey]! : undefined;
  if (chain === undefined) return "keep"; // chain unknown to this build: cannot judge, keep
  const tokenName = pyGet(r, "token");
  if (typeof tokenName !== "string" || !hasOwn(chain.tokens, tokenName)) return "drop";
  const familyWallets = Array.isArray(wallets)
    ? wallets as readonly string[]
    : (hasOwn(wallets, chain.family) ? (wallets as Record<string, readonly string[]>)[chain.family]! : []);
  if (familyWallets.length === 0) return "keep"; // no wallet of this family configured any more: cannot judge, keep
  return await backendFor(chain).reverify(r, { rpc: rpc_for(chain.key), chain, wallets: [...familyWallets] });
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
