/**
 * The SCFG glue on the MZ shim: configuration, queries, the boot order and persistence. Twins of
 * godot/tests/test_scfg_api.gd (the same names, camelCase), driven as MZ drives the plugin: the
 * hooks installed at plugin load, `await bootShim()` for Scene_Boot, `shim.frame()` per frame.
 */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { FakeChain } from "../../test/chain_fakes.ts";
import { FakeClock, FakeTransport } from "../../test/rpc_fakes.ts";
import {
  MemoryStorage, bootShim, encryptForDeployment, installShim, resetShim, type PluginEntry, type Shim,
} from "../../test/mz_shim/index.ts";
import { ConfigError, type Chain, type Engine } from "../core/index.ts";
import { addReceipt } from "../core/receipts.ts";
import { bootPlugin, installHooks } from "./boot.ts";
import { Scfg, VERSION, type ScfgHooks } from "./scfg.ts";

const g = globalThis as any;
const DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
const STATE = "save/scfg.rmmzsave";
const CHAPTER2 = { code: 37, price: "4.99", token: "USDC", title: "Chapter 2" };

afterEach(() => resetShim());

const code = (c: string) => (e: unknown) => e instanceof ConfigError && e.code === c;

function receipt(tx: string, extra: Record<string, unknown> = {}) {
  return {
    chain: "arbitrum", tx_hash: tx, log_index: 3, block: 990, kind: "unlock", item: "chapter2", amount: "4.99", token: "USDC",
    payer: "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed", recipient: DEV.toLowerCase(), source: "payment", ...extra,
  };
}

interface Env {
  shim: Shim;
  s: Scfg;
  storage: MemoryStorage;
  chains: Record<string, FakeChain>;
  clock: FakeClock;
  errors: string[];
  warnings: string[];
  notices: string[];
  /** The fake NW.js game folder: path -> text or bytes. */
  files: Map<string, string | Uint8Array>;
}

/** The hooks every test uses: fake chains, a fake clock, a timer that never fires, recorded reports, a fake NW.js fs. */
function hooksFor(env: Omit<Env, "shim" | "s" | "storage">): ScfgHooks {
  const factory = (chain: Chain) => {
    env.chains[chain.key] ??= new FakeChain(1000);
    env.chains[chain.key]!.chain_obj = chain;
    return env.chains[chain.key]!;
  };
  const fs = {
    existsSync: (p: string) => env.files.has(p),
    readFileSync: (p: string) => {
      const v = env.files.get(p);
      if (v === undefined) throw Object.assign(new Error(`ENOENT: no such file or directory, open '${p}'`), { code: "ENOENT" });
      return Buffer.from(v);
    },
    mkdirSync: () => undefined,
    writeFileSync: (p: string, text: string) => void env.files.set(p, text),
  };
  const path = { join: (...parts: string[]) => parts.join("/"), dirname: (p: string) => p.slice(0, p.lastIndexOf("/")) };
  const line = (args: unknown[]) => args.map((a) => (a instanceof Error ? `${a.name}: ${a.message}` : String(a))).join(" ");
  return {
    clock: env.clock.call, transport: new FakeTransport({}),
    engine_opts: { rpc_factory: factory, clock: env.clock.call },
    worker_opts: { schedule: () => null, cancel: () => undefined },
    log: { error: (...a: unknown[]) => void env.errors.push(line(a)), warn: (...a: unknown[]) => void env.warnings.push(line(a)) },
    platform: {
      requireNode: (name: string) => ({ fs, path })[name as "fs" | "path"],
      gameDir: () => (g.Utils.isNwjs() ? "/game" : null),
      devNotice: (text: string) => void env.notices.push(text),
    },
  };
}

/** The shim and an SCFG with its hooks installed, as at plugin load (before the boot). */
function make(opts: {
  storage?: MemoryStorage; nwjs?: boolean; plugins?: PluginEntry[]; boot?: boolean; beforeLoad?: () => void;
  /** Utils.isOptionValid: default ["test"] (test play); [] is a released game. */
  options?: string[];
  encryption?: { images?: boolean; key: string };
  files?: Record<string, string | Uint8Array>;
} = {}): Env {
  const storage = opts.storage ?? new MemoryStorage();
  const shim = installShim(g, {
    nwjs: opts.nwjs ?? true, storage, options: opts.options ?? ["test"], plugins: opts.plugins, encryption: opts.encryption,
  });
  opts.beforeLoad?.();
  const clock = new FakeClock();
  const files = new Map<string, string | Uint8Array>(Object.entries(opts.files ?? {}));
  const base = { chains: {}, clock, errors: [], warnings: [], notices: [], files };
  const hooks = hooksFor(base);
  let s: Scfg;
  if (opts.boot) {
    s = bootPlugin(hooks);
  } else {
    s = new Scfg(hooks);
    g.SCFG = s;
    installHooks(s);
  }
  return { shim, s, storage, ...base };
}

/** Configured with chapter2, then booted (Scene_Boot read the state; the pump started the Worker). */
async function configured(opts: { storage?: MemoryStorage; nwjs?: boolean } = {}): Promise<Env> {
  const env = make(opts);
  env.s.configure({ wallet: DEV });
  env.s.addItem("chapter2", CHAPTER2);
  await bootShim();
  await env.s._mirror.idle();
  return env;
}

/** The Worker's first pass (launch re-verification, which a FakeChain does not know), before a test adds receipts. */
async function start(s: Scfg): Promise<void> {
  s.pump();
  assert.ok(s._worker !== null, "the pump started the worker");
  await s._worker.runOnce();
  s.pump();
}

/** `fn(engine)` in one Worker pass, then one frame (saves may still be in flight: see settle). */
async function run<T>(env: Env, fn: (e: Engine) => T | Promise<T>): Promise<T> {
  const job = env.s._worker!.submit(fn);
  await env.s._worker!.runOnce();
  env.shim.frame();
  return job.result();
}

async function settle(env: Env): Promise<void> {
  await env.s._mirror.idle();
}

const startSession = (env: Env) => run(env, (e) => e.startSession("unlock", "arbitrum", { item: "chapter2" }));
const addTx = (env: Env, tx: string, extra: Record<string, unknown> = {}) =>
  run(env, (e) => addReceipt(e.receipts, receipt(tx, extra) as any));

function onDisk(env: Env, key = STATE): { version: number; pending: Record<string, any>; receipts: Record<string, any> } {
  const text = env.storage.read(key);
  assert.ok(text !== null, `${key} is stored`);
  return JSON.parse(text);
}

// --- configuration ---------------------------------------------------------------------------

test("configureValidatesLikeRenpy", async () => {
  const { s } = make();
  assert.throws(() => s.configure({ wallet: "0x123" }), code("address.invalid"), "a bad wallet");
  assert.equal(s._config, null, "not configured");
  assert.throws(() => s.configure({}), code("wallet.missing"), "no wallet");
  assert.throws(() => s.configure({ wallet: DEV, colour: "red" }), TypeError, "an unknown option");
  assert.throws(() => s.configure({ wallet: DEV, custom_chains: [] }), TypeError, "custom_chains is addChain's");
  assert.throws(() => s.addItem("chapter2", CHAPTER2), code("configure.not_called"), "addItem before configure");
  assert.throws(() => s.owns("chapter2"), code("configure.not_called"), "a query before configure");
  s.configure({ wallet: DEV, testnet: true });
  assert.equal(s._config!.wallet, DEV, "kept");
  assert.throws(() => s.addItem("chapter2", { ...CHAPTER2, code: 0 }), code("item.invalid_code"), "addItem validates");
  assert.equal(s.supported(), true, "supported() is always true");
  s.configure({ wallet: DEV }); // again, before payments start
  assert.equal(s._config!.testnet, false);
  assert.equal(s.version, VERSION);
});

test("configureAfterStartIsAnError", async () => {
  const { s } = await configured();
  assert.ok(s._worker !== null, "payments started on a frame after the state was read");
  assert.throws(() => s.configure({ wallet: DEV }), code("configure.after_start"));
});

test("addChainAfterConfigureIsAnError", () => {
  const { s } = make();
  const chain = {
    name: "My L2", chain_id: 777001, rpc: ["https://rpc.my-l2.example"], tokens: { USDC: DEV }, confirmations: 3,
    explorer_tx: "https://scan.my-l2.example/tx/{}",
  };
  s.addChain("myl2", chain);
  assert.throws(() => s.addChain("bad key!", chain), code("chain.invalid_key"), "addChain validates");
  s.configure({ wallet: DEV, chains: ["base", "myl2"] });
  assert.deepEqual(s._config!.chains, ["base", "myl2"], "the custom chain is usable");
  assert.throws(() => s.addChain("other", chain), code("configure.after_add_chain"));
});

test("logoNamesAFileUnderImgScfg", async () => {
  const env = make();
  const { s, files } = env;
  assert.throws(() => s.configure({ wallet: DEV, logo: "logo" }), code("logo.file_missing"), "checked on desktop");
  files.set("/game/img/scfg/logo.png", "PNG");
  s.configure({ wallet: DEV, logo: "logo" });
  assert.equal(s._config!.logo, "img/scfg/logo.png", "the extension defaults to .png");
  assert.equal(await s.logoDataUrl(), "data:image/png;base64," + Buffer.from("PNG").toString("base64"));
  files.set("/game/img/scfg/brand/mark.webp", "W");
  s.configure({ wallet: DEV, logo: "brand/mark.webp" });
  assert.equal(s._config!.logo, "img/scfg/brand/mark.webp");
  assert.throws(() => s.configure({ wallet: DEV, logo: "../logo" }), code("logo.bad_path"), "no escaping img/scfg");
  assert.throws(() => s.configure({ wallet: DEV, logo: "logo.svg" }), code("logo.bad_type"), "raster images only");
  s.configure({ wallet: DEV });
  assert.equal(await s.logoDataUrl(), null, "no logo");
});

// --- the logo in an encrypted deployment (Ruling 11) ------------------------------------------

const KEY = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
const LOGO_PNG = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52, 7, 7, 7]);
const LOGO_PARAMS = (extra: Record<string, string> = {}): PluginEntry[] =>
  [{ name: "SCFG", status: true, parameters: { wallet: DEV, logo: "logo", ...extra } }];

test("anEncryptedDeploymentReadsTheLogoFromItsUnderscoreFile", async () => {
  // "Encrypt image files" ships img/scfg/logo.png_ only: read it and decrypt it the MZ way
  // (Utils.hasEncryptedImages, Utils.decryptArrayBuffer), on desktop and on the web
  const shipped = encryptForDeployment(LOGO_PNG, KEY);
  const env = make({ boot: true, options: [], encryption: { images: true, key: KEY }, plugins: LOGO_PARAMS(),
    files: { "/game/img/scfg/logo.png_": shipped } });
  assert.equal(env.s._disabled, null, "the parameters are fine: payments are on");
  await bootShim();
  await env.s._mirror.idle();
  env.shim.frame();
  assert.ok(env.s._worker !== null, "payments started");
  const want = "data:image/png;base64," + Buffer.from(LOGO_PNG).toString("base64");
  assert.equal(await env.s.logoDataUrl(), want, "the sheet's logo, decrypted");
  const page = env.s._logoForPage();
  assert.ok(page !== null, "the desktop checkout page has a logo");
  const [bytes, type] = page();
  assert.deepEqual([[...bytes], type], [[...LOGO_PNG], "image/png"], "the page serves the decrypted PNG");
  assert.deepEqual([env.errors, env.warnings], [[], []], "nothing to report");
  resetShim();

  // the web: fetched beside index.html, the "_" file first
  const fetched: string[] = [];
  const realFetch = g.fetch;
  g.fetch = async (url: string) => {
    fetched.push(url);
    return url === "img/scfg/logo.png_"
      ? { ok: true, status: 200, arrayBuffer: async () => shipped.slice().buffer }
      : { ok: false, status: 404, arrayBuffer: async () => new ArrayBuffer(0) };
  };
  try {
    const web = make({ nwjs: false, boot: true, options: [], encryption: { images: true, key: KEY }, plugins: LOGO_PARAMS() });
    await bootShim();
    assert.equal(await web.s.logoDataUrl(), want);
    assert.deepEqual(fetched, ["img/scfg/logo.png_"]);
  } finally {
    g.fetch = realFetch;
  }
});

test("anEncryptedDeploymentKeepsAPlainLogoMzDidNotEncrypt", async () => {
  // MZ encrypts the png images; a webp logo ships as it is, and is read as it is
  const env = make({ options: [], encryption: { images: true, key: KEY }, files: { "/game/img/scfg/mark.webp": "WEBP" } });
  await bootShim();
  env.s.configure({ wallet: DEV, logo: "mark.webp" });
  assert.equal(await env.s.logoDataUrl(), "data:image/webp;base64," + Buffer.from("WEBP").toString("base64"));
  assert.deepEqual(env.warnings, []);
});

test("aMissingLogoInAReleaseShowsNoLogoAndPaymentsStayOn", async () => {
  // Ruling 11: outside test play a logo that cannot be read is a warning, never "payments off"
  const env = make({ boot: true, options: [], plugins: LOGO_PARAMS() });
  assert.equal(env.s._disabled, null, "not disabled");
  await bootShim();
  await env.s._mirror.idle();
  env.shim.frame();
  assert.ok(env.s._worker !== null, "payments started");
  assert.equal(await env.s.logoDataUrl(), null, "no logo");
  assert.equal(env.s._logoForPage(), null, "the checkout page has none either");
  assert.equal(env.warnings.length, 2, "warned (console.warn), each time it was looked for");
  assert.match(env.warnings[0]!, /scfg: cannot read the logo img\/scfg\/logo\.png.*no logo/);
  assert.deepEqual([env.errors, env.notices], [[], []], "no error and nothing over the game");
  // an encrypted deployment whose logo.png_ is missing: the same
  const enc = make({ boot: true, options: [], encryption: { images: true, key: KEY }, plugins: LOGO_PARAMS() });
  assert.equal(enc.s._disabled, null);
  await bootShim();
  assert.equal(await enc.s.logoDataUrl(), null);
  assert.equal(enc.warnings.length, 1);
});

test("aMissingLogoInTestPlayIsReported", async () => {
  // test play (the project, never encrypted): neither logo.png nor logo.png_ -> logo.file_missing,
  // reported, and the plugin stays disabled until the developer fixes it (Ren'Py's ConfigError)
  const env = make({ boot: true, plugins: LOGO_PARAMS() });
  assert.match(String(env.s._disabled), /logo file not found/);
  assert.ok(env.errors.some((e) => /logo file not found/.test(e)), "console.error");
  assert.ok(env.notices.some((e) => /logo file not found/.test(e)), "shown over the game");
  // with the file there, fine
  const ok = make({ boot: true, plugins: LOGO_PARAMS(), files: { "/game/img/scfg/logo.png": "PNG" } });
  assert.equal(ok.s._disabled, null);
});

test("versionMatchesScfgCore", () => {
  const init = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..", "plugin", "game", "python-packages", "scfg_core", "__init__.py");
  assert.equal(VERSION, /__version__ = "([^"]+)"/.exec(readFileSync(init, "utf8"))![1]);
});

// --- queries ---------------------------------------------------------------------------------

test("ownsAfterReceipt", async () => {
  const env = await configured();
  const { s } = env;
  await start(s);
  assert.equal(s.owns("chapter2"), false, "not owned yet");
  let changed = 0;
  const unsubscribe = s.onReceiptsChanged(() => changed++);
  assert.equal(await addTx(env, "0x" + "ab".repeat(32)), true, "a receipt is stored");
  assert.equal(s.owns("chapter2"), true, "owned");
  assert.equal(changed, 1, "onReceiptsChanged once");
  assert.equal(s.receipts().length, 1, "receipts()");
  s.receipts()[0]!.item = "hacked";
  assert.equal(s.receipts()[0]!.item, "chapter2", "receipts() are copies");
  assert.deepEqual([s.hasTipped(), s.tipTotal()], [false, "0"], "tipTotal() is Python's str(Decimal)");
  assert.throws(() => s.owns("nothing"), code("item.unknown"), "an unknown item");
  await addTx(env, "0x" + "cd".repeat(32), { kind: "tip", item: null, amount: "2.50" });
  assert.deepEqual([s.hasTipped(), s.tipTotal()], [true, "2.50"], "a tip");
  assert.equal(changed, 2);
  unsubscribe();
  // desktop: written to the save folder, as Ren'Py writes a file
  assert.equal(s.exportReceipts(), "save/scfg-receipts.json", "exportReceipts() returns the file's path");
  const listed = JSON.parse(String(env.files.get("save/scfg-receipts.json")));
  assert.equal(listed.length, 2, "the receipts as a JSON list");
  assert.ok(String(env.files.get("save/scfg-receipts.json")).startsWith("[\n  {\n    \"amount\": "), "indented, sorted keys");
  s.clearLocalData();
  assert.deepEqual([s.owns("chapter2"), s.receipts(), s.pending()], [false, [], []], "clearLocalData forgets them at once");
  await settle(env);
  assert.deepEqual(onDisk(env).receipts, {}, "and in storage");
  await s._worker!.runOnce();
  env.shim.frame();
  await settle(env);
  assert.deepEqual([s.owns("chapter2"), s.receipts()], [false, []], "the engine is wiped too");
  assert.deepEqual(onDisk(env).receipts, {}, "still empty in storage");
  assert.equal(changed, 2, "an unsubscribed listener is not called");
});

test("exportReceiptsOnTheWebReturnsTheJson", async () => {
  const env = await configured({ nwjs: false });
  await start(env.s);
  await addTx(env, "0x" + "ab".repeat(32), { payer: "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed", item: "chapter2" });
  const text = env.s.exportReceipts();
  assert.deepEqual(JSON.parse(text), env.s.receipts());
  assert.equal(env.files.size, 0, "no file in a browser");
});

test("stateSurvivesRestart", async () => {
  const storage = new MemoryStorage();
  const env = await configured({ storage });
  await start(env.s);
  await addTx(env, "0x" + "ab".repeat(32));
  const sid = (await startSession(env)).id;
  await settle(env);
  const receipts = env.s.receipts();
  const pending = env.s.pending();
  assert.equal(receipts.length, 1, "one receipt");
  assert.deepEqual(pending.map((r) => r.id), [sid], "one session");
  resetShim(); // the game closes
  const again = make({ storage });
  const b = again.s;
  assert.deepEqual(b.receipts(), [], "nothing is read at plugin load");
  b.configure({ wallet: DEV });
  b.addItem("chapter2", CHAPTER2);
  await bootShim();
  assert.deepEqual(b.receipts(), receipts, "the same receipts after a restart");
  assert.deepEqual(b.pending(), pending, "the same pending sessions");
  assert.equal(b.owns("chapter2"), true, "owned");
  assert.deepEqual(Object.keys(b._worker!.snapshot.receipts).length, 1, "the Engine got them");
});

// --- persistence -----------------------------------------------------------------------------

test("newSessionIsSavedAtOnce", async () => {
  const env = await configured();
  await start(env.s);
  const first = (await startSession(env)).id;
  await settle(env);
  assert.ok(first in onDisk(env).pending, "the first session is stored");
  env.clock.advance(1.0); // well inside the save interval
  const second = (await startSession(env)).id;
  await settle(env);
  assert.deepEqual(Object.keys(onDisk(env).pending), [first, second], "a new session is written at once");
  env.clock.advance(1.0);
  await run(env, (e) => e.reportHint(second, "0x" + "ef".repeat(32)));
  await settle(env);
  assert.equal(onDisk(env).pending[second].hint_tx, "0x" + "ef".repeat(32), "a tx hint is written at once");
  env.clock.advance(1.0);
  await run(env, (e) => {
    e.pending[second].state = "confirming";
  });
  await settle(env);
  assert.equal(onDisk(env).pending[second].state, "confirming", "a state change is written at once");
  env.clock.advance(1.0);
  await run(env, (e) => e.discardSession(first));
  await settle(env);
  assert.deepEqual(Object.keys(onDisk(env).pending), [second], "a removed session is written at once");
});

test("scanProgressIsSavedAtMostEvery10s", async () => {
  const env = await configured();
  const { s, clock } = env;
  await start(s);
  const sid = (await startSession(env)).id;
  await settle(env);
  const chain = env.chains["arbitrum"]!;
  const savedCursor = onDisk(env).pending[sid].cursor;
  const scan = async () => {
    chain.head += 50;
    await s._worker!.runOnce();
    env.shim.frame();
    await settle(env);
  };
  clock.advance(4.0);
  await scan();
  const cursor = s.pending()[0]!.cursor;
  assert.notDeepEqual(cursor, savedCursor, "the scan moved the cursor");
  assert.deepEqual(onDisk(env).pending[sid].cursor, savedCursor, "scan progress waits for the interval");
  clock.advance(5.9);
  await scan();
  assert.deepEqual(onDisk(env).pending[sid].cursor, savedCursor, "still waiting at 9.9 s");
  clock.advance(0.1);
  await scan();
  assert.deepEqual(onDisk(env).pending[sid].cursor, s.pending()[0]!.cursor, "written once 10 s passed");
  // progress inside the interval is written when it passes, with no new snapshot
  clock.advance(1.0);
  await scan();
  const last = s.pending()[0]!.cursor;
  assert.notDeepEqual(onDisk(env).pending[sid].cursor, last, "unsaved progress");
  clock.advance(9.0);
  env.shim.frame();
  await settle(env);
  assert.deepEqual(onDisk(env).pending[sid].cursor, last, "flushed by a later frame");
  assert.equal(s._mirror.dirty, false);
});

for (const nwjs of [true, false]) {
  test(`corruptStateIsKeptAsideAndStartsEmpty[${nwjs ? "nwjs" : "web"}]`, async () => {
    const storage = new MemoryStorage();
    const key = nwjs ? STATE : "rmmzsave.12345678.scfg";
    const bad = "{\"version\": 1, \"pending\": {\"ps_1\": ";
    storage.write(key, bad);
    const env = await configured({ storage, nwjs });
    const { s } = env;
    assert.deepEqual([s.receipts(), s.pending(), s.owns("chapter2")], [[], [], false], "empty stores");
    assert.equal(env.errors.length, 1, env.errors.join("\n"));
    const aside = nwjs ? "save/scfg_corrupt_1.rmmzsave" : "rmmzsave.12345678.scfg_corrupt_1";
    assert.ok(env.errors[0]!.startsWith("scfg: the payment state ") && env.errors[0]!.includes(aside), env.errors[0]);
    assert.deepEqual(env.notices.length, 1, "shown over the game in test play");
    assert.equal(storage.read(aside), bad, "the bad state is kept aside");
    await start(s);
    await addTx(env, "0x" + "ab".repeat(32));
    await settle(env);
    assert.equal(Object.keys(onDisk(env, key).receipts).length, 1, "a fresh state");
    assert.equal(storage.read(aside), bad, "the copy is kept");
  });
}

test("storedTipBeyondTheAmountRangeIsIgnoredThenRemoved", async () => {
  // Python parity: a stored amount out of the amount range (|adjusted exponent| > 40) neither owns
  // nor tips, so the state is used as it is, and the launch re-verification removes that receipt.
  const storage = new MemoryStorage();
  const text = JSON.stringify({
    version: 1, pending: {}, receipts: { "arbitrum:0xab:3": receipt("0x" + "ab".repeat(32), { kind: "tip", item: null, amount: "1E+1000000" }) },
  });
  storage.write(STATE, text);
  const env = await configured({ storage });
  const { s } = env;
  assert.ok(s._worker !== null, "payments started from the stored state");
  assert.deepEqual([s.tipTotal(), s.hasTipped()], ["0", false], "the tip does not count");
  assert.equal(storage.has("save/scfg_corrupt_1.rmmzsave"), false, "nothing is kept aside");
  await start(s);
  await settle(env);
  assert.deepEqual(s.receipts(), [], "removed by the launch re-verification");
  assert.ok(env.errors.some((l) => l.includes("is invalid and was removed")), env.errors.join("\n"));
  assert.equal(Object.keys(onDisk(env).receipts).length, 0, "and from the stored state");
});

test("stateTheEngineRefusesAtStartIsKeptAside", async () => {
  // The same rule where the Engine/Worker build itself throws: here the state was changed after it
  // was read (a stand-in for anything the read cannot foresee).
  const storage = new MemoryStorage();
  const text = JSON.stringify({ version: 1, pending: {}, receipts: { "arbitrum:0xab:3": receipt("0x" + "ab".repeat(32)) } });
  storage.write(STATE, text);
  const env = make({ storage });
  await bootShim(); // read, not configured yet: no Worker
  Object.defineProperty(env.s._mirror.doc!.receipts, "x", { enumerable: true, get() { throw new Error("unreadable entry"); } });
  env.s.configure({ wallet: DEV });
  env.s.addItem("chapter2", CHAPTER2);
  assert.doesNotThrow(() => env.shim.frame());
  assert.equal(env.s._worker, null, "not started from the refused state");
  await settle(env);
  env.shim.frame();
  assert.ok(env.s._worker !== null, "started empty on a later frame");
  assert.deepEqual(env.s.receipts(), []);
  assert.equal(storage.read("save/scfg_corrupt_1.rmmzsave"), text, "the stored state is kept aside");
  assert.ok(env.errors.some((l) => l.includes("cannot be used (Error: unreadable entry")), env.errors.join("\n"));
});

test("unreadableStateIsNeverOverwritten", async () => {
  const storage = new MemoryStorage();
  const before = JSON.stringify({ version: 1, pending: {}, receipts: { "arbitrum:0xab:3": receipt("0x" + "ab".repeat(32)) } });
  storage.write(STATE, before);
  const env = make({ storage });
  env.shim.failNext("load", Object.assign(new Error("EBUSY: resource busy or locked"), { code: "EBUSY" }));
  env.s.configure({ wallet: DEV });
  env.s.addItem("chapter2", CHAPTER2);
  await bootShim();
  env.shim.frame(3);
  assert.equal(env.s._worker, null, "payments do not start");
  assert.ok(env.errors.some((l) => l.startsWith("scfg: cannot read the stored receipts:") && l.includes("EBUSY")),
    env.errors.join("\n"));
  assert.equal(env.notices.length, 1, "shown in test play");
  assert.throws(() => env.s.configure({ wallet: DEV }), code("configure.after_start"), "the start failed for this run");
  env.s.clearLocalData();
  env.shim.frame();
  await settle(env);
  assert.equal(storage.read(STATE), before, "the state is untouched (not saved over)");
  assert.equal(storage.has("save/scfg_corrupt_1.rmmzsave"), false, "and not moved aside");
  resetShim();
  const next = await configured({ storage });
  assert.equal(next.s.owns("chapter2"), true, "the next launch reads the receipt");
});

test("failedSaveIsRetriedEvery10s", async () => {
  const env = await configured();
  await start(env.s);
  env.shim.failNext("save", Object.assign(new Error("ENOSPC: no space left on device"), { code: "ENOSPC" }));
  const sid = (await startSession(env)).id;
  await settle(env);
  assert.equal(env.storage.has(STATE), false, "the save failed");
  assert.ok(env.errors.some((l) => l.startsWith("scfg: cannot save the payment state:") && l.includes("ENOSPC")),
    env.errors.join("\n"));
  env.clock.advance(9.0);
  env.shim.frame();
  await settle(env);
  assert.equal(env.storage.has(STATE), false, "not retried inside the interval");
  env.clock.advance(1.0);
  env.shim.frame();
  await settle(env);
  assert.ok(sid in onDisk(env).pending, "retried once the interval passed, with no new snapshot");
  assert.equal(env.s._mirror.dirty, false, "and clean");
});

test("savesNeverOverlap", async () => {
  const env = await configured();
  await start(env.s);
  const sm = g.StorageManager;
  const save = sm.saveObject.bind(sm);
  let inflight = 0;
  let most = 0;
  let calls = 0;
  const gates: (() => void)[] = [];
  sm.saveObject = (name: string, object: unknown) => {
    calls++;
    inflight++;
    most = Math.max(most, inflight);
    const copy = JSON.parse(JSON.stringify(object));
    return new Promise<void>((resolve) => gates.push(resolve))
      .then(() => save(name, copy))
      .finally(() => inflight--);
  };
  const ids: string[] = [];
  for (let i = 0; i < 3; i++) {
    env.clock.advance(1.0);
    ids.push((await startSession(env)).id); // each wants a save at once
  }
  assert.equal(calls, 1, "one save in flight; the others wait");
  while (gates.length > 0) {
    gates.shift()!();
    await new Promise((r) => setTimeout(r, 0));
  }
  await settle(env);
  assert.equal(most, 1, "never two at once");
  assert.equal(calls, 2, "the waiting saves became one, of the latest state");
  assert.deepEqual(Object.keys(onDisk(env).pending), ids, "the latest state is stored");
  assert.equal(env.s._mirror.dirty, false);
});

// --- boot ------------------------------------------------------------------------------------

for (const nwjs of [true, false]) {
  test(`bootReadsTheStateOnlyOnceTheDatabaseAndSaveKeysAreReady[${nwjs ? "nwjs" : "web"}]`, async () => {
    const storage = new MemoryStorage();
    storage.write(nwjs ? STATE : "rmmzsave.12345678.scfg",
      JSON.stringify({ version: 1, pending: {}, receipts: { "arbitrum:0xab:3": receipt("0x" + "ab".repeat(32)) } }));
    const item = JSON.stringify({ id: "chapter2", code: "37", price: "4.99", token: "USDC", title: "Chapter 2", ownedSwitch: "1" });
    const plugins = [{ name: "SCFG", status: true, parameters: { wallet: DEV, items: JSON.stringify([item]) } }];
    const calls: [string, boolean][] = [];
    const seen: boolean[] = [];
    const watch = () => {
      // every storage call, with what was loaded when it was made
      const sm = g.StorageManager;
      for (const name of ["exists", "loadObject", "loadZip", "saveObject", "remove"]) {
        const real = sm[name];
        sm[name] = function (this: unknown, ...args: unknown[]) {
          calls.push([name, g.$dataSystem !== null && sm.forageKeysUpdated()]);
          return real.apply(this, args);
        };
      }
      // a slow read (several frames): the boot must wait for it
      const loadZip = sm.loadZip;
      sm.loadZip = async function (this: unknown, ...args: unknown[]) {
        for (let i = 0; i < 5; i++) await new Promise((r) => setTimeout(r, 0));
        return loadZip.apply(this, args);
      };
      const setupNewGame = g.DataManager.setupNewGame;
      g.DataManager.setupNewGame = function (this: unknown) {
        seen.push(g.SCFG.owns("chapter2")); // Scene_Boot.start: the first scene's code
        return setupNewGame.call(this);
      };
    };
    const env = make({ storage, nwjs, plugins, boot: true, beforeLoad: watch }); // the plugin script runs
    assert.equal(g.$dataSystem, null, "at plugin load the database is not loaded");
    assert.equal(calls.length, 0, "and the plugin does not touch the storage");
    assert.equal(env.s.owns("chapter2"), false, "nothing is read yet");
    assert.equal(env.warnings.length, 1, "which test play points out");
    await bootShim();
    assert.ok(calls.some(([n]) => n === "loadZip"), JSON.stringify(calls));
    assert.ok(calls.every(([, ready]) => ready), `every storage call came after the database and save keys: ${JSON.stringify(calls)}`);
    assert.deepEqual(seen, [true], "Scene_Boot waited for the state: the purchase is known before the game starts");
    assert.deepEqual(env.errors, []);
  });
}

test("bootReportsBadParamsWithoutThrowing", async () => {
  const cases: [Record<string, string>, RegExp][] = [
    [{ wallet: "0x123" }, /wallet/],
    [{ wallet: DEV, rpcUrls: "{base:" }, /rpcUrls is not valid JSON/],
    [{ wallet: DEV, items: JSON.stringify([JSON.stringify({ id: "chapter2", code: "0", price: "4.99", token: "USDC", title: "C" })]) },
      /item code/],
  ];
  for (const [parameters, why] of cases) {
    const plugins = [{ name: "SCFG", status: true, parameters }];
    let env: Env | null = null;
    assert.doesNotThrow(() => {
      env = make({ plugins, boot: true });
    }, JSON.stringify(parameters));
    const { s, errors, notices } = env!;
    assert.equal(g.SCFG, s, "window.SCFG exists");
    assert.equal(errors.length, 1, errors.join("\n"));
    assert.ok(errors[0]!.startsWith("scfg: the plugin parameters are invalid") && why.test(errors[0]!), errors[0]);
    assert.equal(notices.length, 1, "shown over the game in test play");
    assert.equal(s._config, null, "the plugin stays disabled");
    assert.throws(() => s.owns("chapter2"), code("params.invalid"));
    assert.throws(() => s.configure({ wallet: DEV }), code("params.invalid"));
    await bootShim(); // the game still boots
    env!.shim.frame(5);
    assert.equal(s._worker, null);
    resetShim();
  }
  // good parameters configure the plugin
  const item = JSON.stringify({ id: "chapter2", code: "37", price: "4.99", token: "USDC", title: "Chapter 2", minPrice: "" });
  const env = make({ boot: true, plugins: [{ name: "SCFG", status: true, parameters: { wallet: DEV, testnet: "true", notify: "false", items: JSON.stringify([item]) } }] });
  assert.deepEqual([env.s._config?.testnet, env.s.notify, env.errors], [true, false, []]);
  await bootShim();
  assert.equal(env.s.owns("chapter2"), false);
});

// --- carried from Task 2 -------------------------------------------------------------------------

test("crashMidSaveOnDesktopKeepsTheReceipts", async () => {
  const storage = new MemoryStorage();
  const env = await configured({ storage });
  await start(env.s);
  await addTx(env, "0x" + "ab".repeat(32));
  await settle(env);
  const before = storage.read(STATE);
  env.shim.crashNextSave(); // the game dies while MZ writes the next save
  await startSession(env);
  await new Promise((r) => setTimeout(r, 0));
  assert.equal(storage.has(STATE), false, "MZ renamed the state file away before writing");
  assert.equal(storage.read(STATE + "_"), before, "only its backup is left");
  resetShim();
  const next = await configured({ storage });
  assert.deepEqual(next.errors, [], "no corrupt or unreadable state");
  assert.equal(next.s.owns("chapter2"), true, "the receipt is read from the backup");
  assert.equal(storage.read(STATE), before, "which is the state file again");
  assert.equal(storage.has(STATE + "_"), false, "so the next save has no backup to unlink");
});

for (const nwjs of [true, false]) {
  test(`corruptStateThatCannotBeKeptAsideStartsNoPayments[${nwjs ? "nwjs" : "web"}]`, async () => {
    const storage = new MemoryStorage();
    const key = nwjs ? STATE : "rmmzsave.12345678.scfg";
    const bad = "{\"version\": 1, \"pending\": {\"ps_1\": ";
    storage.write(key, bad);
    const env = make({ storage, nwjs });
    env.shim.failNext("save", Object.assign(new Error("ENOSPC: no space left on device"), { code: "ENOSPC" }));
    env.s.configure({ wallet: DEV });
    env.s.addItem("chapter2", CHAPTER2);
    await bootShim();
    env.shim.frame(3);
    assert.equal(env.s._worker, null, "payments do not start");
    assert.ok(env.errors.some((l) => l.startsWith("scfg: cannot read the stored receipts:") && l.includes("ENOSPC")),
      env.errors.join("\n"));
    env.s.clearLocalData();
    env.shim.frame();
    await settle(env);
    assert.equal(storage.read(key), bad, "nothing is saved over it");
    assert.equal(storage.keys().length, 1, "and no copy was made");
  });
}

test("refusedStateThatCannotBeKeptAsideStartsNoPayments", async () => {
  // the core refuses the stored state (an entry it cannot read) and the copy cannot be written: no
  // empty state that a later save would write over the player's receipts
  const storage = new MemoryStorage();
  const text = JSON.stringify({ version: 1, pending: {}, receipts: { "arbitrum:0xab:3": receipt("0x" + "ab".repeat(32)) } });
  storage.write(STATE, text);
  const env = make({ storage });
  env.shim.failNext("save", new Error("EIO: i/o error, write"));
  await bootShim(); // read, not configured yet: no Worker
  Object.defineProperty(env.s._mirror.doc!.receipts, "x", { enumerable: true, get() { throw new Error("unreadable entry"); } });
  env.s.configure({ wallet: DEV });
  env.s.addItem("chapter2", CHAPTER2);
  env.shim.frame(3);
  await settle(env);
  assert.equal(env.s._worker, null, "payments do not start");
  assert.ok(env.errors.some((l) => l.includes("could not be kept aside")), env.errors.join("\n"));
  env.s.clearLocalData();
  env.shim.frame();
  await settle(env);
  assert.equal(storage.read(STATE), text, "nothing is saved over it");
});

for (const nwjs of [true, false]) {
  test(`clearLocalDataBeforeTheStateIsReadAppliesOnceItIs (${nwjs ? "desktop" : "web"})`, async () => {
    // a script that clears before Scene_Boot read the state: the clear waits for the read, so what
    // the game holds and what is stored agree (both empty) and the old receipts never come back
    // (on the web a save before the database loaded cannot even name its key)
    const key = nwjs ? STATE : "rmmzsave.12345678.scfg";
    const storage = new MemoryStorage();
    storage.write(key, JSON.stringify({ version: 1, pending: {}, receipts: { "arbitrum:0xab:3": receipt("0x" + "ab".repeat(32)) } }));
    const env = make({ storage, nwjs });
    env.s.configure({ wallet: DEV });
    env.s.addItem("chapter2", CHAPTER2);
    env.s.clearLocalData(); // while the plugins load: nothing is read yet
    await new Promise((resolve) => setTimeout(resolve, 0));
    assert.ok(storage.read(key)!.includes("0xabab"), "nothing written before the read");
    await bootShim();
    await settle(env);
    assert.deepEqual([env.s.owns("chapter2"), env.s.receipts(), env.s.pending()], [false, [], []], "memory: cleared");
    assert.deepEqual(onDisk(env, key).receipts, {}, "disk: cleared too");
    env.shim.frame(2);
    await settle(env);
    assert.deepEqual([env.s.receipts(), onDisk(env, key).receipts], [[], {}], "still both empty once payments started");
    assert.deepEqual(env.errors, [], "nothing failed");
  });
}

test("defaultRpcFactoryUsesTheMonotonicClock", async () => {
  // no rpc_factory hook: the plugin's own factory builds the RPC clients, on the monotonic clock
  // (SCFG's clock), never on the Engine's wall clock
  const wall = () => 1_700_000_000;
  for (const monotonicHook of [true, false]) {
    const shim = installShim(g, { nwjs: true });
    const clock = new FakeClock();
    const s = new Scfg({
      ...(monotonicHook ? { clock: clock.call } : {}),
      transport: new FakeTransport({}), engine_opts: { clock: wall },
      worker_opts: { schedule: () => null, cancel: () => undefined },
      log: { error: () => undefined, warn: () => undefined }, platform: { devNotice: () => undefined },
    });
    installHooks(s);
    s.configure({ wallet: DEV });
    await bootShim();
    shim.frame();
    assert.ok(s._engine !== null);
    assert.equal(s._engine.clock(), 1_700_000_000, "the Engine keeps its wall clock (session times)");
    const rpc = s._engine.rpc("arbitrum");
    const now = (globalThis as any).performance.now() / 1000;
    if (monotonicHook) assert.equal(rpc.clock(), 1000, "the client reads SCFG's clock");
    else assert.ok(Math.abs(rpc.clock() - now) < 5, `the client reads performance.now() / 1000 (${rpc.clock()} vs ${now})`);
    resetShim();
  }
});

test("theEnginesAndTheWorkersErrorsAreReportedOnTheConsole", async () => {
  // the Engine's on_error (a crash it contains: one stored receipt's re-verification) and the
  // Worker's (a pass that failed) are the glue's report, "scfg: worker error:" on console.error
  const env = await configured();
  await start(env.s);
  const engine = env.s._engine as any;
  engine.receipts["k"] = receipt("0x" + "cd".repeat(32));
  engine._rpcs.clear();
  engine._rpc_factory = () => {
    throw new Error("boom");
  };
  assert.deepEqual(await engine.reverify(), [], "an unverifiable receipt is kept");
  assert.deepEqual(env.errors, ["scfg: worker error: Error: boom"]);
  env.s._worker!.on_error(new Error("the pass failed"));
  assert.deepEqual(env.errors, ["scfg: worker error: Error: boom", "scfg: worker error: Error: the pass failed"]);
});

test("truncatedStateFileOnDesktopFallsBackToMzsBackup", async () => {
  // the review's case: a crash while MZ wrote the state file leaves it truncated beside MZ's backup;
  // before the fix the state started empty and the first save unlinked the only good copy
  const first = await configured();
  await start(first.s);
  await addTx(first, "0x" + "ab".repeat(32));
  const sid = (await startSession(first)).id;
  await settle(first);
  const good = first.storage.read(STATE)!;
  resetShim();
  const storage = new MemoryStorage();
  storage.write(STATE, good.slice(0, 30));
  storage.write(STATE + "_", good);
  const env = await configured({ storage });
  assert.equal(env.s.owns("chapter2"), true, "the backup's receipt");
  assert.deepEqual(env.s.pending().map((r) => r.id), [sid], "and its pending payment");
  assert.equal(env.errors.length, 1, "reported");
  assert.ok(env.errors[0]!.includes("MZ's backup of it save/scfg.rmmzsave_"), env.errors[0]);
  env.s._mirror.dirty = true;
  env.s._mirror.save(); // the next save
  await settle(env);
  assert.deepEqual([Object.keys(onDisk(env).receipts).length, Object.keys(onDisk(env).pending)], [1, [sid]], "survives the next save");
  assert.equal(storage.has(STATE + "_"), false);
  assert.equal(storage.read("save/scfg_corrupt_1.rmmzsave"), good.slice(0, 30), "the truncated file is kept aside");
});
