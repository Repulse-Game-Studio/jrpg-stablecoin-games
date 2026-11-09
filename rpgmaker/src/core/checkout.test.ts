/** Twin of tests/unit/test_checkout.py. CheckoutController's async starts are awaited through submit.idle(). */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import { FakeServer, immediateSubmitter } from "../../test/checkout_fakes.ts";
import { FakeClock } from "../../test/rpc_fakes.ts";
import { MAINNET, type Chain } from "./chains.ts";
import { CheckoutController, FINAL_STATES, HINT_VERDICTS, MESSAGES, STEP_ORDER } from "./checkout.ts";
import { addItem, buildConfig, type BuildConfigOptions } from "./config.ts";
import { Dec } from "./decimal.ts";
import { Engine } from "./engine.ts";
import { preparePayment } from "./payments.ts";
import { makeStepOutcome } from "./watcher.ts";
import { ValueError } from "./errors.ts";
import { readFileSync } from "node:fs";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const USDC_ARB = MAINNET["arbitrum"]!.tokens["USDC"]!.address;
const TX = "0x" + "ab".repeat(32);

interface SetupOpts {
  kind?: string;
  mobile?: boolean;
  methods?: BuildConfigOptions["methods"];
  appearance?: Partial<BuildConfigOptions>;
}

function setup({ kind = "unlock", mobile = false, methods, appearance }: SetupOpts = {}) {
  const cfg = buildConfig({ wallet: DEV, methods, ...(appearance ?? {}) });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const fakes: Record<string, FakeChain> = {};
  const factory = (chain: Chain) => {
    fakes[chain.key] ??= new FakeChain(1000);
    fakes[chain.key]!.chain_obj = chain;
    return fakes[chain.key]!;
  };
  const clock = new FakeClock();
  clock.now = 100.0;
  const engine = new Engine(cfg, { rpc_factory: factory, clock: clock.call });
  const spec = kind === "unlock"
    ? preparePayment(cfg, "unlock", { item: "chapter2" })
    : preparePayment(cfg, "tip", { amount: "3", token: "USDC" });
  const servers: FakeServer[] = [];
  const submit = immediateSubmitter(engine);
  const ctl = new CheckoutController({
    config: cfg, spec, submit, mobile,
    server_factory: (kw) => { const s = new FakeServer(kw); servers.push(s); return s; },
  });
  return { ctl, engine, fakes, clock, servers, submit };
}

async function started(o: SetupOpts = {}) {
  const s = setup(o);
  s.ctl.selectChain("arbitrum");
  await s.submit.idle();
  s.ctl.poll();
  return s;
}

test("test_initial_state_and_chain_options", () => {
  const { ctl } = setup();
  assert.ok(ctl.state === "choose" && ctl.session === null);
  assert.deepEqual(ctl.chainOptions().map(([k]) => k), ["arbitrum", "base", "arc", "polygon", "ethereum"]);
  assert.equal(ctl.payAmountText, "4.99");
});

test("test_select_chain_starts_session", async () => {
  const { ctl, engine } = await started();
  assert.equal(ctl.state, "waiting");
  assert.ok(ctl.session!["id"] in engine.pending);
  assert.equal(ctl.method, "browser");
  assert.equal(ctl.required, 20);
  assert.ok(ctl.payAmountText.startsWith("4.9937"));
  assert.equal(ctl.priceText, "4.99");
});

test("test_select_unknown_chain_raises", () => {
  const { ctl } = setup();
  assert.throws(() => ctl.selectChain("solana"), ValueError);
});

test("test_switching_chain_discards_old_session", async () => {
  const { ctl, engine, submit } = await started();
  const old = ctl.sessionId!;
  ctl.selectChain("base");
  await submit.idle();
  ctl.poll();
  assert.ok(!(old in engine.pending) && ctl.sessionId! in engine.pending);
  assert.equal(ctl.session!["chain"], "base");
});

test("test_start_failure_returns_to_choose_with_error", async () => {
  const { ctl, fakes, submit } = setup();
  fakes["arbitrum"] = new FakeChain();
  fakes["arbitrum"].down = true;
  ctl.selectChain("arbitrum");
  await submit.idle();
  ctl.poll();
  assert.ok(ctl.state === "choose" && ctl.error === "network_unavailable");
  assert.ok(ctl.message());
});

test("test_methods_on_mobile_and_config", async () => {
  let s = await started({ mobile: true });
  assert.deepEqual(s.ctl.methods(), ["qr", "manual"]);
  s = await started({ methods: { qr: { chains: ["base"] } } });
  assert.deepEqual(s.ctl.methods(), ["browser", "manual"]); // QR limited to base, we're on arbitrum
});

test("test_payment_uri", async () => {
  const { ctl } = await started();
  assert.equal(ctl.paymentUri(),
    `ethereum:${USDC_ARB}@42161/transfer?address=${DEV}&uint256=${ctl.session!["units"]}`);
});

test("test_submit_hash_validation_and_hint", async () => {
  const { ctl, engine } = await started();
  assert.ok(ctl.submitHash("garbage") === false && ctl.error === "invalid_hash");
  ctl.tx_input = TX;
  assert.ok(ctl.submitHash() === true);
  assert.equal(engine.pending[ctl.sessionId!]["hint_tx"], TX);
  assert.ok(ctl.hint_tx === TX && ctl.tx_input === "" && ctl.error === null);
});

test("test_outcomes_drive_states_to_paid", async () => {
  const { ctl, engine, fakes } = await started();
  fakes["arbitrum"]!.logs.push(transferLog(USDC_ARB, PAYER, DEV, ctl.session!["units"], { block: 999, tx_hash: TX }));
  for (const o of await engine.tick()) ctl.onOutcome(o);
  assert.ok(ctl.state === "confirming" && ctl.confirmations === 2);
  fakes["arbitrum"]!.head = 1100;
  for (const o of await engine.tick()) ctl.onOutcome(o);
  assert.equal(String(ctl.state), "paid");
  assert.equal(ctl.result!.status, "paid");
  assert.equal(ctl.explorerUrl(), `https://arbiscan.io/tx/${TX}`);
  assert.equal(ctl.finish().status, "paid");
});

test("test_outcomes_for_other_sessions_are_ignored", async () => {
  const { ctl } = await started();
  ctl.onOutcome(makeStepOutcome("ps_other", "paid"));
  assert.equal(ctl.state, "waiting");
});

test("test_reorg_and_hint_errors", async () => {
  const { ctl } = await started();
  ctl.hint_tx = TX;
  ctl.onOutcome(makeStepOutcome(ctl.sessionId!, "confirming", { confirmations: 3, required: 20 }));
  ctl.onOutcome(makeStepOutcome(ctl.sessionId!, "reorged"));
  assert.ok(ctl.state === "waiting" && ctl.confirmations === 0);
  ctl.onOutcome(makeStepOutcome(ctl.sessionId!, "waiting", { error: "tx_not_a_payment" }));
  assert.ok(ctl.error === "tx_not_a_payment" && ctl.hint_tx === null);
});

test("test_expiry_shows_expired_state", async () => {
  const { ctl, engine, clock } = await started();
  clock.advance(1800);
  for (const o of await engine.tick()) ctl.onOutcome(o);
  assert.ok(ctl.state === "expired" && ctl.finish().status === "expired");
});

test("test_cancel_marks_session_and_returns_result", async () => {
  const { ctl, engine } = await started();
  const result = ctl.cancel();
  assert.ok(result.status === "cancelled" && result.session_id === ctl.sessionId);
  assert.equal(engine.pending[ctl.sessionId!]["ui_status"], "cancelled");
});

test("test_keep_playing", async () => {
  const { ctl, engine } = await started();
  assert.equal(ctl.keepPlaying().status, "pending");
  assert.equal(engine.pending[ctl.sessionId!]["ui_status"], "pending");
});

test("test_cancel_before_choosing_chain", () => {
  const { ctl } = setup();
  assert.equal(ctl.cancel().status, "cancelled");
});

test("test_tip_amount_changes_restart_session", async () => {
  const { ctl, engine, submit } = await started({ kind: "tip" });
  const first = ctl.sessionId!;
  assert.equal(ctl.setTipAmount("5"), true);
  await submit.idle();
  ctl.poll();
  assert.ok(ctl.amount.cmp(Dec.parse("5.00") as Dec) === 0 && ctl.session!["price"] === "5.00");
  assert.ok(!(first in engine.pending));
  assert.ok(ctl.setTipAmount("0.10") === false && ctl.error === "invalid_amount");
  ctl.custom_amount = " 7.50 ";
  assert.ok(ctl.applyCustomAmount() === true && ctl.amount.cmp(Dec.parse("7.50") as Dec) === 0);
});

test("test_tip_amount_ignored_for_unlocks", async () => {
  const { ctl } = await started();
  assert.equal(ctl.setTipAmount("1"), false);
});

test("test_browser_server_lifecycle_and_hints", async () => {
  const { ctl, engine, servers } = await started();
  const url = ctl.startBrowser();
  assert.equal(url, "http://127.0.0.1:1234/c/secret/");
  assert.ok(ctl.startBrowser() === url && servers.length === 1); // reused
  const s = servers[0]!;
  assert.ok(s.started && s.session_info["amount_units"] === String(ctl.session!["units"]));
  assert.equal(s.session_info["chain_id_hex"], "0xa4b1");
  assert.equal(s.session_info["logo"], false);
  assert.ok(s.session_info["accent"] === "#0e7c74" && s.session_info["on_accent"] === "#ffffff");
  s.on_tx(TX, PAYER); // page reports the tx
  assert.equal(engine.pending[ctl.sessionId!]["hint_tx"], TX);
  assert.equal(s.get_status()["tx_hash"], TX);
  ctl.cancel();
  assert.ok(s.stopped);
});

test("test_browser_failure_falls_back", async () => {
  const { ctl } = await started();
  ctl.server_factory = () => { throw new Error("no sockets"); };
  assert.equal(ctl.startBrowser(), null);
  assert.equal(ctl.error, "browser_unavailable");
  assert.ok(!ctl.methods().includes("browser") && ctl.method === "qr");
});

test("test_browser_session_info_carries_appearance", async () => {
  const { ctl, servers } = await started({ appearance: { logo: "gui/logo.png", accent: "#ffcc00" } });
  ctl.startBrowser();
  const info = servers[0]!.session_info;
  assert.equal(info["logo"], true);
  assert.ok(info["accent"] === "#ffcc00" && info["on_accent"] === "#17202e");
});

for (const [chain, gas_is_token, min_fee, native] of [
  ["arc", true, "20000000000", "USDC"], // USDC is Arc's gas coin; 20 gwei mempool floor
  ["base", false, "0", "ETH"],
] as const) {
  test(`test_browser_info_tells_the_page_how_gas_is_paid[${chain}]`, async () => {
    const { ctl, servers, submit } = setup();
    ctl.selectChain(chain);
    await submit.idle();
    ctl.poll();
    ctl.startBrowser();
    const info = servers[0]!.session_info;
    assert.equal(info["gas_is_token"], gas_is_token);
    assert.equal(info["min_fee_wei"], min_fee);
    assert.equal(info["native_symbol"], native);
    assert.equal(info["chain_id"], MAINNET[chain]!.chain_id);
  });
}

// --- additions with no Python twin: the shared page reads session_info, so pin it exactly ---

test("test_session_info_is_the_exact_python_dict", async () => {
  const { ctl, servers } = await started();
  ctl.startBrowser();
  const info = servers[0]!.session_info;
  const units = ctl.session!["units"];
  assert.deepEqual(Object.keys(info), [
    "title", "kind", "token", "family", "decimals", "amount_units", "amount_text", "price_text",
    "recipient", "chain_name", "native_symbol", "explorer_tx", "testnet", "logo", "accent", "on_accent",
    "chain_id", "chain_id_hex", "rpc_url", "token_address", "gas_is_token", "min_fee_wei"]);
  assert.deepEqual(info, {
    title: "Chapter 2", kind: "unlock", token: "USDC", family: "evm", decimals: 6,
    amount_units: String(units), amount_text: ctl.payAmountText, price_text: "4.99",
    recipient: DEV, chain_name: "Arbitrum", native_symbol: "ETH",
    explorer_tx: "https://arbiscan.io/tx/{}", testnet: false, logo: false, accent: "#0e7c74",
    on_accent: "#ffffff", chain_id: 42161, chain_id_hex: "0xa4b1",
    rpc_url: "https://arbitrum-one-rpc.publicnode.com", token_address: USDC_ARB,
    gas_is_token: false, min_fee_wei: "0",
  });
  assert.equal(typeof info["amount_units"], "string");
});

test("test_status_info_keys", async () => {
  const { ctl } = await started();
  assert.deepEqual(ctl.statusInfo(), {
    state: "waiting", confirmations: 0, required: 20, tx_hash: null, error: null, dropped_tx: null,
  });
  assert.deepEqual(Object.keys(ctl.statusInfo()), ["state", "confirmations", "required", "tx_hash", "error", "dropped_tx"]);
});

test("test_poll_treats_a_cancelled_job_as_a_failed_start", () => {
  const { ctl } = setup();
  // a submit whose job was cancelled before it ran (stopped Worker)
  const job = { done: () => true, exception: () => new Error("cancelled"), result: () => { throw new Error("x"); }, cancelled: () => true, promise: Promise.resolve() };
  (ctl as any).submit = () => job;
  ctl.selectChain("arbitrum");
  ctl.poll();
  assert.ok(ctl.state === "choose" && ctl.error === "start_failed" && ctl.chain_key === null);
});

test("test_messages_match_catalogue", () => {
  const url = new URL("../../../conformance/vectors/messages.json", import.meta.url);
  const catalogue = JSON.parse(readFileSync(url, "utf8")).messages;
  assert.deepEqual(MESSAGES, catalogue);
});

test("test_constants", () => {
  assert.deepEqual(FINAL_STATES, ["paid", "underpaid", "expired"]);
  assert.deepEqual(HINT_VERDICTS, ["tx_reverted", "tx_not_a_payment", "tx_not_landed"]);
  assert.deepEqual(STEP_ORDER, { amount: 0, network: 1, method: 2, browser: 3, qr: 3, manual: 3, waiting: 4, done: 5 });
});
