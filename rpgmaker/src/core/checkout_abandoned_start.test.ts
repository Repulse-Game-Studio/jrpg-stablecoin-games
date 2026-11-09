/** Twin of tests/unit/test_checkout_abandoned_start.py: a start the player left must not leave a session behind. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain } from "../../test/chain_fakes.ts";
import { DeferredSubmitter } from "../../test/checkout_fakes.ts";
import { FakeClock } from "../../test/rpc_fakes.ts";
import type { Chain } from "./chains.ts";
import { CheckoutController } from "./checkout.ts";
import { addItem, buildConfig } from "./config.ts";
import { Engine } from "./engine.ts";
import { preparePayment } from "./payments.ts";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";

function setup(kind = "unlock") {
  const cfg = buildConfig({ wallet: DEV });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const fakes: Record<string, FakeChain> = {};
  const clock = new FakeClock();
  clock.now = 100.0;
  const engine = new Engine(cfg, {
    rpc_factory: (c: Chain) => (fakes[c.key] ??= new FakeChain(1000)),
    clock: clock.call,
  });
  const spec = kind === "unlock"
    ? preparePayment(cfg, "unlock", { item: "chapter2" })
    : preparePayment(cfg, "tip", { amount: "3", token: "USDC" });
  const submit = new DeferredSubmitter(engine);
  const ctl = new CheckoutController({ config: cfg, spec, submit: submit.call });
  return { ctl, engine, submit, clock };
}

test("test_switching_network_while_starting_discards_the_first_session", async () => {
  const { ctl, engine, submit } = setup();
  ctl.chooseNetwork("arbitrum");
  ctl.chooseNetwork("base");
  await submit.run();
  ctl.poll();
  assert.ok(ctl.state === "waiting" && ctl.chain_key === "base");
  await submit.run(); // the discard queued once the abandoned start finished
  assert.deepEqual(Object.values(engine.pending).map((r) => r["chain"]), ["base"]);
});

test("test_changing_the_tip_while_starting_discards_the_first_session", async () => {
  const { ctl, engine, submit } = setup("tip");
  ctl.nextStep();
  ctl.chooseNetwork("arbitrum");
  assert.ok(ctl.setTipAmount("5"));
  await submit.run();
  ctl.poll();
  await submit.run();
  assert.equal(Object.keys(engine.pending).length, 1);
  assert.equal(Object.values(engine.pending)[0]!["price"], "5.00");
});

test("test_cancel_while_starting_leaves_no_session_to_expire", async () => {
  const { ctl, engine, submit, clock } = setup();
  ctl.chooseNetwork("arbitrum");
  assert.equal(ctl.cancel().status, "cancelled");
  await submit.run();
  await submit.run();
  assert.deepEqual(engine.pending, {});
  clock.advance(31 * 60);
  assert.deepEqual((await engine.tick()).filter((o) => o.status === "expired"), []);
});

test("test_a_failed_abandoned_start_needs_no_discard", async () => {
  const { ctl, engine, submit } = setup();
  ctl.chooseNetwork("arbitrum");
  ctl.cancel();
  const { job } = submit.queue.shift()!;
  job._start();
  job._fail(new Error("rpc down"));
  await submit.run();
  assert.deepEqual(engine.pending, {});
});
