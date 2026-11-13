/**
 * TEST ONLY. The boot of the sheet test page (test/harness/ui.html), built to `dist/test/ui_driver.js`
 * and never into SCFG.js: the real glue (the same sources SCFG.js bundles: `bootPlugin`, the
 * default sheet host, the embedded stylesheet) with fake chains behind the Engine and a Worker that
 * runs only when the test asks (`scfgTest.pass()`), so a test scripts every controller state.
 *
 * The page loads plugins.js, the shim, this file, then harness.js (SceneManager.run). Drivers
 * (tests/rpgmaker_ui) call `window.scfgTest`:
 *
 *   await scfgTest.setup({ config: {...}, items: [["chapter2", {...}]] })  // configure, start, first pass
 *   scfgTest.pay("unlock", { item: "chapter2" })                           // the sheet opens; scfgTest.box
 *   await scfgTest.pass()                                                  // one worker pass + frames
 *   await scfgTest.payOnChain()                                            // the open payment confirms
 *
 * Reports of the glue go to `console.error` / `console.warn`, so a test sees them as page errors.
 */
import embedded from "scfg:assets";
import { MAINNET, qrRects, type Chain, type PaymentResult, type RestoreResult } from "../../src/core/index.ts";
import { installAssets } from "../../src/mz/assets.ts";
import { bootPlugin } from "../../src/mz/boot.ts";
import type { ScfgHooks } from "../../src/mz/scfg.ts";
import { DEFAULT_TEXTS } from "../../src/mz/texts.ts";
import { FakeChain, transferLog } from "../chain_fakes.ts";
import { FakeServer } from "../checkout_fakes.ts";
import { FakeClock, FakeTransport } from "../rpc_fakes.ts";
import { HeadlessHost } from "../e2e/headless_host.ts";

const w = globalThis as any;

export const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
export const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
export const TX = "0x" + "ab".repeat(32);
export const CHAPTER2 = { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" };

installAssets(embedded);

const chains: Record<string, FakeChain> = {};
const clock = new FakeClock();
const chainFor = (chain: Chain): FakeChain => {
  chains[chain.key] ??= new FakeChain(1000);
  chains[chain.key]!.chain_obj = chain;
  return chains[chain.key]!;
};

const hooks: ScfgHooks = {
  transport: new FakeTransport({}),
  engine_opts: { rpc_factory: chainFor as any, clock: clock.call },
  worker_opts: { schedule: () => null, cancel: () => undefined },
};
const scfg = bootPlugin(hooks);
/** URLs the desktop platform opened (nw.Shell.openExternal), after `scfgTest.desktop()`. */
const opened: string[] = [];

/** Resolves after `n` animation frames (each one SceneManager.updateMain, the pump and the screens). */
function frames(n = 2): Promise<void> {
  return new Promise((resolve) => {
    const step = (left: number) => (left <= 0 ? resolve() : w.requestAnimationFrame(() => step(left - 1)));
    step(n);
  });
}

async function until(what: string, ok: () => boolean, maxFrames = 300): Promise<void> {
  for (let i = 0; i < maxFrames; i++) {
    if (ok()) return;
    await frames(1);
  }
  throw new Error(`scfgTest: ${what} did not happen in ${maxFrames} frames`);
}

/** One worker pass (the pending commands and a watch tick), then a few frames for the pump and the screens. */
async function pass(): Promise<void> {
  await scfg._worker!.runOnce();
  await frames(3);
}

interface Box<T> {
  result: T | null;
  error: string | null;
  done: boolean;
}

const box = <T>(p: Promise<T>): Box<T> => {
  const b: Box<T> = { result: null, error: null, done: false };
  p.then((r) => Object.assign(b, { result: r, done: true }), (e) => Object.assign(b, { error: String(e), done: true }));
  return b;
};

/** The open payment sheet's controller (the newest), or null. */
function checkout(): any {
  const all = [...scfg._checkouts.values()];
  return all.length > 0 ? all[all.length - 1] : null;
}

function restoreController(): any {
  const all = [...scfg._restores.values()];
  return all.length > 0 ? all[all.length - 1] : null;
}

/** What the game sees each frame (Game_Player's view of Input, and MZ's own key state), recorded by `probe.start()`. */
const probe = {
  log: [] as Record<string, unknown>[],
  on: false,
  start(): void {
    this.log = [];
    this.on = true;
  },
  stop(): Record<string, unknown>[] {
    this.on = false;
    return this.log;
  },
};
{
  const sm = w.SceneManager;
  const updateInputData = sm.updateInputData;
  sm.updateInputData = function (this: unknown, ...args: unknown[]) {
    const r = updateInputData.apply(this, args);
    if (probe.on) {
      const I = w.Input;
      const raw = Object.keys(I._currentState).filter((k) => I._currentState[k]);
      probe.log.push({
        raw, dir4: I.dir4, dir8: I.dir8, canMove: w.$gamePlayer ? w.$gamePlayer.canMove() : null,
        game: ["ok", "cancel", "menu", "up", "down", "left", "right", "pageup", "pagedown", "escape"].filter((k) => I.isPressed(k) || I.isTriggered(k)),
        touch: w.TouchInput.isTriggered() || w.TouchInput.isPressed() || w.TouchInput.isClicked(),
        x: w.$gamePlayer?.x, y: w.$gamePlayer?.y, scene: sm._scene?.constructor?.name ?? null,
      });
    }
    return r;
  };
}

w.scfgTest = {
  DEV, PAYER, TX, CHAPTER2, chains, clock, probe, frames, pass, until, checkout, restoreController, opened,
  texts: DEFAULT_TEXTS,
  box: null as Box<PaymentResult> | Box<RestoreResult> | null,

  /** True once the shim booted to the map and the stored state was read. */
  booted(): boolean {
    const scene = w.SceneManager._scene;
    return !!scene && scene instanceof w.Scene_Map && scene.isStarted() && scfg.stateLoaded();
  },

  /**
   * Play a desktop (NW.js) game from here on: the platform says NW.js, `nw.Shell.openExternal`
   * records into `opened`, and the browser checkout page is a FakeServer (no Node server in a page).
   * Call before pay(): the controller learns the platform when it is made.
   */
  desktop(): void {
    const p = scfg._platform as any;
    p.isNwjs = () => true;
    p.isWeb = () => false;
    p.nw = () => ({ Shell: { openExternal: (url: string) => void opened.push(url) } });
    hooks.server_factory = (args) => new FakeServer(args as any) as any;
  },

  /** configure() (wallet DEV unless given), the items, then the first worker pass. */
  async setup(opts: { config?: Record<string, unknown>; items?: [string, Record<string, unknown>][] } = {}): Promise<void> {
    scfg.configure({ wallet: DEV, ...opts.config });
    for (const [id, item] of opts.items ?? [["chapter2", CHAPTER2]]) scfg.addItem(id, item);
    await until("the worker start", () => scfg._worker !== null);
    await pass();
  },

  pay(kind: string, opts: Record<string, unknown> = {}): void {
    this.box = box(scfg.pay(kind, opts as any));
  },

  restore(opts: Record<string, unknown> = {}): void {
    this.box = box(scfg.restore(opts as any));
  },

  /** The button names the desktop e2e driver's headless host (test/e2e/headless_host.ts) gives the open payment sheet's step. */
  headlessButtons(): string[] | null {
    const c = checkout();
    return c === null ? null : new HeadlessHost(scfg).buttonNames("payment", c);
  },

  /** The open checkout controller's fields the tests compare the sheet with. */
  view(): Record<string, unknown> | null {
    const c = checkout();
    if (c === null) return null;
    return {
      step: c.step, state: c.state, canGoBack: c.canGoBack, txSeen: c.txSeen, dots: c.stepDots(), method: c.method,
      chainKey: c.chain_key, chainOptions: c.chainOptions(), methods: c.methods(), session: c.sessionId, hint: c.hint_tx,
      txInput: c.tx_input, error: c.error, uri: c.paymentUri(), payAmount: c.payAmountText, recipient: c.recipient,
      tokenAddress: c.tokenAddress,
    };
  },

  restoreView(): Record<string, unknown> | null {
    const c = restoreController();
    return c === null ? null : { state: c.state, txInput: c.tx_input, error: c.error, choices: { ...c.choices }, added: c.added.length };
  },

  /** The open payment is paid on its chain in a new block (TX), then gets its confirmations. */
  async payOnChain(opts: { units?: string } = {}): Promise<void> {
    const c = checkout();
    const chain = chains[c.chain_key]!;
    const token = c.tokenAddress as string;
    const units = BigInt(opts.units ?? c.session.units);
    const block = chain.head + 1;
    chain.logs.push(transferLog(token, PAYER, DEV, units, { block, tx_hash: TX }));
    chain.receipts[TX] = { status: "0x1", blockNumber: "0x" + block.toString(16), logs: [transferLog(token, PAYER, DEV, units, { block, tx_hash: TX })] };
    chain.head = block;
    await pass();
    chain.head = block + (c.required > 0 ? c.required : 1) - 1;
    await pass();
    await pass();
  },

  /** A finished USDC transfer to DEV on Arbitrum, for the restore screen to find by its hash. */
  receiptOnArbitrum(units: string, tx: string = TX): void {
    const chain = chains["arbitrum"] ??= new FakeChain(1000);
    const token = MAINNET["arbitrum"]!.tokens["USDC"]!.address;
    chain.receipts[tx] = { status: "0x1", blockNumber: "0xa", logs: [transferLog(token, PAYER, DEV, BigInt(units), { block: 10, tx_hash: tx })] };
  },

  /** The core's QR runs for `text` (size, [x, y, width] runs), as the sheet should draw them. */
  qrRects(text: string): unknown {
    return qrRects(text, 2);
  },
};
