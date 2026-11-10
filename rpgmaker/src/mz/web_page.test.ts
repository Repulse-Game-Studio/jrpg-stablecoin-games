/**
 * WebPage (web_page.ts): the game's side of a web deployment, every call into the page's bridge
 * (plugin/game/scfg/web/bridge.js) behind a small method. Twins of tests/unit/test_web_page.py
 * (the same names) and godot/tests/test_web_page.gd, with a fake `window.scfgBridge` that records
 * the values it is given: in RPG Maker there is no eval boundary, so values cross as values.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { FakeBridge, fakePageWindow, type FakePageWindow } from "../../test/web_fakes.ts";
import { loadAssetsFromDisk } from "../../test/mz_assets.ts";
import type { PageFile } from "./assets.ts";
import { WebPage } from "./web_page.ts";

const HOSTILE = "\"); alert(1); (\"";
const FILES: Record<PageFile, string> = {
  "bridge.js": "/*bridge*/", "evm.js": "/*evm*/", "solana.js": "/*sol*/", "app.js": "/*app*/", "index.html": "<main id=\"x\">\"</main>",
};

function make(opts: { blocked?: boolean; files?: Record<PageFile, string> } = {}) {
  const bridge = new FakeBridge();
  const files = opts.files ?? FILES;
  const win = fakePageWindow(bridge, { bridgeText: (t) => t === files["bridge.js"], blocked: opts.blocked });
  const warnings: string[] = [];
  const page = new WebPage({ window: win, asset: (name) => files[name], warn: (m) => void warnings.push(m) });
  return { bridge, win, page, warnings };
}

const scriptTexts = (win: FakePageWindow) => win.scripts.map((s) => s.textContent);

test("test_install_loads_bridge_first_then_the_page_once", () => {
  const { bridge, win, page, warnings } = make();
  assert.equal(page.installed, false);
  page.install();
  page.install();
  assert.equal(page.installed, true);
  assert.deepEqual(scriptTexts(win), ["/*bridge*/", "/*evm*/", "/*sol*/", "/*app*/"], "bridge.js first, as <script> elements");
  assert.deepEqual(win.scripts.map((s) => s.attributes["data-scfg"]), ["bridge.js", "evm.js", "solana.js", "app.js"]);
  assert.deepEqual(bridge.calls, [["setPage", "<main id=\"x\">\"</main>"]], "then the page, as a string value, once");
  assert.equal(page.available, true);
  assert.deepEqual(warnings, []);
});

test("installReadsTheSharedPageFiles", () => {
  const files = loadAssetsFromDisk().page;
  const { bridge, win, page } = make({ files });
  page.install();
  assert.equal(win.scripts.length, 4);
  assert.ok(win.scripts[0]!.textContent.includes("window.scfgBridge"), "bridge.js first");
  assert.ok(win.scripts[3]!.textContent.length > 100, "app.js");
  const html = bridge.callsTo("setPage")[0]![0] as string;
  assert.match(html, /^<!doctype html>/i, "index.html");
});

test("installWithoutTheBridgeWarnsOnceAndEveryCallIsSafe", () => {
  // a Content-Security-Policy that blocks inline scripts: the scripts are added but never run
  const { win, page, warnings } = make({ blocked: true });
  page.install();
  page.install();
  assert.equal(win.scripts.length, 4);
  assert.equal(page.available, false);
  assert.equal(warnings.length, 1, `one warning (${warnings})`);
  assert.match(warnings[0]!, /browser wallet/);
  assert.equal(page.hasWallet({ family: "evm", wallet_chain: "" }), false);
  assert.doesNotThrow(() => {
    page.open("o1", { title: "T" }, null);
    page.show("o1");
    page.setStatus("o1", { state: "waiting" });
    page.reply("o1", 1, 200, {});
    page.close("o1");
    page.copy("x");
    page.requestPaste();
    page.openUrl("https://example.com/");
  });
  assert.deepEqual(page.take("o1"), []);
  assert.equal(page.takePaste(), null, "no answer");
  assert.equal(page.takeCopyFailed(), false);
  // no page at all (no document): nothing to install, said once
  const bare = new WebPage({ window: {}, asset: (name) => FILES[name], warn: (m) => void warnings.push(m) });
  bare.install();
  bare.install();
  assert.equal(bare.available, false);
  assert.equal(warnings.length, 2);
});

test("installKeepsABridgeThatIsAlreadyThere", () => {
  // another copy (a second SCFG on the page) installed it: the scripts are not run twice
  const { bridge, win, page } = make();
  win.scfgBridge = bridge;
  page.install();
  assert.equal(win.scripts.length, 0);
  assert.deepEqual(bridge.calls, []);
  assert.equal(page.available, true);
});

test("valuesReachThePageAsValues", () => {
  const { bridge, win, page } = make();
  page.install();
  const info = { title: HOSTILE, chain_name: "Arb\nitrum", amount_units: "4990000" };
  page.open("o1", info, "data:image/png;base64,AA");
  assert.deepEqual(bridge.callsTo("openCheckout"), [["o1", info, "data:image/png;base64,AA"]]);
  const [, got] = bridge.callsTo("openCheckout")[0]!;
  assert.equal((got as Record<string, unknown>)["title"], HOSTILE, "the title arrives as the same string");
  page.setStatus("o\")x", { state: HOSTILE });
  page.copy(HOSTILE);
  page.openUrl(HOSTILE);
  page.reply("k", 3, 200, { error: HOSTILE });
  assert.deepEqual(bridge.callsTo("setStatus"), [["o\")x", { state: HOSTILE }]]);
  assert.deepEqual(bridge.callsTo("copy"), [[HOSTILE]]);
  assert.deepEqual(bridge.callsTo("openUrl"), [[HOSTILE]]);
  assert.deepEqual(bridge.callsTo("reply"), [["k", 3, 200, { error: HOSTILE }]]);
  for (const s of win.scripts) assert.ok(!s.textContent.includes(HOSTILE) && !s.textContent.includes("alert(1)"), "no script holds game data");
  assert.equal(win.scripts.length, 4, "no script was added for a call");
});

test("test_has_wallet_asks_for_the_chain_family", () => {
  const { bridge, page } = make();
  page.install();
  bridge.answers["hasWallet"] = [1, 0, true, "1", null, 2];
  const sol = { family: "solana", wallet_chain: "solana:mainnet" };
  const evm = { family: "evm", wallet_chain: "" };
  assert.equal(page.hasWallet(sol), true, "a Solana wallet");
  assert.equal(page.hasWallet(evm), false, "no EVM wallet");
  assert.equal(page.hasWallet(evm), true, "true is a wallet");
  assert.equal(page.hasWallet(evm), false, "a string is no answer");
  assert.equal(page.hasWallet(evm), false, "no value is no wallet");
  assert.equal(page.hasWallet(evm), true, "any other number is one");
  assert.deepEqual(bridge.callsTo("hasWallet")[0], ["solana", "solana:mainnet"], "family and wallet chain");
});

test("test_overlay_calls", () => {
  const { bridge, page } = make();
  page.install();
  bridge.calls = [];
  bridge.answers["take"] = ["[{\"type\": \"tx\"}]"];
  page.open("o1", { title: "T" }, null);
  page.show("o1");
  page.setStatus("o1", { state: "waiting" });
  page.reply("o1", 2, 200, { transaction: "AA" });
  assert.deepEqual(page.take("o1"), [{ type: "tx" }]);
  page.close("o1");
  assert.deepEqual(bridge.calls, [
    ["openCheckout", "o1", { title: "T" }, null],
    ["show", "o1"],
    ["setStatus", "o1", { state: "waiting" }],
    ["reply", "o1", 2, 200, { transaction: "AA" }],
    ["take", "o1"],
    ["closeCheckout", "o1"],
  ]);
});

test("takeParsesTheJsonTextOfMessages", () => {
  const { bridge, page } = make();
  page.install();
  bridge.answers["take"] = ["[{\"type\":\"build\",\"id\":7,\"body\":{\"account\":\"x\"}}]", "not json", "{\"a\":1}", undefined, 5, ["already", "a list"]];
  assert.deepEqual(page.take("o1"), [{ type: "build", id: 7, body: { account: "x" } }]);
  assert.deepEqual(page.take("o1"), [], "garbage is no messages");
  assert.deepEqual(page.take("o1"), [], "an object is no list of messages");
  assert.deepEqual(page.take("o1"), [], "no value is no messages");
  assert.deepEqual(page.take("o1"), [], "a number is no messages");
  assert.deepEqual(page.take("o1"), [], "only JSON text is read");
});

test("test_clipboard_and_links", () => {
  const { bridge, page } = make();
  page.install();
  bridge.calls = [];
  bridge.answers["takePaste"] = ["null", "\"0xab\"", "\"\"", "garbage", "5", undefined];
  page.copy("0xabc");
  page.requestPaste();
  assert.equal(page.takePaste(), null, "not answered yet");
  assert.equal(page.takePaste(), "0xab", "the clipboard text");
  assert.equal(page.takePaste(), "", "the browser refused");
  assert.equal(page.takePaste(), "", "an answer that is not JSON reads as a refusal");
  assert.equal(page.takePaste(), "", "an answer that is not a string reads as a refusal");
  assert.equal(page.takePaste(), null, "no value: not answered");
  page.openUrl("https://arbiscan.io/tx/1");
  assert.deepEqual(bridge.calls[0], ["copy", "0xabc"]);
  assert.deepEqual(bridge.calls[1], ["requestPaste"]);
  assert.deepEqual(bridge.calls[bridge.calls.length - 1], ["openUrl", "https://arbiscan.io/tx/1"]);
});

test("test_take_copy_failed_reads_the_one_shot_flag", () => {
  const { bridge, page } = make();
  page.install();
  bridge.calls = [];
  bridge.answers["takeCopyFailed"] = [1, 0, null, true];
  assert.equal(page.takeCopyFailed(), true);
  assert.equal(page.takeCopyFailed(), false);
  assert.equal(page.takeCopyFailed(), false, "no value: not failed");
  assert.equal(page.takeCopyFailed(), true);
  assert.deepEqual(bridge.calls, [["takeCopyFailed"], ["takeCopyFailed"], ["takeCopyFailed"], ["takeCopyFailed"]]);
});

test("aBridgeThatThrowsIsAnsweredWithNothingAndWarnedOncePerCall", () => {
  const { bridge, page, warnings } = make();
  page.install();
  bridge.throws = new Error("broken");
  const evm = { family: "evm", wallet_chain: "" };
  assert.equal(page.hasWallet(evm), false);
  assert.equal(page.hasWallet(evm), false);
  assert.deepEqual(page.take("o1"), []);
  assert.equal(page.takePaste(), null);
  assert.doesNotThrow(() => page.open("o1", { title: "T" }, null));
  assert.equal(warnings.filter((w) => w.includes("hasWallet")).length, 1, `once per entry point (${warnings})`);
  assert.ok(warnings.some((w) => w.includes("openCheckout") && w.includes("broken")));
});

test("theOverlaysHostIsGuardedWhileItIsOpen", () => {
  // the element bridge.js adds for an opened checkout gets guardOverlay's listeners (its keys and
  // pointer events are the overlay's); close() takes them off again, the window's ones included
  const { bridge, win, page } = make();
  page.install();
  const listening = { host: 0, window: 0 };
  const counter = (k: keyof typeof listening) => ({
    addEventListener: () => void listening[k]++,
    removeEventListener: () => void listening[k]--,
  });
  const view = counter("window");
  const host = { shadowRoot: {}, ownerDocument: { defaultView: view }, ...counter("host") };
  const open = bridge.openCheckout.bind(bridge);
  bridge.openCheckout = (...a: unknown[]) => {
    (win.document.body.children as unknown[]).push(host);
    return open(...a);
  };
  page.open("o1", { title: "T" }, null);
  assert.ok(listening.host > 0 && listening.window > 0, `listening (${JSON.stringify(listening)})`);
  page.open("o1", { title: "T" }, null); // the same key again: bridge.js shows it, no second guard
  const once = { ...listening };
  page.close("o1");
  assert.deepEqual(listening, { host: 0, window: 0 }, `every listener removed (${JSON.stringify(once)})`);
});
