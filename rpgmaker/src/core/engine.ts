/**
 * The Engine composes config, sessions, watcher, receipts and restore (twin of scfg_core/engine.py).
 *
 * It owns all plugin state (deep copies of what the game persists) and is NOT re-entrant: only
 * the single-writer worker (Task 9) calls it, one command at a time. Readers get Snapshots, which
 * are detached copies. Methods that need the network are async; the others are plain.
 */
import { backendFor } from "./backends/index.ts";
import type { Chain } from "./chains.ts";
import type { Transport } from "./coio.ts";
import { acceptedWalletsFor, enabledChains, registry, walletFor, type Config } from "./config.ts";
import { deepCopy } from "./copy.ts";
import type { Dec } from "./decimal.ts";
import { ConfigError, KeyError, ValueError, printError } from "./errors.ts";
import { makePaymentResult } from "./models.ts";
import { MIN_TIP, preparePayment } from "./payments.ts";
import { computeOwnership } from "./receipts.ts";
import { apply as applyRestore, lookup as lookupRestore, type RestoreCandidate } from "./restore.ts";
import { reverifyReceipts } from "./reverify.ts";
import { RpcError, RpcUnavailable } from "./rpc.ts";
import { hasOwn, pyGet, pyTruthy } from "./pystr.ts";
import {
  createSession, defaultEntropy, isPastHorizon, isPastWindow, removeSession, setHint, setUiStatus,
  type Entropy, type SessionRecord, type SessionStore,
} from "./sessions.ts";
import { makeStepOutcome, type StepOutcome } from "./watcher.ts";
import { START_BLOCK_MARGIN } from "./backends/evm.ts";

export { MIN_TIP, START_BLOCK_MARGIN };

/**
 * The `Snapshot.unready` key of a (chain, token) route. Python keeps `(chain, token)` tuples in a
 * frozenset; a JS Set needs a primitive, so the two are joined by NUL (never part of a chain key
 * or token name).
 */
export function unreadyKey(chain: string, token: string): string {
  return `${chain}\u0000${token}`;
}

/** Inverse of `unreadyKey`: `[chain, token]`. ValueError for a string that is not such a key. */
export function parseUnreadyKey(key: string): [string, string] {
  const i = key.indexOf("\u0000");
  if (i < 0) throw new ValueError(`not an unready key: ${JSON.stringify(key)}`);
  return [key.slice(0, i), key.slice(i + 1)];
}

/** What readers see: detached copies. `unready` holds `unreadyKey(chain, token)` keys. */
export interface Snapshot {
  pending: SessionStore;
  receipts: Record<string, any>;
  owned: Set<string>;
  tip_total: Dec;
  unready: Set<string>;
}

export interface EngineOptions {
  transport?: Transport;
  pending?: SessionStore | null;
  receipts?: Record<string, any> | null;
  clock?: () => number;
  rpc_factory?: (chain: Chain) => any;
  on_error?: (e: unknown) => void;
  entropy?: Entropy;
}

export interface StartSessionOptions {
  item?: string | null;
  amount?: unknown;
  token?: string | null;
  title?: string | null;
}



export class Engine {
  config: Config;
  on_error: (e: unknown) => void;
  pending: SessionStore;
  receipts: Record<string, any>;
  clock: () => number;
  transport: Transport | undefined;
  entropy: Entropy;
  unready: Set<string>;
  private _rpc_factory: (chain: Chain) => any;
  private _rpcs: Map<string, any>;

  constructor(config: Config, opts: EngineOptions = {}) {
    this.config = config;
    this.on_error = opts.on_error ?? printError;
    this.pending = deepCopy({ ...(opts.pending ?? {}) });
    this.receipts = deepCopy({ ...(opts.receipts ?? {}) });
    this.clock = opts.clock ?? (() => Date.now() / 1000);
    this.transport = opts.transport;
    this.entropy = opts.entropy ?? defaultEntropy;
    this._rpc_factory = opts.rpc_factory ?? ((chain) => {
      if (this.transport === undefined) throw new TypeError("Engine needs a transport to build RPC clients");
      return backendFor(chain).makeRpc(chain, this.transport, this.clock);
    });
    this._rpcs = new Map();
    this.unready = new Set();
  }

  // --- chains & RPC ---------------------------------------------------------

  /** Full registry chain (all tokens, so old receipts stay verifiable) with overrides. */
  chain(key: string): Chain {
    const base = registry(this.config).get(key);
    if (base === undefined) throw new KeyError(key); // Python: config.registry[key]
    return {
      ...base,
      rpc: [...(hasOwn(this.config.rpc_urls, key) ? this.config.rpc_urls[key]! : []), ...base.rpc],
      confirmations: hasOwn(this.config.confirmations, key) ? this.config.confirmations[key]! : base.confirmations,
    };
  }

  rpc(key: string): any {
    if (!this._rpcs.has(key)) this._rpcs.set(key, this._rpc_factory(this.chain(key)));
    return this._rpcs.get(key);
  }

  // --- payments ---------------------------------------------------------------

  async startSession(kind: string, chain: string, opts: StartSessionOptions = {}): Promise<SessionRecord> {
    for (const k of Object.keys(opts)) {
      if (!["item", "amount", "token", "title"].includes(k)) {
        throw new TypeError(`start_session() got an unexpected keyword argument '${k}'`);
      }
    }
    const spec = preparePayment(this.config, kind, {
      item: opts.item, amount: opts.amount, token: opts.token, title: opts.title,
    });
    if (!spec.chains.includes(chain)) {
      throw new ConfigError(`${spec.token} is not available on chain ${JSON.stringify(chain)}`,
        { code: "chain.token_unavailable" });
    }
    const chainObj = this.chain(chain);
    const fields = await backendFor(chainObj).start({
      rpc: this.rpc(chain), chain: chainObj, wallet: walletFor(this.config, chainObj), token: spec.token,
      entropy: this.entropy,
    });
    const start = pyGet(fields, "start_block");
    delete fields["start_block"];
    const rec = createSession(this.pending, {
      kind: spec.kind, item: spec.item, title: spec.title, token: spec.token, chain, price: spec.price,
      code: spec.code, start_block: start, now: this.clock(), extra: fields,
    }, this.entropy);
    return deepCopy(rec);
  }

  /** The unsigned transaction a browser wallet signs for a Solana session (`account` pays). */
  async buildSolanaPayment(sid: string, account: string): Promise<Record<string, unknown>> {
    const rec = pyGet(this.pending, sid);
    const chain = this.chain(pyGet(rec, "chain"));
    if (chain.family !== "solana") throw new ValueError("not a Solana session");
    return backendFor(chain).buildPayment!(rec, { rpc: this.rpc(chain.key), chain, account });
  }

  reportHint(sid: string, tx_hash: unknown): void {
    const rec = pyGet(this.pending, sid);
    const backend = backendFor(this.chain(pyGet(rec, "chain")));
    setHint(this.pending, sid, tx_hash, this.clock(), (t) => backend.normalizeTx(t));
  }

  setUiStatus(sid: string, status: unknown): void {
    if (hasOwn(this.pending, sid)) setUiStatus(this.pending, sid, status);
  }

  /** Drop a session the player abandoned before paying (e.g. switched network). */
  discardSession(sid: string): void {
    const rec = hasOwn(this.pending, sid) ? this.pending[sid] : null;
    if (rec !== null && rec !== undefined && pyGet(rec, "state") === "awaiting" && !pyTruthy(pyGet(rec, "hint_tx"))) {
      delete this.pending[sid];
    }
  }

  /** Every session, one after another in pending order (never concurrently), then the expiries. */
  async tick(): Promise<StepOutcome[]> {
    const now = this.clock();
    const outcomes: StepOutcome[] = [];
    for (const [sid, rec] of Object.entries(this.pending)) {
      const outcome = await this._advance(sid, rec, now);
      if (outcome !== null) outcomes.push(outcome);
    }
    outcomes.push(...this._expire(now));
    return outcomes;
  }

  /**
   * One session's step. A session this build can no longer watch (its chain left the registry,
   * or the game stopped accepting that family) is kept until its scan horizon, then closed.
   * A crash in one session never stops the others.
   */
  private async _advance(sid: string, rec: SessionRecord, now: number): Promise<StepOutcome | null> {
    try {
      const key = pyGet(rec, "chain");
      if (typeof key === "object" && key !== null) throw new TypeError("unhashable chain key");
      const known = typeof key === "string" && registry(this.config).has(key);
      if (!known || walletFor(this.config, this.chain(key)) === null) {
        if (!isPastHorizon(rec, now)) return null;
        removeSession(this.pending, sid);
        return makeStepOutcome(sid, "closed");
      }
      const chain = this.chain(key);
      return await backendFor(chain).advance(this.pending, this.receipts, sid, {
        rpc: this.rpc(key), chain, wallet: walletFor(this.config, chain)!, items: this.config.items, now,
      });
    } catch (e) {
      this.on_error(e);
      return makeStepOutcome(sid, "waiting", { error: "internal_error" });
    }
  }

  /** Report the 30-minute payment window closing (scanning continues up to 24 h). */
  private _expire(now: number): StepOutcome[] {
    const out: StepOutcome[] = [];
    for (const [sid, rec] of Object.entries(this.pending)) {
      const ui = pyGet(rec, "ui_status");
      if ((ui === "open" || ui === "pending") && isPastWindow(rec, now)) {
        rec["ui_status"] = "expired";
        out.push(makeStepOutcome(sid, "expired", {
          result: makePaymentResult({
            status: "expired", kind: pyGet(rec, "kind"), item: pyGet(rec, "item"), token: pyGet(rec, "token"),
            chain: pyGet(rec, "chain"), session_id: sid,
          }),
        }));
      }
    }
    return out;
  }

  // --- receipts ---------------------------------------------------------------

  /**
   * Launch checks: re-verify stored receipts, then check that the developer can receive each
   * enabled token (Solana needs a token account). Returns the removed receipt keys.
   */
  async reverify(): Promise<string[]> {
    const chains: Record<string, Chain> = {};
    for (const k of registry(this.config).keys()) chains[k] = this.chain(k);
    const wallets: Record<string, string[]> = {};
    for (const f of ["evm", "solana"]) wallets[f] = acceptedWalletsFor(this.config, f);
    const removed = await reverifyReceipts(this.receipts, { rpc_for: (k) => this.rpc(k), chains, wallets, on_error: (e) => this._report(e) });
    await this._checkReady();
    return removed;
  }

  private async _checkReady(): Promise<void> {
    const unready = new Set<string>();
    for (const [key, chain] of enabledChains(this.config)) {
      const wallet = walletFor(this.config, chain);
      let ready: Record<string, boolean>;
      try {
        ready = await backendFor(chain).checkReady({ rpc: this._lazyRpc(key), chain, wallet });
      } catch (e) {
        if (e instanceof RpcUnavailable || e instanceof RpcError) continue; // never hide a payment option on doubt
        this.on_error(e);
        continue;
      }
      for (const [token, ok] of Object.entries(ready)) if (!ok) unready.add(unreadyKey(key, token));
    }
    this.unready = unready;
  }

  /**
   * Defers building a chain's RPC client until a backend actually uses it, so chains whose
   * readiness needs no network (EVM) never construct a client.
   */
  private _lazyRpc(key: string): any {
    return new Proxy({}, {
      get: (_t, name) => {
        const rpc = this.rpc(key);
        const v = rpc[name];
        return typeof v === "function" ? v.bind(rpc) : v;
      },
    });
  }

  async restoreLookup(tx_hash: string): Promise<RestoreCandidate[]> {
    const reg = registry(this.config);
    const ordered = [...this.config.chains, ...[...reg.keys()].filter((k) => !this.config.chains.includes(k))];
    const chains = new Map<string, Chain>();
    for (const k of ordered) chains.set(k, this.chain(k));
    return lookupRestore(tx_hash, {
      config: this.config, chains, rpc_for: (k) => this.rpc(k), receipts: this.receipts, on_error: (e) => this._report(e),
    });
  }

  /**
   * The on_error handed to reverify and restoreLookup, which report as they go: it calls
   * `on_error` as it is when the error is reported, not as it was when the pass started (the
   * Godot autoload replaces its handler at quit, with a pass possibly in flight). A handler
   * unset by then (null, or anything falsy) prints the error, as `on_error || printError` does.
   */
  private _report(e: unknown): void {
    (this.on_error || printError)(e);
  }

  restoreApply(candidates: RestoreCandidate[], choices: Record<number | string, string | null> | null = null): Record<string, any>[] {
    return applyRestore(this.receipts, candidates, choices);
  }

  snapshot(): Snapshot {
    const [owned, tips] = computeOwnership(this.receipts, this.config.items);
    return {
      pending: deepCopy(this.pending), receipts: deepCopy(this.receipts), owned: new Set(owned), tip_total: tips,
      unready: new Set(this.unready),
    };
  }
}
