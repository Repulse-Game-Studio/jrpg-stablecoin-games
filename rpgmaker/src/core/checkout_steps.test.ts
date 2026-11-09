/** Twin of tests/unit/test_checkout_steps.py: the stepped payment sheet and its navigation. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import { FakeServer, PageWithPump, immediateSubmitter } from "../../test/checkout_fakes.ts";
import { FakeClock } from "../../test/rpc_fakes.ts";
import { MAINNET, type Chain } from "./chains.ts";
import { CheckoutController } from "./checkout.ts";
import { addItem, buildConfig } from "./config.ts";
import { Engine } from "./engine.ts";
import { preparePayment } from "./payments.ts";
import { makeStepOutcome } from "./watcher.ts";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const USDC_ARB = MAINNET["arbitrum"]!.tokens["USDC"]!.address;
const TX = "0x" + "ab".repeat(32);
const ONLY_MANUAL = { browser: { enabled: false }, qr: { enabled: false } };

interface SetupOpts {
  kind?: string;
  chains?: string[];
  methods?: Record<string, Record<string, unknown>>;
  browser_wallet?: (chain: Chain) => boolean;
}

function setup({ kind = "unlock", chains, methods, browser_wallet }: SetupOpts = {}) {
  const cfg = buildConfig({ wallet: DEV, chains, methods });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const fakes: Record<string, FakeChain> = {};
  const clock = new FakeClock();
  clock.now = 100.0;
  const engine = new Engine(cfg, {
    rpc_factory: (c: Chain) => { fakes[c.key] ??= new FakeChain(1000); fakes[c.key]!.chain_obj = c; return fakes[c.key]!; },
    clock: clock.call,
  });
  const spec = kind === "unlock"
    ? preparePayment(cfg, "unlock", { item: "chapter2" })
    : preparePayment(cfg, "tip", { amount: "3", token: "USDC" });
  const submit = immediateSubmitter(engine);
  const ctl = new CheckoutController({
    config: cfg, spec, submit, server_factory: (kw) => new FakeServer(kw), browser_wallet,
  });
  return { ctl, engine, fakes, clock, submit };
}

async function onMethodStep(o: SetupOpts = {}) {
  const s = setup(o);
  s.ctl.chooseNetwork("arbitrum");
  await s.submit.idle();
  s.ctl.poll();
  return s;
}

async function deliver(ctl: CheckoutController, engine: Engine) {
  for (const o of await engine.tick()) ctl.onOutcome(o);
}

// --- where the sheet starts ---------------------------------------------------------

test("test_unlock_starts_on_network_and_tip_on_amount", () => {
  let { ctl } = setup();
  assert.ok(ctl.step === "network" && ctl.canGoBack === false);
  ({ ctl } = setup({ kind: "tip" }));
  assert.ok(ctl.step === "amount" && ctl.canGoBack === false);
});

test("test_step_path_counts_the_steps_shown", () => {
  let { ctl } = setup();
  assert.deepEqual(ctl.stepPath(), ["network", "method", "browser", "waiting"]);
  assert.deepEqual(ctl.stepDots(), [0, 4]);
  ({ ctl } = setup({ kind: "tip" }));
  assert.deepEqual(ctl.stepPath(), ["amount", "network", "method", "browser", "waiting"]);
});

// --- forward ------------------------------------------------------------------------

test("test_network_waits_for_the_session_then_moves_to_method", async () => {
  const { ctl, engine, submit } = setup();
  ctl.chooseNetwork("arbitrum");
  assert.ok(ctl.step === "network" && ctl.state === "starting");
  await submit.idle();
  ctl.poll();
  assert.equal(String(ctl.step), "method");
  assert.equal(ctl.nav_dir, 1);
  assert.ok(ctl.sessionId! in engine.pending);
  assert.deepEqual(ctl.stepDots(), [1, 4]);
});

test("test_choosing_a_method_opens_its_page", async () => {
  for (const name of ["browser", "qr", "manual"]) {
    const { ctl } = await onMethodStep();
    ctl.chooseMethod(name);
    assert.ok(ctl.step === name && ctl.method === name);
  }
});

test("test_unknown_method_is_ignored", async () => {
  const { ctl } = await onMethodStep({ methods: { qr: { enabled: false } } });
  ctl.chooseMethod("qr");
  assert.equal(ctl.step, "method");
});

test("test_opening_the_browser_moves_to_waiting", async () => {
  const { ctl } = await onMethodStep();
  ctl.chooseMethod("browser");
  assert.equal(ctl.openBrowser(), FakeServer.url);
  assert.ok(ctl.step === "waiting" && ctl.canGoBack === true);
});

test("test_browser_failure_returns_to_method", async () => {
  const { ctl } = await onMethodStep();
  ctl.chooseMethod("browser");
  ctl.server_factory = () => { throw new Error("no sockets"); };
  assert.equal(ctl.openBrowser(), null);
  assert.ok(ctl.step === "method" && ctl.nav_dir === -1);
  assert.ok(ctl.error === "browser_unavailable" && ctl.method === "qr");
});

test("test_ive_sent_it_moves_qr_and_manual_to_waiting", async () => {
  for (const name of ["qr", "manual"]) {
    const { ctl } = await onMethodStep();
    ctl.chooseMethod(name);
    ctl.markSent();
    assert.ok(ctl.step === "waiting" && ctl.canGoBack === true);
  }
});

test("test_tip_amount_step_applies_a_typed_amount", () => {
  const { ctl } = setup({ kind: "tip" });
  ctl.custom_amount = "0.10";
  assert.equal(ctl.nextStep(), false);
  assert.ok(ctl.step === "amount" && ctl.error === "invalid_amount");
  ctl.custom_amount = "7.50";
  assert.equal(ctl.nextStep(), true);
  assert.equal(String(ctl.step), "network");
  assert.equal(String(ctl.amount), "7.50");
});

// --- skipping single-option steps -------------------------------------------------------

test("test_single_network_is_skipped_for_unlocks", async () => {
  const { ctl, submit } = setup({ chains: ["base"] });
  assert.ok(ctl.state === "starting" && ctl.chain_key === "base");
  await submit.idle();
  ctl.poll();
  assert.ok(ctl.step === "method" && ctl.canGoBack === false);
  assert.deepEqual(ctl.stepPath(), ["method", "browser", "waiting"]);
});

test("test_single_network_is_skipped_for_tips_both_ways", async () => {
  const { ctl, submit } = setup({ kind: "tip", chains: ["base"] });
  ctl.nextStep();
  await submit.idle();
  ctl.poll();
  assert.equal(ctl.step, "method");
  ctl.back();
  assert.equal(String(ctl.step), "amount");
  assert.equal(ctl.nav_dir, -1);
});

test("test_single_method_is_skipped_both_ways", async () => {
  const { ctl } = await onMethodStep({ methods: ONLY_MANUAL });
  assert.equal(ctl.step, "manual");
  ctl.back();
  assert.equal(ctl.step, "network");
});

// --- back -------------------------------------------------------------------------------

test("test_back_walks_the_path_and_keeps_the_session", async () => {
  const { ctl, engine } = await onMethodStep();
  const sid = ctl.sessionId!;
  ctl.chooseMethod("qr");
  ctl.markSent();
  ctl.back();
  assert.ok(ctl.step === "qr" && ctl.nav_dir === -1);
  ctl.back();
  assert.equal(ctl.step, "method");
  ctl.back();
  assert.ok(ctl.step === "network" && ctl.canGoBack === false);
  ctl.back(); // nothing before network on an unlock
  assert.equal(ctl.step, "network");
  assert.ok(ctl.sessionId === sid && sid in engine.pending);
});

test("test_choosing_the_same_network_again_keeps_the_session", async () => {
  const { ctl } = await onMethodStep();
  const sid = ctl.sessionId;
  ctl.back();
  ctl.chooseNetwork("arbitrum");
  assert.ok(ctl.step === "method" && ctl.sessionId === sid && ctl.nav_dir === 1);
});

test("test_choosing_another_network_restarts_the_session", async () => {
  const { ctl, engine, submit } = await onMethodStep();
  const sid = ctl.sessionId!;
  ctl.back();
  ctl.chooseNetwork("base");
  await submit.idle();
  ctl.poll();
  assert.ok(ctl.step === "method" && ctl.sessionId !== sid);
  assert.ok(!(sid in engine.pending));
});

test("test_back_to_amount_and_same_amount_keeps_the_session", async () => {
  const { ctl, submit } = await onMethodStep({ kind: "tip" });
  const sid = ctl.sessionId;
  ctl.back();
  ctl.back();
  assert.equal(ctl.step, "amount");
  assert.ok(ctl.setTipAmount("3") === true && ctl.sessionId === sid);
  assert.equal(ctl.setTipAmount("5"), true);
  await submit.idle();
  ctl.poll();
  assert.ok(ctl.sessionId !== sid && ctl.step === "amount");
  ctl.nextStep();
  ctl.chooseNetwork("arbitrum");
  assert.equal(ctl.step, "method");
});

test("test_back_clears_the_message", async () => {
  const { ctl } = await onMethodStep();
  ctl.chooseMethod("manual");
  ctl.submitHash("garbage");
  assert.equal(ctl.error, "invalid_hash");
  ctl.back();
  assert.equal(ctl.error, null);
});

// --- automatic moves ----------------------------------------------------------------------

test("test_a_seen_payment_moves_to_waiting_and_hides_back", async () => {
  const { ctl, engine, fakes } = await onMethodStep();
  ctl.chooseMethod("qr");
  fakes["arbitrum"]!.logs.push(transferLog(USDC_ARB, PAYER, DEV, ctl.session!["units"], { block: 999, tx_hash: TX }));
  await deliver(ctl, engine);
  assert.equal(ctl.state, "confirming");
  assert.ok(ctl.step === "waiting" && ctl.nav_dir === 1 && ctl.canGoBack === false);
});

test("test_a_pasted_hash_moves_to_waiting_and_hides_back", async () => {
  const { ctl } = await onMethodStep();
  ctl.chooseMethod("manual");
  assert.equal(ctl.submitHash(TX), true);
  assert.ok(ctl.step === "waiting" && ctl.canGoBack === false);
});

test("test_a_hash_from_the_browser_page_moves_to_waiting", async () => {
  const { ctl } = await onMethodStep();
  ctl.chooseMethod("browser");
  ctl.openBrowser();
  (ctl._server as FakeServer).on_tx(TX, PAYER);
  assert.ok(ctl.step === "waiting" && ctl.canGoBack === false);
});

test("test_a_dropped_transaction_gives_back_the_arrow", async () => {
  const { ctl } = await onMethodStep();
  ctl.chooseMethod("manual");
  ctl.submitHash(TX);
  ctl.onOutcome(makeStepOutcome(ctl.sessionId!, "waiting", { error: "tx_not_landed", hint_tx: TX }));
  assert.ok(ctl.step === "waiting" && ctl.canGoBack === true);
  ctl.back();
  assert.equal(ctl.step, "manual");
});

test("test_final_states_move_to_done", async () => {
  let s = await onMethodStep();
  s.ctl.chooseMethod("qr");
  s.fakes["arbitrum"]!.logs.push(transferLog(USDC_ARB, PAYER, DEV, s.ctl.session!["units"], { block: 999, tx_hash: TX }));
  await deliver(s.ctl, s.engine);
  s.fakes["arbitrum"]!.head = 1100;
  await deliver(s.ctl, s.engine);
  assert.ok(s.ctl.state === "paid" && s.ctl.step === "done" && s.ctl.canGoBack === false);

  s = await onMethodStep();
  s.ctl.chooseMethod("browser");
  s.clock.advance(1800);
  await deliver(s.ctl, s.engine);
  assert.ok(s.ctl.state === "expired" && s.ctl.step === "done");
});

test("test_start_failure_returns_to_network_with_the_message", async () => {
  const { ctl, fakes, submit } = setup();
  fakes["arbitrum"] = new FakeChain();
  fakes["arbitrum"].down = true;
  ctl.chooseNetwork("arbitrum");
  await submit.idle();
  ctl.poll();
  assert.ok(ctl.step === "network" && ctl.error === "network_unavailable");
});

test("test_start_failure_on_a_skipped_network_shows_the_network_step", async () => {
  const { ctl, fakes, submit } = setup({ kind: "tip", chains: ["base"] });
  fakes["base"] = new FakeChain();
  fakes["base"].down = true;
  ctl.nextStep();
  await submit.idle();
  ctl.poll();
  assert.ok(ctl.step === "network" && ctl.error === "network_unavailable");
  assert.equal(ctl.canGoBack, true);
  fakes["base"].down = false;
  ctl.chooseNetwork("base");
  await submit.idle();
  ctl.poll();
  assert.equal(ctl.step, "method");
});

test("test_a_session_restarted_on_the_amount_step_waits_for_a_network_pick", async () => {
  const { ctl, submit } = setup({ kind: "tip" });
  ctl.nextStep();
  ctl.chooseNetwork("arbitrum");
  await submit.idle();
  ctl.poll();
  ctl.back();
  ctl.back();
  assert.equal(ctl.step, "amount");
  ctl.setTipAmount("5"); // restarts the session on arbitrum
  ctl.nextStep();
  await submit.idle();
  ctl.poll(); // ... which is ready now
  assert.equal(ctl.step, "network"); // but the player hasn't picked yet
  ctl.chooseNetwork("arbitrum");
  assert.equal(ctl.step, "method");
});

test("test_leaving_the_network_step_while_starting_stays_put", async () => {
  const { ctl, submit } = setup({ kind: "tip" });
  ctl.nextStep();
  ctl.chooseNetwork("arbitrum");
  ctl.back();
  await submit.idle();
  ctl.poll();
  assert.ok(ctl.step === "amount" && ctl.state === "waiting");
});

// --- web build ----------------------------------------------------------------------

test("test_web_offers_browser_wallet_only_when_the_page_has_one", async () => {
  let { ctl } = await onMethodStep({ browser_wallet: () => false });
  assert.deepEqual(ctl.methods(), ["qr", "manual"]);
  ({ ctl } = await onMethodStep({ browser_wallet: (chain) => chain.key === "arbitrum" }));
  assert.deepEqual(ctl.methods(), ["browser", "qr", "manual"]);
});

test("test_web_browser_wallet_ignores_the_mobile_flag", async () => {
  const { ctl, submit } = setup({ browser_wallet: () => true });
  ctl.mobile = true;
  ctl.chooseNetwork("arbitrum");
  await submit.idle();
  ctl.poll();
  assert.ok(ctl.methods().includes("browser"));
});

test("test_the_controller_pumps_and_reopens_an_overlay_page", async () => {
  const { ctl } = await onMethodStep();
  ctl.server_factory = (kw) => new PageWithPump(kw);
  ctl.chooseMethod("browser");
  ctl.openBrowser();
  ctl.poll();
  assert.equal((ctl._server as PageWithPump).pumps, 1);
  ctl.openBrowser();
  assert.equal((ctl._server as PageWithPump).reopens, 1);
});
