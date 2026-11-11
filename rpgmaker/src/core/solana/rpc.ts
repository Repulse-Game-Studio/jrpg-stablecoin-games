/**
 * Solana JSON-RPC on top of the shared failover client (twin of scfg_core/solana/rpc.py).
 *
 * History-sensitive lookups (restore, launch re-check) ask endpoints one by one, because
 * public endpoints keep very different amounts of history. A missing transaction only
 * counts as "doesn't exist" when an endpoint whose history reaches back to the slot in
 * question says so.
 */
import type { RpcClient } from "../rpc.ts";
import { PY_DIGIT, digitRunValue, pyGet, pyInt, pyIntNumber, pyTruthy } from "../pystr.ts";
import { RpcError, RpcUnavailable } from "../rpc.ts";
import { ValueError } from "../errors.ts";

export const TX_VERSION = 1; // highest transaction version this build can read (only meta is used)
const UNKNOWN_HISTORY = 2 ** 62;
const UNSUPPORTED_VERSION = -32015;
// Python's re.compile(r"version \((\d+)\)", re.IGNORECASE): \d is any Unicode decimal digit (PY_DIGIT), and
// IGNORECASE also matches \u017f for s and \u0130, \u0131 for i (JS's /i does not)
const NAMED_VERSION = new RegExp(`[vV][eE][rR][sS\u017f][iI\u0130\u0131][oO][nN] \\((${PY_DIGIT}+)\\)`, "u");

export class SolanaRpc {
  client: RpcClient;
  tx_version: number;
  private readonly _first_slot = new Map<string, number>();

  constructor(client: RpcClient) {
    this.client = client;
    this.tx_version = TX_VERSION;
  }

  async call(method: string, params: unknown[]): Promise<any> {
    return this.client.call(method, params);
  }

  async getSlot(commitment: string = "confirmed"): Promise<number> {
    return pyIntNumber(await this.call("getSlot", [{ commitment }]));
  }

  async getSignaturesForAddress(address: string, opts: {
    until?: string | null; before?: string | null; limit?: number; commitment?: string;
  } = {}): Promise<any[]> {
    const { until = null, before = null, limit = 100, commitment = "confirmed" } = opts;
    const o: Record<string, unknown> = { limit, commitment };
    if (until) o["until"] = until;
    if (before) o["before"] = before;
    const res = await this.call("getSignaturesForAddress", [address, o]);
    return pyTruthy(res) ? res : []; // Python `... or []`: {} and "" are empty too
  }

  private _txParams(sig: string, commitment: string, version: number | null = null): unknown[] {
    return [sig, {
      encoding: "json", commitment,
      maxSupportedTransactionVersion: version === null ? this.tx_version : version,
    }];
  }

  /**
   * The newer version a -32015 "Transaction version (N) is not supported" names. Only meta,
   * slot and accountKeys are read, so any version can be read. A number beyond 18 significant
   * digits is no match (Ruling 6).
   */
  private _namedVersion(error: RpcError): number | null {
    if (error.code !== UNSUPPORTED_VERSION) return null;
    const found = NAMED_VERSION.exec(error.message);
    const version = found === null ? null : digitRunValue(found[1]!);
    if (version === null || version <= this.tx_version) return null;
    return version;
  }

  /** Older nodes reject a maxSupportedTransactionVersion they don't know (-32602). */
  private _lowerVersion(error: RpcError): boolean {
    if (this.tx_version > 0 && error.code === -32602 && error.message.toLowerCase().includes("version")) {
      this.tx_version -= 1;
      return true;
    }
    return false;
  }

  async getTransaction(sig: string, commitment: string = "confirmed"): Promise<any> {
    try {
      return await this.call("getTransaction", this._txParams(sig, commitment));
    } catch (e) {
      if (!(e instanceof RpcError)) throw e;
      if (this._lowerVersion(e)) return this.getTransaction(sig, commitment);
      const version = this._namedVersion(e);
      if (version === null) throw e;
      return this.call("getTransaction", this._txParams(sig, commitment, version));
    }
  }

  private async _firstAvailable(url: string): Promise<number> {
    if (!this._first_slot.has(url)) {
      try {
        this._first_slot.set(url, pyIntNumber(await this.client.callAt(url, "getFirstAvailableBlock", [])));
      } catch (e) {
        // Python: except (RpcUnavailable, RpcError, TypeError, ValueError)
        if (e instanceof RpcUnavailable || e instanceof RpcError || e instanceof TypeError
            || e instanceof ValueError) {
          return UNKNOWN_HISTORY; // unknown: treat as pruned, ask again next time
        }
        throw e;
      }
    }
    return this._first_slot.get(url)!;
  }

  async getTransactionAny(sig: string, opts: { min_slot?: number | null; commitment?: string } = {}): Promise<[any, boolean]> {
    const { min_slot = null, commitment = "finalized" } = opts;
    let answered = false;
    let authoritative = false;
    for (const url of this.client.liveUrls()) {
      const attempt = await this.client.tryAt(url, "getTransaction", this._txParams(sig, commitment));
      if (attempt === null) continue;
      let res: any = attempt[1];
      if (res instanceof RpcError) {
        if (this._lowerVersion(res)) return this.getTransactionAny(sig, { min_slot, commitment });
        const version = this._namedVersion(res);
        if (version === null) continue;
        try {
          res = await this.client.callAt(url, "getTransaction", this._txParams(sig, commitment, version));
        } catch (e) {
          if (e instanceof RpcUnavailable || e instanceof RpcError) continue;
          throw e;
        }
      }
      answered = true;
      if (res !== null && res !== undefined) return [res, false];
      if (min_slot !== null && (await this._firstAvailable(url)) <= min_slot) authoritative = true;
    }
    if (!answered) throw new RpcUnavailable(`no endpoint answered getTransaction for ${sig}`);
    return [null, authoritative];
  }

  async getSignatureStatuses(sigs: Iterable<string>): Promise<any[]> {
    const res = await this.call("getSignatureStatuses", [[...sigs], { searchTransactionHistory: true }]);
    return pyGet(res, "value");
  }

  async accountExists(address: string): Promise<boolean> {
    const res = await this.call("getAccountInfo", [address, { encoding: "base64", commitment: "confirmed" }]);
    return pyGet(res, "value") !== null;
  }

  async getLatestBlockhash(): Promise<string> {
    const res = await this.call("getLatestBlockhash", [{ commitment: "confirmed" }]);
    return pyGet(pyGet(res, "value"), "blockhash");
  }

  async getTokenBalance(tokenAccount: string): Promise<bigint> {
    const res = await this.call("getAccountInfo", [tokenAccount, { encoding: "jsonParsed", commitment: "confirmed" }]);
    const value = pyGet(res, "value");
    if (value === null) return 0n;
    return pyInt(["data", "parsed", "info", "tokenAmount", "amount"].reduce(pyGet, value));
  }

  async getBalance(address: string): Promise<bigint> {
    const res = await this.call("getBalance", [address, { commitment: "confirmed" }]);
    return pyInt(pyGet(res, "value"));
  }

  async getPriorityFee(accounts: Iterable<string>): Promise<number> {
    const rows = await this.call("getRecentPrioritizationFees", [[...accounts]]);
    const fees = ((pyTruthy(rows) ? rows : []) as any[]).map((f) => pyIntNumber(pyGet(f, "prioritizationFee"))).sort((a, b) => a - b);
    return fees.length > 0 ? fees[Math.floor(fees.length / 2)]! : 0;
  }
}
