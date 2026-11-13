/**
 * The default sheet host outside a page (Node, or a page without a body): SCFG's default, it shows
 * nothing there, so pay() and restore() reject "sheet.unavailable" instead of hanging, and its
 * notify() and per-frame update() do nothing. Its screens are tested in Chrome (tests/rpgmaker_ui).
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { Scfg } from "../scfg.ts";
import { DefaultSheetHost } from "./default_sheet_host.ts";

test("theDefaultSheetHostIsScfgsDefault", () => {
  const s = new Scfg();
  assert.ok(s.sheetHost instanceof DefaultSheetHost);
  assert.equal(s.sheetHost, s._defaultHost);
  assert.equal(s.css, "", "no stylesheet of the game's by default");
});

test("withoutAPageTheDefaultHostShowsNothing", () => {
  const host = new DefaultSheetHost(new Scfg());
  assert.equal(host.openPayment({} as any), null);
  assert.equal(host.openRestore({} as any), null);
  host.notify("Payment confirmed.");
  host.update();
  host.close({ closed: Promise.resolve("cancel") });
  assert.equal(host.input.active, false, "no screen owns the input");
  assert.deepEqual(host.screens(), []);
});
