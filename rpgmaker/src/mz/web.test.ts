/**
 * The SCFG glue in a web deployment: the checkout is the in-page overlay (core/overlay.ts over
 * web_page.ts), the browser wallet is the page's hasWallet, and copy, paste and links go through
 * the page's bridge. Twins of godot/tests/test_scfg_web.gd (the same names, camelCase); behaviour
 * authority plugin/game/scfg/scfg_api.rpy's WEB branches (_web_page, _server_factory,
 * _logo_data_url, _periodic's paste and copy-failed handling, _open_url, Copy).
 *
 * No browser runs here: the shim says "not NW.js", and the page is a fake window whose bridge
 * (test/web_fakes.ts) records the values the glue hands it. The real bridge and page are
 * exercised in Chrome by tests/rpgmaker_ui (the overlay) and the web e2e.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { FakeChain } from "../../test/chain_fakes.ts";
import { FakeClock, FakeTransport } from "../../test/rpc_fakes.ts";
import { installDiskAssets } from "../../test/mz_assets.ts";
import { FakeBridge, fakePageWindow, type FakePageWindow } from "../../test/web_fakes.ts";
import { MemoryStorage, bootShim, installShim, resetShim, type Shim } from "../../test/mz_shim/index.ts";
import { BUILD_TIMEOUT_S, OverlayCheckout, Job, type Chain, type CheckoutController, type RestoreController } from "../core/index.ts";
import { installHooks } from "./boot.ts";
import { webStoragePersists } from "./platform.ts";
import { LOGO_WAIT_S, Scfg, type ScfgHooks } from "./scfg.ts";
import type { SheetHandle, SheetHost } from "./sheet_host.ts";

const g = globalThis as any;
const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const TX = "0x" + "ab".repeat(32);
const CHAPTER2 = { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" };
const SOL_ACCOUNT = "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe";

afterEach(() => resetShim());

interface Sheet {
  ctl: any;
  close(how: string): void;
}

class FakeHost implements SheetHost {
  sheets: Sheet[] = [];
  notes: string[] = [];
  openPayment(ctl: CheckoutController): SheetHandle {
    return this._open(ctl);
  }
  openRestore(ctl: RestoreController): SheetHandle {
    return this._open(ctl);
  }
  _open(ctl: unknown): SheetHandle {
    let close!: (how: string) => void;
    const handle: SheetHandle = { closed: new Promise<any>((resolve) => (close = resolve)) };
    this.sheets.push({ ctl, close });
    return handle;
  }
  close(): void {}
  notify(text: string): void {
    this.notes.push(text);
  }
  get last(): Sheet {
    return this.sheets[this.sheets.length - 1]!;
  }
}

interface Env {
  shim: Shim;
  s: Scfg;
  bridge: FakeBridge;
  win: FakePageWindow;
  clock: FakeClock;
  host: FakeHost;
  opened: unknown[][];
  clipboard: string[];
  errors: string[];
  warnings: string[];
}

/**
 * A configured web SCFG (or desktop, `nwjs: true`) with chapter2, booted on the shim with its first
 * worker pass run. `wallet`: what the page's hasWallet answers. `blocked`: the page's scripts never run.
 */
async function make(opts: {
  nwjs?: boolean; wallet?: number; blocked?: boolean; persists?: boolean; config?: Record<string, unknown>; hooks?: Partial<ScfgHooks>;
} = {}): Promise<Env> {
  const assets = installDiskAssets();
  const shim = installShim(g, { nwjs: opts.nwjs ?? false, storage: new MemoryStorage(), options: ["test"] });
  const bridge = new FakeBridge();
  bridge.answers["hasWallet"] = Array.from({ length: 200 }, () => opts.wallet ?? 1);
  const win = fakePageWindow(bridge, { bridgeText: (t) => t === assets.page["bridge.js"], blocked: opts.blocked });
  const opened: unknown[][] = [];
  const clipboard: string[] = [];
  (win as any).open = (...a: unknown[]) => void opened.push(["window", ...a]);
  const chains: Record<string, FakeChain> = {};
  const clock = new FakeClock();
  const errors: string[] = [];
  const warnings: string[] = [];
  const line = (args: unknown[]) => args.map((a) => (a instanceof Error ? `${a.name}: ${a.message}` : String(a))).join(" ");
  const hooks: ScfgHooks = {
    clock: clock.call, transport: new FakeTransport({}),
    engine_opts: {
      rpc_factory: (chain: Chain) => {
        chains[chain.key] ??= new FakeChain(1000);
        chains[chain.key]!.chain_obj = chain;
        return chains[chain.key]!;
      },
      clock: clock.call,
    },
    worker_opts: { schedule: () => null, cancel: () => undefined },
    log: { error: (...a: unknown[]) => void errors.push(line(a)), warn: (...a: unknown[]) => void warnings.push(line(a)) },
    ...opts.hooks,
    platform: {
      devNotice: () => undefined, nw: () => null, window: () => win,
      navigator: () => ({ clipboard: { writeText: async (t: string) => void clipboard.push(t), readText: async () => "0xnav" } }),
      webStoragePersists: async () => opts.persists ?? true,
      ...opts.hooks?.platform,
    },
  };
  const s = new Scfg(hooks);
  g.SCFG = s;
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
  return { shim, s, bridge, win, clock, host, opened, clipboard, errors, warnings };
}

const tick = () => new Promise<void>((r) => setTimeout(r, 0));

async function pass(env: Env): Promise<void> {
  await env.s._worker!.runOnce();
  env.shim.frame();
}

/** pay() on chapter2, with a session on Arbitrum. */
async function session(env: Env): Promise<any> {
  void env.s.pay("unlock", { item: "chapter2" });
  const ctl = env.host.last.ctl;
  ctl.chooseNetwork("arbitrum");
  await pass(env);
  assert.ok(ctl.sessionId, "a session started");
  return ctl;
}

test("webCheckoutIsTheOverlayNotAServer", async () => {
  const env = await make();
  const { s, bridge, win } = env;
  assert.equal(win.scripts.length, 0, "no page script before the first checkout");
  const ctl = await session(env);
  assert.deepEqual(win.scripts.map((x) => x.attributes["data-scfg"]), ["bridge.js", "evm.js", "solana.js", "app.js"], "the four scripts were added");
  assert.equal(bridge.callsTo("setPage").length, 1, "then the page");
  assert.ok(bridge.callsTo("hasWallet").length > 0, "the controller asked the page for a wallet");
  assert.deepEqual(bridge.callsTo("hasWallet")[0], ["evm", ""], "for the chosen chain's family");
  assert.ok(ctl.methods().includes("browser"), `a wallet in the page offers the browser checkout (${ctl.methods()})`);
  s.openBrowser(ctl);
  const opens = bridge.callsTo("openCheckout");
  assert.equal(opens.length, 1, "the overlay opened");
  const [key, info, logo] = opens[0]!;
  assert.match(String(key), /^o\d+$/, "under a key o<n>");
  assert.equal((info as Record<string, unknown>)["title"], "Chapter 2", "with the session's title");
  assert.equal(logo, null, "and no logo");
  assert.ok(ctl._server instanceof OverlayCheckout, "the controller's page is the overlay");
  assert.deepEqual(env.opened, [], "no browser window on the web: the overlay is already showing");
  assert.ok(bridge.callsTo("setStatus").length >= 1, "the status was pushed to the page");
  // the page posts a tx hash: it reaches the session as a hint, through the pump
  bridge.answers["take"] = [JSON.stringify([{ type: "tx", body: { tx_hash: TX } }])];
  env.shim.frame();
  await pass(env);
  assert.ok(s.pending().some((r) => r["hint_tx"] === TX), `the page's tx hash became the session's hint (${JSON.stringify(s.pending())})`);
  // reopening shows the same overlay
  s.openBrowser(ctl);
  assert.equal(bridge.callsTo("openCheckout").length, 1);
  assert.deepEqual(bridge.callsTo("show"), [[key]]);
  // closing the sheet closes the overlay
  env.host.last.close("cancel");
  await tick();
  assert.deepEqual(bridge.callsTo("closeCheckout"), [[key]], "the overlay closed with the sheet");
  assert.deepEqual(env.errors, []);
});

test("noWalletInThePageMeansNoBrowserCheckout", async () => {
  const env = await make({ wallet: 0 });
  const ctl = await session(env);
  assert.ok(!ctl.methods().includes("browser"), `no browser checkout without a wallet (${ctl.methods()})`);
});

test("desktopNeverTouchesThePage", async () => {
  const shell: string[] = [];
  const env = await make({ nwjs: true, hooks: { platform: { nw: () => ({ Shell: { openExternal: (u: string) => void shell.push(u) } }) } as any } });
  const { s, win, bridge } = env;
  const ctl = await session(env);
  s.copy(TX);
  s.openExplorer("https://arbiscan.io/tx/" + TX);
  s.pasteInto(ctl);
  env.shim.frame();
  assert.deepEqual(win.scripts, [], "no script was added");
  assert.deepEqual(bridge.calls, [], "and the bridge never called");
  assert.equal(s._page, null, "no page was built");
  assert.deepEqual(shell, ["https://arbiscan.io/tx/" + TX], "links: nw.Shell.openExternal");
});

test("linksAndCopyGoThroughThePage", async () => {
  const env = await make();
  const { s, bridge, host } = env;
  s.openExplorer("https://arbiscan.io/tx/1");
  assert.deepEqual(bridge.callsTo("openUrl"), [["https://arbiscan.io/tx/1"]], "openExplorer opens from the page");
  assert.deepEqual(env.opened, [], "not the glue's own window.open");
  s.copy(TX);
  await tick();
  assert.deepEqual(bridge.callsTo("copy"), [[TX]], "copy goes to the page");
  assert.deepEqual(env.clipboard, [], "not the glue's navigator.clipboard");
  // the browser refuses the copy: the player is told, once (whatever `notify` is)
  s.notify = false;
  host.notes = [];
  bridge.answers["takeCopyFailed"] = [1, 0];
  env.shim.frame();
  assert.deepEqual(host.notes, ["Couldn't copy: your browser blocked the clipboard."], "told once");
  env.shim.frame();
  assert.equal(host.notes.length, 1, "and not again");
  // a failure after the 5 second watch is not reported, nor asked for
  s.copy(TX);
  env.clock.advance(6);
  const asked = bridge.callsTo("takeCopyFailed").length;
  bridge.answers["takeCopyFailed"] = [1];
  env.shim.frame();
  assert.equal(host.notes.length, 1, "a late answer is ignored");
  assert.equal(bridge.callsTo("takeCopyFailed").length, asked, "the page is not asked outside the watch");
});

test("pasteComesFromThePageAsynchronously", async () => {
  const env = await make();
  const { s, bridge } = env;
  const ctl = await session(env);
  ctl.error = "invalid_hash";
  s.pasteInto(ctl);
  assert.equal(bridge.callsTo("requestPaste").length, 1, "the page was asked");
  assert.deepEqual([ctl.tx_input, ctl.error], ["", "invalid_hash"], "nothing yet");
  bridge.answers["takePaste"] = ["null", JSON.stringify("  0x12ab-CD!ef and more\n")];
  env.shim.frame();
  assert.deepEqual([ctl.tx_input, ctl.error], ["", "invalid_hash"], "still waiting for the browser");
  env.shim.frame();
  assert.deepEqual([ctl.tx_input, ctl.error], ["0x12abCDef", null], "the first word, hash characters only (Ren'Py's _clean_paste)");
  // a refusal: an empty answer
  s.pasteInto(ctl);
  bridge.answers["takePaste"] = [JSON.stringify("")];
  env.shim.frame();
  assert.equal(ctl.error, "paste_blocked", "the browser refused: paste_blocked");
  assert.equal(ctl.tx_input, "0x12abCDef", "the field is kept");
  // the pump asks only while a paste is outstanding
  const asked = bridge.callsTo("takePaste").length;
  env.shim.frame(3);
  assert.equal(bridge.callsTo("takePaste").length, asked, "no polling without a request");
  // a sheet closed before the browser answered drops the paste
  s.pasteInto(ctl);
  env.host.last.close("cancel");
  await tick();
  bridge.answers["takePaste"] = [JSON.stringify(TX)];
  env.shim.frame();
  assert.equal(ctl.tx_input, "0x12abCDef", "dropped with its sheet");
  // a restore screen that moved past its input drops the paste
  void s.restore();
  const rc = env.host.last.ctl;
  s.pasteInto(rc);
  rc.state = "checking";
  bridge.answers["takePaste"] = [JSON.stringify(TX)];
  env.shim.frame();
  assert.equal(rc.tx_input, "", "dropped once the player pressed Check");
  assert.deepEqual(env.errors, []);
});

test("warnsWhenWebStorageIsNotPersistent", async () => {
  const env = await make({ persists: false });
  await tick();
  const notes = env.warnings.filter((w) => w.includes("receipts"));
  assert.equal(notes.length, 1, `one warning (${env.warnings})`);
  assert.match(notes[0]!, /^scfg: .*not persistent/);
  env.shim.frame(5);
  await tick();
  assert.equal(env.warnings.filter((w) => w.includes("receipts")).length, 1, "once");
  resetShim();
  const ok = await make({ persists: true });
  await tick();
  assert.deepEqual(ok.warnings.filter((w) => w.includes("persistent")), [], "no warning when persistent");
  resetShim();
  let asked = 0;
  const desk = await make({ nwjs: true, hooks: { platform: { webStoragePersists: async () => (asked++, false) } as any } });
  await tick();
  assert.equal(asked, 0, "desktop never asks");
  assert.deepEqual(desk.warnings.filter((w) => w.includes("persistent")), []);
  resetShim();
  let played = 0;
  const release = await make({ hooks: { platform: { isTestPlay: () => false, webStoragePersists: async () => (played++, false) } as any } });
  await tick();
  assert.equal(played, 0, "outside test play nothing is asked");
  assert.deepEqual(release.warnings, []);
});

test("theOverlayGetsTheLogoAsADataUrl", async () => {
  const fetched: string[] = [];
  const realFetch = g.fetch;
  g.fetch = async (url: string) => {
    fetched.push(url);
    return { ok: true, status: 200, arrayBuffer: async () => new Uint8Array([0x89, 0x50, 0x4e, 0x47]).buffer };
  };
  try {
    const env = await make({ config: { logo: "logo" } });
    const ctl = await session(env);
    await tick();
    assert.deepEqual(fetched, ["img/scfg/logo.png"], "read once from the game's folder, beside index.html");
    env.s.openBrowser(ctl);
    const [, info, logo] = env.bridge.callsTo("openCheckout")[0]!;
    assert.equal((info as Record<string, unknown>)["logo"], true);
    assert.equal(logo, "data:image/png;base64,iVBORw==", "a PNG data url");
  } finally {
    g.fetch = realFetch;
  }
});

test("theLogoIsReadWhenPayStartsSoItIsThereBeforeOpenWallet", async () => {
  // the overlay takes the logo as it is when it opens: pay() starts reading it at once (before
  // the sheet's first step), so a read beside index.html is done long before "Open wallet"
  const fetched: string[] = [];
  const realFetch = g.fetch;
  let answer: (() => void) | null = null;
  g.fetch = (url: string) => {
    fetched.push(url);
    return new Promise((resolve) => {
      answer = () => resolve({ ok: true, status: 200, arrayBuffer: async () => new Uint8Array([0xff, 0xd8]).buffer });
    });
  };
  try {
    const env = await make({ config: { logo: "brand.jpg" } });
    assert.deepEqual(fetched, [], "not read before a payment");
    void env.s.pay("unlock", { item: "chapter2" });
    assert.deepEqual(fetched, ["img/scfg/brand.jpg"], "pay() asked for the logo before returning");
    answer!();
    await tick();
    const ctl = env.host.last.ctl;
    ctl.chooseNetwork("arbitrum");
    await pass(env);
    env.s.openBrowser(ctl);
    const [, , logo] = env.bridge.callsTo("openCheckout")[0]!;
    assert.equal(logo, "data:image/jpeg;base64,/9g=", "the overlay opened with it");
    void env.s.pay("tip", { amount: "1.00", token: "USDC" });
    assert.equal(fetched.length, 1, "read once per configuration");
  } finally {
    g.fetch = realFetch;
  }
});

test("aLogoStillBeingReadAtOpenWalletIsWaitedForBriefly", async () => {
  // Open wallet while the logo read is still in flight: the overlay waits for it, at most
  // LOGO_WAIT_S on the glue's clock, then opens with the logo or, past that, without it
  const realFetch = g.fetch;
  const answers: (() => void)[] = [];
  g.fetch = () => new Promise((resolve) => {
    answers.push(() => resolve({ ok: true, status: 200, arrayBuffer: async () => new Uint8Array([0xff, 0xd8]).buffer }));
  });
  try {
    // the read arrives within the bound: the overlay opens with it, and gets the status it missed
    let env = await make({ config: { logo: "brand.jpg" } });
    let ctl = await session(env);
    env.s.openBrowser(ctl);
    env.shim.frame();
    assert.equal(env.bridge.callsTo("openCheckout").length, 0, "not opened while the logo is read");
    env.clock.advance(LOGO_WAIT_S / 2);
    answers.shift()!();
    await tick();
    env.shim.frame();
    let opens = env.bridge.callsTo("openCheckout");
    assert.equal(opens.length, 1, "opened once the logo is there");
    assert.equal(opens[0]![2], "data:image/jpeg;base64,/9g=", "with the logo");
    assert.ok(env.bridge.callsTo("setStatus").length > 0, "and the status");
    const sets = env.bridge.calls.map((c) => c[0]);
    assert.ok(sets.indexOf("openCheckout") < sets.indexOf("setStatus"), "the status after the open");
    resetShim();
    // the read takes longer than the bound: the overlay opens without a logo, nothing is reported
    env = await make({ config: { logo: "brand.jpg" } });
    ctl = await session(env);
    env.s.openBrowser(ctl);
    env.shim.frame();
    assert.equal(env.bridge.callsTo("openCheckout").length, 0);
    env.clock.advance(LOGO_WAIT_S);
    env.shim.frame();
    opens = env.bridge.callsTo("openCheckout");
    assert.deepEqual([opens.length, opens[0]![2]], [1, null], "opened without the logo");
    answers.shift()!();
    await tick();
    env.shim.frame();
    assert.equal(env.bridge.callsTo("openCheckout").length, 1, "a late logo opens nothing again");
    assert.deepEqual([env.errors, env.warnings.filter((w) => w.includes("logo"))], [[], []], "no error");
    resetShim();
    // cancelled while it waits: the overlay is never opened (nor closed)
    env = await make({ config: { logo: "brand.jpg" } });
    ctl = await session(env);
    env.s.openBrowser(ctl);
    env.host.last.close("cancel");
    await tick();
    env.shim.frame();
    answers.shift()!();
    await tick();
    env.clock.advance(LOGO_WAIT_S);
    env.shim.frame();
    assert.deepEqual([env.bridge.callsTo("openCheckout").length, env.bridge.callsTo("closeCheckout").length], [0, 0]);
  } finally {
    g.fetch = realFetch;
  }
});

test("theWebOverlayGetsBuildTxAndTheClock", async () => {
  const env = await make();
  const { s, bridge, clock } = env;
  const asked: string[] = [];
  const jobs: Job<any>[] = [];
  const build = (a: string) => {
    asked.push(a);
    const job = new Job<any>(); // the Worker never gets to it
    jobs.push(job);
    return job;
  };
  const kw = { session_info: { family: "solana" }, on_tx: () => undefined, get_status: () => ({}) };
  s._webPage(); // installed by the first checkout in a game
  const overlay = s._serverFactory({ ...kw, build_tx: build });
  assert.ok(overlay instanceof OverlayCheckout, "the overlay");
  overlay.start();
  bridge.answers["take"] = [JSON.stringify([{ type: "build", id: 7, body: { account: SOL_ACCOUNT } }])];
  overlay.pump!();
  assert.deepEqual(asked, [SOL_ACCOUNT], "a build request from the page reaches build_tx");
  clock.advance(BUILD_TIMEOUT_S - 0.5);
  overlay.pump!();
  assert.equal(bridge.callsTo("reply").length, 0, "no answer before the build timeout on the glue's clock");
  clock.advance(0.5);
  overlay.pump!();
  assert.deepEqual(bridge.callsTo("reply").map((r) => r.slice(1, 3)), [[7, 503]], "503 once it passed");
  assert.equal(jobs[0]!.cancelled(), true, "and the build is cancelled");
  overlay.stop();
  // EVM: no build_tx, so the page has no build route
  const evm = s._serverFactory(kw);
  evm.start();
  bridge.answers["take"] = [JSON.stringify([{ type: "build", id: 8, body: { account: SOL_ACCOUNT } }])];
  evm.pump!();
  assert.ok(bridge.callsTo("reply").some((r) => r[1] === 8 && r[2] === 404), "a build request is a 404 there");
  evm.stop();
});

test("aPageWithoutTheBridgeFallsBackToTheBrowsersOwnApis", async () => {
  // a Content-Security-Policy that stops inline scripts: no bridge, no browser wallet checkout
  const env = await make({ blocked: true });
  const { s, host } = env;
  const ctl = await session(env);
  assert.equal(env.win.scripts.length, 4, "the scripts were added");
  assert.ok(env.warnings.some((w) => w.includes("browser wallet")), `the developer is told (${env.warnings})`);
  assert.ok(!ctl.methods().includes("browser"), "no browser checkout");
  assert.throws(() => s._serverFactory({ session_info: {}, on_tx: () => undefined, get_status: () => ({}) }), /bridge/);
  s.copy(TX);
  await tick();
  assert.deepEqual(env.clipboard, [TX], "copy: navigator.clipboard");
  s.pasteInto(ctl);
  await tick();
  assert.equal(ctl.tx_input, "0xnav", "paste: navigator.clipboard");
  s.openExplorer("https://arbiscan.io/tx/1");
  assert.deepEqual(env.opened, [["window", "https://arbiscan.io/tx/1", "_blank", "noopener"]], "links: window.open");
  assert.deepEqual(host.notes, []);
});

test("webStorageIsAskedOfMzsLocalforage", async () => {
  const had = Object.prototype.hasOwnProperty.call(g, "localforage");
  const before = g.localforage;
  try {
    delete g.localforage;
    assert.equal(await webStoragePersists(), false, "no localforage: nothing keeps the saves");
    g.localforage = { ready: () => Promise.resolve() };
    assert.equal(await webStoragePersists(), true, "localforage started");
    g.localforage = { ready: () => Promise.reject(new Error("No available storage method found.")) };
    assert.equal(await webStoragePersists(), false, "no storage driver works here");
    g.localforage = { ready: () => { throw new Error("broken"); } };
    assert.equal(await webStoragePersists(), false);
  } finally {
    if (had) g.localforage = before;
    else delete g.localforage;
  }
});
