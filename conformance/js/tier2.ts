/**
 * Tier 2 runner: replays conformance/vectors/tier2/*.json against the TS Engine. A mirror of
 * conformance/python/scfg_conformance/replay.py (build_config_from, build_engine, run_action,
 * encode_returns, snapshot_json, ScriptedEntropy, ReplayTransport, replay); see the README's
 * "Tier 2" section for the contract.
 *
 * `ENABLED` admits the vectors the TS core can run: since Task 8 (the Solana backend), every one.
 * A vector it ever refuses is reported as skipped, never as passed.
 */
import { AssertionError } from "node:assert";
import { asUnits } from "../../rpgmaker/src/core/amounts.ts";
import { backendFor } from "../../rpgmaker/src/core/backends/index.ts";
import { pyInt } from "../../rpgmaker/src/core/pystr.ts";
import { hexToBytes } from "../../rpgmaker/src/core/bytes.ts";
import type { Chain } from "../../rpgmaker/src/core/chains.ts";
import { TransportError, type Transport } from "../../rpgmaker/src/core/coio.ts";
import { addItem, buildChain, buildConfig, type Config } from "../../rpgmaker/src/core/config.ts";
import { Dec } from "../../rpgmaker/src/core/decimal.ts";
import { Engine, parseUnreadyKey } from "../../rpgmaker/src/core/engine.ts";
import { ConfigError, KeyError } from "../../rpgmaker/src/core/errors.ts";
import { RestoreError, type RestoreCandidate } from "../../rpgmaker/src/core/restore.ts";
import type { Entropy } from "../../rpgmaker/src/core/sessions.ts";
import { firstDiff, jsonEqual, safeStringify } from "./json.ts";

export const ACTIONS: readonly string[] = [
  "start_session", "build_solana_payment", "report_hint", "set_ui_status", "discard_session", "tick", "reverify",
  "restore_lookup", "restore_apply",
];

export function ENABLED(_vector: any): boolean {
  return true;
}

// --- values that JSON cannot carry exactly -------------------------------------------------------

/** `{"float": repr}` -> a JS number. An integral float (`3.0`) has no JS form distinct from an int: refuse loudly. */
export function decodeValue(v: unknown): unknown {
  if (typeof v === "object" && v !== null && !Array.isArray(v) && Object.keys(v).length === 1 && "float" in v) {
    const n = Number((v as { float: string }).float);
    if (Number.isInteger(n)) throw new AssertionError({ message: `integral float ${(v as any).float} cannot be expressed in JS` });
    return n;
  }
  return v;
}

function decoded(d: Record<string, unknown>): Record<string, any> {
  const out: Record<string, any> = {};
  for (const [k, v] of Object.entries(d)) out[k] = decodeValue(v);
  return out;
}

// --- engine -------------------------------------------------------------------------------------

export function buildConfigFrom(setup: any): Config {
  const kw = decoded(setup.config);
  if (setup.custom_chains.length > 0) {
    kw["custom_chains"] = setup.custom_chains.map((c: Record<string, unknown>) => {
      const { key, ...rest } = decoded(c);
      return buildChain(key, rest as never);
    });
  }
  const config = buildConfig(kw);
  for (const item of setup.items) {
    const { item_id, ...rest } = decoded(item);
    addItem(config, item_id, rest as never);
  }
  return config;
}

/** The Engine of a vector's setup. One clock drives the Engine and every RpcClient. */
export function buildEngine(
  setup: any, opts: { transport: Transport; clock: () => number; on_error: (e: unknown) => void; entropy: Entropy },
): Engine {
  const rpc_factory = (chain: Chain) => {
    const rpc = backendFor(chain).makeRpc(chain, opts.transport, opts.clock);
    ("client" in rpc ? rpc.client : rpc).clock = opts.clock; // SolanaRpc wraps its RpcClient
    return rpc;
  };
  return new Engine(buildConfigFrom(setup), {
    transport: opts.transport, pending: JSON.parse(JSON.stringify(setup.pending)),
    receipts: JSON.parse(JSON.stringify(setup.receipts)), clock: opts.clock, rpc_factory, on_error: opts.on_error,
    entropy: opts.entropy,
  });
}

// --- restore candidates ---------------------------------------------------------------------------

const TRANSFER_FIELDS = ["token", "sender", "recipient", "value", "tx_hash", "log_index", "block_number"];
const TRANSFER_CLASSES: Record<string, readonly string[]> = {
  Transfer: TRANSFER_FIELDS,
  TransferLog: [...TRANSFER_FIELDS, "block_hash"],
};

/** candidate_to_json: the candidate as plain JSON, its transfer tagged with the Python class name. */
export function candidateToJson(c: RestoreCandidate): any {
  const out = toJsonable(c, "") as any;
  out.transfer["class"] = "block_hash" in c.transfer ? "TransferLog" : "Transfer";
  return out;
}

/** candidate_from_json: the inverse; `value` becomes a bigint again. Wrong fields are a TypeError like Python's. */
export function candidateFromJson(d: any): RestoreCandidate {
  const { class: cls, ...fields } = d.transfer;
  if (!Object.prototype.hasOwnProperty.call(TRANSFER_CLASSES, cls)) throw new KeyError(cls);
  const want = TRANSFER_CLASSES[cls]!;
  if (Object.keys(fields).length !== want.length || !want.every((k) => k in fields)) {
    throw new TypeError(`${cls}() got the wrong fields: ${Object.keys(fields).join(", ")}`);
  }
  return {
    chain: d.chain, token: d.token, transfer: { ...fields, value: asUnits(BigInt(fields["value"])) }, kind: d.kind, item: d.item,
    choices: [...d.choices], already_restored: d.already_restored,
  };
}

// --- actions --------------------------------------------------------------------------------------

export type Raises = null | { type: string; code?: string };

/** Run one action: [result, raises]; raises is null or the vector's `raises` value. */
export async function runAction(engine: Engine, action: string, args: Record<string, unknown>, steps: any[]): Promise<[unknown, Raises]> {
  const a = decoded(args);
  let result: unknown;
  try {
    if (action === "start_session") {
      const { kind, chain, ...rest } = a;
      result = await engine.startSession(kind, chain, rest);
    } else if (action === "build_solana_payment") {
      result = await engine.buildSolanaPayment(a["sid"], a["account"]);
    } else if (action === "report_hint") {
      result = engine.reportHint(a["sid"], a["tx"]);
    } else if (action === "set_ui_status") {
      result = engine.setUiStatus(a["sid"], a["status"]);
    } else if (action === "discard_session") {
      result = engine.discardSession(a["sid"]);
    } else if (action === "tick") {
      result = await engine.tick();
    } else if (action === "reverify") {
      result = await engine.reverify();
    } else if (action === "restore_lookup") {
      result = await engine.restoreLookup(a["tx"]);
    } else if (action === "restore_apply") {
      const candidates = (steps[a["from_step"]].returns as any[]).map(candidateFromJson);
      let choices: Record<number, string> | null = a["choices"] as never;
      if (choices !== null && choices !== undefined) {
        const keyed: Record<number, string> = {};
        for (const [k, v] of Object.entries(choices)) keyed[Number(pyInt(k))] = v as string; // int(k)
        choices = keyed;
      }
      result = engine.restoreApply(candidates, choices);
    } else {
      throw new Error(`unknown action ${JSON.stringify(action)}`);
    }
  } catch (e) {
    if (e instanceof ConfigError) return [null, { type: "ConfigError", code: e.code }];
    if (e instanceof RestoreError) return [null, { type: "RestoreError", code: e.code }];
    return [null, { type: "other" }];
  }
  return [result, null];
}

const MAX_SAFE = 2n ** 53n;

function where(path: string): string {
  return path || "<root>";
}

/** canonical.to_jsonable: plain JSON, with the vector rules for numbers, Decimals and sets. */
function toJsonable(v: unknown, path: string): unknown {
  if (v === undefined) throw new Error(`undefined at ${where(path)}: only a top-level void return maps to null`);
  if (v === null) return null;
  if (typeof v === "boolean" || typeof v === "string") return v;
  if (typeof v === "bigint") {
    if (v >= MAX_SAFE || v <= -MAX_SAFE) throw new Error(`integer ${v} at ${where(path)} is not safe in JSON (|n| >= 2**53)`);
    return Number(v);
  }
  if (typeof v === "number") {
    if (!Number.isInteger(v)) throw new Error(`non-integral number ${v} at ${where(path)}`);
    if (Math.abs(v) >= 2 ** 53) throw new Error(`integer ${v} at ${where(path)} is not safe in JSON (|n| >= 2**53)`);
    return v;
  }
  if (v instanceof Dec) return String(v);
  if (v instanceof Uint8Array) throw new Error(`bytes at ${where(path)}: hex-encode into a *_hex field`);
  if (Array.isArray(v)) return v.map((x, i) => toJsonable(x, `${path}[${i}]`));
  if (v instanceof Set) {
    return [...v].map((x) => toJsonable(x, `${path}[]`)).sort((x, y) => (String(x) < String(y) ? -1 : String(x) > String(y) ? 1 : 0));
  }
  if (typeof v === "object" && Object.getPrototypeOf(v) === Object.prototype) {
    const out: Record<string, unknown> = {};
    for (const [k, x] of Object.entries(v)) {
      Object.defineProperty(out, k, {
        value: toJsonable(x, path ? `${path}.${k}` : k), enumerable: true, writable: true, configurable: true,
      });
    }
    return out;
  }
  throw new Error(`unsupported value ${safeStringify(v)} at ${where(path)}`);
}

export function encodeReturns(action: string, result: unknown): unknown {
  if (result === undefined) return null; // a void action (report_hint, ...) returns None
  if (action === "restore_lookup" && result !== null) return (result as RestoreCandidate[]).map(candidateToJson);
  return toJsonable(result, "");
}

/** pending, receipts, owned (sorted), tip_total (str), unready (sorted [chain, token]). */
export function snapshotJson(engine: Pick<Engine, "snapshot">): unknown {
  const snap = engine.snapshot();
  const pairs = [...snap.unready].map(parseUnreadyKey);
  const cmp = (x: string, y: string) => (x < y ? -1 : x > y ? 1 : 0);
  pairs.sort((x, y) => cmp(x[0], y[0]) || cmp(x[1], y[1]));
  return {
    pending: toJsonable(snap.pending, "pending"),
    receipts: toJsonable(snap.receipts, "receipts"),
    owned: toJsonable(snap.owned, "owned"),
    tip_total: String(snap.tip_total),
    unready: pairs,
  };
}

// --- entropy --------------------------------------------------------------------------------------

/** Hands out one step's recorded draws in order; any difference is a problem. */
export class ScriptedEntropy implements Entropy {
  private draws: { fn: string; n: number; value: any }[];
  problems: string[] = [];

  constructor(draws: { fn: string; n: number; value: any }[]) {
    this.draws = [...draws];
  }

  private take(fn: string, n: number): any {
    const draw = this.draws.shift();
    if (draw === undefined) {
      this.problems.push(`entropy exhausted at ${fn}(${n})`);
      throw new AssertionError({ message: this.problems[this.problems.length - 1]! });
    }
    if (draw.fn !== fn || draw.n !== n) {
      this.problems.push(`entropy mismatch: drew ${fn}(${n}), recorded ${draw.fn}(${draw.n})`);
      throw new AssertionError({ message: this.problems[this.problems.length - 1]! });
    }
    return draw.value;
  }

  tokenHex(n: number): string {
    return this.take("token_hex", n);
  }

  randBelow(n: number): number {
    return this.take("randbelow", n);
  }

  tokenBytes(n: number): Uint8Array {
    return hexToBytes(this.take("token_bytes", n));
  }

  leftover(): string[] {
    if (this.draws.length === 0) return [];
    return [`unconsumed entropy: ${JSON.stringify(this.draws.map((d) => [d.fn, d.n]))}`];
  }
}

/** Swaps the draws per step while the Engine keeps one Entropy object. */
class StepEntropy implements Entropy {
  current = new ScriptedEntropy([]);
  tokenHex(n: number): string { return this.current.tokenHex(n); }
  randBelow(n: number): number { return this.current.randBelow(n); }
  tokenBytes(n: number): Uint8Array { return this.current.tokenBytes(n); }
}

// --- transport ------------------------------------------------------------------------------------

interface RecordedCall {
  url: string;
  method: string;
  params: unknown;
  response_text?: string;
  transport_error?: { message: string; status: number | null };
}

/**
 * Answers each request from the current step's recorded calls, as a multiset: any order, each
 * recorded call used once, identical requests take their copies in recorded order. The JSON-RPC
 * id is ignored; params match by JSON equality (`true` is not `1`).
 */
export class ReplayTransport implements Transport {
  private calls: RecordedCall[] = [];
  private used: boolean[] = [];
  problems: string[] = [];

  load(calls: RecordedCall[]): void {
    this.calls = [...calls];
    this.used = this.calls.map(() => false);
    this.problems = [];
  }

  async postJson(url: string, payload: any, _timeout: number): Promise<unknown> {
    for (let i = 0; i < this.calls.length; i++) {
      const c = this.calls[i]!;
      if (!this.used[i] && c.url === url && c.method === payload.method && jsonEqual(c.params, payload.params)) {
        this.used[i] = true;
        if (c.transport_error !== undefined) throw new TransportError(c.transport_error.message, c.transport_error.status);
        return JSON.parse(c.response_text!);
      }
    }
    this.problems.push(`unexpected call ${url} ${payload.method} ${safeStringify(payload.params)}`);
    throw new AssertionError({ message: this.problems[this.problems.length - 1]! });
  }

  leftover(): string[] {
    return this.calls.filter((_, i) => !this.used[i])
      .map((c) => `expected call not made: ${c.url} ${c.method} ${safeStringify(c.params)}`);
  }
}

// --- replay ---------------------------------------------------------------------------------------

function fail(message: string): never {
  throw new AssertionError({ message });
}

/** Re-run a Tier 2 vector; AssertionError("step {i} ({action}): ...") on any difference. */
export async function replay(vector: any): Promise<void> {
  if (vector?.tier !== 2) throw new Error("not a Tier 2 vector");
  const setup = vector.setup;
  let now: number = setup.clock;
  let errors = 0;
  const transport = new ReplayTransport();
  const entropy = new StepEntropy();
  const engine = buildEngine(setup, {
    transport, clock: () => now, on_error: () => { errors += 1; }, entropy,
  });
  const steps: any[] = vector.steps;
  for (let i = 0; i < steps.length; i++) {
    const step = steps[i];
    const at = `step ${i} (${step.action})`;
    now = step.at;
    errors = 0;
    transport.load(step.calls);
    entropy.current = new ScriptedEntropy(step.entropy);
    const [result, raises] = await runAction(engine, step.action, step.args, steps.slice(0, i));
    const problems = [...transport.problems, ...transport.leftover(), ...entropy.current.problems, ...entropy.current.leftover()];
    if (problems.length > 0) fail(`${at}: ${problems.join("; ")}`);
    if (!jsonEqual(raises, step.raises)) fail(`${at}: raises: recorded ${safeStringify(step.raises)}, got ${safeStringify(raises)}`);
    const returns = raises ? null : encodeReturns(step.action, result);
    const rd = firstDiff(step.returns, returns);
    if (rd !== "") fail(`${at}: returns: ${rd} (recorded vs got)`);
    if (errors !== step.internal_errors) fail(`${at}: internal_errors: recorded ${step.internal_errors}, got ${errors}`);
    const sd = firstDiff(step.snapshot, snapshotJson(engine));
    if (sd !== "") fail(`${at}: snapshot: ${sd} (recorded vs got)`);
  }
}
