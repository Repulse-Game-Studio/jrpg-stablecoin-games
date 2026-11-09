/** Twin of tests/unit/test_engine_solana.py and the Engine half of test_solana_build.py. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain } from "../../test/chain_fakes.ts";
import { FakeClock } from "../../test/rpc_fakes.ts";
import { FakeSolanaChain, sig } from "../../test/solana_fakes.ts";
import { MAINNET, type Chain } from "./chains.ts";
import { addItem, buildConfig } from "./config.ts";
import { Engine } from "./engine.ts";
import { KeyError, ValueError } from "./errors.ts";
import { RpcError, RpcUnavailable } from "./rpc.ts";
import { SCAN_HORIZON_S } from "./sessions.ts";
import { associatedTokenAddress } from "./solana/pda.ts";

const DEV_EVM = "0x52908400098527886E0F7030069857D2E4169EE7";
const DEV = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU";
const OLD = "4SELZ24sTinbBYo27VvsUAHepTRXGVBghc8WTUpvx6Pn";
const PAYER = "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe";
const MINT = MAINNET["solana"]!.tokens["USDC"]!.address;
const SIG = sig(7);

function make() {
  const cfg = buildConfig({ wallet: DEV_EVM, solana_wallet: DEV });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const sol = new FakeSolanaChain(1000);
  const evm = new FakeChain(1000);
  sol.accounts.add(associatedTokenAddress(DEV, MINT));
  sol.accounts.add(associatedTokenAddress(DEV, MAINNET["solana"]!.tokens["USDT"]!.address));
  const factory = (chain: Chain) => (chain.family === "solana" ? sol : evm);
  const clock = new FakeClock();
  clock.now = 100.0;
  return { e: new Engine(cfg, { rpc_factory: factory, clock: clock.call }), sol, evm };
}

const statuses = (outcomes: { session_id: string; status: string }[], sid: string) =>
  outcomes.filter((o) => o.session_id === sid).map((o) => o.status);

test("test_solana_payment_end_to_end", async () => {
  const { e, sol } = make();
  const rec = await e.startSession("unlock", "solana", { item: "chapter2" });
  assert.equal(rec["dest"], associatedTokenAddress(DEV, MINT));
  assert.ok(rec["reference"]);
  sol.pay(SIG, { mint: MINT, payer: PAYER, owner: DEV, amount: rec["units"], addresses: [rec["reference"], rec["dest"]] });
  assert.deepEqual(statuses(await e.tick(), rec["id"]), ["confirming"]);
  sol.finalize(SIG);
  const paid = (await e.tick()).filter((o) => o.status === "paid");
  assert.equal(paid.length, 1);
  assert.equal(paid[0]!.result!.chain, "solana");
  assert.deepEqual([...e.snapshot().owned], ["chapter2"]);
  assert.deepEqual(await e.reverify(), []);
  assert.deepEqual([...e.snapshot().unready], []);
  for (const k of Object.keys(e.receipts)) delete e.receipts[k];
  assert.deepEqual((await e.restoreLookup(SIG)).map((c) => c.item), ["chapter2"]);
});

test("test_hint_on_solana_session_must_be_a_signature", async () => {
  const { e } = make();
  const rec = await e.startSession("tip", "solana", { amount: "1", token: "USDC" });
  e.reportHint(rec["id"], SIG);
  assert.equal(e.pending[rec["id"]]["hint_tx"], SIG);
  assert.throws(() => e.reportHint(rec["id"], "0x" + "ab".repeat(32)), ValueError);
});

test("test_missing_token_account_marks_token_unready", async () => {
  const { e, sol } = make();
  sol.accounts.clear();
  await e.reverify();
  assert.deepEqual([...e.snapshot().unready].sort(), ["solana\u0000USDC", "solana\u0000USDT"]);
});

test("test_readiness_check_failure_assumes_ready", async () => {
  const { e, sol } = make();
  sol.accounts.clear();
  sol.down = true;
  await e.reverify();
  assert.deepEqual([...e.snapshot().unready], []);
});

for (const [why, exc] of [
  ["unavailable", () => new RpcUnavailable("down")],
  ["rpc_error", () => new RpcError(-32005, "node is behind")],
] as const) {
  test(`test_readiness_check_hides_nothing_on_network_doubt[${why}]`, async () => {
    // Task 7 review gap: an RpcUnavailable/RpcError from checkReady leaves `unready` empty, unreported
    const { e, sol } = make();
    e.unready = new Set(["solana\u0000USDC"]); // a stale verdict is replaced, not kept
    const errors: unknown[] = [];
    e.on_error = (x) => errors.push(x);
    sol.accounts.clear();
    sol.accountExists = async () => { throw exc(); };
    await e.reverify();
    assert.deepEqual([...e.snapshot().unready], []);
    assert.deepEqual(errors, []);
  });
}

test("test_readiness_check_crash_assumes_ready_and_is_reported", async () => {
  const errors: unknown[] = [];
  const { e, sol } = make();
  e.on_error = (x) => errors.push(x);
  sol.accounts.clear();
  sol.accountExists = async () => { throw new KeyError("value"); };
  await e.reverify();
  assert.deepEqual([...e.snapshot().unready], []);
  assert.equal(errors.length, 1);
  assert.ok(errors[0] instanceof KeyError);
});

/**
 * An EVM session left in persistent by a build that accepted EVM, loaded by a build that is
 * Solana-only: its chain is still in the registry but has no wallet.
 */
async function evmSessionInSolanaOnlyGame() {
  const { e: old } = make();
  const rec = await old.startSession("tip", "arbitrum", { amount: "1", token: "USDC" });
  const cfg = buildConfig({ solana_wallet: DEV });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const sol = new FakeSolanaChain(1000);
  const evm = new FakeChain(1000);
  const factory = (chain: Chain) => (chain.family === "solana" ? sol : evm);
  const errors: unknown[] = [];
  const clock = new FakeClock();
  clock.now = 100.0;
  const e = new Engine(cfg, { rpc_factory: factory, clock: clock.call, pending: old.pending, on_error: (x) => errors.push(x) });
  return { e, sol, stale: rec, clock, errors };
}

test("test_session_without_a_wallet_for_its_chain_does_not_stall_the_others", async () => {
  const { e, sol, stale, errors } = await evmSessionInSolanaOnlyGame();
  const rec = await e.startSession("unlock", "solana", { item: "chapter2" });
  sol.pay(SIG, { mint: MINT, payer: PAYER, owner: DEV, amount: rec["units"], addresses: [rec["reference"], rec["dest"]] });
  assert.deepEqual(statuses(await e.tick(), rec["id"]), ["confirming"]);
  sol.finalize(SIG);
  const outcomes = await e.tick();
  assert.deepEqual(statuses(outcomes, rec["id"]), ["paid"]);
  assert.deepEqual(outcomes.filter((o) => o.session_id === stale["id"]), []); // skipped silently
  assert.ok(stale["id"] in e.pending);
  assert.deepEqual(errors, []);
});

test("test_session_on_an_unwatchable_chain_is_closed_after_the_horizon", async () => {
  const { e, stale, clock } = await evmSessionInSolanaOnlyGame();
  e.pending[stale["id"]]["chain"] = "removed_custom_chain"; // gone from the registry too
  e.pending["ps_other"] = { ...e.pending[stale["id"]], id: "ps_other", chain: "arbitrum" };
  clock.advance(SCAN_HORIZON_S);
  const closed = (await e.tick()).filter((o) => o.status === "closed");
  assert.deepEqual(closed.map((o) => o.session_id).sort(), [stale["id"], "ps_other"].sort());
  assert.deepEqual(e.pending, {});
});

test("test_an_exception_in_one_session_does_not_stop_the_others", async () => {
  const errors: unknown[] = [];
  const { e, sol } = make();
  e.on_error = (x) => errors.push(x);
  const evmRec = await e.startSession("tip", "arbitrum", { amount: "1", token: "USDC" });
  const rec = await e.startSession("unlock", "solana", { item: "chapter2" });
  const boom = new Error("boom");
  e.rpc("arbitrum").blockNumber = async () => { throw boom; };
  sol.pay(SIG, { mint: MINT, payer: PAYER, owner: DEV, amount: rec["units"], addresses: [rec["reference"], rec["dest"]] });
  const outcomes = Object.fromEntries((await e.tick()).map((o) => [o.session_id, o]));
  assert.deepEqual([outcomes[evmRec["id"]]!.status, outcomes[evmRec["id"]]!.error], ["waiting", "internal_error"]);
  assert.equal(outcomes[rec["id"]]!.status, "confirming");
  assert.deepEqual(errors, [boom]);
  assert.ok(evmRec["id"] in e.pending);
});

test("test_default_error_hook_prints_the_traceback", async () => {
  const { e: base } = make();
  // a fresh Engine with the default on_error hook, sharing the fake chains
  const e = new Engine(base.config, { rpc_factory: (c) => base.rpc(c.key), clock: () => 100.0 });
  const rec = await e.startSession("tip", "arbitrum", { amount: "1", token: "USDC" });
  e.rpc("arbitrum").blockNumber = async () => { throw new Error("boom"); };
  const printed: unknown[] = [];
  const original = console.error;
  console.error = (...args: unknown[]) => { printed.push(...args); };
  try {
    assert.deepEqual((await e.tick()).filter((o) => o.session_id === rec["id"]).map((o) => o.error), ["internal_error"]);
  } finally {
    console.error = original;
  }
  assert.equal(printed.length, 1);
  assert.match(String(printed[0]), /Error: boom/);
});

// --- Solana build through the Engine (test_solana_build.py) ----------------------------------

const BLOCKHASH = "4sGjMW1sUnHzSxGspuhpqLDx6wiyjNtZAMdL4VZHirAn";

class BuildRpc extends FakeSolanaChain {
  async getTokenBalance(account: string): Promise<bigint> {
    return account === associatedTokenAddress(PAYER, MINT) ? 7_000_000n : 0n;
  }
  async getBalance(_address: string): Promise<bigint> { return 2_000_000n; }
  async getLatestBlockhash(): Promise<string> { return BLOCKHASH; }
  async getPriorityFee(_accounts: Iterable<string>): Promise<number> { return 5_000; }
}

test("test_engine_builds_only_for_solana_sessions", async () => {
  const cfg = buildConfig({ wallet: DEV_EVM, solana_wallet: DEV });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const sol = new BuildRpc();
  const e = new Engine(cfg, { rpc_factory: (c) => (c.family === "solana" ? sol : new FakeChain()), clock: () => 0.0 });
  const solRec = await e.startSession("unlock", "solana", { item: "chapter2" });
  const built = await e.buildSolanaPayment(solRec["id"], PAYER);
  assert.ok("transaction" in built);
  assert.equal(built["token_balance"], "7000000");
  const evmRec = await e.startSession("unlock", "base", { item: "chapter2" });
  await assert.rejects(e.buildSolanaPayment(evmRec["id"], PAYER), ValueError);
  await assert.rejects(e.buildSolanaPayment("ps_missing", PAYER), KeyError);
});

// --- previous Solana wallets only (Task 7 review follow-up) --------------------------------------

test("test_reverify_judges_solana_receipts_of_a_game_with_only_previous_solana_wallets", async () => {
  // No solana_wallet any more, but Solana receipts paid to a previous one: the Solana backend
  // (not a placeholder) judges them through Engine.reverify, and readiness skips Solana.
  const cfg = buildConfig({ wallet: DEV_EVM, previous_wallets: [OLD] });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  assert.equal(cfg.solana_wallet, null);
  const sol = new FakeSolanaChain(1000);
  sol.pay(sig(1), { mint: MINT, payer: PAYER, owner: OLD, amount: 4_993_742, slot: 950 });
  const asked: string[] = [];
  const e = new Engine(cfg, {
    rpc_factory: (c) => { asked.push(c.key); return c.family === "solana" ? sol : new FakeChain(); }, clock: () => 0.0,
    receipts: {
      [`solana:${sig(1)}:2`]: {
        chain: "solana", tx_hash: sig(1), log_index: 2, block: 950, kind: "unlock", item: "chapter2", amount: "4.993742",
        token: "USDC", payer: PAYER, recipient: OLD, source: "payment",
      },
      [`solana:${sig(2)}:2`]: { // invented, and history covers its slot
        chain: "solana", tx_hash: sig(2), log_index: 2, block: 950, kind: "unlock", item: "chapter2", amount: "4.993742",
        token: "USDC", payer: PAYER, recipient: OLD, source: "payment",
      },
    },
  });
  assert.deepEqual(await e.reverify(), [`solana:${sig(2)}:2`]);
  assert.deepEqual(Object.keys(e.receipts), [`solana:${sig(1)}:2`]);
  assert.deepEqual([...e.snapshot().owned], ["chapter2"]);
  assert.deepEqual(asked, ["solana"]); // readiness never touched Solana: it is not enabled
  assert.ok(!sol.calls.includes("account_exists"));
});
