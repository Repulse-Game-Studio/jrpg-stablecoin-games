// Builds the RPG Maker MZ plugin with esbuild:
//   <outdir>/js/plugins/SCFG.js   the plugin: the MZ header (src/mz/plugin_header.ts), then one
//                                 unminified ES2020 IIFE of src/mz/main.ts and everything it imports
//   <outdir>/test/shim.js         the MZ shim for the harness page (test/harness/index.html)
//   <outdir>/test/harness.js      the harness page's boot
//   <outdir>/test/ui_driver.js    TEST ONLY: the sheet test page's glue (test/harness/ui_driver.ts:
//                                 the real glue sources with fake chains), never part of SCFG.js
//   <outdir>/test/e2e_config.js   TEST ONLY: the web e2e page's parameter override
//                                 (test/harness/e2e_config.ts, read before the engine loads)
//
//   node build.mjs [--outdir DIR]     (default: dist)
//
// The shared checkout page (../plugin/game/scfg/web/*) and the sheet's stylesheets (src/mz/ui/*.css)
// are embedded as strings with esbuild's text loader (never copied, so they cannot drift), through
// the virtual module "scfg:assets" generated below. The output depends only on the sources: two
// builds are byte-identical.
import { build } from "esbuild";
import { existsSync, readdirSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { PAGE_FILES } from "./src/mz/assets.ts";
import { renderHeader } from "./src/mz/plugin_header.ts";

const ROOT = dirname(fileURLToPath(import.meta.url));
const WEB_DIR = resolve(ROOT, "..", "plugin", "game", "scfg", "web");
const UI_DIR = join(ROOT, "src", "mz", "ui");

/** A path for an import in the generated module, relative to ROOT, with forward slashes. */
const importPath = (file) => {
  const rel = relative(ROOT, file).split("\\").join("/");
  return rel.startsWith(".") ? rel : "./" + rel;
};

/** The virtual module "scfg:assets": { page: { "index.html": "...", ... }, css: { "sheet.css": "..." } }. */
const assetsPlugin = {
  name: "scfg-assets",
  setup(b) {
    b.onResolve({ filter: /^scfg:assets$/ }, () => ({ path: "scfg:assets", namespace: "scfg-assets" }));
    b.onLoad({ filter: /.*/, namespace: "scfg-assets" }, () => {
      const onDisk = readdirSync(WEB_DIR).sort();
      const expected = [...PAGE_FILES].sort();
      if (JSON.stringify(onDisk) !== JSON.stringify(expected)) {
        throw new Error(`plugin/game/scfg/web holds ${onDisk.join(", ")}; src/mz/assets.ts PAGE_FILES lists ${expected.join(", ")}`);
      }
      const css = existsSync(UI_DIR) ? readdirSync(UI_DIR).filter((f) => f.endsWith(".css")).sort() : [];
      const lines = [];
      const page = [];
      const sheets = [];
      PAGE_FILES.forEach((name, i) => {
        lines.push(`import page${i} from ${JSON.stringify(importPath(join(WEB_DIR, name)))} with { type: "text" };`);
        page.push(`${JSON.stringify(name)}: page${i}`);
      });
      css.forEach((name, i) => {
        lines.push(`import css${i} from ${JSON.stringify(importPath(join(UI_DIR, name)))} with { type: "text" };`);
        sheets.push(`${JSON.stringify(name)}: css${i}`);
      });
      lines.push(`export default { page: { ${page.join(", ")} }, css: { ${sheets.join(", ")} } };`);
      return {
        contents: lines.join("\n") + "\n", loader: "js", resolveDir: ROOT,
        watchDirs: [WEB_DIR, ...(existsSync(UI_DIR) ? [UI_DIR] : [])],
      };
    });
  },
};

const COMMON = {
  absWorkingDir: ROOT,
  bundle: true,
  format: "iife",
  platform: "browser",
  target: "es2020",
  minify: false,
  charset: "utf8",
  legalComments: "inline",
  sourcemap: false,
  logLevel: "warning",
  // Class fields with [[Define]] semantics, as Node runs the sources in the tests (tsconfig's
  // ES2020 target would otherwise make esbuild assign them).
  tsconfigRaw: { compilerOptions: { useDefineForClassFields: true, verbatimModuleSyntax: true } },
};

export async function buildAll(outdir) {
  await Promise.all([
    build({
      ...COMMON,
      entryPoints: [join(ROOT, "src", "mz", "main.ts")],
      outfile: join(outdir, "js", "plugins", "SCFG.js"),
      banner: { js: renderHeader() },
      plugins: [assetsPlugin],
    }),
    build({ ...COMMON, entryPoints: [join(ROOT, "test", "mz_shim", "browser.ts")], outfile: join(outdir, "test", "shim.js") }),
    build({ ...COMMON, entryPoints: [join(ROOT, "test", "harness", "harness.ts")], outfile: join(outdir, "test", "harness.js") }),
    build({ ...COMMON, entryPoints: [join(ROOT, "test", "harness", "e2e_config.ts")], outfile: join(outdir, "test", "e2e_config.js") }),
    build({
      ...COMMON,
      entryPoints: [join(ROOT, "test", "harness", "ui_driver.ts")],
      outfile: join(outdir, "test", "ui_driver.js"),
      plugins: [assetsPlugin],
    }),
  ]);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const { values } = parseArgs({ options: { outdir: { type: "string", default: join(ROOT, "dist") } } });
  await buildAll(resolve(values.outdir));
}
