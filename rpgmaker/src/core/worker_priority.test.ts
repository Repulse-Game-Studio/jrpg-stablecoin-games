/**
 * Twin of tests/unit/test_worker_priority.py. Testbed U15: a session start the player waits on
 * comes before the background watching of pending payments. With pending sessions whose checks
 * hang (a DNS that never answers), the start submitted while a tick runs makes the tick yield at
 * once, its request in flight included, and starts in well under the per-request timeout; the
 * yielded tick keeps its outcomes and the next tick steps the sessions again. A real Engine on a
 * real Worker loop.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { TransportError, type Cancel } from "./coio.ts";
import { addItem, buildConfig } from "./config.ts";
import { Engine } from "./engine.ts";
import { Worker } from "./worker.ts";

const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";

/** Answers eth_blockNumber and eth_getLogs, or hangs every request while `hang` is set. */
class Node {
  hang = false;
  methods: string[] = [];
  cancels: Cancel[] = [];
  private _release!: () => void;
  private readonly _released = new Promise<void>((r) => { this._release = r; });
  private _hung!: () => void;
  readonly hung = new Promise<void>((r) => { this._hung = r; });

  async postJson(_url: string, payload: any, timeout: number, cancel?: Cancel): Promise<unknown> {
    this.methods.push(payload.method);
    if (this.hang) {
      if (cancel) this.cancels.push(cancel);
      this._hung();
      let timer: ReturnType<typeof setTimeout> | undefined;
      await Promise.race([this._released, new Promise((r) => { timer = setTimeout(r, timeout * 1000); })]);
      clearTimeout(timer);
      throw new TransportError(`timed out after ${timeout}s`);
    }
    const result = payload.method === "eth_blockNumber" ? "0x3e8" : [];
    return { jsonrpc: "2.0", id: payload.id, result };
  }

  release(): void {
    this._release();
  }
}

function within<T>(p: Promise<T>, ms: number, what: string): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  return Promise.race([p, new Promise<never>((_r, reject) => {
    timer = setTimeout(() => reject(new Error(what)), ms);
  })]).finally(() => clearTimeout(timer));
}

test("test_a_start_does_not_wait_behind_the_watching_of_pending_payments", async () => {
  const cfg = buildConfig({ wallet: DEV, chains: ["base", "arbitrum"], testnet: true });
  addItem(cfg, "chapter2", { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" });
  const node = new Node();
  const errors: unknown[] = []; // the Worker's and the Engine's (an Abandoned step is no internal_error)
  const engine = new Engine(cfg, { transport: node, clock: () => 1700000000, on_error: (e) => errors.push(e) });
  for (let i = 0; i < 3; i++) { // three pending tips, as in the testbed
    await engine.startSession("tip", "base", { amount: "1.00", token: "USDC" });
  }
  assert.equal(Object.keys(engine.pending).length, 3);
  const w = new Worker(engine, { interval: () => 60, on_error: (e) => errors.push(e) });
  node.hang = true;
  w.start();
  try {
    await within(node.hung, 5000, "the tick asked the node");
    node.hang = false; // the network is back for the start (the tick's request still hangs)
    const started = performance.now();
    const job = w.submit((e) => e.startSession("unlock", "arbitrum", { item: "chapter2" }));
    const rec = await within(job.promise, 5000, "the start never finished");
    const took = performance.now() - started;
    assert.ok(took < 2000, `the start waited ${(took / 1000).toFixed(1)} s behind the tick`);
    assert.equal(rec["chain"], "arbitrum");
    assert.equal(Object.keys(engine.pending).length, 4, "the pending payments are untouched");
    assert.ok(node.cancels.length >= 1 && node.cancels.every((c) => c.cancelled), "the tick's request was cancelled");
    assert.deepEqual(errors, []);
  } finally {
    node.release();
    w.stop();
  }
});
