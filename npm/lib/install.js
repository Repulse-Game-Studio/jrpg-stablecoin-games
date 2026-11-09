// Copy the Stablecoin for Games plugin into a Ren'Py project, or update the copy there.
// Same behaviour as installer/scfg_renpy/cli.py, for developers who have Node but not Python.
//
// Ren'Py loads code only from a game's own game/ folder, so the package carries the plugin
// files and this command puts them where Ren'Py looks:
//
//     game/scfg/                         (screens, API, browser checkout page)
//     game/python-packages/scfg_core/    (the engine)

"use strict";

const fs = require("node:fs");
const path = require("node:path");

const DOCS_URL = "https://jrpg.com/stablecoinforgames/docs/";
// Relative to a project's game/ folder.
const PARTS = [path.join("scfg"), path.join("python-packages", "scfg_core")];
const IGNORED = /(^|[\\/])__pycache__([\\/]|$)|\.(pyc|rpyc|rpymc)$/;
const VERSION = /^__version__\s*=\s*["']([^"']+)["']/m;

class InstallError extends Error {}

/** The plugin's game/ folder: inside the package, or in the repository from a checkout. */
function bundledGameDir() {
  const here = path.resolve(__dirname, "..");
  for (const candidate of [path.join(here, "plugin", "game"),
                           path.join(here, "..", "plugin", "game")]) {
    if (isDir(path.join(candidate, "scfg"))) return candidate;
  }
  throw new InstallError("the plugin files are missing from this installation; reinstall stablecoin-games-renpy");
}

function readVersion(gameDir) {
  const init = path.join(gameDir, "python-packages", "scfg_core", "__init__.py");
  if (!isFile(init)) return null;
  const match = VERSION.exec(fs.readFileSync(init, "utf8"));
  return match ? match[1] : null;
}

/** The game/ folder of the Ren'Py project at `p` (the project folder or game/ itself). */
function findGameDir(p) {
  const resolved = path.resolve(p.replace(/^~(?=$|[\\/])/, require("node:os").homedir()));
  if (!isDir(resolved)) throw new InstallError(`${resolved} is not a folder`);
  const game = path.basename(resolved) === "game" ? resolved : path.join(resolved, "game");
  if (isDir(game) && hasRenpyFiles(game)) return game;
  throw new InstallError(
    `${resolved} doesn't look like a Ren'Py project: expected a game/ folder with .rpy files. ` +
    "Pass your project folder, the one that contains game/.");
}

function hasRenpyFiles(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (entry.isFile() && /\.rpyc?$/.test(entry.name)) return true;
    if (entry.isDirectory() && hasRenpyFiles(path.join(dir, entry.name))) return true;
  }
  return false;
}

function isInstalled(gameDir) {
  return PARTS.some((part) => fs.existsSync(path.join(gameDir, part)));
}

function copyPlugin(sourceGame, gameDir) {
  for (const part of PARTS) {
    fs.cpSync(path.join(sourceGame, part), path.join(gameDir, part), {
      recursive: true,
      filter: (src) => !IGNORED.test(src),
    });
  }
}

function removePlugin(gameDir) {
  for (const part of PARTS) {
    fs.rmSync(path.join(gameDir, part), { recursive: true, force: true });
  }
}

function install(project, sourceGame = bundledGameDir()) {
  const gameDir = findGameDir(project);
  if (isInstalled(gameDir)) {
    const current = readVersion(gameDir) || "unknown version";
    throw new InstallError(`the plugin is already in ${gameDir} (${current}). ` +
                           "Use `stablecoin-games-renpy update` to replace it.");
  }
  copyPlugin(sourceGame, gameDir);
  return `Installed Stablecoin for Games ${readVersion(gameDir)} into ${gameDir}`;
}

function update(project, sourceGame = bundledGameDir()) {
  const gameDir = findGameDir(project);
  const old = isInstalled(gameDir) ? readVersion(gameDir) : null;
  // Remove first, so files dropped in the new version don't linger.
  removePlugin(gameDir);
  copyPlugin(sourceGame, gameDir);
  const now = readVersion(gameDir);
  if (old === null) return `Installed Stablecoin for Games ${now} into ${gameDir}`;
  return `Updated Stablecoin for Games ${old} -> ${now} in ${gameDir}`;
}

const USAGE = `usage: stablecoin-games-renpy {install,update} [project]

Add the Stablecoin for Games plugin to a Ren'Py project, or update it.

  install [project]   copy the plugin into a project
  update [project]    replace the plugin in a project with this version

project: your Ren'Py project folder (default: the current folder)`;

/** Runs the command; returns the exit code. `out`/`err` take one line each. */
function main(argv, out = console.log, err = console.error) {
  const [command, project = ".", ...extra] = argv;
  if (command === "--version" || command === "-V") {
    let version = null;
    try { version = readVersion(bundledGameDir()); } catch { /* reported as unknown */ }
    out(`stablecoin-games-renpy ${version || "unknown"}`);
    return 0;
  }
  if (command === "--help" || command === "-h") {
    out(USAGE);
    return 0;
  }
  if ((command !== "install" && command !== "update") || extra.length) {
    err(USAGE);
    return 2;
  }
  try {
    out((command === "install" ? install : update)(project));
  } catch (e) {
    if (!(e instanceof InstallError)) throw e;
    err(`stablecoin-games-renpy: ${e.message}`);
    return 1;
  }
  if (command === "install") out(`Next: add scfg.configure(...) in any .rpy file. Guide: ${DOCS_URL}`);
  return 0;
}

function isDir(p) {
  try { return fs.statSync(p).isDirectory(); } catch { return false; }
}

function isFile(p) {
  try { return fs.statSync(p).isFile(); } catch { return false; }
}

module.exports = { InstallError, bundledGameDir, readVersion, findGameDir, install, update, main };
