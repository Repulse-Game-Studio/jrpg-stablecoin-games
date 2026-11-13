/**
 * Text assets the plugin carries inside SCFG.js: the shared browser checkout page
 * (`plugin/game/scfg/web/*`, embedded at build time, never copied, so it cannot drift)
 * and the sheet's stylesheets (`src/mz/ui/*.css`).
 *
 * This module holds no import of the files themselves, so the glue and its node:test tests can
 * load it: under Node's type stripping, importing `app.js` would run it instead of reading it. The
 * bundle's entry (`main.ts`) installs the embedded copy, which `build.mjs` generates as the
 * virtual module "scfg:assets" with esbuild's text loader; tests install the files read from disk
 * (`test/mz_assets.ts`).
 */

/** The shared checkout page's files, as `plugin/game/scfg/web/` holds them (the build checks the list). */
export const PAGE_FILES = ["index.html", "app.js", "evm.js", "solana.js", "bridge.js"] as const;

export type PageFile = (typeof PAGE_FILES)[number];

export interface Assets {
  /** File name -> exact text of each shared checkout page file. */
  readonly page: Readonly<Record<PageFile, string>>;
  /** File name (`sheet.css`) -> text of each `src/mz/ui/*.css` stylesheet. */
  readonly css: Readonly<Record<string, string>>;
}

let installed: Assets | null = null;

/** Make `assets` available to the glue. The page must be complete; every value must be a string. */
export function installAssets(assets: Assets): void {
  for (const name of PAGE_FILES) {
    if (typeof assets.page[name] !== "string") throw new TypeError(`scfg: page asset ${name} is missing`);
  }
  for (const [name, text] of Object.entries(assets.css)) {
    if (typeof text !== "string") throw new TypeError(`scfg: stylesheet ${name} is not text`);
  }
  installed = assets;
}

/** The installed assets; an Error before `installAssets` (a build or test setup mistake). */
export function assets(): Assets {
  if (installed === null) throw new Error("scfg: assets are not installed");
  return installed;
}

/** Text of one checkout page file. */
export const pageAsset = (name: PageFile): string => assets().page[name];
