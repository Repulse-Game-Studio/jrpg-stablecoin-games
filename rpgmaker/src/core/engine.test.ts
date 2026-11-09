/** Twin of tests/unit/test_engine.py. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import { FakeClock } from "../../test/rpc_fakes.ts";
import { MAINNET, type Chain } from "./chains.ts";
import type { Transport } from "./coio.ts";
import { addItem, buildConfig, type BuildConfigOptions } from "./config.ts";
import { Dec } from "./decimal.ts";
import { MIN_TIP, START_BLOCK_MARGIN, Engine } from "./engine.ts";
import { ConfigError, ValueError } from "./errors.ts";
import { RestoreError } from "./restore.ts";
import { RpcClient } from "./rpc.ts";
import type { Entropy } from "./sessions.ts";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const USDC_ARB = MAINNET["arbitrum"]!.tokens["USDC"]!.address;
const TX = "0x" + "ab".repeat(32);

function makeEngine(cfgKw: BuildConfigOptions = {}) {
  const cfg = buildConfig({ wallet: DEV, ...cfgKw });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const chains: Record<string, FakeChain> = {};
  const factory = (chain: Chain) => {
    chains[chain.key] ??= new FakeChain(1000);
    chains[chain.key]!.chain_obj = chain;
    return chains[chain.key]!;
  };
  const clock = new FakeClock();
  clock.now = 100.0;
  return { e: new Engine(cfg, { rpc_factory: factory, clock: clock.call }), chains, clock };
}

test("test_start_unlock_session_uses_item_defaults", async () => {
  const { e } = makeEngine();
  const rec = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  assert.deepEqual([rec.kind, rec.item, rec.token, rec.title, rec.code], ["unlock", "chapter2", "USDC", "Chapter 2", 37]);
  assert.equal(rec.price, "4.99");
  assert.equal(rec.start_block, 1000 - START_BLOCK_MARGIN);
  assert.equal(rec.created_at, 100.0);
  assert.ok(rec.id in e.pending);
});

test("test_start_session_returns_a_copy", async () => {
  const { e } = makeEngine();
  const rec = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  rec.state = "hacked";
  assert.equal(e.pending[rec.id].state, "awaiting");
});

test("test_start_tip_session", async () => {
  const { e } = makeEngine();
  const rec = await e.startSession("tip", "base", { amount: "3", token: "USDC" });
  assert.deepEqual([rec.code, rec.title, rec.price, rec.item], [0, "Tip", "3.00", null]);
});

const REJECTS: [string, string, Record<string, unknown>][] = [
  ["unlock", "arbitrum", { item: "missing" }],
  ["unlock", "arbitrum", { item: "chapter2", amount: "1.00" }], // below min_price
  ["tip", "arbitrum", { token: "USDC" }], // no amount
  ["tip", "arbitrum", { amount: "1" }], // no token
  ["tip", "arbitrum", { amount: String(MIN_TIP.sub(Dec.parse("0.01") as Dec)), token: "USDC" }],
  ["refund", "arbitrum", {}],
  ["tip", "base", { amount: "3", token: "USDT" }], // no USDT on base
  ["tip", "solana", { amount: "3", token: "USDC" }],
];
REJECTS.forEach(([kind, chain, kw], i) => {
  test(`test_start_session_rejects[${i}]`, async () => {
    const { e } = makeEngine();
    await assert.rejects(e.startSession(kind, chain, kw), ConfigError);
    assert.deepEqual(e.pending, {});
  });
});

test("test_start_session_rejects_float_amount", async () => {
  // Python passes 3.0; a JS number cannot be an integral float, so a non-integral one stands in.
  const { e } = makeEngine();
  await assert.rejects(e.startSession("tip", "base", { amount: 3.5, token: "USDC" }), TypeError);
});

test("test_start_session_rejects_unknown_keyword", async () => {
  const { e } = makeEngine();
  await assert.rejects(e.startSession("tip", "base", { amount: "3", token: "USDC", colour: "red" } as never), TypeError);
});

test("test_start_session_rejects_disabled_chain", async () => {
  const { e } = makeEngine({ chains: ["base"] });
  await assert.rejects(e.startSession("unlock", "arbitrum", { item: "chapter2" }), ConfigError);
});

test("test_chain_applies_config_overrides", () => {
  const { e } = makeEngine({ rpc_urls: { arbitrum: ["https://mine"] }, confirmations: { arbitrum: 2 } });
  const c = e.chain("arbitrum");
  assert.equal(c.rpc[0], "https://mine");
  assert.equal(c.confirmations, 2);
  assert.deepEqual(new Set(Object.keys(c.tokens)), new Set(["USDC", "USDT"])); // full registry
  assert.equal(MAINNET["arbitrum"]!.rpc[0], "https://arb1.arbitrum.io/rpc"); // registry untouched
});

test("test_rpc_clients_are_cached_per_chain", () => {
  const { e } = makeEngine();
  assert.equal(e.rpc("arbitrum"), e.rpc("arbitrum"));
  assert.notEqual(e.rpc("arbitrum"), e.rpc("base"));
});

test("test_default_rpc_factory_builds_an_rpc_client_on_the_engine_clock", () => {
  const cfg = buildConfig({ wallet: DEV });
  const transport: Transport = { postJson: async () => ({}) };
  const clock = () => 42;
  const e = new Engine(cfg, { transport, clock });
  const rpc = e.rpc("arbitrum") as RpcClient;
  assert.ok(rpc instanceof RpcClient);
  assert.equal(rpc.clock, clock);
  assert.deepEqual([...rpc.urls], MAINNET["arbitrum"]!.rpc);
  assert.equal(rpc.logRange, MAINNET["arbitrum"]!.max_log_range);
});

test("test_end_to_end_tick_pays_and_updates_snapshot", async () => {
  const { e, chains } = makeEngine();
  const rec = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  const chain = chains["arbitrum"]!;
  chain.logs.push(transferLog(USDC_ARB, PAYER, DEV, rec.units, { block: 999, tx_hash: TX }));
  assert.deepEqual((await e.tick()).map((o) => o.status), ["confirming"]);
  assert.ok(!e.snapshot().owned.has("chapter2"));
  chain.head = 999 + 19;
  const outcomes = await e.tick();
  assert.equal(outcomes[0]!.status, "paid");
  const snap = e.snapshot();
  assert.deepEqual(snap.owned, new Set(["chapter2"]));
  assert.deepEqual(snap.pending, {});
  assert.equal(Object.keys(snap.receipts).length, 1);
  assert.equal(String(snap.tip_total), "0");
  assert.deepEqual(snap.unready, new Set());
});

test("test_report_hint_and_ui_status", async () => {
  const { e } = makeEngine();
  const rec = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  e.reportHint(rec.id, TX.toUpperCase().replace("0X", "0x"));
  e.setUiStatus(rec.id, "pending");
  const stored = e.pending[rec.id];
  assert.equal(stored.hint_tx, TX);
  assert.equal(stored.hint_at, 100.0);
  assert.equal(stored.ui_status, "pending");
});

test("test_report_hint_rejects_bad_hash_and_unknown_session", async () => {
  const { e } = makeEngine();
  const rec = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  assert.throws(() => e.reportHint(rec.id, "0x1234"));
  assert.throws(() => e.reportHint("ps_missing", TX));
  assert.equal(e.pending[rec.id].hint_tx, null);
});

test("test_engine_copies_initial_state", () => {
  const pending = { ps_x: { chain: "arbitrum" } };
  const receipts = { k: { kind: "tip" } };
  const cfg = buildConfig({ wallet: DEV });
  const e = new Engine(cfg, { pending, receipts, rpc_factory: () => new FakeChain() });
  e.pending["ps_x"].chain = "base";
  e.receipts["k"].kind = "unlock";
  assert.equal(pending.ps_x.chain, "arbitrum");
  assert.equal(receipts.k.kind, "tip");
});

test("test_snapshot_is_detached_from_engine_state", async () => {
  const { e } = makeEngine();
  await e.startSession("tip", "base", { amount: "3", token: "USDC" });
  const snap = e.snapshot();
  Object.values(snap.pending)[0].state = "x";
  assert.equal(Object.values(e.pending)[0].state, "awaiting");
});

// --- expiry, discard ----------------------------------------------------------

test("test_window_expiry_is_reported_once_and_scanning_continues", async () => {
  const { e, clock } = makeEngine();
  const rec = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  clock.advance(30 * 60);
  const expired = (await e.tick()).filter((o) => o.status === "expired");
  assert.equal(expired.length, 1);
  assert.equal(expired[0]!.result!.status, "expired");
  assert.equal(expired[0]!.result!.session_id, rec.id);
  assert.deepEqual(expired[0], {
    session_id: rec.id, status: "expired", confirmations: 0, required: 0, error: null, hint_tx: null,
    result: {
      status: "expired", kind: "unlock", item: "chapter2", amount: null, token: "USDC", chain: "arbitrum",
      tx_hash: null, payer: null, session_id: rec.id, error: null,
    },
  });
  assert.equal(e.pending[rec.id].ui_status, "expired");
  assert.deepEqual((await e.tick()).filter((o) => o.status === "expired"), []); // not repeated
  assert.ok(rec.id in e.pending); // still watched
});

test("test_cancelled_sessions_do_not_report_expiry", async () => {
  const { e, clock } = makeEngine();
  const rec = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  e.setUiStatus(rec.id, "cancelled");
  clock.advance(31 * 60);
  assert.deepEqual((await e.tick()).filter((o) => o.status === "expired"), []);
});

test("test_set_ui_status_ignores_finished_sessions", () => {
  const { e } = makeEngine();
  e.setUiStatus("ps_gone", "cancelled"); // no KeyError
});

test("test_discard_session_only_drops_untouched_sessions", async () => {
  const { e } = makeEngine();
  const a = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  const b = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  e.reportHint(b.id, TX);
  e.discardSession(a.id);
  e.discardSession(b.id);
  e.discardSession("ps_gone");
  assert.deepEqual(Object.keys(e.pending), [b.id]);
});

test("test_unwatchable_session_waits_for_its_horizon_then_closes", async () => {
  const cfg = buildConfig({ wallet: DEV });
  const rec = {
    id: "ps_gone", chain: "nochain", created_at: 0, ui_status: "cancelled", kind: "tip", item: null, token: "USDC",
  };
  const clock = new FakeClock();
  clock.now = 100;
  const e = new Engine(cfg, { pending: { ps_gone: rec }, clock: clock.call, rpc_factory: () => new FakeChain() });
  assert.deepEqual(await e.tick(), []);
  clock.now = 86400;
  assert.deepEqual(await e.tick(), [{
    session_id: "ps_gone", status: "closed", confirmations: 0, required: 0, result: null, error: null, hint_tx: null,
  }]);
  assert.deepEqual(e.pending, {});
});

// --- reverify and restore ---------------------------------------------------------------------

test("test_reverify_uses_accepted_wallets", async () => {
  const { e } = makeEngine({ previous_wallets: [PAYER] });
  e.receipts["k"] = {
    chain: "arbitrum", tx_hash: TX, log_index: 0, block: 900, amount: "1.000000", token: "USDC", kind: "tip", item: null,
  };
  const seen: string[] = [];
  (e as any)._rpc_factory = (chain: Chain) => { seen.push(chain.key); return new FakeChain(1000); };
  assert.deepEqual(await e.reverify(), ["k"]); // no matching log in the fake chain -> revoked
  assert.deepEqual(seen, ["arbitrum"]);
  assert.deepEqual(e.receipts, {});
});

test("test_restore_through_engine", async () => {
  const { e, chains } = makeEngine();
  const chain = new FakeChain();
  chain.receipts[TX] = { status: "0x1", logs: [transferLog(USDC_ARB, PAYER, DEV, 4_993_742, { block: 10, tx_hash: TX })] };
  for (const k of Object.keys(MAINNET)) (chains[k] = k === "arbitrum" ? chain : new FakeChain());
  const cands = await e.restoreLookup(TX);
  assert.equal(cands[0]!.item, "chapter2");
  assert.equal(e.restoreApply(cands).length, 1);
  assert.deepEqual([...e.snapshot().owned], ["chapter2"]);
});

test("test_restore_lookup_asks_the_enabled_chains_first_then_the_rest", async () => {
  const { e, chains } = makeEngine({ chains: ["polygon", "base"] });
  const asked: string[] = [];
  for (const k of Object.keys(MAINNET)) {
    const fake = new FakeChain();
    const orig = fake.getTransactionReceipt.bind(fake);
    fake.getTransactionReceipt = async (t) => { asked.push(k); return orig(t); };
    chains[k] = fake;
  }
  await assert.rejects(e.restoreLookup(TX), (x: unknown) => x instanceof RestoreError && x.code === "tx_not_found");
  assert.deepEqual(new Set(asked.slice(0, 2)), new Set(["polygon", "base"])); // enabled first
  assert.ok(asked.length > 2); // then the rest of the registry
  assert.ok(!asked.includes("solana")); // an EVM hash is not a Solana signature
});

test("test_reverify_replaces_unready_with_the_readiness_check", async () => {
  const { e } = makeEngine();
  e.unready = new Set(["arbitrum\u0000USDC"]);
  const errors: unknown[] = [];
  e.on_error = (x) => errors.push(x);
  await e.reverify();
  assert.equal(e.unready.size, 0); // EVM tokens are always ready
  assert.deepEqual(errors, []);
});

// --- crash containment (Review Focus #5) ----------------------------------------

/** Answers eth_blockNumber until `exc` is set, then throws it from postJson. */
class ThrowingTransport implements Transport {
  exc: unknown = null;
  async postJson(_url: string, payload: any): Promise<unknown> {
    if (this.exc !== null) throw this.exc;
    return { jsonrpc: "2.0", id: payload.id, result: "0x3e8" };
  }
}

test("test_tick_survives_any_transport_exception_in_each_session", async () => {
  const cfg = buildConfig({ wallet: DEV });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const transport = new ThrowingTransport();
  const errors: unknown[] = [];
  const clock = new FakeClock();
  clock.now = 100.0;
  const e = new Engine(cfg, { transport, clock: clock.call, on_error: (x) => errors.push(x) });
  const sids = new Set([
    (await e.startSession("unlock", "arbitrum", { item: "chapter2" })).id,
    (await e.startSession("tip", "arbitrum", { amount: "3", token: "USDC" })).id,
  ]);
  transport.exc = new Error("incomplete read");
  const out = await e.tick();
  assert.deepEqual(new Set(out.map((o) => o.session_id)), sids);
  assert.ok(out.every((o) => o.status === "waiting" && o.error === "internal_error"));
  assert.equal(errors.length, 2);
  assert.ok(errors.every((x) => x instanceof Error && x.message === "incomplete read"));
});

test("test_tick_contains_a_session_crash", async () => {
  // One session's step throws (a malformed log); the others still advance, in pending order,
  // and the crashed one is reported with Python's StepOutcome defaults.
  const { e, chains } = makeEngine();
  const errors: unknown[] = [];
  e.on_error = (x) => errors.push(x);
  const a = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  const b = await e.startSession("tip", "base", { amount: "3", token: "USDC" });
  const c = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  const bad = transferLog(USDC_ARB, PAYER, DEV, 1, { block: 999, tx_hash: TX });
  bad["data"] = "0xzz";
  chains["base"]!.logs.push({ ...bad, address: MAINNET["base"]!.tokens["USDC"]!.address.toLowerCase() });
  const order: string[] = [];
  for (const key of ["arbitrum", "base"]) {
    const fake = chains[key]!;
    const original = fake.blockNumber.bind(fake);
    fake.blockNumber = async () => { order.push(key); return original(); };
  }
  const out = await e.tick();
  assert.deepEqual(order, ["arbitrum", "base", "arbitrum"]); // sequential, insertion order
  assert.deepEqual(out.map((o) => o.session_id), [a.id, b.id, c.id]);
  assert.deepEqual(out[1], {
    session_id: b.id, status: "waiting", confirmations: 0, required: 0, result: null, error: "internal_error",
    hint_tx: null,
  });
  assert.deepEqual([out[0]!.status, out[2]!.status], ["waiting", "waiting"]);
  assert.equal(errors.length, 1);
  assert.ok(errors[0] instanceof ValueError); // the hex parse of data "0xzz"
  assert.ok(b.id in e.pending);
});

test("test_tick_never_runs_sessions_concurrently", async () => {
  const { e, chains } = makeEngine();
  await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  await e.startSession("tip", "base", { amount: "3", token: "USDC" });
  let inFlight = 0;
  let maxInFlight = 0;
  for (const fake of Object.values(chains)) {
    const original = fake.blockNumber.bind(fake);
    fake.blockNumber = async () => {
      inFlight++;
      maxInFlight = Math.max(maxInFlight, inFlight);
      await new Promise((r) => setTimeout(r, 5));
      inFlight--;
      return original();
    };
  }
  await e.tick();
  assert.equal(maxInFlight, 1);
});

test("test_injected_entropy_supplies_id_and_nonce", async () => {
  const cfg = buildConfig({ wallet: DEV });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const draws: string[] = [];
  const entropy: Entropy = {
    tokenHex: (n) => { draws.push(`token_hex(${n})`); return "ab".repeat(n); },
    randBelow: (n) => { draws.push(`randbelow(${n})`); return 42; },
    tokenBytes: (n) => { draws.push(`token_bytes(${n})`); return new Uint8Array(n); },
  };
  const e = new Engine(cfg, { rpc_factory: () => new FakeChain(), entropy, clock: () => 1 });
  const rec = await e.startSession("unlock", "arbitrum", { item: "chapter2" });
  assert.deepEqual(draws, ["randbelow(100)", "token_hex(8)"]);
  assert.equal(rec.id, "ps_" + "ab".repeat(8));
  assert.equal(rec.units, 4_993_742);
});
