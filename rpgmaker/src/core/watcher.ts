/**
 * The payment watcher: advances one pending session per call (twin of scfg_core/watcher.py).
 *
 *     awaiting --(hinted tx verified | exact amount found by scan)--> confirming
 *     confirming --(N confirmations, block hash unchanged)--> paid (receipt stored)
 *     confirming --(block hash changed: reorg)--> awaiting
 *     awaiting --(hinted tx pays less than min_price)--> underpaid
 *     awaiting --(24 h, nothing found)--> closed
 *
 * `advance` is logic over plain records plus an RPC object, so it is testable with a fake chain.
 * State is only mutated after RPC calls succeed, so a network outage never corrupts a session.
 * Record fields are read the way Python indexes them (`pyGet`): a malformed record throws, and
 * the Engine contains it per session, instead of JS coercing `undefined` into a different path.
 *
 * Known limit: `match.value` is persisted as a JSON number, so a transfer of 2^53 units or more
 * (about 9e9 tokens) makes `matchDict` throw ValueError instead of rounding; the Engine contains it
 * as `internal_error` each tick until the 24 h horizon. Python would store the exact int.
 */
import { asUnits, unitsToDecimal, type Units } from "./amounts.ts";
import { logSource, parseTransfer, type Chain } from "./chains.ts";
import type { Dec } from "./decimal.ts";
import { ValueError } from "./errors.ts";
import { TRANSFER_TOPIC, addressTopic, type TransferLog } from "./evm.ts";
import { makePaymentResult, type Item, type PaymentResult } from "./models.ts";
import { hasOwn, pyGet, pyTruthy } from "./pystr.ts";
import { addReceipt, makeReceipt, receiptKey } from "./receipts.ts";
import { RpcError, RpcUnavailable } from "./rpc.ts";
import {
  HINT_TIMEOUT_S, clearHint, isPastHorizon, numField, pyDecimal, removeSession, sessionPrice, type SessionRecord, type SessionStore,
} from "./sessions.ts";

export const SCAN_STEP_BLOCKS = 10_000; // upper bound on blocks scanned per advance()
export const MAX_SCAN_CALLS = 5; // eth_getLogs calls per advance(), to be gentle with public RPCs

/** Python's StepOutcome dataclass: every field present, `null` for None. */
export interface StepOutcome {
  session_id: string;
  status: string; // missing | waiting | confirming | reorged | paid | underpaid | closed | expired
  confirmations: number;
  required: number;
  result: PaymentResult | null;
  // network_unavailable | tx_reverted | tx_not_a_payment | tx_not_landed (Solana)
  // | internal_error (Engine.tick: the session's step crashed)
  error: string | null;
  // The hint a tx_reverted / tx_not_a_payment / tx_not_landed verdict is about.
  hint_tx: string | null;
}

/** `StepOutcome(session_id, status, ...)` with the dataclass defaults. */
export function makeStepOutcome(
  session_id: string, status: string, fields: Partial<Omit<StepOutcome, "session_id" | "status">> = {},
): StepOutcome {
  return { session_id, status, confirmations: 0, required: 0, result: null, error: null, hint_tx: null, ...fields };
}

/** The RPC surface the EVM watcher needs (RpcClient; FakeChain in tests). */
export interface EvmRpc {
  blockNumber(): Promise<number>;
  getTransactionReceipt(txHash: string): Promise<any>;
  getBlockHash(number: number): Promise<string | null>;
  getLogs(address: string, topics: unknown[], fromBlock: number, toBlock: number): Promise<any[]>;
  /** RpcClient's eth_getLogs range for its preferred endpoint; absent on objects that have none. */
  readonly logRange?: number;
}

export interface AdvanceArgs {
  rpc: EvmRpc;
  chain: Chain;
  wallet: string;
  items: ReadonlyMap<string, Item>;
  now: number;
}

export function minAmount(rec: SessionRecord, items: ReadonlyMap<string, Item>): Dec {
  if (pyGet(rec, "kind") === "unlock") {
    const key = pyGet(rec, "item");
    const item = typeof key === "string" ? items.get(key) : undefined;
    return item ? item.min_price : sessionPrice(rec);
  }
  return pyDecimal("0.000001");
}

/** A JSON number for a stored value; an unsafe one cannot be persisted exactly, so it is refused. */
export function storedInt(v: bigint, what: string): number {
  if (v > BigInt(Number.MAX_SAFE_INTEGER) || v < -BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new ValueError(`${what} ${v} does not fit a JSON number exactly`);
  }
  return Number(v);
}

/** A stored token amount (a JSON number of base units, e.g. `match.value`) as Units. */
export function unitsField(rec: unknown, key: string): Units {
  const v = pyGet(rec, key);
  if (typeof v === "bigint") return asUnits(v);
  if (typeof v === "number" && Number.isInteger(v)) return asUnits(BigInt(v));
  throw new TypeError(`${key} must be an integer`);
}

function matchDict(t: TransferLog): Record<string, unknown> {
  return {
    tx_hash: t.tx_hash, log_index: t.log_index, block: t.block_number, block_hash: t.block_hash,
    value: storedInt(t.value, "transfer value"), payer: t.sender, recipient: t.recipient,
  };
}

export function resultFromMatch(rec: SessionRecord, status: string, chain_key: string, m: Record<string, any>): PaymentResult {
  return makePaymentResult({
    status, kind: pyGet(rec, "kind"), item: pyGet(rec, "item"), amount: unitsToDecimal(unitsField(m, "value")),
    token: pyGet(rec, "token"), chain: chain_key, tx_hash: pyGet(m, "tx_hash"), payer: pyGet(m, "payer"),
    session_id: pyGet(rec, "id"),
  });
}

function qualifies(t: TransferLog | null, rec: SessionRecord, chain: Chain, wallet: string, receipts: SessionStore): t is TransferLog {
  return t !== null
    && t.token === pyGet(chain.tokens, pyGet(rec, "token")).address
    && t.recipient === wallet
    && t.block_number >= numField(rec, "start_block")
    && !hasOwn(receipts, receiptKey(chain.key, t.tx_hash, t.log_index));
}

/** Python `d.get(key)` on a parsed JSON value: AttributeError (TypeError here) unless a dict. */
export function dictGet(d: unknown, key: string): any {
  if (typeof d !== "object" || d === null || Array.isArray(d)) {
    throw new TypeError(`'${d === null ? "NoneType" : Array.isArray(d) ? "list" : typeof d}' object has no attribute 'get'`);
  }
  return hasOwn(d, key) ? (d as Record<string, unknown>)[key] : null;
}

/** `for x in v` where every non-list item would fail downstream anyway. */
export function asList(v: unknown): unknown[] {
  if (!Array.isArray(v)) throw new TypeError("logs must be a list");
  return v;
}

/**
 * Check the tx hash reported by the browser page or pasted by the player.
 * Returns "matched", "underpaid", or null (keep waiting / hint discarded).
 */
async function verifyHint(
  rec: SessionRecord, rpc: EvmRpc, chain: Chain, wallet: string, items: ReadonlyMap<string, Item>,
  receipts: SessionStore, now: number, out: StepOutcome,
): Promise<string | null> {
  const receipt = await rpc.getTransactionReceipt(pyGet(rec, "hint_tx"));
  if (receipt === null || receipt === undefined) { // not mined yet (or unknown): give it up to 30 minutes
    if (now - numField(rec, "hint_at") >= HINT_TIMEOUT_S) clearHint(rec);
    return null;
  }
  if (dictGet(receipt, "status") === "0x0") {
    out.hint_tx = rec["hint_tx"]; // the verdict is about this hint
    clearHint(rec);
    out.error = "tx_reverted";
    return null;
  }
  const logs = dictGet(receipt, "logs");
  const candidates: TransferLog[] = [];
  for (const lg of pyTruthy(logs) ? asList(logs) : []) {
    const t = parseTransfer(chain, lg as Record<string, any>);
    if (qualifies(t, rec, chain, wallet, receipts) && t.value > 0n) candidates.push(t);
  }
  if (candidates.length === 0) {
    out.hint_tx = rec["hint_tx"]; // the verdict is about this hint
    clearHint(rec);
    out.error = "tx_not_a_payment";
    return null;
  }
  // Python max(): the first of the largest values.
  const best = candidates.reduce((a, b) => (b.value > a.value ? b : a));
  rec["match"] = matchDict(best);
  if (unitsToDecimal(best.value).cmp(minAmount(rec, items)) < 0) return "underpaid";
  return "matched";
}

/** Look for a transfer of exactly the session's encoded amount since the cursor. */
async function scan(rec: SessionRecord, rpc: EvmRpc, chain: Chain, wallet: string, receipts: SessionStore, head: number): Promise<boolean> {
  const cursor = numField(rec, "cursor");
  if (cursor > head) return false; // this endpoint is behind the one that set start_block
  const span = rpc.logRange;
  const step = pyTruthy(span) ? Math.min(SCAN_STEP_BLOCKS, span! * MAX_SCAN_CALLS) : SCAN_STEP_BLOCKS;
  const to = Math.min(head, cursor + step - 1);
  const source = logSource(pyGet(chain.tokens, pyGet(rec, "token")));
  const logs = await rpc.getLogs(source, [TRANSFER_TOPIC, null, addressTopic(wallet)], cursor, to);
  const found: TransferLog[] = [];
  for (const lg of logs) {
    const t = parseTransfer(chain, lg);
    if (!qualifies(t, rec, chain, wallet, receipts)) continue;
    // Python reads rec["units"] only for a log that qualifies (a record without it is fine until then).
    const units = pyGet(rec, "units");
    // Python `t.value == rec["units"]`: an int equals an integral number of the same value.
    if (typeof units === "number" && Number.isInteger(units) && t.value === asUnits(BigInt(units))) found.push(t);
  }
  found.sort((a, b) => a.block_number - b.block_number || a.log_index - b.log_index);
  rec["cursor"] = to + 1;
  if (found.length > 0) {
    rec["match"] = matchDict(found[0]!);
    return true;
  }
  return false;
}

export async function advance(
  pending: SessionStore, receipts: SessionStore, sid: string, args: AdvanceArgs,
): Promise<StepOutcome> {
  const { rpc, chain, wallet, items, now } = args;
  const out = makeStepOutcome(sid, "missing", { required: chain.confirmations });
  const rec: SessionRecord | null = hasOwn(pending, sid) ? pending[sid] : null; // pending.get(sid)
  if (rec === null || rec === undefined) return out;
  try {
    let head: number | null = null;
    if (pyGet(rec, "state") === "awaiting") {
      out.status = "waiting";
      if (isPastHorizon(rec, now)) {
        removeSession(pending, sid);
        out.status = "closed";
        return out;
      }
      let outcome: string | null = null;
      if (pyTruthy(pyGet(rec, "hint_tx"))) {
        outcome = await verifyHint(rec, rpc, chain, wallet, items, receipts, now, out);
      }
      if (outcome === null && !pyTruthy(rec["hint_tx"])) {
        head = await rpc.blockNumber();
        const found = await scan(rec, rpc, chain, wallet, receipts, head);
        outcome = found ? "matched" : null;
      }
      if (outcome === "underpaid") {
        removeSession(pending, sid);
        out.status = "underpaid";
        out.result = resultFromMatch(rec, "underpaid", chain.key, rec["match"]);
        return out;
      }
      if (outcome !== "matched") return out;
      rec["state"] = "confirming";
    }

    out.status = "confirming";
    const m = pyGet(rec, "match");
    if (head === null) head = await rpc.blockNumber();
    const block = numField(m, "block");
    if (head < block) return out; // lagging endpoint, not a reorg
    const current = await rpc.getBlockHash(block);
    if (current === null || current.toLowerCase() !== pyGet(m, "block_hash")) {
      rec["state"] = "awaiting";
      rec["match"] = null;
      rec["cursor"] = Math.min(numField(rec, "cursor"), block);
      out.status = "reorged";
      return out;
    }
    out.confirmations = head - block + 1;
    if (out.confirmations < chain.confirmations) return out;
    const transfer: TransferLog = {
      token: pyGet(chain.tokens, pyGet(rec, "token")).address, sender: pyGet(m, "payer"),
      recipient: pyGet(m, "recipient"), value: unitsField(m, "value"), tx_hash: pyGet(m, "tx_hash"),
      log_index: pyGet(m, "log_index"), block_number: block, block_hash: pyGet(m, "block_hash"),
    };
    addReceipt(receipts, makeReceipt({
      chain: chain.key, transfer, kind: pyGet(rec, "kind"), item: pyGet(rec, "item"), token: pyGet(rec, "token"),
      source: "payment",
    }));
    removeSession(pending, sid);
    out.status = "paid";
    out.result = resultFromMatch(rec, "paid", chain.key, m);
    return out;
  } catch (e) {
    if (e instanceof RpcUnavailable || e instanceof RpcError) {
      out.error = "network_unavailable";
      return out;
    }
    throw e;
  }
}
