import { test } from "node:test";
import assert from "node:assert/strict";
import { eip681TransferUri, qrMatrix, qrRects } from "./paylinks.ts";
import { solanaPayUri } from "./solana/paylinks.ts";
import { asUnits } from "./amounts.ts";
import { ValueError } from "./errors.ts";

test("test_eip681_uri", () => {
  const uri = eip681TransferUri({
    token_address: "0xaf88d065e77c8cC2239327C5EDb3A432268e5831", chain_id: 42161,
    recipient: "0x52908400098527886E0F7030069857D2E4169EE7", units: asUnits(4_993_742n),
  });
  assert.equal(uri, "ethereum:0xaf88d065e77c8cC2239327C5EDb3A432268e5831@42161/transfer"
    + "?address=0x52908400098527886E0F7030069857D2E4169EE7&uint256=4993742");
});

test("test_qr_matrix_is_square_with_finder_patterns", () => {
  const m = qrMatrix("hello");
  const n = m.length;
  assert.equal(n, 21);
  assert.ok(m.every((row) => row.length === n));
  for (let i = 0; i < 7; i++) assert.ok(m[0]![i] && m[i]![0]);
});

test("test_qr_rects_cover_exactly_the_dark_modules", () => {
  const text = "ethereum:0xaf88d065e77c8cC2239327C5EDb3A432268e5831@42161/transfer?uint256=1";
  const m = qrMatrix(text);
  const [size, rects] = qrRects(text, 2);
  assert.equal(size, m.length + 4);
  const painted = new Set<string>();
  for (const [x, y, w] of rects) for (let dx = 0; dx < w; dx++) painted.add(`${x + dx - 2},${y - 2}`);
  const dark = new Set<string>();
  m.forEach((row, y) => row.forEach((v, x) => { if (v) dark.add(`${x},${y}`); }));
  assert.deepEqual(painted, dark);
  assert.ok(rects.length < dark.size);
  assert.equal(qrRects("a")[0], qrMatrix("a").length + 4); // default border is 2
});

test("test_qr_rejects_text_that_does_not_fit_or_encode", () => {
  assert.throws(() => qrMatrix("x".repeat(3000)), ValueError);
  assert.throws(() => qrMatrix("\ud800"), ValueError);
});

const WALLET = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU";
const MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
const REF = "82ZJ7nbGpixjeDCmEhUcmwXYfvurzAgGdtSMuHnUgyny";

test("test_transfer_request_uri", () => {
  const uri = solanaPayUri({
    recipient: WALLET, units: asUnits(4_993_742n), mint: MINT, reference: REF,
    label: "Ruby Dreams", message: "Chapter 2: The Fall & more",
  });
  assert.equal(uri, `solana:${WALLET}?amount=4.993742&spl-token=${MINT}&reference=${REF}`
    + "&label=Ruby%20Dreams&message=Chapter%202%3A%20The%20Fall%20%26%20more");
});

test("test_whole_amount_keeps_six_decimals_and_empty_label_is_omitted", () => {
  const uri = solanaPayUri({ recipient: WALLET, units: asUnits(5_000_000n), mint: MINT, reference: REF, label: "", message: "Tip" });
  assert.ok(uri.includes("amount=5.000000&") && !uri.includes("label="));
  assert.equal(solanaPayUri({ recipient: "R", units: asUnits(0n), mint: "M", reference: "", label: "", message: "" }),
    "solana:R?amount=0.000000&spl-token=M");
});

test("test_solana_pay_percent_encoding_is_utf8_and_uppercase", () => {
  const uri = solanaPayUri({ recipient: "R", units: asUnits(1n), mint: "M", reference: "F", label: "Café 🎮", message: "日本 ~_.-" });
  assert.ok(uri.endsWith("&label=Caf%C3%A9%20%F0%9F%8E%AE&message=%E6%97%A5%E6%9C%AC%20~_.-"));
});
