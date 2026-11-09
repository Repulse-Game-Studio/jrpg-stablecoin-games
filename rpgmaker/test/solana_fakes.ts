/**
 * In-memory Solana chain for backend/engine tests (twin of tests/unit/fakes.py FakeSolanaChain and
 * solana_tx). Not a test file.
 */
import { b58encode } from "../src/core/solana/base58.ts";
import { isOnCurve } from "../src/core/solana/ed25519.ts";
import { RpcUnavailable } from "../src/core/rpc.ts";
import { sha256 } from "../src/core/sha256.ts";

const hasOwn = (o: object, k: string): boolean => Object.prototype.hasOwnProperty.call(o, k);

/** A distinct 64-byte signature per n (Python `b58encode(n.to_bytes(64, "big"))`). */
export function sig(n: number): string {
  const b = new Uint8Array(64);
  let v = BigInt(n);
  for (let i = 63; i >= 0 && v > 0n; i--) {
    b[i] = Number(v & 0xffn);
    v >>= 8n;
  }
  return b58encode(b);
}

/**
 * A distinct wallet address per n that is a point on the curve (stands in for Python's
 * `str(Keypair().pubkey())`, which config validation needs).
 */
export function pubkey(n: number): string {
  for (let i = 0; ; i++) {
    const seed = new Uint8Array([n & 0xff, (n >> 8) & 0xff, i & 0xff, (i >> 8) & 0xff]);
    const key = sha256(seed);
    if (isOnCurve(key)) return b58encode(key);
  }
}

/**
 * A getTransaction(json) result where `payer` sends `amount` of `mint` to `owner`.
 * accountIndex 1 = payer's token account, 2 = owner's token account.
 */
export function solanaTx(
  s: string,
  opts: { mint: string; payer: string; owner: string; amount: number; slot: number; err?: unknown; payer_start?: number },
): Record<string, any> {
  const { mint, payer, owner, amount, slot, err = null, payer_start = 10 ** 9 } = opts;
  const row = (index: number, who: string, value: number) => ({
    accountIndex: index, mint, owner: who, uiTokenAmount: { amount: String(value), decimals: 6 },
  });
  return {
    slot,
    transaction: { signatures: [s], message: { accountKeys: [payer, "PAYERATA", "OWNERATA", mint] } },
    meta: {
      err,
      preTokenBalances: [row(1, payer, payer_start), row(2, owner, 0)],
      postTokenBalances: [row(1, payer, payer_start - amount), row(2, owner, amount)],
    },
  };
}

/**
 * Same method names as SolanaRpc, driven by tests:
 * - txs: {signature: getTransaction result}
 * - addresses: {address: [signature info dicts, newest first]}
 * - statuses: {signature: getSignatureStatuses entry or null}
 * - accounts: set of existing account addresses (for accountExists)
 * - first_available: slot from which getTransactionAny's null is authoritative
 * `calls` records the Python method names.
 */
export class FakeSolanaChain {
  slot: number;
  txs: Record<string, any> = {};
  addresses: Record<string, Record<string, any>[]> = {};
  statuses: Record<string, any> = {};
  accounts = new Set<string>();
  first_available = 0;
  down = false;
  calls: string[] = [];

  constructor(slot = 1000) {
    this.slot = slot;
  }

  protected check(name: string): void {
    this.calls.push(name);
    if (this.down) throw new RpcUnavailable("fake outage");
  }

  pay(s: string, opts: {
    mint: string; payer: string; owner: string; amount: number; slot?: number; err?: unknown;
    addresses?: string[]; status?: string;
  }): Record<string, any> {
    const { addresses = [], status = "confirmed", err = null } = opts;
    const tx = solanaTx(s, { ...opts, slot: opts.slot ?? this.slot, err });
    this.txs[s] = tx;
    for (const a of addresses) (this.addresses[a] ??= []).unshift({ signature: s, slot: tx["slot"], err });
    this.statuses[s] = {
      slot: tx["slot"], err, confirmationStatus: status, confirmations: status === "finalized" ? null : 1,
    };
    return tx;
  }

  finalize(s: string): void {
    this.statuses[s] = { ...this.statuses[s], confirmationStatus: "finalized", confirmations: null };
  }

  async getSlot(_commitment = "confirmed"): Promise<number> {
    this.check("get_slot");
    return this.slot;
  }

  async getSignaturesForAddress(address: string, opts: {
    until?: string | null; before?: string | null; limit?: number; commitment?: string;
  } = {}): Promise<any[]> {
    const { until = null, before = null, limit = 100 } = opts;
    this.check("get_signatures_for_address");
    let sigs = [...(this.addresses[address] ?? [])];
    let names = sigs.map((x) => x["signature"]);
    if (until !== null && names.includes(until)) sigs = sigs.slice(0, names.indexOf(until));
    names = sigs.map((x) => x["signature"]);
    if (before !== null && names.includes(before)) sigs = sigs.slice(names.indexOf(before) + 1);
    return sigs.slice(0, limit).map((x) => ({ ...x }));
  }

  async getTransaction(s: string, _commitment = "confirmed"): Promise<any> {
    this.check("get_transaction");
    return hasOwn(this.txs, s) ? this.txs[s] : null;
  }

  async getTransactionAny(s: string, opts: { min_slot?: number | null; commitment?: string } = {}): Promise<[any, boolean]> {
    const { min_slot = null } = opts;
    this.check("get_transaction_any");
    if (hasOwn(this.txs, s)) return [this.txs[s], false];
    return [null, min_slot !== null && this.first_available <= min_slot];
  }

  async getSignatureStatuses(sigs: Iterable<string>): Promise<any[]> {
    this.check("get_signature_statuses");
    return [...sigs].map((s) => (hasOwn(this.statuses, s) ? this.statuses[s] : null));
  }

  async accountExists(address: string): Promise<boolean> {
    this.check("account_exists");
    return this.accounts.has(address);
  }
}
