/** Twin of tests/unit/test_restore_flow.py plus test_checkout_solana.py::test_restore_accepts_signatures. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import { immediateSubmitter } from "../../test/checkout_fakes.ts";
import { sig } from "../../test/solana_fakes.ts";
import { Job } from "./worker.ts";
import { MAINNET, type Chain } from "./chains.ts";
import { addItem, buildConfig } from "./config.ts";
import { Engine } from "./engine.ts";
import { TIP_CHOICE } from "./restore.ts";
import { RestoreController } from "./restore_flow.ts";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const USDC_ARB = MAINNET["arbitrum"]!.tokens["USDC"]!.address;
const TX = "0x" + "ab".repeat(32);
const SIG = sig(7);

function setup(logs: Record<string, any>[] = [], status = "0x1") {
  const cfg = buildConfig({ wallet: DEV });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const chains: Record<string, FakeChain> = {};
  for (const k of Object.keys(MAINNET)) chains[k] = new FakeChain();
  if (logs.length > 0 || status === "0x0") {
    chains["arbitrum"]!.receipts[TX] = { status, logs: [...logs] };
  }
  const engine = new Engine(cfg, { rpc_factory: (c: Chain) => chains[c.key]! });
  const submit = immediateSubmitter(engine);
  return { ctl: new RestoreController({ submit }), engine, submit };
}

async function run(ctl: RestoreController, submit: ReturnType<typeof immediateSubmitter>, text = TX) {
  ctl.tx_input = text;
  ctl.check();
  await submit.idle();
  ctl.poll();
  if (ctl.state === "saving") {
    await submit.idle();
    ctl.poll();
  }
}

const log = (value: number) => transferLog(USDC_ARB, PAYER, DEV, value, { block: 5, tx_hash: TX });

test("test_automatic_restore", async () => {
  const { ctl, engine, submit } = setup([log(4_993_742)]);
  await run(ctl, submit);
  assert.equal(ctl.state, "done");
  assert.ok(ctl.result().status === "restored" && ctl.result().restored[0]!["item"] === "chapter2");
  assert.deepEqual([...engine.snapshot().owned], ["chapter2"]);
});

test("test_already_restored_is_nothing_found", async () => {
  const { ctl, submit } = setup([log(4_993_742)]);
  await run(ctl, submit);
  const ctl2 = new RestoreController({ submit: ctl.submit });
  await run(ctl2, submit);
  assert.ok(ctl2.state === "done" && ctl2.result().status === "nothing_found");
});

test("test_ambiguous_amount_needs_choice", async () => {
  const { ctl, engine, submit } = setup([log(5_000_000)]);
  await run(ctl, submit);
  assert.ok(ctl.state === "choose");
  assert.deepEqual(ctl.pendingChoices(), [0]);
  assert.deepEqual(ctl.candidates[0]!.choices, ["chapter2", TIP_CHOICE]);
  ctl.choose(0, "chapter2");
  ctl.confirm();
  await submit.idle();
  ctl.poll();
  assert.equal(ctl.state, "done");
  assert.deepEqual([...engine.snapshot().owned], ["chapter2"]);
});

test("test_restore_waits_until_every_choice_is_answered", async () => {
  const { ctl, submit } = setup([log(5_000_000)]);
  await run(ctl, submit);
  assert.equal(ctl.canConfirm, false);
  ctl.confirm(); // e.g. a click that raced the screen: nothing happens
  assert.equal(ctl.state, "choose");
  ctl.choose(0, TIP_CHOICE);
  assert.equal(ctl.canConfirm, true);
});

test("test_errors_return_to_input", async () => {
  const { ctl, submit } = setup();
  await run(ctl, submit, "nonsense");
  assert.ok(ctl.state === "input" && ctl.error === "invalid_hash");
  await run(ctl, submit);
  assert.ok(ctl.state === "input" && (ctl.error as string | null) === "tx_not_found" && ctl.message() !== null);
});

test("test_reverted", async () => {
  const { ctl, submit } = setup([], "0x0");
  await run(ctl, submit);
  assert.equal(ctl.error, "tx_reverted");
});

test("test_cancel_result", () => {
  const { ctl } = setup();
  assert.equal(ctl.result().status, "cancelled");
});

test("test_a_blocked_paste_shows_the_restore_wording", () => {
  const { ctl } = setup();
  ctl.error = "paste_blocked";
  assert.ok(ctl.message()!.includes("Type the hash instead"));
});

test("test_restore_accepts_signatures", () => {
  const submitted: unknown[] = [];
  const ctl = new RestoreController({ submit: (fn) => { submitted.push(fn); return null as any; } });
  assert.ok(ctl.check(SIG) === true && submitted.length > 0);
  assert.ok(ctl.check("nonsense") === false && ctl.error === "invalid_hash");
});

test("test_a_cancelled_job_lands_in_failed", () => {
  const job = new Job<any>();
  job.cancel();
  const ctl = new RestoreController({ submit: () => job });
  assert.ok(ctl.check(TX) === true);
  ctl.poll();
  assert.ok(ctl.state === "input" && ctl.error === "failed");
  assert.equal(ctl.message(), "Something went wrong. Please try again.");
});
