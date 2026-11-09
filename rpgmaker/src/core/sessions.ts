/**
 * Pending payment sessions (twin of scfg_core/sessions.py).
 *
 * A session is one payment attempt, created when the player picks a network. It is stored as a
 * plain JSON record (the frozen `pending` schema, conformance/schema/state.json) so it survives
 * restarts and the watcher can keep looking for the payment for up to 24 hours.
 *
 * Randomness goes through an `Entropy` (Python's `secrets`), so the conformance runner can feed
 * recorded draws. The draw order and sizes equal Python's: `randbelow(100)` for the nonce, then
 * `token_hex(8)` for the id (Solana's backend adds `token_bytes(32)` for its reference).
 */
import { encode, parseAmount } from "./amounts.ts";
import { bytesToHex } from "./bytes.ts";
import { Dec } from "./decimal.ts";
import { KeyError, ValueError } from "./errors.ts";
import { hasOwn, pyGet, pyStrip } from "./pystr.ts";

export const SESSION_WINDOW_S = 30 * 60;
export const SCAN_HORIZON_S = 24 * 3600;
export const HINT_TIMEOUT_S = 30 * 60;
export const UI_STATUSES: readonly string[] = ["open", "pending", "cancelled", "expired"];
const TX_HASH = /^0x[0-9a-fA-F]{64}$/;

/** A pending session record (plain JSON; see conformance/schema/state.json). */
export type SessionRecord = Record<string, any>;
export type SessionStore = Record<string, any>;

/** Python's `secrets` module, as the payment core uses it. */
export interface Entropy {
  /** `secrets.token_hex(n)`: 2n lowercase hex characters. */
  tokenHex(n: number): string;
  /** `secrets.randbelow(n)`: a uniform integer in [0, n). */
  randBelow(n: number): number;
  /** `secrets.token_bytes(n)`. */
  tokenBytes(n: number): Uint8Array;
}

interface CryptoGlobals {
  crypto?: { getRandomValues?: (a: Uint8Array) => Uint8Array };
}

function randomBytes(n: number): Uint8Array {
  const c = (globalThis as unknown as CryptoGlobals).crypto;
  if (!c || typeof c.getRandomValues !== "function") throw new Error("crypto.getRandomValues is not available");
  const out = new Uint8Array(n);
  for (let i = 0; i < n; i += 65536) c.getRandomValues(out.subarray(i, Math.min(n, i + 65536)));
  return out;
}

/** The OS random source through `globalThis.crypto.getRandomValues`, looked up at call time. */
export const defaultEntropy: Entropy = {
  tokenBytes: randomBytes,
  tokenHex: (n) => bytesToHex(randomBytes(n)),
  randBelow(n) {
    if (!Number.isInteger(n) || n <= 0 || n > 2 ** 32) throw new ValueError("upper bound must be a positive int <= 2**32");
    // Rejection sampling over 32-bit draws keeps the result uniform.
    const limit = Math.floor(2 ** 32 / n) * n;
    for (;;) {
      const b = randomBytes(4);
      const x = ((b[0]! << 24) >>> 0) + (b[1]! << 16) + (b[2]! << 8) + b[3]!;
      if (x < limit) return x % n;
    }
  },
};

export function newSessionId(entropy: Entropy = defaultEntropy): string {
  return "ps_" + entropy.tokenHex(8);
}

export function normalizeTxHash(tx_hash: unknown): string {
  if (typeof tx_hash !== "string" || !TX_HASH.test(pyStrip(tx_hash))) {
    throw new ValueError(`not a transaction hash: ${JSON.stringify(tx_hash)}`);
  }
  return pyStrip(tx_hash).toLowerCase();
}

/** Python `create_session` keyword arguments. */
export interface CreateSessionArgs {
  kind: string;
  item: string | null;
  title: string;
  token: string;
  chain: string;
  price: unknown; // str, int or Dec, validated by parseAmount
  code: number;
  start_block: number;
  now: number;
  nonce?: number | null;
  extra?: Record<string, unknown> | null;
}


export function createSession(store: SessionStore, args: CreateSessionArgs, entropy: Entropy = defaultEntropy): SessionRecord {
  const { kind, item, title, token, chain, price, code, start_block, now } = args;
  if (kind !== "unlock" && kind !== "tip") throw new ValueError(`invalid kind ${JSON.stringify(kind)}`);
  let nonce = args.nonce;
  if (nonce === null || nonce === undefined) nonce = entropy.randBelow(100);
  const p = parseAmount(price);
  const sid = newSessionId(entropy);
  const rec: SessionRecord = {
    id: sid, kind, item, title, token, chain,
    price: String(p), code, nonce, units: Number(encode(p, code, nonce)),
    start_block, cursor: start_block, created_at: now,
    state: "awaiting", ui_status: "open",
    hint_tx: null, hint_at: null, match: null,
  };
  const extra = args.extra ?? {};
  for (const key of Object.keys(extra)) {
    if (hasOwn(rec, key)) throw new ValueError(`session field ${JSON.stringify(key)} is reserved`);
  }
  for (const key of Object.keys(extra)) rec[key] = extra[key];
  store[sid] = rec;
  return rec;
}

/** `store.get(sid)` */
export function getSession(store: SessionStore, sid: string): SessionRecord | null {
  return hasOwn(store, sid) ? store[sid] : null;
}

/** `store.pop(sid, None)` */
export function removeSession(store: SessionStore, sid: string): void {
  delete store[sid];
}

/** Oldest first (a stable sort, like Python's `sorted`). */
export function listSessions(store: SessionStore): SessionRecord[] {
  return Object.values(store).sort((a, b) => {
    const x = numField(a, "created_at");
    const y = numField(b, "created_at");
    return x < y ? -1 : x > y ? 1 : 0;
  });
}

export function setHint(
  store: SessionStore, sid: string, tx_hash: unknown, now: number,
  normalize: (text: unknown) => string = normalizeTxHash,
): void {
  if (!hasOwn(store, sid)) throw new KeyError(sid);
  const rec = store[sid];
  rec["hint_tx"] = normalize(tx_hash);
  rec["hint_at"] = now;
}

export function clearHint(rec: SessionRecord): void {
  rec["hint_tx"] = null;
  rec["hint_at"] = null;
}

export function setUiStatus(store: SessionStore, sid: string, status: unknown): void {
  if (typeof status !== "string" || !UI_STATUSES.includes(status)) {
    throw new ValueError(`invalid ui status ${JSON.stringify(status)}`);
  }
  if (!hasOwn(store, sid)) throw new KeyError(sid);
  store[sid]["ui_status"] = status;
}

/**
 * `rec[key]` used as a number: KeyError when missing, TypeError unless a JSON number (Python's
 * arithmetic and comparisons raise TypeError on None or str, where JS would coerce).
 */
export function numField(rec: unknown, key: string): number {
  const v = pyGet(rec, key);
  if (typeof v !== "number") throw new TypeError(`${key} must be a number, not ${v === null ? "None" : typeof v}`);
  return v;
}

export function isPastWindow(rec: SessionRecord, now: number): boolean {
  return now - numField(rec, "created_at") >= SESSION_WINDOW_S;
}

export function isPastHorizon(rec: SessionRecord, now: number): boolean {
  return now - numField(rec, "created_at") >= SCAN_HORIZON_S;
}

/** Python `Decimal(x)` for a stored price (str or int); non-finite or other types raise. */
export function pyDecimal(x: unknown): Dec {
  if (typeof x === "number" && Number.isInteger(x)) return Dec.fromInt(x);
  if (typeof x !== "string") throw new TypeError(`conversion from ${x === null ? "NoneType" : typeof x} to Decimal is not supported`);
  const d = Dec.parse(x);
  // Python would build NaN/Infinity here; the core never stores them and comparing them is not modelled.
  if (!(d instanceof Dec)) throw new ValueError(`not a finite decimal: ${JSON.stringify(x)}`);
  return d;
}

/** `Decimal(rec["price"])` */
export function sessionPrice(rec: SessionRecord): Dec {
  return pyDecimal(pyGet(rec, "price"));
}
