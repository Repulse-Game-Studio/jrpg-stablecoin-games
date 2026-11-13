/** ScfgStore on the MZ shim's StorageManager, desktop (save files) and web (localforage keys). */
import { afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { DEFAULT_GAME_ID, MemoryStorage, bootShim, installShim, resetShim } from "../../test/mz_shim/index.ts";
import { ScfgStore } from "./store.ts";

afterEach(() => resetShim());

const MODES = [
  { name: "nwjs", nwjs: true, key: (n: string) => `save/${n}.rmmzsave` },
  { name: "web", nwjs: false, key: (n: string) => `rmmzsave.${DEFAULT_GAME_ID}.${n}` },
] as const;

const RECEIPT = {
  chain: "arbitrum", tx_hash: "0x" + "ab".repeat(32), log_index: 3, block: 990, kind: "unlock", item: "chapter2",
  amount: "4.99", token: "USDC", payer: "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed",
  recipient: "0x52908400098527886e0f7030069857d2e4169ee7", source: "payment",
};

async function env(nwjs: boolean, seed: Record<string, string> = {}) {
  const storage = new MemoryStorage();
  for (const [k, v] of Object.entries(seed)) storage.write(k, v);
  const shim = installShim(globalThis, { nwjs, storage });
  await bootShim(); // the database and the save keys
  return { shim, storage, store: new ScfgStore() };
}

for (const mode of MODES) {
  test(`firstLaunchIsEmpty[${mode.name}]`, async () => {
    const { store } = await env(mode.nwjs);
    assert.deepEqual(await store.load(), { pending: {}, receipts: {}, warning: null });
  });

  test(`savedStateLoadsBack[${mode.name}]`, async () => {
    const { storage, store } = await env(mode.nwjs);
    const doc = { pending: { ps_1: { id: "ps_1", state: "awaiting", created_at: 100.5 } }, receipts: { "arbitrum:0xab:3": RECEIPT } };
    await store.save(doc);
    assert.deepEqual(JSON.parse(storage.read(mode.key("scfg"))!), { version: 1, ...doc }, "the document, at MZ's key");
    assert.deepEqual(await new ScfgStore().load(), { ...doc, warning: null });
  });

  test(`corruptStateIsKeptAside[${mode.name}]`, async () => {
    const bad = "{\"version\": 1, \"pending\": {\"ps_1\": ";
    const { storage, store } = await env(mode.nwjs, { [mode.key("scfg")]: bad });
    const got = await store.load();
    assert.ok(!("error" in got));
    assert.deepEqual([got.pending, got.receipts], [{}, {}], "starts empty");
    assert.match(got.warning ?? "", /is not valid JSON; starting with no receipts or pending payments/);
    assert.ok(got.warning!.includes(mode.key("scfg_corrupt_1")), got.warning!);
    assert.equal(storage.read(mode.key("scfg_corrupt_1")), bad, "kept as it was");
    assert.equal(storage.has(mode.key("scfg")), false, "and moved away");
    // a later corruption never replaces an earlier copy
    storage.write(mode.key("scfg"), "[1, 2]");
    if (!mode.nwjs) await (globalThis as any).StorageManager.updateForageKeys();
    const again = await new ScfgStore().load();
    assert.ok(!("error" in again) && again.warning!.includes("is not a JSON object"), JSON.stringify(again));
    assert.equal(storage.read(mode.key("scfg_corrupt_2")), "[1, 2]");
    assert.equal(storage.read(mode.key("scfg_corrupt_1")), bad);
  });

  test(`unreadableStateIsAnError[${mode.name}]`, async () => {
    const good = JSON.stringify({ version: 1, pending: {}, receipts: { k: RECEIPT } });
    const { shim, storage, store } = await env(mode.nwjs, { [mode.key("scfg")]: good });
    // NW.js: the file read throws (EACCES, EBUSY, EIO); web: localforage rejects
    shim.failNext("load", Object.assign(new Error("EACCES: permission denied, open 'save/scfg.rmmzsave'"), { code: "EACCES" }));
    const got = await store.load();
    assert.ok("error" in got && got.error === "unreadable", JSON.stringify(got));
    assert.match((got as { detail: string }).detail, /EACCES/);
    assert.equal(storage.read(mode.key("scfg")), good, "left where it is");
    assert.equal(storage.has(mode.key("scfg_corrupt_1")), false, "not moved aside");
  });
}

test("otherVersionsAndShapesAreCorrupt", async () => {
  for (const [text, why] of [
    [JSON.stringify({ version: 2, pending: {}, receipts: {} }), "has an unknown version 2"],
    [JSON.stringify({ version: 1, pending: [], receipts: {} }), "has no pending/receipts objects"],
    [JSON.stringify({ pending: {}, receipts: {} }), "has an unknown version undefined"],
    ["null", "is not a JSON object"],
  ] as const) {
    const { storage, store } = await env(true, { "save/scfg.rmmzsave": text });
    const got = await store.load();
    assert.ok(!("error" in got) && got.warning!.includes(why), `${text}: ${JSON.stringify(got)}`);
    assert.equal(storage.read("save/scfg_corrupt_1.rmmzsave"), text);
    resetShim();
  }
});

test("emptyFileOnDesktopIsCorrupt", async () => {
  // MZ's NW.js reader (`if (data) resolve(data) else reject(new Error("Savefile not found"))`) rejects
  // an empty file as if it were missing; the shim reads it as "", so the rejection is injected.
  const { shim, storage, store } = await env(true, { "save/scfg.rmmzsave": "" });
  shim.failNext("load", new Error("Savefile not found"));
  const got = await store.load();
  assert.ok(!("error" in got) && got.warning!.includes("is empty"), JSON.stringify(got));
  assert.equal(storage.read("save/scfg_corrupt_1.rmmzsave"), "");
  assert.equal(storage.has("save/scfg.rmmzsave"), false);
  // the shim's own reading of an empty file: corrupt as well
  storage.write("save/scfg.rmmzsave", "");
  const again = await new ScfgStore().load();
  assert.ok(!("error" in again) && again.warning !== null, JSON.stringify(again));
});

test("setAsideKeepsWhatWasLoaded", async () => {
  const text = JSON.stringify({ version: 1, pending: {}, receipts: { k: { ...RECEIPT, kind: "tip", amount: "1E+1000000" } } });
  const { storage, store } = await env(true, { "save/scfg.rmmzsave": text });
  const got = await store.load();
  assert.ok(!("error" in got) && got.warning === null);
  const aside = await store.setAside("cannot be used (Overflow: ...)");
  assert.equal(aside.kept, true);
  assert.match(aside.message, /save\/scfg\.rmmzsave cannot be used \(Overflow: \.\.\.\); starting with no receipts/);
  assert.equal(storage.read("save/scfg_corrupt_1.rmmzsave"), text);
  assert.equal(storage.has("save/scfg.rmmzsave"), false);
});

// --- MZ's save backup after a crash, and state that cannot be kept aside -------------------------

test("backupLeftByACrashMidSaveIsRecovered", async () => {
  // MZ's saveToLocalFile keeps scfg.rmmzsave_ while it writes the new file; a hard crash in between
  // leaves only the backup, which is renamed back before the state is read.
  const good = JSON.stringify({ version: 1, pending: {}, receipts: { k: RECEIPT } });
  const { storage, store } = await env(true, { "save/scfg.rmmzsave_": good });
  assert.deepEqual(await store.load(), { pending: {}, receipts: { k: RECEIPT }, warning: null }, "read from the backup");
  assert.equal(storage.read("save/scfg.rmmzsave"), good, "renamed back to the state file");
  assert.equal(storage.has("save/scfg.rmmzsave_"), false, "no backup left for the next save to unlink");
  // both there (the crash came after the new file was written): the state file is the newer one
  storage.write("save/scfg.rmmzsave_", JSON.stringify({ version: 1, pending: {}, receipts: {} }));
  assert.deepEqual(await new ScfgStore().load(), { pending: {}, receipts: { k: RECEIPT }, warning: null });
  assert.equal(storage.read("save/scfg.rmmzsave"), good, "the state file is kept");
});

test("backupThatCannotBeRecoveredIsUnreadable", async () => {
  const good = JSON.stringify({ version: 1, pending: {}, receipts: { k: RECEIPT } });
  const { storage, store } = await env(true, { "save/scfg.rmmzsave_": good });
  (globalThis as any).StorageManager.fsRename = () => {
    throw Object.assign(new Error("EPERM: operation not permitted, rename"), { code: "EPERM" });
  };
  const got = await store.load();
  assert.ok("error" in got && got.error === "unreadable", JSON.stringify(got));
  assert.match((got as { detail: string }).detail, /scfg\.rmmzsave_.*EPERM/);
  assert.equal(storage.read("save/scfg.rmmzsave_"), good, "the backup is left where it is");
});

test("noBackupRecoveryOnTheWeb", async () => {
  // localforage writes one key; MZ keeps no backup there
  const good = JSON.stringify({ version: 1, pending: {}, receipts: { k: RECEIPT } });
  const { storage, store } = await env(false, { [`rmmzsave.${DEFAULT_GAME_ID}.scfg_`]: good });
  assert.deepEqual(await store.load(), { pending: {}, receipts: {}, warning: null });
  assert.equal(storage.has(`rmmzsave.${DEFAULT_GAME_ID}.scfg`), false);
});

for (const mode of MODES) {
  test(`corruptStateThatCannotBeKeptAsideIsUnreadable[${mode.name}]`, async () => {
    const bad = "{\"version\": 1, \"pending\": {\"ps_1\": ";
    const { shim, storage, store } = await env(mode.nwjs, { [mode.key("scfg")]: bad });
    shim.failNext("save", Object.assign(new Error("ENOSPC: no space left on device"), { code: "ENOSPC" }));
    const got = await store.load();
    assert.ok("error" in got && got.error === "unreadable", JSON.stringify(got));
    assert.match((got as { detail: string }).detail, /is not valid JSON.*could not be kept aside.*ENOSPC/);
    assert.equal(storage.read(mode.key("scfg")), bad, "left where it is");
    assert.equal(storage.has(mode.key("scfg_corrupt_1")), false);
  });
}

test("setAsideThatCannotKeepACopySaysSo", async () => {
  const text = JSON.stringify({ version: 1, pending: {}, receipts: { k: RECEIPT } });
  const { shim, storage, store } = await env(true, { "save/scfg.rmmzsave": text });
  await store.load();
  shim.failNext("save", new Error("EIO: i/o error, write"));
  const aside = await store.setAside("cannot be used (Overflow: ...)");
  assert.equal(aside.kept, false);
  assert.match(aside.message, /cannot be used \(Overflow: \.\.\.\), and it could not be kept aside \(EIO/);
  assert.equal(storage.read("save/scfg.rmmzsave"), text, "the original is not removed");
});

// --- a truncated or empty state file beside MZ's backup; two games on one origin -----------------

const PENDING = { ps_1: { id: "ps_1", state: "awaiting", created_at: 100.5 } };

test("truncatedStateFileWithABackupReadsTheBackup", async () => {
  // a crash while MZ wrote the file: a truncated state file beside MZ's backup of the last good one
  const good = JSON.stringify({ version: 1, pending: PENDING, receipts: { k: RECEIPT } });
  const truncated = good.slice(0, 40);
  const { storage, store } = await env(true, { "save/scfg.rmmzsave": truncated, "save/scfg.rmmzsave_": good });
  const got = await store.load();
  assert.ok(!("error" in got), JSON.stringify(got));
  assert.deepEqual([got.pending, got.receipts], [PENDING, { k: RECEIPT }], "the backup's receipts and pending payments");
  assert.match(got.warning ?? "", /save\/scfg\.rmmzsave is not valid JSON \(it was kept as save\/scfg_corrupt_1\.rmmzsave\); MZ's backup of it save\/scfg\.rmmzsave_, left when a save was cut short, was read instead/);
  assert.equal(storage.read("save/scfg_corrupt_1.rmmzsave"), truncated, "the truncated file is kept aside");
  assert.equal(storage.read("save/scfg.rmmzsave"), good, "the backup is the state file again");
  assert.equal(storage.has("save/scfg.rmmzsave_"), false, "so no save can unlink it");
  await store.save({ pending: PENDING, receipts: { k: RECEIPT, k2: RECEIPT } });
  assert.deepEqual(Object.keys(JSON.parse(storage.read("save/scfg.rmmzsave")!).receipts), ["k", "k2"], "the next save keeps them");
});

test("truncatedStateFileWithACorruptBackupKeepsBothAside", async () => {
  const { storage, store } = await env(true, { "save/scfg.rmmzsave": "{\"vers", "save/scfg.rmmzsave_": "[1, 2]" });
  const got = await store.load();
  assert.ok(!("error" in got), JSON.stringify(got));
  assert.deepEqual([got.pending, got.receipts], [{}, {}], "starts empty");
  assert.match(got.warning ?? "", /was read instead, but the payment state save\/scfg\.rmmzsave is not a JSON object; starting with no receipts/);
  assert.equal(storage.read("save/scfg_corrupt_1.rmmzsave"), "{\"vers", "the state file is kept aside");
  assert.equal(storage.read("save/scfg_corrupt_2.rmmzsave"), "[1, 2]", "and so is the backup");
  assert.deepEqual([storage.has("save/scfg.rmmzsave"), storage.has("save/scfg.rmmzsave_")], [false, false]);
});

test("truncatedStateFileWithAnUnreadableBackupIsUnreadable", async () => {
  const good = JSON.stringify({ version: 1, pending: {}, receipts: { k: RECEIPT } });
  const { storage, store } = await env(true, { "save/scfg.rmmzsave": "{\"vers", "save/scfg.rmmzsave_": good });
  (globalThis as any).StorageManager.fsReadFile = () => {
    throw Object.assign(new Error("EACCES: permission denied, open 'save/scfg.rmmzsave_'"), { code: "EACCES" });
  };
  const got = await store.load();
  assert.ok("error" in got && got.error === "unreadable", JSON.stringify(got));
  assert.match((got as { detail: string }).detail, /MZ's backup of it save\/scfg\.rmmzsave_ cannot be read: .*EACCES/);
  assert.equal(storage.read("save/scfg.rmmzsave_"), good, "the backup is left alone");
  assert.equal(storage.read("save/scfg_corrupt_1.rmmzsave"), "{\"vers");
});

test("truncatedStateFileWithABackupThatCannotBeMovedBackIsUnreadable", async () => {
  const good = JSON.stringify({ version: 1, pending: {}, receipts: { k: RECEIPT } });
  const { storage, store } = await env(true, { "save/scfg.rmmzsave": "{\"vers", "save/scfg.rmmzsave_": good });
  const sm = (globalThis as any).StorageManager;
  const rename = sm.fsRename.bind(sm);
  sm.fsRename = (from: string, to: string) => {
    if (from === "save/scfg.rmmzsave_") throw Object.assign(new Error("EPERM: operation not permitted, rename"), { code: "EPERM" });
    return rename(from, to);
  };
  const got = await store.load();
  assert.ok("error" in got && got.error === "unreadable", JSON.stringify(got));
  assert.match((got as { detail: string }).detail, /MZ's backup of it save\/scfg\.rmmzsave_ cannot be moved back: .*EPERM/);
  assert.equal(storage.read("save/scfg.rmmzsave_"), good, "the backup is left alone");
});

for (const mzReader of [true, false]) {
  test(`emptyStateFileWithABackupReadsTheBackup[${mzReader ? "mz-reader" : "shim-reader"}]`, async () => {
    // a crash right after MZ created the new file: 0 bytes beside the backup
    const good = JSON.stringify({ version: 1, pending: PENDING, receipts: { k: RECEIPT } });
    const { shim, storage, store } = await env(true, { "save/scfg.rmmzsave": "", "save/scfg.rmmzsave_": good });
    // MZ's NW.js reader rejects an empty file as "Savefile not found"; the shim's reads it as ""
    if (mzReader) shim.failNext("load", new Error("Savefile not found"));
    const got = await store.load();
    assert.ok(!("error" in got), JSON.stringify(got));
    assert.deepEqual([got.pending, got.receipts], [PENDING, { k: RECEIPT }], "the backup's state");
    assert.match(got.warning ?? "", mzReader ? /is empty \(it was kept as/ : /is not valid JSON \(it was kept as/);
    assert.equal(storage.read("save/scfg_corrupt_1.rmmzsave"), "", "the empty file is kept aside");
    assert.equal(storage.read("save/scfg.rmmzsave"), good);
    assert.equal(storage.has("save/scfg.rmmzsave_"), false);
  });
}

test("twoGamesOnOneOriginKeepTheirOwnState", async () => {
  // MZ's web build keeps every game of an origin in that origin's one localforage store (the
  // shim's browser build: its localStorage); the state's key holds the game's $dataSystem
  // advanced.gameId, so two games there never read or overwrite each other's receipts.
  const storage = new MemoryStorage();
  const as = async <T>(gameId: number, fn: (store: ScfgStore) => Promise<T>): Promise<T> => {
    installShim(globalThis, { nwjs: false, storage, gameId });
    await bootShim();
    try {
      return await fn(new ScfgStore());
    } finally {
      resetShim();
    }
  };
  const a = { pending: { ps_1: { id: "ps_1", state: "awaiting" } }, receipts: { "arbitrum:0xab:3": RECEIPT } };
  const tx = "0x" + "cd".repeat(32);
  const b = { pending: {}, receipts: { [`arbitrum:${tx}:3`]: { ...RECEIPT, tx_hash: tx, kind: "tip", item: null } } };
  await as(111, (s) => s.save(a));
  assert.deepEqual(await as(222, (s) => s.load()), { pending: {}, receipts: {}, warning: null }, "the other game starts empty");
  await as(222, (s) => s.save(b));
  assert.deepEqual(await as(111, (s) => s.load()), { ...a, warning: null }, "the first game's state is untouched");
  assert.deepEqual(await as(222, (s) => s.load()), { ...b, warning: null }, "and the second keeps its own");
  assert.deepEqual(storage.keys().filter((k) => k.endsWith(".scfg")).sort(), ["rmmzsave.111.scfg", "rmmzsave.222.scfg"],
    "one key per gameId");
});
