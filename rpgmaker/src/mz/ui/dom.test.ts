import { test } from "node:test";
import assert from "node:assert/strict";
import { layoutFor, sheetCss } from "./dom.ts";
import { installDiskAssets } from "../../../test/mz_assets.ts";

test("layoutFollowsRenpysScaleAndSheetWidth", () => {
  // Ren'Py _k(): min(h / 720, w / 960); the sheet is 520 design px docked right, or the whole width when narrow
  assert.deepEqual(layoutFor(1280, 720), { k: 1, sheetWidth: 520 });
  assert.deepEqual(layoutFor(1920, 1080), { k: 1.5, sheetWidth: 780 });
  assert.deepEqual(layoutFor(816, 624), { k: 0.85, sheetWidth: 442 });
  assert.deepEqual(layoutFor(600, 900), { k: 0.625, sheetWidth: 600 });
  assert.deepEqual(layoutFor(0, 0), { k: 1, sheetWidth: 0 });
});

test("sheetCssIsTheMarginStylesheetWithTheAccent", () => {
  installDiskAssets();
  const css = sheetCss();
  for (const token of ["--scfg-accent", "--scfg-on-accent", "--scfg-ink", "--scfg-line", "--scfg-ok", "--scfg-warn", "--scfg-error", "--scfg-dim", "--scfg-k"]) {
    assert.ok(css.includes(token), token);
  }
  assert.ok(css.includes("#fbfcfd") && css.includes("#17202e"), "the Margin palette");
});
