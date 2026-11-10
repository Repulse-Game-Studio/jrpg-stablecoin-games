/**
 * The player-facing texts: built-in English, the "Texts" plugin parameter, the script's
 * `SCFG.texts`, and the ids the other engines share.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { CHECKOUT_MESSAGES, RESTORE_MESSAGES } from "../core/index.ts";
import { Scfg } from "./scfg.ts";
import { DEFAULT_TEXTS, resolveText, text } from "./texts.ts";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");
const VECTORS = JSON.parse(readFileSync(join(ROOT, "conformance", "vectors", "messages.json"), "utf8")).messages as Record<string, string>;

const scfg = (testPlay = false) => new Scfg({ platform: { isTestPlay: () => testPlay } });

test("textsFallBackToEnglish", () => {
  assert.equal(DEFAULT_TEXTS["cancel"], "Cancel");
  assert.equal(text("cancel"), "Cancel");
  const s = scfg();
  assert.equal(s.text("cancel"), "Cancel");
  assert.equal(s.text("payment_confirmed_item", "Chapter 2"), "Payment confirmed: Chapter 2. Thank you!");
  assert.equal(s.text("confirming", "Arbitrum", 1, 3), "Confirming on Arbitrum... 1/3");
  assert.equal(s.text("tx_reverted"), "That transaction failed on-chain; no funds were sent.", "core message keys are kept");
  assert.equal(s.text("restore_tx_reverted"), "That transaction failed on-chain; nothing was paid.", "restore messages are prefixed");
  assert.equal(s.text("copy_failed"), "Couldn't copy: your browser blocked the clipboard.");
});

test("textsParamOverridesDefaults", () => {
  const s = scfg();
  s._paramTexts = { cancel: "Abbrechen", tx_reverted: "Fehlgeschlagen." };
  assert.equal(s.text("cancel"), "Abbrechen");
  assert.equal(s.text("tx_reverted"), "Fehlgeschlagen.");
  assert.equal(s.text("close"), "Close", "ids it does not name keep the default");
});

test("scriptOverrideWins", () => {
  const s = scfg();
  s._paramTexts = { cancel: "Abbrechen" };
  s.texts = { cancel: "Annuler" };
  assert.equal(s.text("cancel"), "Annuler");
  s.texts = {};
  assert.equal(s.text("cancel"), "Abbrechen");
  s.texts = { toString: "x", __proto__: { cancel: "inherited" } } as any;
  assert.equal(s.text("cancel"), "Abbrechen", "inherited keys are not overrides");
  assert.equal(resolveText({ script: { cancel: 5 as any } }, "cancel", [], false), "Cancel", "a non-text override is ignored");
});

test("missingIdFailsLoudlyInTestPlay", () => {
  assert.equal(scfg(false).text("no_such_id"), "no_such_id", "a shipped game shows the id");
  assert.throws(() => scfg(true).text("no_such_id"), /no_such_id/);
  assert.equal(scfg(true).text("cancel"), "Cancel", "known ids are fine in test play");
  const s = scfg(true);
  s.texts = { my_own: "Mine %s" };
  assert.equal(s.text("my_own", "x"), "Mine x", "an id the game defines itself is known");
});

test("formatTakesPercentSAndPercentPercent", () => {
  assert.equal(resolveText({}, "tip", [], false), "Tip");
  assert.equal(resolveText({ script: { a: "100%% of %s, %s" } }, "a", ["x"], false), "100% of x, %s");
});

test("defaultsCoverTheCoreMessagesAndTheSharedIds", () => {
  for (const [id, english] of Object.entries(VECTORS)) assert.equal(DEFAULT_TEXTS[id], english, id);
  for (const [id, english] of Object.entries(CHECKOUT_MESSAGES)) assert.equal(DEFAULT_TEXTS[id], english, id);
  for (const [id, english] of Object.entries(RESTORE_MESSAGES)) assert.equal(DEFAULT_TEXTS["restore_" + id], english, id);
  for (const [id, english] of Object.entries(DEFAULT_TEXTS)) {
    assert.match(id, /^[a-z][a-z0-9_]*$/, "ids are snake_case");
    assert.equal(typeof english, "string");
    assert.ok(!/%d/.test(english), `${id}: only %s takes arguments`);
  }
});
