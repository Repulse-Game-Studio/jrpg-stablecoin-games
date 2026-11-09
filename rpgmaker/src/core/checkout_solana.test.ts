/** Twin of tests/unit/test_checkout_solana.py (the RestoreController and CheckoutServer tests belong to later tasks). */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain } from "../../test/chain_fakes.ts";
import { FakeServer, immediateSubmitter } from "../../test/checkout_fakes.ts";
import { FakeClock } from "../../test/rpc_fakes.ts";
import { FakeSolanaChain, sig } from "../../test/solana_fakes.ts";
import { MAINNET, type Chain } from "./chains.ts";
import { CheckoutController, MESSAGES } from "./checkout.ts";
import { addItem, buildConfig } from "./config.ts";
import { Engine } from "./engine.ts";
import { preparePayment } from "./payments.ts";
import { associatedTokenAddress } from "./solana/pda.ts";
import { makeStepOutcome } from "./watcher.ts";

const DEV_EVM = "0x52908400098527886E0F7030069857D2E4169EE7";
const DEV = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU";
const MINT = MAINNET["solana"]!.tokens["USDC"]!.address;
const SIG = sig(7);
const SIG2 = sig(8);
const EVM_TX = "0x" + "ab".repeat(32);
const UNREADY_SOL_USDC = "solana\u0000USDC";

function setup(unready: ReadonlySet<string> = new Set()) {
  const cfg = buildConfig({ wallet: DEV_EVM, solana_wallet: DEV });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const sol = new FakeSolanaChain(1000);
  const evm = new FakeChain(1000);
  sol.accounts.add(associatedTokenAddress(DEV, MINT));
  sol.accounts.add(associatedTokenAddress(DEV, MAINNET["solana"]!.tokens["USDT"]!.address));
  const clock = new FakeClock();
  clock.now = 0.0;
  const e = new Engine(cfg, { rpc_factory: (c: Chain) => (c.family === "solana" ? sol : evm), clock: clock.call });
  const spec = preparePayment(cfg, "unlock", { item: "chapter2" });
  const submit = immediateSubmitter(e);
  const servers: FakeServer[] = [];
  const ctl = new CheckoutController({
    config: cfg, spec, submit, unready: () => unready, game_name: "Ruby Dreams",
    server_factory: (kw) => { const s = new FakeServer(kw); servers.push(s); return s; },
  });
  return { ctl, e, submit, servers };
}

async function started(chain: string) {
  const s = setup();
  s.ctl.selectChain(chain);
  await s.submit.idle();
  s.ctl.poll();
  return s;
}

test("test_solana_is_offered_and_unready_tokens_are_hidden", () => {
  let { ctl } = setup();
  assert.ok(ctl.chainOptions().some(([k, n, f]) => k === "solana" && n === "Solana" && f === "lowest"));
  ({ ctl } = setup(new Set([UNREADY_SOL_USDC])));
  assert.ok(!ctl.chainOptions().map(([k]) => k).includes("solana"));
  assert.deepEqual(ctl.devWarnings(), ["Your Solana wallet can't receive USDC yet: receive any amount once."]);
});

test("test_only_unready_routes_shows_the_unavailable_message", () => {
  const cfg = buildConfig({ solana_wallet: DEV });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const spec = preparePayment(cfg, "unlock", { item: "chapter2" });
  const ctl = new CheckoutController({
    config: cfg, spec, submit: () => { throw new Error("unused"); }, unready: () => new Set([UNREADY_SOL_USDC]),
  });
  assert.ok(ctl.chainOptions().length === 0 && ctl.message() === MESSAGES["solana_unavailable"]);
});

test("test_solana_recipient_and_payment_link", async () => {
  const { ctl } = await started("solana");
  assert.ok(ctl.family === "solana" && ctl.recipient === DEV && ctl.tokenAddress === MINT);
  const uri = ctl.paymentUri()!;
  assert.ok(uri.startsWith(`solana:${DEV}?amount=4.9937`) && uri.includes(`spl-token=${MINT}`));
  assert.ok(uri.includes(`reference=${ctl.session!["reference"]}`) && uri.includes("label=Ruby%20Dreams"));
  assert.equal(ctl.session!["dest"], associatedTokenAddress(DEV, MINT));
});

test("test_evm_chain_still_uses_eip681", async () => {
  const { ctl } = await started("base");
  assert.ok(ctl.family === "evm" && ctl.recipient === DEV_EVM);
  assert.ok(ctl.paymentUri()!.startsWith("ethereum:"));
});

test("test_pasting_the_wrong_family_is_an_invalid_hash", async () => {
  let { ctl, e } = await started("solana");
  assert.ok(ctl.submitHash(EVM_TX) === false && ctl.error === "invalid_hash");
  assert.ok(ctl.submitHash(SIG) === true && e.pending[ctl.sessionId!]["hint_tx"] === SIG);
  ({ ctl } = await started("base"));
  assert.ok(ctl.submitHash(SIG) === false && ctl.error === "invalid_hash");
  assert.ok(MESSAGES["invalid_hash"]!.includes("signature"));
});

test("test_transaction_that_never_landed_lets_the_player_pay_again", async () => {
  const { ctl } = await started("solana");
  assert.ok(ctl.submitHash(SIG) === true && ctl.hint_tx === SIG);
  ctl.onOutcome(makeStepOutcome(ctl.sessionId!, "waiting", { error: "tx_not_landed" }));
  assert.ok(ctl.hint_tx === null && ctl.state === "waiting");
  assert.equal(ctl.message(), "That transaction didn't reach the network. You can pay again.");
  assert.equal(ctl.statusInfo()["tx_hash"], null);
  assert.equal(ctl.statusInfo()["error"], "tx_not_landed");
});

test("test_status_names_the_signature_that_never_landed", async () => {
  const { ctl } = await started("solana");
  assert.equal(ctl.statusInfo()["dropped_tx"], null);
  ctl.submitHash(SIG);
  ctl.onOutcome(makeStepOutcome(ctl.sessionId!, "waiting", { error: "tx_not_landed", hint_tx: SIG }));
  const info = ctl.statusInfo();
  assert.deepEqual([info["error"], info["tx_hash"], info["dropped_tx"]], ["tx_not_landed", null, SIG]);
});

test("test_a_new_hint_clears_the_old_not_landed_status", async () => {
  const { ctl, e } = await started("solana");
  ctl.submitHash(SIG);
  ctl.onOutcome(makeStepOutcome(ctl.sessionId!, "waiting", { error: "tx_not_landed", hint_tx: SIG }));
  ctl._reportHint(SIG2, DEV); // the page's POST /tx for the next send
  const info = ctl.statusInfo();
  assert.deepEqual([info["error"], info["tx_hash"], info["dropped_tx"]], [null, SIG2, null]);
  assert.ok(ctl.message() === null && e.pending[ctl.sessionId!]["hint_tx"] === SIG2);
});

for (const error of ["tx_not_landed", "tx_reverted", "tx_not_a_payment"]) {
  test(`test_a_verdict_on_an_older_hint_does_not_touch_the_newer_one[${error}]`, async () => {
    // The worker may judge the old hint just before the page reports the next one.
    const { ctl } = await started("solana");
    ctl.submitHash(SIG);
    ctl._reportHint(SIG2, DEV);
    ctl.onOutcome(makeStepOutcome(ctl.sessionId!, "waiting", { error, hint_tx: SIG }));
    const info = ctl.statusInfo();
    assert.deepEqual([info["error"], info["tx_hash"], info["dropped_tx"]], [null, SIG2, null]);
  });
}

// --- additions with no Python twin ---

test("test_solana_session_info_and_build_tx", async () => {
  const { ctl, servers, submit } = await started("solana");
  ctl.startBrowser();
  const s = servers[0]!;
  const info = s.session_info;
  assert.deepEqual(Object.keys(info), [
    "title", "kind", "token", "family", "decimals", "amount_units", "amount_text", "price_text",
    "recipient", "chain_name", "native_symbol", "explorer_tx", "testnet", "logo", "accent", "on_accent",
    "wallet_chain", "mint", "reference"]);
  assert.ok(info["family"] === "solana" && info["mint"] === MINT && info["reference"] === ctl.session!["reference"]);
  assert.equal(typeof s.build_tx, "function");
  const job = s.build_tx!(DEV_EVM);
  await submit.idle();
  assert.ok(job.done());
});
