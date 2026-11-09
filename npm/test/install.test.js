// Mirrors tests/unit/test_installer.py: both installers must behave the same.
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { test, beforeEach, afterEach } = require("node:test");

const cli = require("../lib/install.js");

const SOURCE = path.resolve(__dirname, "..", "..", "plugin", "game");
const VERSION = cli.readVersion(SOURCE);

let tmp;
let project;

beforeEach(() => {
  tmp = fs.mkdtempSync(path.join(os.tmpdir(), "scfg-npm-"));
  project = path.join(tmp, "project");
  fs.mkdirSync(path.join(project, "game"), { recursive: true });
  fs.writeFileSync(path.join(project, "game", "script.rpy"), "label start:\n    return\n");
});

afterEach(() => fs.rmSync(tmp, { recursive: true, force: true }));

function walk(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? [path.join(dir, e.name), ...walk(path.join(dir, e.name))]
                    : [path.join(dir, e.name)]);
}

test("falls back to the repository's plugin from a checkout", () => {
  // npm/plugin/ exists only after `npm pack`; without it the checkout's plugin is used.
  const bundled = path.resolve(__dirname, "..", "plugin", "game");
  const expected = fs.existsSync(path.join(bundled, "scfg")) ? bundled : SOURCE;
  assert.equal(cli.bundledGameDir(), expected);
});

test("install copies both parts", () => {
  const msg = cli.install(project, SOURCE);
  const game = path.join(project, "game");
  assert.ok(fs.statSync(path.join(game, "scfg", "scfg_api.rpy")).isFile());
  assert.ok(fs.statSync(path.join(game, "python-packages", "scfg_core", "__init__.py")).isFile());
  assert.equal(cli.readVersion(game), VERSION);
  assert.ok(msg.includes(VERSION));
  assert.ok(!walk(game).some((p) => /__pycache__|\.rpyc$/.test(p)));
});

test("install accepts the game folder itself", () => {
  cli.install(path.join(project, "game"), SOURCE);
  assert.ok(fs.statSync(path.join(project, "game", "scfg")).isDirectory());
});

test("install refuses a folder that is not a Ren'Py project", () => {
  assert.throws(() => cli.install(tmp, SOURCE), /doesn't look like a Ren'Py project/);
});

test("install refuses when already installed", () => {
  cli.install(project, SOURCE);
  assert.throws(() => cli.install(project, SOURCE), /stablecoin-games-renpy update/);
});

test("update replaces the plugin and drops stale files", () => {
  cli.install(project, SOURCE);
  const game = path.join(project, "game");
  const stale = path.join(game, "scfg", "removed_in_new_version.rpy");
  fs.writeFileSync(stale, "");
  const init = path.join(game, "python-packages", "scfg_core", "__init__.py");
  fs.writeFileSync(init, fs.readFileSync(init, "utf8").replace(VERSION, "0.0.1"));
  const msg = cli.update(project, SOURCE);
  assert.ok(msg.startsWith(`Updated Stablecoin for Games 0.0.1 -> ${VERSION}`));
  assert.ok(!fs.existsSync(stale));
  assert.equal(cli.readVersion(game), VERSION);
});

test("update leaves the developer's files alone", () => {
  const other = path.join(project, "game", "python-packages", "other_lib");
  fs.mkdirSync(other, { recursive: true });
  fs.writeFileSync(path.join(other, "__init__.py"), "");
  cli.update(project, SOURCE);
  assert.ok(fs.existsSync(path.join(other, "__init__.py")));
  assert.ok(fs.existsSync(path.join(project, "game", "script.rpy")));
});

test("update installs when the plugin is missing", () => {
  assert.ok(cli.update(project, SOURCE).startsWith("Installed"));
});

test("main reports errors without a stack trace", () => {
  const errs = [];
  assert.equal(cli.main(["install", tmp], () => {}, (l) => errs.push(l)), 1);
  assert.ok(errs.join("\n").startsWith("stablecoin-games-renpy:"));
});

test("main install, version and usage", () => {
  const out = [];
  assert.equal(cli.main(["install", project], (l) => out.push(l)), 0);
  assert.ok(out.join("\n").includes("Next: add scfg.configure"));
  out.length = 0;
  assert.equal(cli.main(["--version"], (l) => out.push(l)), 0);
  assert.ok(out[0].includes(VERSION));
  assert.equal(cli.main(["frobnicate"], () => {}, () => {}), 2);
});
