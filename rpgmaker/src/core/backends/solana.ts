/**
 * Solana (twin of scfg_core/backends/solana.py): payments are found by their Solana Pay
 * reference, by a reported signature, or by scanning the developer's token account for the
 * exact encoded amount. A payment counts once the cluster marks its transaction finalized.
 *
 * What a transaction paid is read from meta.pre/postTokenBalances, never from its instructions,
 * so it works whatever program or wallet moved the tokens.
 *
 * Fields of node replies and stored records are read the way Python reads them (`pyGet` for
 * `d[k]`, `dictGet` for `d.get(k)`, Python truthiness for `or`), so a malformed reply throws a
 * non-RPC error that the Engine contains, instead of JS coercing it into a different path.
 *
 * Python's tests monkeypatch SIG_PAGE and MAX_SIG_PAGES; ES module constants cannot be patched,
 * so each SolanaBackend carries them as `sig_page` and `max_sig_pages`.
 *
 * Same known limit as EVM (watcher.ts): a transfer of 2^53 units or more makes `matchDict` throw
 * ValueError (contained by the Engine) instead of persisting a rounded JSON number.
 */
import { asUnits, unitsToDecimal } from "../amounts.ts";
import { base64Encode } from "../bytes.ts";
import type { Chain } from "../chains.ts";
import type { Transport } from "../coio.ts";
import { acceptedWalletsFor, type Config } from "../config.ts";
import { IndexError, KeyError, ValueError } from "../errors.ts";
import type { Item } from "../models.ts";
import { hasOwn, pyGet, pyInt, pyIntNumber, pyTruthy, pyType } from "../pystr.ts";
import { addReceipt, makeReceipt, receiptKey } from "../receipts.ts";
import { RestoreError, classifyOrSkip, type RestoreCandidate } from "../restore.ts";
import { FUTURE_TOLERANCE_BLOCKS, amountUnits } from "../reverify.ts";
import { RpcClient, RpcError, RpcUnavailable } from "../rpc.ts";
import {
  clearHint, isPastHorizon, numField, removeSession, type Entropy, type SessionRecord, type SessionStore,
} from "../sessions.ts";
import { b58encode, normalizePubkey, normalizeSignature } from "../solana/base58.ts";
import { associatedTokenAddress } from "../solana/pda.ts";
import { SolanaRpc } from "../solana/rpc.ts";
import {
  compileMessage, setComputeUnitLimit, setComputeUnitPrice, transferChecked, unsignedTransaction,
} from "../solana/tx.ts";
import type { Transfer } from "../transfers.ts";
import {
  asList, dictGet, makeStepOutcome, minAmount, resultFromMatch, storedInt, unitsField, type StepOutcome,
} from "../watcher.ts";
import type { Backend } from "./base.ts";

export const FINAL_CONFIRMATIONS = 32; // Solana's "finalized" corresponds to 32 confirmed blocks
export const SCAN_TX_BUDGET = 5; // getTransaction calls per step for the token-account scan
export const SIG_PAGE = 100;
export const MAX_SIG_PAGES = 10;
export const START_SLOT_MARGIN = 150;
export const COMPUTE_UNITS = 30_000;
export const MIN_PRIORITY_FEE = 1_000; // micro-lamports per compute unit
export const MAX_PRIORITY_FEE = 1_000_000; // x 30,000 CU = at most 30,000 lamports (0.00003 SOL)
// A wallet returns the signature before the transaction lands, and a transaction whose blockhash
// expires (~60-90 s) never lands. A hint unseen for this long was dropped.
export const SOLANA_HINT_TIMEOUT_S = 180;

/** The SolanaRpc methods the watcher, re-check, restore and readiness use (SolanaRpc; FakeSolanaChain in tests). */
export interface SolanaWatchRpc {
  getSlot(commitment?: string): Promise<number>;
  getSignaturesForAddress(address: string, opts?: {
    until?: string | null; before?: string | null; limit?: number; commitment?: string;
  }): Promise<any[]>;
  getTransaction(sig: string, commitment?: string): Promise<any>;
  getTransactionAny(sig: string, opts?: { min_slot?: number | null; commitment?: string }): Promise<[any, boolean]>;
  getSignatureStatuses(sigs: Iterable<string>): Promise<any>;
  accountExists(address: string): Promise<boolean>;
}

/** The SolanaRpc methods buildPayment uses. */
export interface SolanaBuildRpc {
  getTokenBalance(tokenAccount: string): Promise<bigint>;
  getBalance(address: string): Promise<bigint>;
  getLatestBlockhash(): Promise<string>;
  getPriorityFee(accounts: Iterable<string>): Promise<number>;
}

/** `x or {}` / `x or []` */
const orElse = <T>(v: unknown, fallback: T): any => (pyTruthy(v) ? v : fallback);

/** `x[0]` on a parsed JSON value (only a non-empty list has one here). */
function first(v: unknown): any {
  if (!Array.isArray(v)) throw new TypeError(`'${pyType(v)}' object is not subscriptable by 0`);
  if (v.length === 0) throw new IndexError();
  return v[0];
}

/** The fee payer's pubkey (`accountKeys[0]`, a string or a `{pubkey}` entry), or null if there is none. */
function feePayer(tx: unknown): string | null {
  const keys = orElse(dictGet(orElse(dictGet(orElse(dictGet(tx, "transaction"), {}), "message"), {}), "accountKeys"), []);
  if (!pyTruthy(keys)) return null;
  let head: unknown;
  if (Array.isArray(keys) || typeof keys === "string") head = keys[0];
  else if (typeof keys === "object") throw new KeyError(0); // a dict has no int key 0
  else throw new TypeError(`'${pyType(keys)}' object is not subscriptable`);
  return typeof head === "string" ? head : dictGet(head, "pubkey");
}

/**
 * Python iterates `meta.get(name) or []`; only a list yields balance rows (each item of a
 * string or a dict's keys would fail `.get` right away), so anything else is a TypeError.
 */
function balanceRows(v: unknown): unknown[] {
  if (!pyTruthy(v)) return [];
  if (!Array.isArray(v)) throw new TypeError(`'${pyType(v)}' items have no attribute 'get'`);
  return v;
}

/**
 * What `owner` received of `mint` in a transaction, from its token-balance changes.
 *
 * `accountIndex` must be a JSON number (nodes always send one); Python would also accept an
 * all-string set of indexes, which no node sends.
 */
export function transferTo(tx: unknown, args: { sig: string; mint: string; owner: string }): Transfer | null {
  const { sig, mint, owner } = args;
  const meta = orElse(dictGet(tx, "meta"), {});
  if (dictGet(meta, "err") !== null) return null;

  // Rows are [owner, amount]. `owner` is what the node sent: a base58 string, or absent (null) in
  // replies that omit it; like Python, nothing else is checked here.
  const table = (name: string): Map<number, [string | null, bigint]> => {
    const rows = new Map<number, [string | null, bigint]>();
    for (const b of balanceRows(dictGet(meta, name))) {
      if (dictGet(b, "mint") === mint) {
        // Python evaluates the right-hand side before the subscript key.
        const row: [string | null, bigint] = [dictGet(b, "owner"), pyInt(pyGet(pyGet(b, "uiTokenAmount"), "amount"))];
        const idx = pyGet(b, "accountIndex");
        if (typeof idx !== "number") throw new TypeError(`unsupported accountIndex ${JSON.stringify(idx)}`);
        rows.set(idx, row);
      }
    }
    return rows;
  };

  const pre = table("preTokenBalances");
  const post = table("postTokenBalances");
  let gained = 0n;
  let index: number | null = null;
  const drops = new Map<string | null, bigint>();
  const indexes = [...new Set([...pre.keys(), ...post.keys()])].sort((a, b) => a - b);
  for (const idx of indexes) {
    const who = (post.get(idx) ?? pre.get(idx))![0];
    const delta = (post.get(idx)?.[1] ?? 0n) - (pre.get(idx)?.[1] ?? 0n);
    if (who === owner) {
      gained += delta;
      if (delta > 0n && index === null) index = idx;
    } else if (delta < 0n) {
      // A malformed reply can still carry a dict or list owner: Python's dict key would fail.
      if (typeof who === "object" && who !== null) throw new TypeError(`unhashable type: '${pyType(who)}'`);
      drops.set(who, (drops.get(who) ?? 0n) + delta);
    }
  }
  if (gained <= 0n || index === null) return null;
  let payer: string | null; // Python: None when no account lost tokens and there is no fee payer
  if (drops.size > 0) {
    // Python min(drops, key=drops.get): the first of the largest drops.
    let best: [string | null, bigint] | null = null;
    for (const entry of drops) if (best === null || entry[1] < best[1]) best = entry;
    payer = best![0];
  } else {
    payer = feePayer(tx);
  }
  return {
    token: mint, sender: payer, recipient: owner, value: asUnits(gained), tx_hash: sig, log_index: index,
    block_number: pyIntNumber(pyGet(tx, "slot")),
  };
}

function matchDict(t: Transfer): Record<string, unknown> {
  return {
    tx_hash: t.tx_hash, log_index: t.log_index, block: t.block_number, value: storedInt(t.value, "transfer value"),
    payer: t.sender, recipient: t.recipient,
  };
}

export class SolanaBackend implements Backend {
  readonly family = "solana";
  /** SIG_PAGE for this instance. */
  sig_page = SIG_PAGE;
  /** MAX_SIG_PAGES for this instance. */
  max_sig_pages = MAX_SIG_PAGES;

  normalizeTx(text: unknown): string {
    return normalizeSignature(text);
  }

  makeRpc(chain: Chain, transport: Transport, clock?: () => number): SolanaRpc {
    return new SolanaRpc(new RpcClient(chain.rpc, { transport, clock }));
  }

  /**
   * Unsigned transfer from the player's wallet, for the browser page to hand to the wallet (which
   * simulates it for the player).
   */
  async buildPayment(rec: SessionRecord, args: { rpc: SolanaBuildRpc; chain: Chain; account: string }): Promise<Record<string, unknown>> {
    const { rpc, chain } = args;
    const account = normalizePubkey(args.account);
    const mint: string = pyGet(chain.tokens, pyGet(rec, "token")).address;
    const source = associatedTokenAddress(account, mint);
    const tokenBalance = await rpc.getTokenBalance(source);
    const solBalance = await rpc.getBalance(account);
    const blockhash = await rpc.getLatestBlockhash();
    const fee = await rpc.getPriorityFee([pyGet(rec, "dest")]);
    const price = Math.min(MAX_PRIORITY_FEE, Math.max(MIN_PRIORITY_FEE, fee));
    const instructions = [
      setComputeUnitLimit(COMPUTE_UNITS), setComputeUnitPrice(price),
      transferChecked({
        source, mint, destination: pyGet(rec, "dest"), owner: account, amount: pyGet(rec, "units"), decimals: 6,
        extra: [pyGet(rec, "reference")],
      }),
    ];
    const tx = unsignedTransaction(compileMessage(account, instructions, blockhash));
    return { transaction: base64Encode(tx), token_balance: String(tokenBalance), sol_balance: String(solBalance) };
  }

  async start(args: { rpc: SolanaWatchRpc; chain: Chain; wallet: string | null; token: string; entropy: Entropy }): Promise<Record<string, unknown>> {
    const { rpc, chain, wallet, token, entropy } = args;
    const dest = associatedTokenAddress(wallet as string, pyGet(chain.tokens, token).address);
    const slot = await rpc.getSlot();
    const newest = await rpc.getSignaturesForAddress(dest, { limit: 1 });
    return {
      start_block: Math.max(0, slot - START_SLOT_MARGIN),
      reference: b58encode(entropy.tokenBytes(32)), dest,
      scan_cursor: pyTruthy(newest) ? pyGet(first(newest), "signature") : null,
      scan_before: null,
    };
  }

  // --- watching -----------------------------------------------------------------------------

  async advance(
    pending: SessionStore, receipts: SessionStore, sid: string,
    args: { rpc: SolanaWatchRpc; chain: Chain; wallet: string; items: ReadonlyMap<string, Item>; now: number },
  ): Promise<StepOutcome> {
    const { rpc, chain, wallet, items, now } = args;
    const rec: SessionRecord | null = hasOwn(pending, sid) ? pending[sid] : null; // pending.get(sid)
    const out = makeStepOutcome(sid, "missing", { required: FINAL_CONFIRMATIONS });
    if (rec === null || rec === undefined) return out;
    const mint: string = pyGet(chain.tokens, pyGet(rec, "token")).address;
    try {
      if (pyGet(rec, "state") === "awaiting") {
        out.status = "waiting";
        if (isPastHorizon(rec, now)) {
          removeSession(pending, sid);
          out.status = "closed";
          return out;
        }
        let outcome: string | null = null;
        if (pyTruthy(pyGet(rec, "hint_tx"))) {
          outcome = await this._verifyHint(rec, rpc, chain, mint, wallet, items, receipts, now, out);
        }
        if (outcome === null) {
          // The reference also runs while a hint is pending: the hinted transaction may have been
          // dropped and the player paid again.
          outcome = await this._byReference(rec, rpc, chain, mint, wallet, items, receipts);
          if (outcome !== null) {
            clearHint(rec);
            out.error = null;
            out.hint_tx = null;
          } else if (!pyTruthy(pyGet(rec, "hint_tx")) && (await this._scan(rec, rpc, chain, mint, wallet, receipts))) {
            outcome = "matched";
          }
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
      const status = first(await rpc.getSignatureStatuses([pyGet(m, "tx_hash")]));
      if (status === null) {
        // Unknown to this endpoint: a dropped fork or a lagging endpoint. Keep the signature as a
        // hint so it is verified again rather than lost.
        rec["state"] = "awaiting";
        rec["match"] = null;
        rec["hint_tx"] = pyGet(m, "tx_hash");
        rec["hint_at"] = now;
        out.status = "reorged";
        return out;
      }
      if (dictGet(status, "confirmationStatus") !== "finalized") {
        const confirmations = dictGet(status, "confirmations");
        out.confirmations = pyTruthy(confirmations) ? pyIntNumber(confirmations) : 0;
        return out;
      }
      out.confirmations = FINAL_CONFIRMATIONS;
      const transfer: Transfer = {
        token: mint, sender: pyGet(m, "payer"), recipient: pyGet(m, "recipient"), value: unitsField(m, "value"),
        tx_hash: pyGet(m, "tx_hash"), log_index: pyGet(m, "log_index"), block_number: pyGet(m, "block"),
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
        // A verdict on the hint from earlier in this step wins: the hint is already cleared, and
        // later steps are plain "waiting", so it would never be reported.
        if (out.error === null) out.error = "network_unavailable";
        return out;
      }
      throw e;
    }
  }

  private _qualifies(t: Transfer | null, rec: SessionRecord, chain: Chain, receipts: SessionStore): t is Transfer {
    return t !== null && t.block_number >= numField(rec, "start_block")
      && !hasOwn(receipts, receiptKey(chain.key, t.tx_hash, t.log_index));
  }

  private _verdict(t: Transfer, rec: SessionRecord, items: ReadonlyMap<string, Item>): string {
    rec["match"] = matchDict(t);
    return unitsToDecimal(t.value).cmp(minAmount(rec, items)) < 0 ? "underpaid" : "matched";
  }

  private async _verifyHint(
    rec: SessionRecord, rpc: SolanaWatchRpc, chain: Chain, mint: string, wallet: string, items: ReadonlyMap<string, Item>,
    receipts: SessionStore, now: number, out: StepOutcome,
  ): Promise<string | null> {
    const expired = now - numField(rec, "hint_at") >= SOLANA_HINT_TIMEOUT_S;
    let tx: unknown;
    try {
      tx = await rpc.getTransaction(pyGet(rec, "hint_tx"));
    } catch (e) {
      // an endpoint that keeps refusing must not keep a hint forever
      if (!(e instanceof RpcError) || !expired) throw e;
      tx = null;
    }
    if (tx === null || tx === undefined) { // not visible yet, or dropped: the player may pay again after a while
      if (expired) SolanaBackend._dropHint(rec, out, "tx_not_landed");
      return null;
    }
    if (dictGet(orElse(dictGet(tx, "meta"), {}), "err") !== null) {
      SolanaBackend._dropHint(rec, out, "tx_reverted");
      return null;
    }
    const t = transferTo(tx, { sig: rec["hint_tx"], mint, owner: wallet });
    if (!this._qualifies(t, rec, chain, receipts)) {
      SolanaBackend._dropHint(rec, out, "tx_not_a_payment");
      return null;
    }
    return this._verdict(t, rec, items);
  }

  private static _dropHint(rec: SessionRecord, out: StepOutcome, error: string): void {
    out.error = error;
    out.hint_tx = rec["hint_tx"];
    clearHint(rec);
  }

  /** The reference ties a transaction to this session, so any amount matches. */
  private async _byReference(
    rec: SessionRecord, rpc: SolanaWatchRpc, chain: Chain, mint: string, wallet: string, items: ReadonlyMap<string, Item>,
    receipts: SessionStore,
  ): Promise<string | null> {
    const infos = await rpc.getSignaturesForAddress(pyGet(rec, "reference"), { limit: this.sig_page });
    for (const info of [...asList(infos)].reverse()) {
      if (dictGet(info, "err") !== null) continue;
      const tx = await rpc.getTransaction(pyGet(info, "signature"));
      const t = pyTruthy(tx) ? transferTo(tx, { sig: pyGet(info, "signature"), mint, owner: wallet }) : null;
      if (this._qualifies(t, rec, chain, receipts)) return this._verdict(t, rec, items);
    }
    return null;
  }

  /**
   * Pages of signatures on `address` between `until` (exclusive, older bound) and `before`
   * (exclusive, newer bound), newest first. Also says whether the walk reached `until`; if not,
   * the pages cover only the newest max_sig_pages * sig_page of them.
   *
   * A signature from before `minSlot` also ends the walk (and is dropped): with no cursor, or one
   * this endpoint doesn't know, the walk would otherwise go through the account's whole history.
   */
  private async _walk(
    rpc: SolanaWatchRpc, address: string, until: string | null, before: string | null, minSlot: unknown,
  ): Promise<[any[], boolean]> {
    const found: any[] = [];
    for (let p = 0; p < this.max_sig_pages; p++) {
      const page = asList(await rpc.getSignaturesForAddress(address, { until, before, limit: this.sig_page }));
      for (let n = 0; n < page.length; n++) {
        const info = page[n];
        if (dictGet(info, "slot") !== null) {
          const slot = pyIntNumber(pyGet(info, "slot"));
          if (typeof minSlot !== "number") throw new TypeError(`'<' not supported between int and '${pyType(minSlot)}'`);
          if (slot < minSlot) {
            found.push(...page.slice(0, n));
            return [found, true];
          }
        }
      }
      found.push(...page);
      if (page.length < this.sig_page) return [found, true];
      before = pyGet(page[page.length - 1], "signature");
    }
    // The last page was full too: reached `until` only if nothing older is left.
    const more = await rpc.getSignaturesForAddress(address, { until, before, limit: 1 });
    return [found, !pyTruthy(more)];
  }

  /**
   * Exchange and manual transfers carry no reference: match the exact amount.
   *
   * Signatures are listed newest first, so a backlog larger than one walk is crossed in steps:
   * a walk that cannot reach `scan_cursor` moves `scan_before` back and examines nothing; once a
   * walk reaches the cursor its window is processed oldest first and `scan_before` is cleared.
   * A busy token account only delays detection. Signatures before the start slot are never examined.
   */
  private async _scan(
    rec: SessionRecord, rpc: SolanaWatchRpc, chain: Chain, mint: string, wallet: string, receipts: SessionStore,
  ): Promise<boolean> {
    const [found, reached] = await this._walk(
      rpc, pyGet(rec, "dest"), pyGet(rec, "scan_cursor"), dictGet(rec, "scan_before"), pyGet(rec, "start_block"));
    if (!reached) {
      rec["scan_before"] = pyGet(found[found.length - 1], "signature");
      return false;
    }
    let budget = SCAN_TX_BUDGET;
    for (const info of [...found].reverse()) {
      if (dictGet(info, "err") !== null) {
        rec["scan_cursor"] = pyGet(info, "signature");
        continue;
      }
      if (budget === 0) return false;
      budget -= 1;
      const tx = await rpc.getTransaction(pyGet(info, "signature"));
      if (tx === null || tx === undefined) return false; // listed but not served yet: retry from here next step
      rec["scan_cursor"] = pyGet(info, "signature");
      const t = transferTo(tx, { sig: pyGet(info, "signature"), mint, owner: wallet });
      if (this._qualifies(t, rec, chain, receipts)) {
        // Python reads rec["units"] only for a transfer that qualifies; int == integral number.
        const units = pyGet(rec, "units");
        if (typeof units === "number" && Number.isInteger(units) && t.value === asUnits(BigInt(units))) {
          rec["match"] = matchDict(t);
          return true;
        }
      }
    }
    rec["scan_before"] = null; // window done: look at the newer signatures again
    return false;
  }

  // --- launch re-check, restore, readiness -----------------------------------------------------

  async reverify(receipt: Record<string, unknown>, args: { rpc: SolanaWatchRpc; chain: Chain; wallets: readonly string[] }): Promise<string> {
    const { rpc, chain, wallets } = args;
    const slot = pyIntNumber(pyGet(receipt, "block"));
    const owner = dictGet(receipt, "recipient");
    if (!wallets.includes(owner)) return "drop";
    let tx: unknown;
    let authoritative: boolean;
    try {
      const current = await rpc.getSlot("finalized");
      if (slot > current) return slot > current + FUTURE_TOLERANCE_BLOCKS ? "drop" : "keep";
      [tx, authoritative] = await rpc.getTransactionAny(pyGet(receipt, "tx_hash"), { min_slot: slot });
    } catch (e) {
      if (e instanceof RpcUnavailable || e instanceof RpcError) return "keep";
      throw e;
    }
    if (tx === null || tx === undefined) return authoritative ? "drop" : "keep";
    let t: Transfer | null;
    try {
      t = transferTo(tx, {
        sig: pyGet(receipt, "tx_hash"), mint: pyGet(chain.tokens, pyGet(receipt, "token")).address, owner,
      });
    } catch (e) {
      // KeyError, TypeError, ValueError (AttributeError in Python): a payload this build can't read,
      // so it can't judge the receipt
      if (e instanceof TypeError || e instanceof ValueError || e instanceof KeyError) return "keep";
      throw e;
    }
    const genuine = t !== null && t.block_number === slot
      && t.log_index === pyIntNumber(pyGet(receipt, "log_index"))
      && t.value === amountUnits(pyGet(receipt, "amount"));
    return genuine ? "keep" : "drop";
  }

  async lookup(
    tx: string, args: {
      config: Config; chain: Chain; rpc: SolanaWatchRpc; receipts: SessionStore; on_error?: ((e: unknown) => void) | undefined;
    },
  ): Promise<RestoreCandidate[] | null> {
    const { config, chain, rpc, receipts } = args;
    const [found] = await rpc.getTransactionAny(tx);
    if (found === null || found === undefined) return null;
    if (dictGet(orElse(dictGet(found, "meta"), {}), "err") !== null) throw new RestoreError("tx_reverted");
    const out: RestoreCandidate[] = [];
    for (const [name, token] of Object.entries(chain.tokens)) {
      for (const owner of acceptedWalletsFor(config, "solana")) {
        const t = transferTo(found, { sig: tx, mint: token.address, owner });
        if (t === null) continue;
        const classified = classifyOrSkip(config, t.value, args.on_error);
        if (classified === null) continue;
        const [kind, item, choices] = classified;
        out.push({
          chain: chain.key, token: name, transfer: t, kind, item, choices,
          already_restored: hasOwn(receipts, receiptKey(chain.key, t.tx_hash, t.log_index)),
        });
      }
    }
    if (out.length === 0) throw new RestoreError("tx_not_a_payment");
    return out;
  }

  /** Wallets refuse to pay a wallet with no token account for the mint. */
  async checkReady(args: { rpc: SolanaWatchRpc; chain: Chain; wallet: string | null }): Promise<Record<string, boolean>> {
    const { rpc, chain, wallet } = args;
    const ready: Record<string, boolean> = {};
    for (const [name, token] of Object.entries(chain.tokens)) {
      ready[name] = await rpc.accountExists(associatedTokenAddress(wallet as string, token.address));
    }
    return ready;
  }
}
