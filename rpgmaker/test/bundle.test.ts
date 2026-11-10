/**
 * The built plugin file: reproducible, ES2020 only, MZ header first, the shared checkout page
 * embedded unchanged (Ruling M2-2), and no string-built code in the glue.
 */
import { before, after, test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readdirSync, readFileSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { runInNewContext } from "node:vm";
import { transform } from "esbuild";

const ROOT = dirname(dirname(fileURLToPath(import.meta.url))); // rpgmaker/
const WEB = join(ROOT, "..", "plugin", "game", "scfg", "web");
const PLUGIN = join("js", "plugins", "SCFG.js");

let dirs: string[] = [];

function build(): string {
  const out = mkdtempSync(join(tmpdir(), "scfg-build-"));
  dirs.push(out);
  execFileSync(process.execPath, [join(ROOT, "build.mjs"), "--outdir", out], { cwd: ROOT, stdio: "pipe" });
  return out;
}

function files(dir: string): string[] {
  return readdirSync(dir, { recursive: true, encoding: "utf8" })
    .filter((f) => statSync(join(dir, f)).isFile())
    .sort();
}

let first = "";
let bundle = "";

before(() => {
  first = build();
  bundle = readFileSync(join(first, PLUGIN), "utf8");
});

after(() => {
  for (const d of dirs) rmSync(d, { recursive: true, force: true });
  dirs = [];
});

test("builtTwiceIsIdentical", () => {
  const second = build();
  const names = files(first);
  assert.deepEqual(names, files(second));
  assert.ok(names.includes(PLUGIN) && names.includes(join("test", "shim.js")) && names.includes(join("test", "harness.js"))
    && names.includes(join("test", "ui_driver.js")) && names.includes(join("test", "e2e_config.js")), names.join(", "));
  for (const f of names) assert.ok(readFileSync(join(first, f)).equals(readFileSync(join(second, f))), f);
});

test("bundleIsEs2020", async () => {
  // Nothing in the bundle needs lowering for ES2020: transforming to es2020 changes nothing esnext keeps.
  const es2020 = await transform(bundle, { target: "es2020", loader: "js", charset: "utf8" });
  const esnext = await transform(bundle, { target: "esnext", loader: "js", charset: "utf8" });
  assert.equal(es2020.code, esnext.code);
  assert.ok(!/^\s*(import|export)\s/m.test(bundle), "one IIFE: no import/export");
});

test("bundleStartsWithTheMzHeader", () => {
  assert.ok(bundle.startsWith("/*:\n"), bundle.slice(0, 40));
  const header = bundle.slice(0, bundle.indexOf("*/") + 2);
  assert.match(header, /^ \* @target MZ$/m);
  assert.match(header, /^ \* @plugindesc Stablecoin for Games \(SCFG\): \S/m);
  assert.ok(bundle.indexOf("(() => {") > header.length, "the IIFE follows the header");
});

/**
 * The string literals assigned by `var name = <literal>` in esbuild output: esbuild's text loader
 * emits each file as one (a quoted string or a template literal without substitutions, with
 * escapes such as `<\/script`), so each is evaluated to recover its exact text.
 */
function stringLiterals(code: string): Set<string> {
  const out = new Set<string>();
  for (const m of code.matchAll(/\bvar [\w$]+ = (?=["'`])/g)) {
    const start = m.index! + m[0].length;
    const quote = code[start];
    let i = start + 1;
    while (i < code.length && code[i] !== quote) i += code[i] === "\\" ? 2 : 1;
    const v = runInNewContext(code.slice(start, i + 1));
    if (typeof v === "string") out.add(v);
  }
  return out;
}

test("bundleEmbedsTheSharedPage", () => {
  const literals = stringLiterals(bundle);
  const web = readdirSync(WEB).sort();
  assert.deepEqual(web, ["app.js", "bridge.js", "evm.js", "index.html", "solana.js"]);
  for (const f of web) assert.ok(literals.has(readFileSync(join(WEB, f), "utf8")), `${f} is not embedded verbatim`);
});

test("builtPluginCarriesNoTestCode", () => {
  // the sheet test page's glue (test/harness/ui_driver.ts) is its own bundle; SCFG.js has the default sheet host
  for (const name of ["scfgTest", "FakeChain", "FakeTransport", "ui_driver", "mzShim"]) assert.ok(!bundle.includes(name), name);
  assert.match(bundle, /\bvar DefaultSheetHost = class\b/, "the default sheet host is in the plugin");
});

test("noEvalInGlue", () => {
  const src = join(ROOT, "src", "mz");
  const glue = readdirSync(src, { recursive: true, encoding: "utf8" })
    .filter((f) => f.endsWith(".ts") && !f.endsWith(".test.ts"));
  assert.ok(glue.length > 0);
  for (const f of glue) {
    const text = readFileSync(join(src, f), "utf8");
    assert.ok(!/\beval\s*\(/.test(text), `eval( in ${relative(ROOT, join(src, f))}`);
    assert.ok(!/\bnew\s+Function\b/.test(text), `new Function in ${relative(ROOT, join(src, f))}`);
  }
});

test("bundleRequiresNoNodeModuleStatically", () => {
  // Node's modules are looked up at run time on NW.js only (platform.ts requireNode), never
  // bundled or required at load: SCFG.js also runs in a browser, where there is no require.
  assert.ok(!/\brequire\s*\(\s*["'`]/.test(bundle), "no require(\"...\") with a literal name");
  assert.ok(!/\b__require\b/.test(bundle), "no esbuild require shim (an external Node import)");
  assert.ok(!/["'`]node:[a-z_]+["'`]/.test(bundle), "no node: module specifier");
  for (const name of ["net", "http", "https", "crypto"]) {
    assert.match(bundle, new RegExp(`\\.requireNode\\("${name}"\\)`), `${name} comes through the runtime lookup`);
  }
});
