/**
 * JSON-lines driver: runs the TypeScript Engine as a child process for the Python integration
 * tests (tests/integration/js_driver.py), so the same Anvil / solana-test-validator fixtures can
 * exercise both cores.
 *
 * Protocol: one JSON command per stdin line, one JSON reply per stdout line, in order:
 *   {"ok": <result>}  or  {"error": {"type", "code", "message"}}
 * Nothing else is ever written to stdout (diagnostics go to stderr). Commands:
 *   init     {override, config, custom_chains?, items}  patch MAINNET/TESTNET[key], build Engine + FetchTransport
 *            override = {network: "mainnet"|"testnet", key, rpc: [url], confirmations?, tokens?: {name: address}}
 *   start_session {kind, chain, item?, amount?, token?, title?}   report_hint {sid, tx}
 *   tick   reverify   restore_lookup {tx}   restore_apply {candidates, choices}
 *   build_solana_payment {sid, account}   snapshot
 * Test-only helpers for the receipt tampering the Python tests do on `engine.receipts`:
 *   set_receipt {key, receipt}   clear_receipts
 * Results are encoded exactly like the Tier 2 `returns` / `snapshot` (conformance/js/tier2.ts).
 */
import { createInterface } from "node:readline";
import { MAINNET, TESTNET, makeToken, type Chain } from "../../src/core/chains.ts";
import { FetchTransport } from "../../src/core/coio.ts";
import { Engine } from "../../src/core/engine.ts";
import { buildConfigFrom, candidateFromJson, encodeReturns, snapshotJson } from "../../../conformance/js/tier2.ts";

class DriverError extends Error {
  code: string | null;
  constructor(message: string, code: string | null = null) {
    super(message);
    this.name = "DriverError";
    this.code = code;
  }
}

function applyOverride(o: any): void {
  const registry: Record<string, Chain> = o.network === "testnet" ? TESTNET : MAINNET;
  const base = registry[o.key];
  if (base === undefined) throw new DriverError(`unknown chain ${JSON.stringify(o.key)}`);
  const next: Chain = { ...base, rpc: [...o.rpc] };
  if (o.confirmations !== undefined && o.confirmations !== null) next.confirmations = o.confirmations;
  if (o.tokens) {
    next.tokens = Object.fromEntries(Object.entries(o.tokens as Record<string, string>).map(([k, a]) => [k, makeToken(a)]));
  }
  registry[o.key] = next;
}

let engine: Engine | null = null;

function need(): Engine {
  if (engine === null) throw new DriverError("init first");
  return engine;
}

async function run(cmd: any): Promise<unknown> {
  const op = cmd.op;
  if (op === "init") {
    if (cmd.override) applyOverride(cmd.override);
    const config = buildConfigFrom({ config: cmd.config, custom_chains: cmd.custom_chains ?? [], items: cmd.items ?? [] });
    engine = new Engine(config, {
      transport: new FetchTransport(),
      // Whole seconds: the Tier 2 encoders (reused below) refuse the fractional timestamps of a wall clock.
      clock: () => Math.floor(Date.now() / 1000),
      on_error: (e) => process.stderr.write(`engine error: ${e instanceof Error ? (e.stack ?? e.message) : String(e)}\n`),
    });
    return null;
  }
  const e = need();
  switch (op) {
    case "start_session": {
      const { op: _op, kind, chain, ...rest } = cmd;
      return encodeReturns(op, await e.startSession(kind, chain, rest));
    }
    case "report_hint":
      return encodeReturns(op, e.reportHint(cmd.sid, cmd.tx));
    case "tick":
      return encodeReturns(op, await e.tick());
    case "reverify":
      return encodeReturns(op, await e.reverify());
    case "restore_lookup":
      return encodeReturns(op, await e.restoreLookup(cmd.tx));
    case "restore_apply": {
      let choices: Record<number, string | null> | null = null;
      if (cmd.choices) {
        choices = {};
        for (const [k, v] of Object.entries(cmd.choices as Record<string, string | null>)) choices[Number(k)] = v;
      }
      return encodeReturns(op, e.restoreApply((cmd.candidates as any[]).map(candidateFromJson), choices));
    }
    case "build_solana_payment":
      return encodeReturns(op, await e.buildSolanaPayment(cmd.sid, cmd.account));
    case "snapshot":
      return snapshotJson(e);
    case "set_receipt":
      e.receipts[cmd.key] = cmd.receipt;
      return null;
    case "clear_receipts":
      for (const k of Object.keys(e.receipts)) delete e.receipts[k];
      return null;
    default:
      throw new DriverError(`unknown op ${JSON.stringify(op)}`);
  }
}

/** The error as JSON: its class, its `code` (ConfigError/RestoreError strings, RpcError numbers) or null. */
function describe(err: unknown): { type: string; code: string | number | null; message: string } {
  if (err instanceof Error) {
    const code = (err as { code?: unknown }).code;
    const keep = typeof code === "string" || (typeof code === "number" && Number.isFinite(code));
    return { type: err.constructor.name, code: keep ? code : null, message: err.message };
  }
  return { type: "other", code: null, message: String(err) };
}

const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
for await (const line of rl) {   // sequential: the next command waits for the previous reply
  if (line.trim() === "") continue;
  let reply: string;
  try {
    reply = JSON.stringify({ ok: await run(JSON.parse(line)) ?? null });
  } catch (err) {
    reply = JSON.stringify({ error: describe(err) });
  }
  process.stdout.write(reply + "\n");
}
