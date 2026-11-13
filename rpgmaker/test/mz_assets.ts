/**
 * The plugin's text assets read from disk, for node:test (the bundle embeds the same files; see
 * src/mz/assets.ts). Not a test file.
 */
import { existsSync, readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { PAGE_FILES, installAssets, type Assets, type PageFile } from "../src/mz/assets.ts";

const ROOT = dirname(dirname(fileURLToPath(import.meta.url))); // rpgmaker/
export const WEB_DIR = join(ROOT, "..", "plugin", "game", "scfg", "web");
export const UI_DIR = join(ROOT, "src", "mz", "ui");

export function loadAssetsFromDisk(): Assets {
  const page = {} as Record<PageFile, string>;
  for (const name of PAGE_FILES) page[name] = readFileSync(join(WEB_DIR, name), "utf8");
  const css: Record<string, string> = {};
  if (existsSync(UI_DIR)) {
    for (const name of readdirSync(UI_DIR).filter((f) => f.endsWith(".css")).sort()) {
      css[name] = readFileSync(join(UI_DIR, name), "utf8");
    }
  }
  return { page, css };
}

/** Install the on-disk assets for the glue under test. */
export function installDiskAssets(): Assets {
  const assets = loadAssetsFromDisk();
  installAssets(assets);
  return assets;
}
