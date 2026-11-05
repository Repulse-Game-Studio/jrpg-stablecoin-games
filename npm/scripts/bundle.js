// prepack: copy the plugin and LICENSE into the package and check the package version matches it.
// scfg_core.__version__ is the single source; the release workflow sets package.json from it.
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const { readVersion } = require("../lib/install.js");

const root = path.resolve(__dirname, "..");
const source = path.join(root, "..", "plugin", "game");
const target = path.join(root, "plugin", "game");
const IGNORED = /(^|[\\/])__pycache__([\\/]|$)|\.(pyc|rpyc|rpymc)$/;

fs.rmSync(path.join(root, "plugin"), { recursive: true, force: true });
for (const part of ["scfg", path.join("python-packages", "scfg_core")]) {
  fs.cpSync(path.join(source, part), path.join(target, part), {
    recursive: true,
    filter: (src) => !IGNORED.test(src),
  });
}

fs.copyFileSync(path.join(root, "..", "LICENSE"), path.join(root, "LICENSE"));

const plugin = readVersion(target);
const pkg = JSON.parse(fs.readFileSync(path.join(root, "package.json"), "utf8")).version;
if (pkg !== plugin) {
  console.error(`package.json version ${pkg} != scfg_core.__version__ ${plugin}. ` +
                `Run: npm pkg set version=${plugin}`);
  process.exit(1);
}
console.log(`bundled plugin ${plugin}`);
