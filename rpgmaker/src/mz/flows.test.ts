/**
 * The SCFG glue's payment and restore flows on the MZ shim: `await SCFG.pay()`, `await
 * SCFG.restore()`, the sheet host they show their screens through, "keep playing" payments,
 * callbacks versus `SCFG.onResult`, notifications, the sheet's actions (links, clipboard) and the
 * shutdown. Twins of godot/tests/test_scfg_flows.gd (the same names, camelCase); behaviour
 * authority plugin/game/scfg/scfg_api.rpy (_open_checkout, _close_checkout, _deliver,
 * _open_restore, _close_restore, _deliver_restore, _open_*, Copy, _take_paste, _shutdown).
 *
 * Each test runs on a fresh shim with a fake sheet host, fake NW.js / browser objects behind the
 * platform seams, FakeChains behind the Engine and a Worker that passes only on `runOnce()`.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import crypto from "node:crypto";
import http from "node:http";
import https from "node:https";
import net from "node:net";
import fs, { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { runInNewContext } from "node:vm";
import { FakeChain, transferLog } from "../../test/chain_fakes.ts";
import { FakeServer } from "../../test/checkout_fakes.ts";
import { FakeClock, FakeTransport } from "../../test/rpc_fakes.ts";
import { installDiskAssets } from "../../test/mz_assets.ts";
import { MemoryStorage, bootShim, installShim, resetShim, type Shim } from "../../test/mz_shim/index.ts";
import {
  ConfigError, FetchTransport, MAINNET, makePaymentResult, type Chain, type CheckoutController, type PaymentResult, type RestoreController,
  type RestoreResult, type ServerArgs,
} from "../core/index.ts";
import { addReceipt } from "../core/receipts.ts";
import { makeStepOutcome } from "../core/watcher.ts";
import { TX_CHARS, cleanPaste, scanPaste } from "./actions.ts";
import { pageAsset } from "./assets.ts";
import { installHooks } from "./boot.ts";
import { CheckoutServer as LocalServer } from "./local_server.ts";
import { NodeHttpsTransport } from "./node_transport.ts";
import { Scfg, type ScfgHooks } from "./scfg.ts";
import type { SheetHandle, SheetHost } from "./sheet_host.ts";

const g = globalThis as any;
const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const TX = "0x" + "ab".repeat(32);
const STATE = "save/scfg.rmmzsave";
const CHAPTER2 = { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" };
const USDC_ARB = MAINNET["arbitrum"]!.tokens["USDC"]!.address;

afterEach(() => resetShim());

const code = (c: string) => (e: unknown) => e instanceof ConfigError && e.code === c;

interface Sheet {
  kind: "payment" | "restore";
  ctl: any;
  handle: SheetHandle;
  close(how: string): void;
}

/** The sheet host the flows show their screens through, recording what they ask for. */
class FakeHost implements SheetHost {
  sheets: Sheet[] = [];
  closes = 0;
  notes: string[] = [];
  journal: unknown[] = []; // shared with a RecordingServer: the order things happened in

  openPayment(ctl: CheckoutController): SheetHandle | null {
    return this._open("payment", ctl);
  }

  openRestore(ctl: RestoreController): SheetHandle | null {
    return this._open("restore", ctl);
  }

  _open(kind: Sheet["kind"], ctl: unknown): SheetHandle {
    let close!: (how: string) => void;
    const handle: SheetHandle = { closed: new Promise<any>((resolve) => (close = resolve)) };
    this.sheets.push({ kind, ctl, handle, close });
    return handle;
  }

  close(_handle: SheetHandle): void {
    this.closes++;
    this.journal.push("sheet closed");
  }

  notify(text: string): void {
    this.notes.push(text);
  }

  get last(): Sheet {
    assert.ok(this.sheets.length > 0, "a sheet is open");
    return this.sheets[this.sheets.length - 1]!;
  }

  get kinds(): string[] {
    return this.sheets.map((s) => s.kind);
  }
}

/** The desktop page, noting when it is stopped. */
class RecordingServer extends FakeServer {
  journal: unknown[];
  constructor(kw: ServerArgs, journal: unknown[]) {
    super(kw as any);
    this.journal = journal;
  }
  override stop(): void {
    if (this.running) this.journal.push("page stopped");
    super.stop();
  }
}

/** What the platform seams reach: NW.js's `nw`, the page's `window` and `navigator`. */
interface World {
  /** Every URL opened: ["nw", url] (nw.Shell.openExternal) or ["window", url, target, features] (window.open). */
  opened: unknown[][];
  /** `held`: when an array, the browser's readText answers wait there until the test resolves them. */
  clipboard: { text: string; refuse: boolean; nwBroken: boolean; held: ((text: string) => void)[] | null };
  nwListeners: Map<string, ((this: any) => void)[]>;
  nwClosed: unknown[];
  nwAvailable: boolean;
  win: EventTarget & { open?: (...a: unknown[]) => unknown; visibilityState?: string };
}

interface Env {
  shim: Shim;
  s: Scfg;
  storage: MemoryStorage;
  chains: Record<string, FakeChain>;
  clock: FakeClock;
  host: FakeHost;
  world: World;
  errors: string[];
}

function makeWorld(win: EventTarget): World {
  const world: World = {
    opened: [], clipboard: { text: "", refuse: false, nwBroken: false, held: null }, nwListeners: new Map(), nwClosed: [], nwAvailable: true,
    win: win as World["win"],
  };
  world.win.open = (...a: unknown[]) => {
    world.opened.push(["window", ...a]);
    return {};
  };
  return world;
}

function fakeNw(world: World) {
  // the NW.js Window object: on(event, fn) and close(force); not an EventEmitter (no listeners())
  const nwWindow = {
    on(event: string, fn: (this: any) => void) {
      if (!world.nwListeners.has(event)) world.nwListeners.set(event, []);
      world.nwListeners.get(event)!.push(fn);
      return nwWindow;
    },
    close(force?: boolean) {
      world.nwClosed.push(force);
    },
  };
  return {
    Shell: { openExternal: (url: string) => void world.opened.push(["nw", url]) },
    Clipboard: {
      get: () => ({
        get: (type?: string) => {
          if (world.clipboard.nwBroken) throw new Error("no clipboard");
          assert.equal(type ?? "text", "text");
          return world.clipboard.text;
        },
        set: (text: string, type?: string) => {
          if (world.clipboard.nwBroken) throw new Error("no clipboard");
          assert.equal(type ?? "text", "text");
          world.clipboard.text = text;
        },
      }),
    },
    Window: { get: () => nwWindow },
  };
}

function fakeNavigator(world: World) {
  return {
    clipboard: {
      writeText: async (text: string) => {
        if (world.clipboard.refuse) throw new DOMException("Write permission denied.", "NotAllowedError");
        world.clipboard.text = text;
      },
      readText: async () => {
        if (world.clipboard.held !== null) return new Promise<string>((resolve) => world.clipboard.held!.push(resolve));
        if (world.clipboard.refuse) throw new DOMException("Read permission denied.", "NotAllowedError");
        return world.clipboard.text;
      },
    },
  };
}

/**
 * A configured SCFG with chapter2 declared, booted on the shim, its Worker started and its first
 * pass run. Desktop (NW.js) unless `nwjs: false`.
 */
async function make(opts: {
  nwjs?: boolean; mobile?: boolean; hooks?: Partial<ScfgHooks>; storage?: MemoryStorage; beforeHooks?: () => void;
  config?: Record<string, unknown>;
} = {}): Promise<Env> {
  const storage = opts.storage ?? new MemoryStorage();
  const shim = installShim(g, { nwjs: opts.nwjs ?? true, mobile: opts.mobile ?? false, storage, options: ["test"] });
  const world = makeWorld(shim.window);
  const chains: Record<string, FakeChain> = {};
  const clock = new FakeClock();
  const errors: string[] = [];
  const factory = (chain: Chain) => {
    chains[chain.key] ??= new FakeChain(1000);
    chains[chain.key]!.chain_obj = chain;
    return chains[chain.key]!;
  };
  const line = (args: unknown[]) => args.map((a) => (a instanceof Error ? `${a.name}: ${a.message}` : String(a))).join(" ");
  const nw = fakeNw(world);
  const nav = fakeNavigator(world);
  const hooks: ScfgHooks = {
    clock: clock.call, transport: new FakeTransport({}),
    engine_opts: { rpc_factory: factory, clock: clock.call },
    worker_opts: { schedule: () => null, cancel: () => undefined },
    log: { error: (...a: unknown[]) => void errors.push(line(a)), warn: () => undefined },
    platform: {
      devNotice: () => undefined,
      nw: () => (g.Utils.isNwjs() && world.nwAvailable ? nw : null),
      window: () => world.win,
      navigator: () => nav,
    },
    ...opts.hooks,
  };
  hooks.platform = {
    devNotice: () => undefined, nw: () => (g.Utils.isNwjs() && world.nwAvailable ? nw : null), window: () => world.win, navigator: () => nav,
    ...opts.hooks?.platform,
  };
  const s = new Scfg(hooks);
  g.SCFG = s;
  opts.beforeHooks?.();
  installHooks(s);
  const host = new FakeHost();
  s.sheetHost = host;
  s.configure({ wallet: DEV, ...opts.config });
  s.addItem("chapter2", CHAPTER2);
  await bootShim();
  await s._mirror.idle();
  shim.frame();
  assert.ok(s._worker !== null, "the pump started the worker");
  await s._worker.runOnce();
  shim.frame();
  return { shim, s, storage, chains, clock, host, world, errors };
}

/** One worker pass, then one frame. */
async function pass(env: Env): Promise<void> {
  await env.s._worker!.runOnce();
  env.shim.frame();
}

/** Lets promise reactions run (a sheet's `closed`, the clipboard's answer). */
const tick = () => new Promise<void>((r) => setTimeout(r, 0));

/** The open sheet's controller, with a session started on Arbitrum. */
async function session(env: Env): Promise<any> {
  const ctl = env.host.last.ctl;
  ctl.chooseNetwork("arbitrum");
  await pass(env);
  assert.ok(ctl.sessionId, "a session started");
  return ctl;
}

/** Pays the session on the fake chain in a new block and lets it confirm (Arbitrum: 20 confirmations). */
async function payOnChain(env: Env, ctl: any): Promise<void> {
  const chain = env.chains["arbitrum"]!;
  const block = chain.head + 1;
  chain.logs.push(transferLog(USDC_ARB, PAYER, DEV, BigInt(ctl.session.units), { block, tx_hash: TX }));
  chain.head = block;
  await pass(env);
  chain.head = block + 19;
  await pass(env);
}

/** A pay() whose result the test reads later; `box.result` is set once it resolves. */
function pay(s: Scfg, kind: unknown, opts: Record<string, unknown> = {}) {
  const box: { result?: PaymentResult; error?: unknown; promise: Promise<PaymentResult> } = { promise: s.pay(kind, opts as any) };
  box.promise.then((r) => (box.result = r), (e) => (box.error = e));
  return box;
}

function restore(s: Scfg, opts: Record<string, unknown> = {}) {
  const box: { result?: RestoreResult; error?: unknown; promise: Promise<RestoreResult> } = { promise: s.restore(opts as any) };
  box.promise.then((r) => (box.result = r), (e) => (box.error = e));
  return box;
}

const pendingById = (s: Scfg, sid: string) => s.pending().find((r) => r["id"] === sid);

// --- pay -------------------------------------------------------------------------------------

test("payAlreadyOwnedShortCircuits", async () => {
  const env = await make();
  const { s, host } = env;
  const job = s._worker!.submit((e) => addReceipt(e.receipts, {
    chain: "arbitrum", tx_hash: TX, log_index: 3, block: 990, kind: "unlock", item: "chapter2", amount: "4.99",
    token: "USDC", payer: PAYER.toLowerCase(), recipient: DEV.toLowerCase(), source: "payment",
  } as any));
  await pass(env);
  assert.equal(job.result(), true, "a receipt for chapter2");
  const called: PaymentResult[] = [];
  const resolved: PaymentResult[] = [];
  s.onResult = (r) => void resolved.push(r);
  const r = await s.pay("unlock", { item: "chapter2", callback: (got) => void called.push(got) });
  assert.deepEqual([r.status, r.kind, r.item, r.token, r.session_id], ["already_owned", "unlock", "chapter2", "USDC", null],
    "already_owned, at once");
  assert.equal(host.sheets.length, 0, "no sheet was shown");
  assert.equal(s._checkouts.size, 0, "no checkout");
  assert.ok(called.length === 1 && called[0] === r, "the callback gets it too (as Ren'Py's ScfgPay and Godot)");
  assert.equal(resolved.length, 0, "not onResult");
});

test("payCancelReturnsCancelledAndCallsCallback", async () => {
  const env = await make();
  const { s, host } = env;
  const called: PaymentResult[] = [];
  const resolved: PaymentResult[] = [];
  s.onResult = (r) => void resolved.push(r);
  let box = pay(s, "unlock", { item: "chapter2", callback: (r: PaymentResult) => void called.push(r) });
  assert.deepEqual(host.kinds, ["payment"], "the payment sheet is shown at once");
  await tick();
  assert.equal(box.result, undefined, "pay() waits for the sheet");
  const ctl = await session(env);
  const sid = ctl.sessionId;
  assert.ok(s._callbacks.has(sid), "the callback is registered for the session");
  host.last.close("cancel");
  const r = await box.promise;
  assert.deepEqual([r.status, r.kind, r.item, r.session_id], ["cancelled", "unlock", "chapter2", sid], "cancelled");
  assert.ok(called.length === 1 && called[0] === r, "the callback got the same result");
  assert.equal(resolved.length, 0, "not onResult: the callback had it");
  assert.deepEqual([host.closes, s._checkouts.size, s._background.size], [1, 0, 0], "the sheet is closed and the checkout gone");
  await pass(env);
  assert.equal(pendingById(s, sid)?.["ui_status"], "cancelled", "the session is marked cancelled");
  // without a callback, the cancel reaches onResult (Ren'Py's on_result)
  box = pay(s, "unlock", { item: "chapter2" });
  host.last.close("cancel");
  const r2 = await box.promise;
  assert.equal(r2.status, "cancelled", "cancelled before any session");
  assert.deepEqual(resolved.map((x) => x === r2), [true], "and given to onResult");
});

test("keepPlayingDeliversLaterResultViaOnResult", async () => {
  const env = await make();
  const { s, host } = env;
  const resolved: PaymentResult[] = [];
  s.onResult = (r) => void resolved.push(r);
  const box = pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  const sid = ctl.sessionId;
  host.last.close("keep_playing");
  const r = await box.promise;
  assert.deepEqual([r.status, r.session_id], ["pending", sid], "pending, while the payment confirms");
  assert.deepEqual([s._checkouts.size, s._background.size], [0, 1], "the controller moved to the background");
  assert.equal(resolved.length, 0, "nothing resolved yet");
  await pass(env);
  assert.equal(pendingById(s, sid)?.["ui_status"], "pending", "the session is marked pending");
  await payOnChain(env, ctl);
  assert.equal(resolved.length, 1, "the final result reaches onResult");
  assert.deepEqual([resolved[0]!.status, resolved[0]!.item, resolved[0]!.session_id, resolved[0]!.tx_hash], ["paid", "chapter2", sid, TX]);
  assert.deepEqual(host.notes, ["Payment confirmed: Chapter 2. Thank you!"], "a notification (notify is on by default, as Ren'Py)");
  assert.equal(s._background.size, 0, "a final background controller is closed and dropped");
  assert.equal(s.owns("chapter2"), true, "owned");
});

test("paidTipNotifiesWithoutATitleAndRespectsNotifyOff", async () => {
  const env = await make();
  const { s, host } = env;
  s.notify = false;
  const called: PaymentResult[] = [];
  pay(s, "tip", { amount: "3.00", token: "USDC", callback: (r: PaymentResult) => void called.push(r) });
  assert.equal(host.last.ctl.nextStep(), true, "past the amount step");
  const ctl = await session(env);
  await payOnChain(env, ctl);
  assert.deepEqual(called.map((r) => r.status), ["paid"], "the callback gets the paid result");
  assert.deepEqual(host.notes, [], "no notification: notify is off");
  s.notify = true;
  s._deliver(makePaymentResult({ status: "paid", kind: "tip", session_id: "ps_x" }));
  assert.deepEqual(host.notes, ["Payment confirmed. Thank you for your support!"], "a tip's notification");
  s._deliver(makePaymentResult({ status: "expired", kind: "tip", session_id: "ps_y" }));
  assert.equal(host.notes.length, 1, "only paid results notify");
});

test("notificationsUseTheTexts", async () => {
  const env = await make();
  const { s, host } = env;
  assert.equal(s.text("payment_confirmed_item", "Chapter 2"), "Payment confirmed: Chapter 2. Thank you!");
  s._paramTexts = { payment_confirmed: "Danke!", payment_confirmed_item: "From the parameter: %s" };
  s.texts = { payment_confirmed_item: "Bezahlt: %s. Danke!" };
  s._deliver(makePaymentResult({ status: "paid", kind: "unlock", item: "chapter2", session_id: "ps_x" }));
  s._deliver(makePaymentResult({ status: "paid", kind: "tip", session_id: "ps_y" }));
  assert.deepEqual(host.notes, ["Bezahlt: Chapter 2. Danke!", "Danke!"], "SCFG.texts wins over the Texts parameter, which wins over the default");
  assert.equal(new Scfg({ platform: { isTestPlay: () => false } }).text("no_such_id"), "no_such_id", "an unknown id shows itself in a shipped game");
  assert.throws(() => s.text("no_such_id"), /no_such_id/, "and fails loudly in test play");
});

test("closingTheSheetReleasesThePageFirst", async () => {
  const journal: unknown[] = [];
  const servers: RecordingServer[] = [];
  const factory = (kw: ServerArgs) => {
    const srv = new RecordingServer(kw, journal);
    servers.push(srv);
    return srv;
  };
  const env = await make({ hooks: { server_factory: factory } });
  const { s, host, world } = env;
  host.journal = journal;
  pay(s, "unlock", { item: "chapter2" });
  let ctl = await session(env);
  s.openBrowser(ctl);
  assert.equal(servers.length, 1, "the browser checkout started the desktop page");
  let srv = servers[0]!;
  assert.deepEqual(world.opened, [["nw", srv.url]], "and opened the browser on it (nw.Shell.openExternal)");
  assert.equal(srv.build_tx, undefined, "an EVM page gets no build_tx (no /build route, as in Python)");
  host.last.close("cancel");
  await tick();
  assert.deepEqual(journal, ["page stopped", "sheet closed"], "the page is stopped first, and only then is the sheet closed");
  assert.equal(srv.running, false, "not running");
  // keep playing: the page keeps answering until the session is final or the browser timeout
  journal.length = 0;
  pay(s, "unlock", { item: "chapter2" });
  ctl = await session(env);
  s.openBrowser(ctl);
  srv = servers[servers.length - 1]!;
  host.last.close("keep_playing");
  await tick();
  assert.deepEqual(journal, ["sheet closed"], "keep playing leaves the page running");
  assert.equal(srv.running, true);
  env.shim.frame(3);
  assert.equal(srv.running, true, "still running inside the browser timeout");
  env.clock.advance(Number(s._config!.methods["browser"]!["timeout_min"]) * 60 + 1);
  env.shim.frame();
  assert.deepEqual(journal, ["sheet closed", "page stopped"], "past the browser timeout the page is stopped");
  assert.equal(s._background.size, 0, "and the controller dropped");
});

test("noSheetUiFailsClearly", async () => {
  // as Godot's test_no_sheet_ui_fails_clearly: a host that shows nothing is not the
  // script's misuse, so pay()/restore() resolve "failed" with the code (never reject: an
  // unhandled rejection stops an MZ game), reported, and the callback gets it too
  const env = await make();
  const { s } = env;
  const called: unknown[] = [];
  s.sheetHost = { openPayment: () => null, openRestore: () => null, close: () => undefined, notify: () => undefined };
  const r = await s.pay("unlock", { item: "chapter2", callback: (x) => void called.push(x) });
  assert.deepEqual([r.status, r.error, r.kind, r.item], ["failed", "sheet.unavailable", "unlock", "chapter2"], "a failed result");
  assert.deepEqual(called, [r], "given to the callback too");
  assert.equal(s._checkouts.size, 0, "no checkout left open");
  const rr = await s.restore({ callback: (x) => void (called as unknown[]).push(x) });
  assert.deepEqual([rr.status, rr.error, rr.restored], ["failed", "sheet.unavailable", []], "restore too");
  assert.deepEqual(called, [r, rr]);
  assert.equal(s._restores.size, 0, "no restore left open");
  assert.equal(env.errors.filter((e) => /sheet\.unavailable|no screen to show/.test(e)).length, 2, "each reported");
  s.sheetHost = { openPayment: () => ({}) as any, openRestore: () => 7 as any, close: () => undefined, notify: () => undefined };
  assert.equal((await s.pay("unlock", { item: "chapter2" })).error, "sheet.unavailable", "a handle without a closed Promise");
  assert.equal((await s.restore()).error, "sheet.unavailable");
  assert.deepEqual([s._checkouts.size, s._restores.size], [0, 0]);
});

test("aNonHandleIsClosedAfterTheControllerIsDropped", async () => {
  // Godot's order everywhere: the controller first, then the screen
  const env = await make();
  const { s } = env;
  const seen: unknown[] = [];
  let shown: any = null;
  s.sheetHost = {
    openPayment: (ctl) => {
      shown = ctl;
      ctl.chooseNetwork("arbitrum"); // a session start in flight
      return { closed: "not a promise" } as any;
    },
    openRestore: (ctl) => ((shown = ctl), { nope: true } as any),
    close: () => void seen.push(shown instanceof Object && "rid" in shown
      ? ["restore", s._restores.has(shown.rid)]
      : ["payment", s._checkouts.has(shown.cid), shown.result?.status ?? null, shown._future === null]),
    notify: () => undefined,
  };
  assert.equal((await s.pay("unlock", { item: "chapter2" })).error, "sheet.unavailable");
  assert.equal((await s.restore()).error, "sheet.unavailable");
  assert.deepEqual(seen, [["payment", false, "cancelled", true], ["restore", false]],
    "when host.close() runs, the controller is already dropped and cancelled (its session start abandoned)");
});

test("misusedPayRejectsWithConfigError", async () => {
  // before configure: Ren'Py raises; here the Promise rejects
  installShim(g, { nwjs: true });
  const bare = new Scfg({ log: { error: () => undefined, warn: () => undefined }, platform: { devNotice: () => undefined } });
  const early = bare.pay("unlock", { item: "chapter2" });
  assert.ok(early instanceof Promise, "pay() never throws: it returns a rejected Promise");
  await assert.rejects(early, code("configure.not_called"));
  await assert.rejects(bare.restore(), code("configure.not_called"));
  resetShim();

  const env = await make();
  const { s, host } = env;
  const called: unknown[] = [];
  const cb = (r: PaymentResult) => void called.push(r);
  const cases: [unknown, Record<string, unknown>, (e: unknown) => boolean][] = [
    ["unlock", { item: "nope" }, code("item.unknown")],
    ["unlock", {}, code("item.unknown")],
    ["gift", { item: "chapter2" }, code("payment.bad_kind")],
    ["tip", { amount: "3.00" }, code("tip.needs_amount_and_token")],
    ["tip", { amount: "0.10", token: "USDC" }, code("tip.below_minimum")],
    ["unlock", { item: "chapter2", token: "DAI" }, code("token.not_enabled")],
    ["unlock", { item: "chapter2", colour: "red" }, (e) => e instanceof TypeError],
    ["unlock", { item: "chapter2", callback: "not callable" }, (e) => e instanceof TypeError],
  ];
  for (const [kind, opts, check] of cases) {
    const o = "callback" in opts ? opts : { ...opts, callback: cb };
    await assert.rejects(s.pay(kind, o as any), check, `${String(kind)} ${JSON.stringify(opts)}`);
  }
  await assert.rejects(s.pay("unlock", "chapter2" as any), TypeError, "options must be an object");
  await assert.rejects(s.restore({ colour: "red" } as any), TypeError, "restore() with an unknown option");
  await assert.rejects(s.restore({ callback: 5 } as any), TypeError, "restore() with a callback that is not a function");
  assert.deepEqual(called, [], "a misused call gives nothing to the callback (Ren'Py raises)");
  assert.deepEqual([host.sheets.length, s._checkouts.size, s._restores.size], [0, 0, 0], "no sheet was shown");
});

test("payBeforePaymentsCanStartResolvesFailed", async () => {
  // the stored receipts cannot be read this run: payments are off, and pay() says so in a
  // "failed" result (not a rejection, which would stop the game unless caught)
  const storage = new MemoryStorage();
  storage.write(STATE, JSON.stringify({ version: 1, pending: {}, receipts: {} }));
  const shim = installShim(g, { nwjs: true, storage, options: ["test"] });
  const errors: string[] = [];
  const s = new Scfg({
    log: { error: (...a: unknown[]) => void errors.push(a.map(String).join(" ")), warn: () => undefined },
    platform: { devNotice: () => undefined },
    worker_opts: { schedule: () => null, cancel: () => undefined },
  });
  installHooks(s);
  s.sheetHost = new FakeHost();
  shim.failNext("load", Object.assign(new Error("EBUSY: resource busy or locked"), { code: "EBUSY" }));
  s.configure({ wallet: DEV });
  s.addItem("chapter2", CHAPTER2);
  await bootShim();
  shim.frame();
  const reported = errors.length; // the unreadable state, once
  const called: unknown[] = [];
  const r = await s.pay("unlock", { item: "chapter2", callback: (x) => void called.push(x) });
  assert.deepEqual([r.status, r.error, r.item], ["failed", "payments.unavailable", "chapter2"]);
  assert.deepEqual(called, [r], "the callback gets it");
  const rr = await s.restore();
  assert.deepEqual([rr.status, rr.error], ["failed", "payments.unavailable"]);
  assert.equal((s.sheetHost as FakeHost).sheets.length, 0, "no sheet");
  assert.equal(errors.length, reported, "the start failure was reported once already: not again per call");
});

test("payWithoutACatchNeverStopsTheGame", async () => {
  // MZ's SceneManager.onReject stops the game on any rejection nobody handled (the shim models it).
  // A script that only writes .then() gets "failed" results for what is not its mistake.
  const env = await make();
  const { s, shim } = env;
  s.sheetHost = { openPayment: () => null, openRestore: () => null, close: () => undefined, notify: () => undefined };
  const seen: string[] = [];
  void s.pay("unlock", { item: "chapter2" }).then((r) => void seen.push(`${r.status} ${r.error}`));
  void s.restore().then((r) => void seen.push(`${r.status} ${r.error}`));
  s._shutdown(); // the game is closing: payments are off
  void s.pay("tip", { amount: "1.00", token: "USDC" }).then((r) => void seen.push(`${r.status} ${r.error}`));
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.deepEqual(seen, ["failed sheet.unavailable", "failed sheet.unavailable", "failed payments.unavailable"]);
  assert.equal(shim.stopped, false, "the game still runs");
  assert.deepEqual(shim.errors, []);
});

test("intervalIs3sWhileASheetIsOpen", async () => {
  const env = await make();
  const { s, host } = env;
  const w = s._worker!;
  assert.equal(w.interval(), 15.0, "15 s with nothing open");
  const box = pay(s, "unlock", { item: "chapter2" });
  assert.equal(w.interval(), 3.0, "3 s while the payment sheet is open");
  await session(env);
  host.last.close("keep_playing");
  await box.promise;
  assert.equal(w.interval(), 15.0, "15 s again for a background payment");
  const rbox = restore(s);
  assert.equal(w.interval(), 3.0, "3 s while the restore screen is open");
  host.last.close("close");
  await rbox.promise;
  assert.equal(w.interval(), 15.0, "15 s once it is closed");
});

test("aTextLookupThatThrowsInANotificationIsReportedAndThePumpGoesOn", async () => {
  // test play: SCFG.text() throws for an unknown id (a developer's typo); here the notification's own lookup throws
  const env = await make();
  const { s, host, errors } = env;
  const resolved: PaymentResult[] = [];
  s.onResult = (r) => void resolved.push(r);
  const real = s.text.bind(s);
  s.text = (id: string, ...args: unknown[]) => {
    if (id.startsWith("payment_confirmed")) throw new Error(`scfg: unknown text id ${JSON.stringify(id)}`);
    return real(id, ...args);
  };
  pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  await payOnChain(env, ctl); // the pump delivers the paid result and announces it
  assert.equal(ctl.state, "paid");
  assert.deepEqual(resolved.map((r) => r.status), ["paid"], "the result is delivered all the same");
  assert.deepEqual(host.notes, ["payment_confirmed_item"], "the notification shows the id");
  assert.ok(errors.some((e) => e.includes('text "payment_confirmed_item"') && e.includes("unknown text id")), errors.join("\n"));
  assert.ok(!errors.some((e) => e.includes("error in the payment pump")), "nothing escaped the pump");
  assert.equal(s._lastSnapshot, s._worker!.snapshot, "the pump ran to its end (the snapshot is mirrored)");
});

// --- restore ---------------------------------------------------------------------------------

test("restoreFlowRestoresAndNotifies", async () => {
  const env = await make();
  const { s, host } = env;
  const chain = (env.chains["arbitrum"] ??= new FakeChain(1000));
  chain.receipts[TX] = { status: "0x1", logs: [transferLog(USDC_ARB, PAYER, DEV, 4_993_742, { block: 10, tx_hash: TX })] };
  const called: RestoreResult[] = [];
  let box = restore(s, { callback: (r: RestoreResult) => void called.push(r) });
  assert.deepEqual(host.kinds, ["restore"], "the restore screen is shown");
  const ctl = host.last.ctl;
  assert.equal(ctl.check(TX), true, "a transaction hash");
  await pass(env); // the lookup; one candidate, classified: saved at once
  await pass(env); // the save
  assert.equal(ctl.state, "done", "restored");
  assert.equal(called.length, 1, "the callback gets the result when it is done");
  assert.deepEqual([called[0]!.status, called[0]!.restored.length], ["restored", 1], "one receipt restored");
  assert.deepEqual(host.notes, ["Purchases restored."], "a notification");
  assert.equal(s.owns("chapter2"), true, "owned");
  await tick();
  assert.equal(box.result, undefined, "restore() waits for the screen to close");
  host.last.close("close");
  const r = await box.promise;
  assert.deepEqual([r.status, r.restored.length], ["restored", 1], "restore() returns the result");
  assert.deepEqual([s._restores.size, host.closes, called.length], [0, 1, 1], "the screen is closed; the callback was called once");
  // closing before anything is done: cancelled, and the callback is dropped
  box = restore(s, { callback: (got: RestoreResult) => void called.push(got) });
  host.last.close("close");
  const r2 = await box.promise;
  assert.equal(r2.status, "cancelled", "closed at once");
  assert.deepEqual([called.length, s._restoreCallbacks.size], [1, 0], "no callback for a cancelled restore");
});

// --- actions for the sheet -------------------------------------------------------------------

test("mobileOffersWalletAppLink", async () => {
  const env = await make({ nwjs: false, mobile: true });
  const { s, host, world } = env;
  pay(s, "unlock", { item: "chapter2" });
  let ctl = host.last.ctl;
  assert.equal(ctl.mobile, true, "a mobile checkout");
  s.openWalletApp(ctl);
  assert.deepEqual(world.opened, [], "nothing to open before a session");
  ctl = await session(env);
  assert.ok(!ctl.methods().includes("browser"), `no browser checkout on a phone (${ctl.methods()})`);
  const uri = ctl.paymentUri();
  assert.ok(typeof uri === "string" && uri.startsWith("ethereum:"), `an EIP-681 link (${uri})`);
  s.openWalletApp(ctl);
  assert.deepEqual(world.opened, [["window", uri, "_blank", "noopener"]], "Open wallet app opens it with window.open");
  s.openExplorer("https://arbiscan.io/tx/" + TX);
  s.openExplorer(null);
  s.openExplorer("");
  assert.equal(world.opened.length, 2, "openExplorer opens a URL, and nothing for none");
  s.openWalletApp({ paymentUri: () => "ethereum:0xevil" } as any);
  assert.equal(world.opened.length, 2, "only an open sheet's controller");
});

test("linksOpenWithNwShellOnDesktopAndNeverThrowWithoutIt", async () => {
  const env = await make();
  const { s, host, world } = env;
  pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  s.openWalletApp(ctl);
  s.openExplorer("https://arbiscan.io/tx/" + TX);
  assert.deepEqual(world.opened, [["nw", ctl.paymentUri()], ["nw", "https://arbiscan.io/tx/" + TX]], "nw.Shell.openExternal on desktop");
  world.nwAvailable = false; // NW.js without its `nw` object: the page's window.open
  s.openExplorer("https://arbiscan.io/tx/1");
  assert.deepEqual(world.opened[2], ["window", "https://arbiscan.io/tx/1", "_blank", "noopener"]);
  delete world.win.open; // and with neither: nothing, never an exception
  assert.doesNotThrow(() => s.openExplorer("https://arbiscan.io/tx/2"));
  assert.doesNotThrow(() => s.copy("x"));
  assert.equal(world.opened.length, 3);
  assert.equal(host.closes, 0);
});

for (const nwjs of [true, false]) {
  test(`copyAndPasteUseTheClipboard[${nwjs ? "nwjs" : "web"}]`, async () => {
    const env = await make({ nwjs });
    const { s, host, world } = env;
    s.copy(TX);
    await tick();
    assert.equal(world.clipboard.text, TX, nwjs ? "copy (nw.Clipboard)" : "copy (navigator.clipboard)");
    pay(s, "unlock", { item: "chapter2" });
    const ctl = await session(env);
    ctl.error = "invalid_hash";
    world.clipboard.text = "  0x12ab-CD!ef  and more words\n";
    s.pasteInto(ctl);
    await tick();
    assert.deepEqual([ctl.tx_input, ctl.error], ["0x12abCDef", null], "the first word, hash characters only (Ren'Py's _clean_paste)");
    world.clipboard.text = "x".repeat(100);
    s.pasteInto(ctl);
    await tick();
    assert.equal(ctl.tx_input.length, 90, "at most 90 characters");
    world.clipboard.text = "   ";
    s.pasteInto(ctl);
    await tick();
    if (nwjs) assert.deepEqual([ctl.tx_input.length, ctl.error], [90, null], "an empty clipboard pastes nothing");
    else assert.deepEqual([ctl.tx_input.length, ctl.error], [90, "paste_blocked"], "an empty answer from the browser: blocked");
    restore(s);
    const rc = host.last.ctl;
    world.clipboard.text = TX;
    s.pasteInto(rc);
    await tick();
    assert.equal(rc.tx_input, TX, "into the restore screen's field");
    rc.tx_input = "";
    if (nwjs) {
      rc.state = "checking"; // the player pressed Check
      s.pasteInto(rc);
    } else {
      s.pasteInto(rc);
      rc.state = "checking"; // the player pressed Check before the browser answered
    }
    await tick();
    assert.equal(rc.tx_input, "", "dropped once the player moved on");
    s.pasteInto({ tx_input: "" } as any); // not an open screen's controller: ignored
    await tick();
  });
}

test("copyRefusedShowsCouldntCopyEvenWithNotifyOff", async () => {
  const env = await make({ nwjs: false });
  const { s, host, world } = env;
  s.notify = false;
  world.clipboard.refuse = true;
  s.copy(TX);
  await tick();
  assert.deepEqual(host.notes, ["Couldn't copy: your browser blocked the clipboard."], "the player pressed Copy: told whatever notify is");
  // NW.js whose clipboard fails falls back to the browser's, and is told the same way
  resetShim();
  const desk = await make({ nwjs: true });
  desk.s.notify = false;
  desk.world.clipboard.nwBroken = true;
  desk.world.clipboard.refuse = true;
  desk.s.copy(TX);
  await tick();
  assert.deepEqual(desk.host.notes, ["Couldn't copy: your browser blocked the clipboard."]);
});

test("pasteRefusedOnTheWebIsPasteBlocked", async () => {
  const env = await make({ nwjs: false });
  const { s, host, world } = env;
  pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  ctl.tx_input = "0xabc";
  world.clipboard.refuse = true;
  s.pasteInto(ctl);
  await tick();
  assert.deepEqual([ctl.tx_input, ctl.error], ["0xabc", "paste_blocked"], "the field is kept; the sheet says so");
  assert.equal(host.notes.length, 0);
});

test("onlyTheNewestPasteLands", async () => {
  // web: two Paste presses before the browser answers the first; answers arrive newest first
  const env = await make({ nwjs: false });
  const { s, world } = env;
  pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  world.clipboard.held = [];
  s.pasteInto(ctl);
  s.pasteInto(ctl);
  await tick();
  assert.equal(world.clipboard.held.length, 2, "two reads waiting");
  const [first, second] = world.clipboard.held;
  second!("0x" + "22".repeat(32));
  await tick();
  assert.equal(ctl.tx_input, "0x" + "22".repeat(32), "the newer press's answer lands");
  first!("0x" + "11".repeat(32));
  await tick();
  assert.equal(ctl.tx_input, "0x" + "22".repeat(32), "the older press's late answer is dropped");
  assert.equal(ctl.error, null);
});

test("cleanPasteStopsAt90", () => {
  const cases: [unknown, string][] = [
    ["  0x12ab-CD!ef  and more words\n", "0x12abCDef"],
    ["x".repeat(100), "x".repeat(90)],
    ["-".repeat(50) + "a".repeat(100), "a".repeat(90)],
    ["a-".repeat(100), "a".repeat(90)],
    ["　abc def", "abc"],
    ["ab cd", "ab"],
    ["ab\u001ccd", "ab"], // Python's str.split() also splits at \x1c-\x1f and \x85
    ["ab\u0085cd", "ab"],
    ["﻿ab cd", "ab"], // the BOM is not whitespace in Python: kept out as a non-hash character only
    ["\n\t ", ""],
    ["", ""],
    [null, ""],
    [undefined, ""],
  ];
  for (const [text, want] of cases) assert.equal(cleanPaste(text), want, `cleanPaste(${JSON.stringify(text)})`);
  assert.equal(TX_CHARS, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
  assert.equal(cleanPaste("a".repeat(5_000_000)).length, 90, "a 5 MB word: 90 characters");
  // it stops at the 90th kept character instead of filtering the whole word: count the reads
  let reads = 0;
  const word = (i: number) => {
    reads++;
    return i < 3 ? 0x20 : i % 2 === 0 ? 0x61 : 0x2d; // "   a-a-a-..." of 5 million characters
  };
  assert.equal(scanPaste(word, 5_000_000), "a".repeat(90));
  assert.equal(reads, 3 + 180, "3 spaces, then 90 dropped and 90 kept characters: nothing past the 90th kept one");
});

// --- the desktop page (Node's server) and transport --------------------------------------------

/** A desktop page whose listen() completes when the test says (as Node's server: url "" until then). */
class LateServer extends FakeServer {
  constructor(kw: ServerArgs) {
    super(kw as any);
    this.url = "";
  }
  failed: unknown = null;
  listen(url = FakeServer.url): void {
    this.url = url;
  }
}

test("openBrowserWaitsForTheDesktopPageToListen", async () => {
  const servers: LateServer[] = [];
  const env = await make({ hooks: { server_factory: (kw) => { const srv = new LateServer(kw); servers.push(srv); return srv; } } });
  const { s, world } = env;
  pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  ctl.chooseMethod("browser");
  const step = ctl.step;
  s.openBrowser(ctl);
  assert.equal(servers.length, 1, "the page is started");
  assert.equal(servers[0]!.running, true);
  assert.deepEqual(world.opened, [], "but no browser is opened on an empty URL");
  assert.equal(ctl.step, step, "the sheet stays on its step while the page starts");
  s.openBrowser(ctl); // pressed again before listen() completed
  env.shim.frame(2);
  assert.deepEqual(world.opened, [], "still nothing while the page is not listening");
  assert.equal(servers.length, 1, "one page");
  servers[0]!.listen();
  env.shim.frame();
  assert.deepEqual(world.opened, [["nw", FakeServer.url]], "opened once, on the frame after listening");
  assert.equal(ctl.step, "waiting", "and the sheet moves on as for an immediate URL");
  env.shim.frame(3);
  assert.equal(world.opened.length, 1, "the deferred open runs once");
  s.openBrowser(ctl); // "Reopen checkout page": the URL is there now
  assert.deepEqual(world.opened, [["nw", FakeServer.url], ["nw", FakeServer.url]]);
  assert.equal(servers.length, 1, "the same page");
});

test("aDeferredOpenIsDroppedWhenTheSheetCloses", async () => {
  const servers: LateServer[] = [];
  const env = await make({ hooks: { server_factory: (kw) => { const srv = new LateServer(kw); servers.push(srv); return srv; } } });
  const { s, host, world } = env;
  pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  s.openBrowser(ctl);
  host.last.close("keep_playing");
  await tick();
  servers[0]!.listen();
  env.shim.frame(2);
  assert.deepEqual(world.opened, [], "a sheet closed before the page listened opens nothing");
});

test("aDesktopPageThatCannotListenFailsLikeAFailedStart", async () => {
  const servers: LateServer[] = [];
  const env = await make({ hooks: { server_factory: (kw) => { const srv = new LateServer(kw); servers.push(srv); return srv; } } });
  const { s, world, errors } = env;
  pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  ctl.chooseMethod("browser");
  s.openBrowser(ctl);
  servers[0]!.failed = new Error("listen EACCES: permission denied 127.0.0.1");
  servers[0]!.running = false;
  env.shim.frame();
  assert.deepEqual(world.opened, [], "nothing is opened");
  assert.equal(servers[0]!.stopped, true, "the page is stopped");
  assert.equal(ctl._server, null, "and dropped");
  assert.equal(ctl.browser_failed, true, "the controller fails the browser method as when start() throws");
  assert.equal(ctl.error, "browser_unavailable");
  assert.notEqual(ctl.method, "browser", "another method is offered");
  assert.ok(errors.some((e) => e.includes("cannot start the browser checkout page") && e.includes("EACCES")), errors.join("\n"));
  env.shim.frame(2);
  assert.equal(servers.length, 1, "no retry on its own");
});

/** The desktop platform's Node modules, as NW.js's require gives them. */
const nodeModules: Record<string, unknown> = { net, crypto, http, https, fs, path };

function get(url: string, method = "GET", body?: string): Promise<{ status: number; body: string; type: string | undefined }> {
  return new Promise((resolve, reject) => {
    const req = http.request(url, { method, agent: false }, (res) => {
      const parts: Buffer[] = [];
      res.on("data", (b: Buffer) => parts.push(b));
      res.on("end", () => resolve({ status: res.statusCode!, body: Buffer.concat(parts).toString("utf8"), type: res.headers["content-type"] }));
    });
    req.on("error", reject);
    req.end(body);
  });
}

test("desktopCheckoutPageIsTheNodeServer", async () => {
  installDiskAssets();
  const dir = mkdtempSync(path.join(tmpdir(), "scfg-game-"));
  let local: { stop(): void } | null = null; // stopped whatever happens: a listening server would keep the test run alive
  try {
    mkdirSync(path.join(dir, "img", "scfg"), { recursive: true });
    writeFileSync(path.join(dir, "img", "scfg", "logo.png"), Buffer.from([0x89, 0x50, 0x4e, 0x47, 1, 2]));
    const env = await make({
      config: { logo: "logo" },
      hooks: { platform: { requireNode: (name: string) => nodeModules[name], gameDir: () => dir } },
    });
    const { s, world, host } = env;
    pay(s, "unlock", { item: "chapter2" });
    const ctl = await session(env);
    s.openBrowser(ctl);
    const srv = ctl._server;
    local = srv;
    assert.ok(srv instanceof LocalServer, "desktop: the page is local_server.ts's CheckoutServer on Node's net");
    assert.equal(srv.url, "", "listening has not completed yet");
    assert.deepEqual(world.opened, []);
    await srv.ready();
    env.shim.frame();
    assert.equal(world.opened.length, 1);
    const url = String(world.opened[0]![1]);
    assert.equal(url, srv.url);
    assert.match(url, /^http:\/\/127\.0\.0\.1:\d+\/c\/[A-Za-z0-9_-]{22}\/$/);
    const page = await get(url);
    assert.equal(page.status, 200);
    assert.equal(page.body, pageAsset("index.html"), "the embedded checkout page");
    assert.equal((await get(url + "app.js")).body, pageAsset("app.js"));
    const info = JSON.parse((await get(url + "session.json")).body);
    assert.equal(info.amount_units, String(ctl.session.units));
    assert.equal(info.logo, true);
    const logo = await get(url + "logo");
    assert.deepEqual([logo.status, logo.type], [200, "image/png"], "the logo file, read when the page started");
    assert.equal((await get(url + "status")).status, 200);
    assert.equal((await get(url + "tx", "POST", JSON.stringify({ tx_hash: TX }))).status, 202);
    assert.equal(ctl.hint_tx, TX, "the page's hint reached the controller");
    host.last.close("cancel");
    await tick();
    assert.equal(srv.running, false, "closing the sheet stops the page");
    await assert.rejects(get(url), /ECONNREFUSED/);
  } finally {
    local?.stop();
    rmSync(dir, { recursive: true, force: true });
  }
});

test("aLogoReadInNodesOwnRealmIsServed", async () => {
  // NW.js's Node may run in a separate context: fs.readFileSync's Buffer is not the page's Uint8Array
  installDiskAssets();
  const foreignFs = {
    ...fs,
    existsSync: () => true,
    readFileSync: () => runInNewContext("new Uint8Array([0x89, 0x50, 0x4e, 0x47])"),
  };
  const errors: string[] = [];
  const env = await make({
    config: { logo: "logo" },
    hooks: {
      log: { error: (...a: unknown[]) => void errors.push(a.join(" ")), warn: () => undefined },
      platform: { requireNode: (name: string) => (name === "fs" ? foreignFs : nodeModules[name]), gameDir: () => "/game" },
    },
  });
  const { s, world } = env;
  pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  s.openBrowser(ctl);
  const srv = ctl._server as LocalServer;
  try {
    await srv.ready();
    env.shim.frame();
    const logo = await get(String(world.opened[0]![1]) + "logo");
    assert.deepEqual([logo.status, logo.type], [200, "image/png"]);
    assert.deepEqual(errors, []);
  } finally {
    srv.stop();
  }
});

test("desktopRpcGoesThroughNodeHttpsAndTheWebThroughFetch", async () => {
  let env = await make({ hooks: { transport: undefined, platform: { requireNode: (name: string) => nodeModules[name] } } });
  assert.ok(env.s._engine!.transport instanceof NodeHttpsTransport, "NW.js: Node's https");
  assert.ok((env.s._engine!.transport as NodeHttpsTransport).fallback instanceof FetchTransport,
    "with fetch for the requests Node's CA bundle or the OS proxy fails");
  resetShim();
  env = await make({ nwjs: false, hooks: { transport: undefined } });
  assert.ok(env.s._engine!.transport instanceof FetchTransport, "a browser: fetch");
  resetShim();
  const errors: string[] = [];
  env = await make({
    hooks: {
      transport: undefined, log: { error: (...a: unknown[]) => void errors.push(a.join(" ")), warn: () => undefined },
      platform: { requireNode: () => { throw new Error("no require here"); } },
    },
  });
  assert.ok(env.s._engine!.transport instanceof FetchTransport, "NW.js without Node's https: fetch, reported");
  assert.ok(errors.some((e) => e.includes("no require here")), errors.join("\n"));
});

// --- shutdown (Ren'Py's _shutdown) -----------------------------------------------------------

test("shutdownClosesWithoutDelivery", async () => {
  const journal: unknown[] = [];
  const factory = (kw: ServerArgs) => new RecordingServer(kw, journal);
  const env = await make({ hooks: { server_factory: factory } });
  const { s, host } = env;
  const called: unknown[] = [];
  const resolved: unknown[] = [];
  s.onResult = (r) => void resolved.push(r);
  const cb = (r: PaymentResult) => void called.push(r);
  // one payment kept playing in the background, one with its sheet open, and the restore screen
  pay(s, "unlock", { item: "chapter2", callback: cb });
  const background = await session(env);
  s.openBrowser(background);
  host.last.close("keep_playing");
  await tick();
  const box = pay(s, "unlock", { item: "chapter2", callback: cb });
  const ctl = await session(env);
  const sid = ctl.sessionId;
  s.openBrowser(ctl);
  const rbox = restore(s, { callback: (r: RestoreResult) => void called.push(r) });
  // scan progress inside the save interval: only the quit flush writes it
  env.clock.advance(1);
  env.chains["arbitrum"]!.head += 50;
  await pass(env);
  await s._mirror.idle();
  const cursor = pendingById(s, sid)!["cursor"];
  const stored = () => JSON.parse(env.storage.read(STATE)!).pending[sid].cursor;
  assert.notDeepEqual(stored(), cursor, "the scan progress is not stored yet");
  assert.equal(called.length + resolved.length, 0, "nothing delivered yet");
  journal.length = 0;
  // the NW.js window is closing
  env.world.win.dispatchEvent(new Event("beforeunload"));
  const r = await box.promise;
  assert.deepEqual([r.status, r.session_id], ["cancelled", sid], "pay() is released, cancelled");
  const rr = await rbox.promise;
  assert.equal(rr.status, "cancelled", "restore() is released, cancelled");
  assert.deepEqual([called.length, resolved.length, host.notes.length], [0, 0, 0], "nothing is delivered or announced");
  assert.deepEqual(journal.slice(0, 2), ["page stopped", "page stopped"], "both pages are stopped before any sheet is closed");
  assert.deepEqual([s._checkouts.size, s._background.size, s._restores.size], [0, 0, 0], "every controller is closed");
  assert.equal(s._worker!.running, false, "the Worker is stopped");
  await s._mirror.idle();
  assert.deepEqual(stored(), cursor, "the quit flush wrote the scan progress");
  assert.notEqual(pendingById(s, sid)?.["ui_status"], "cancelled", "the session is not cancelled: it is still watched on the next launch");
  // a Worker pass still in flight at the quit finishes with the payment confirmed: not delivered
  const chain = env.chains["arbitrum"]!;
  chain.logs.push(transferLog(USDC_ARB, PAYER, DEV, BigInt(ctl.session.units), { block: chain.head + 1, tx_hash: TX }));
  chain.head += 30;
  await s._worker!.runOnce();
  await s._worker!.runOnce();
  assert.ok(s._worker!.snapshot.owned.has("chapter2"), "the pass in flight confirmed the payment");
  env.shim.frame(3);
  assert.deepEqual([called.length, resolved.length, host.notes.length], [0, 0, 0], "the pump delivers nothing after the shutdown");
  const late = await s.pay("tip", { amount: "3.00", token: "USDC" });
  assert.deepEqual([late.status, late.error], ["failed", "payments.unavailable"], "no new payment while closing");
  assert.deepEqual([(await s.restore()).status, (await s.restore()).error], ["failed", "payments.unavailable"]);
});

test("shutdownHooksFlushAndShutDownWithoutAWindowListener", async () => {
  // NW.js: no nw.Window "close" listener (it would have to force the close and override
  // other plugins); the page's beforeunload shuts down and flushes, delivering nothing
  let env = await make();
  assert.deepEqual([...env.world.nwListeners.keys()], [], "nw.Window.get().on() is never called");
  const delivered: unknown[] = [];
  env.s.onResult = (r) => void delivered.push(r);
  pay(env.s, "unlock", { item: "chapter2", callback: (r: PaymentResult) => void delivered.push(r) });
  let ctl = await session(env);
  const sid = ctl.sessionId;
  env.clock.advance(1);
  env.chains["arbitrum"]!.head += 50;
  await pass(env);
  const cursor = pendingById(env.s, sid)!["cursor"];
  env.world.win.dispatchEvent(new Event("beforeunload"));
  assert.equal(env.s._checkouts.size, 0, "shut down");
  await env.s._mirror.idle();
  await tick();
  assert.deepEqual(JSON.parse(env.storage.read(STATE)!).pending[sid].cursor, cursor, "flushed");
  assert.deepEqual([delivered, env.host.notes, env.world.nwClosed], [[], [], []], "nothing delivered; the window is not closed by us");
  resetShim();

  // SceneManager.terminate (MZ's exit: nw.App.quit) runs after the shutdown and its flush
  const quits: unknown[] = [];
  env = await make({ beforeHooks: () => {
    g.SceneManager.terminate = () => void quits.push(JSON.parse(env.storage.read(STATE)!).pending);
  } });
  const atQuit: unknown[] = [];
  env.s.onResult = (r) => void atQuit.push(r);
  pay(env.s, "unlock", { item: "chapter2", callback: (r: PaymentResult) => void atQuit.push(r) });
  ctl = await session(env);
  env.clock.advance(1);
  env.chains["arbitrum"]!.head += 50;
  await pass(env);
  const scanned = pendingById(env.s, ctl.sessionId)!["cursor"];
  g.SceneManager.terminate();
  assert.equal(env.s._checkouts.size, 0, "the shutdown runs at once");
  assert.deepEqual(quits, [], "the quit waits for the flush");
  await env.s._mirror.idle();
  await tick();
  assert.equal(quits.length, 1, "then MZ's terminate runs");
  assert.deepEqual(env.host.notes, [], "nothing announced");
  env.shim.frame(3);
  assert.deepEqual(atQuit, [], "no callback and no onResult: nothing is delivered at the quit");
  assert.deepEqual((quits[0] as any)[ctl.sessionId].cursor, scanned, "with the scan progress stored");
  resetShim();

  // web: pagehide shuts down; a pagehide into the back/forward cache, or a beforeunload that
  // another script may still cancel, only flushes
  env = await make({ nwjs: false });
  assert.equal(env.world.nwListeners.size, 0, "no nw in a browser");
  pay(env.s, "unlock", { item: "chapter2" });
  await session(env);
  env.world.win.dispatchEvent(new Event("beforeunload"));
  assert.equal(env.s._checkouts.size, 1, "beforeunload on the web: still open");
  env.world.win.dispatchEvent(Object.assign(new Event("pagehide"), { persisted: true }));
  assert.equal(env.s._checkouts.size, 1, "into the bfcache: still open");
  env.world.win.dispatchEvent(Object.assign(new Event("pagehide"), { persisted: false }));
  assert.equal(env.s._checkouts.size, 0, "the page is going away: shut down");
  resetShim();

  // NW.js without its `nw` object: the hooks install without throwing
  await assert.doesNotReject(async () => {
    const shim = installShim(g, { nwjs: true });
    const s = new Scfg({ platform: { nw: () => null, window: () => shim.window, devNotice: () => undefined } });
    installHooks(s);
  });
});

for (const nwjs of [false, true]) test(`aHiddenPageFlushes[${nwjs ? "nwjs" : "web"}]`, async () => {
  // phones' browsers may end a background tab without pagehide
  const env = await make({ nwjs });
  const { s, world } = env;
  pay(s, "unlock", { item: "chapter2" });
  const ctl = await session(env);
  env.clock.advance(1);
  env.chains["arbitrum"]!.head += 50;
  await pass(env);
  const cursor = pendingById(s, ctl.sessionId)!["cursor"];
  const stored = () => JSON.parse(env.storage.read(nwjs ? STATE : "rmmzsave.12345678.scfg")!).pending[ctl.sessionId].cursor;
  world.win.visibilityState = "visible";
  world.win.dispatchEvent(new Event("visibilitychange"));
  await s._mirror.idle();
  assert.notDeepEqual(stored(), cursor, "visible again: nothing to do");
  world.win.visibilityState = "hidden";
  world.win.dispatchEvent(new Event("visibilitychange"));
  await s._mirror.idle();
  assert.deepEqual(stored(), cursor, "hidden: flushed");
  assert.equal(s._checkouts.size, 1, "not shut down");
});

// --- the pay() callback map (Ren'Py's _state.callbacks; tests/unit/test_renpy_callbacks.py) ----------

test("aDeliveredPaidResultDropsTheCallback", async () => {
  const env = await make();
  const { s, host } = env;
  const called: PaymentResult[] = [];
  pay(s, "unlock", { item: "chapter2", callback: (r: PaymentResult) => void called.push(r) });
  const ctl = await session(env);
  const sid = ctl.sessionId;
  host.last.close("keep_playing");
  await tick();
  await pass(env);
  assert.ok(s._callbacks.has(sid), "registered while the payment is pending");
  await payOnChain(env, ctl);
  assert.deepEqual(called.map((r) => r.status), ["paid"], "the callback got the final result");
  assert.equal(s._callbacks.size, 0, "and is dropped: the session is over");
  await pass(env);
  assert.equal(s._callbacks.size, 0, "not registered again");
});

test("anExpiredSessionKeepsItsCallbackUntilItIsPaid", async () => {
  const env = await make();
  const { s, host } = env;
  const called: PaymentResult[] = [];
  pay(s, "unlock", { item: "chapter2", callback: (r: PaymentResult) => void called.push(r) });
  const ctl = await session(env);
  const sid = ctl.sessionId;
  host.last.close("keep_playing");
  await tick();
  await pass(env);
  env.clock.advance(31 * 60);
  await pass(env);
  assert.deepEqual(called.map((r) => r.status), ["expired"], "the window closed");
  assert.equal(s._background.size, 0, "its controller is closed");
  assert.ok(s._callbacks.has(sid), "the scan goes on: a later payment still reaches the callback");
  await payOnChain(env, ctl);
  assert.deepEqual(called.map((r) => r.status), ["expired", "paid"]);
  assert.equal(s._callbacks.size, 0);
});

// A sheet left open on an expired session: once the session leaves the Engine at its scan horizon
// the callback is dropped, and the sheet's controller, still polled by every pump, does not register
// it again (no snapshot would ever list the session to prune it).
test("anOpenSheetPastTheScanHorizonDoesNotRegisterItsCallbackAgain", async () => {
  const env = await make();
  const { s, host } = env;
  pay(s, "unlock", { item: "chapter2", callback: () => undefined });
  const ctl = await session(env);
  const sid = ctl.sessionId!;
  await pass(env);
  env.clock.advance(31 * 60);
  await pass(env);
  assert.equal(String(ctl.state), "expired", "the open sheet shows the expired session");
  assert.ok(s._callbacks.has(sid), "the scan goes on");
  env.clock.advance(24 * 3600);
  await pass(env);
  await pass(env);
  assert.ok(!(sid in s._worker!.snapshot.pending), "the session left the Engine");
  assert.equal(s._callbacks.size, 0, "its callback is dropped");
  await pass(env);
  await pass(env);
  assert.equal(s._callbacks.size, 0, "not registered again");
  assert.equal(s._checkouts.size, 1, "the sheet is still open");
  host.last.close("cancel");
});

test("aDiscardedSessionDropsItsCallback", async () => {
  const env = await make();
  const { s, host } = env;
  pay(s, "unlock", { item: "chapter2", callback: () => undefined });
  const ctl = await session(env);
  const first = ctl.sessionId;
  await pass(env);
  assert.ok(s._callbacks.has(first));
  ctl.chooseNetwork("base"); // the player switched networks: the unpaid session is discarded
  await pass(env);
  await pass(env);
  const second = ctl.sessionId;
  assert.ok(second && second !== first, "a new session");
  assert.deepEqual([...s._callbacks.keys()], [second], "only the live session keeps a callback");
  host.last.close("cancel");
});

test("aSessionPaidBeforeAnySnapshotListedItReleasesItsCallback", async () => {
  // The state guard of _registerCallback: no snapshot ever lists the session (so _pruneCallbacks
  // cannot drop it), and the paid controller is still polled in the pump that delivered the result.
  const env = await make();
  const { s, host } = env;
  const worker = s._worker!;
  const frozen = worker.snapshot;
  const pass = async () => {
    await worker.runOnce();
    worker.snapshot = frozen; // no snapshot lists the session
    env.shim.frame();
  };
  const called: PaymentResult[] = [];
  pay(s, "unlock", { item: "chapter2", callback: (r: PaymentResult) => void called.push(r) });
  const ctl = host.last.ctl;
  ctl.chooseNetwork("arbitrum");
  await pass();
  const sid = ctl.sessionId;
  assert.ok(sid && s._callbacks.has(sid), "registered");
  host.last.close("keep_playing");
  await tick();
  const chain = env.chains["arbitrum"]!;
  const block = chain.head + 1;
  chain.logs.push(transferLog(USDC_ARB, PAYER, DEV, BigInt(ctl.session.units), { block, tx_hash: TX }));
  chain.head = block;
  await pass();
  chain.head = block + 19;
  await pass();
  assert.deepEqual(called.map((r) => r.status), ["paid"]);
  assert.equal(s._callbacks.size, 0, "released although no snapshot listed it");
  await pass();
  assert.equal(s._callbacks.size, 0);
});

test("theSnapshotIsReadBeforeTheOutcomesAreDrained", async () => {
  // A pass landing between the drain and a later snapshot read: right after a drain that found
  // nothing, the paid session leaves the snapshot and its outcome is queued for the next drain. A
  // pump that read the snapshot after draining would drop the callback before the result arrived.
  const env = await make();
  const { s, host } = env;
  const called: PaymentResult[] = [];
  pay(s, "unlock", { item: "chapter2", callback: (r: PaymentResult) => void called.push(r) });
  const ctl = await session(env);
  const sid = ctl.sessionId;
  host.last.close("keep_playing");
  await tick();
  await pass(env);
  await pass(env); // listed by a snapshot: the callback could be pruned
  const worker = s._worker!;
  const drain = worker.drainOutcomes.bind(worker);
  let armed = true;
  worker.drainOutcomes = () => {
    const out = drain();
    if (armed && out.length === 0) {
      armed = false;
      const pending = { ...worker.snapshot.pending };
      delete pending[sid];
      worker.snapshot = { ...worker.snapshot, pending };
      (worker as any)._outcomes.push(makeStepOutcome(sid, "paid", {
        result: makePaymentResult({ status: "paid", kind: "unlock", item: "chapter2", session_id: sid }),
      }));
    }
    return out;
  };
  env.shim.frame(); // drains nothing, then the pass lands
  env.shim.frame(); // drains the paid outcome
  assert.deepEqual(called.map((r) => r.status), ["paid"], "the paid result reached the callback");
  assert.equal(s._callbacks.size, 0);
});
