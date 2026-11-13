import { test } from "node:test";
import assert from "node:assert/strict";
import { qrRects } from "../../core/index.ts";
import { qrSvg } from "./qr.ts";

const URI = "ethereum:0xaf88d065e77c8cC2239327C5EDb3A432268e5831@42161/transfer?uint256=1";

function rectsOf(svg: string): { attrs: Record<string, string> }[] {
  return [...svg.matchAll(/<rect\b([^>]*)\/>/g)].map((m) => ({
    attrs: Object.fromEntries([...m[1]!.matchAll(/([\w-]+)="([^"]*)"/g)].map((a) => [a[1]!, a[2]!])),
  }));
}

test("qrMatchesCoreMatrix", () => {
  const [size, runs] = qrRects(URI, 2);
  const svg = qrSvg(URI);
  assert.match(svg, /^<svg\b[^>]*xmlns="http:\/\/www\.w3\.org\/2000\/svg"/);
  assert.match(svg, new RegExp(`viewBox="0 0 ${size} ${size}"`));
  assert.match(svg, /shape-rendering="crispEdges"/);
  const [bg, ...dark] = rectsOf(svg);
  assert.deepEqual([bg!.attrs["x"], bg!.attrs["y"], bg!.attrs["width"], bg!.attrs["height"], bg!.attrs["fill"]],
    ["0", "0", String(size), String(size), "#ffffff"], "a white quiet-zone background first");
  assert.equal(dark.length, runs.length);
  const painted = new Set<string>();
  for (const r of dark) {
    assert.equal(r.attrs["fill"], "#000000");
    assert.equal(r.attrs["height"], "1");
    const [x, y, w] = [Number(r.attrs["x"]), Number(r.attrs["y"]), Number(r.attrs["width"])];
    for (let dx = 0; dx < w; dx++) painted.add(`${x + dx},${y}`);
  }
  const expected = new Set<string>();
  for (const [x, y, w] of runs) for (let dx = 0; dx < w; dx++) expected.add(`${x + dx},${y}`);
  assert.deepEqual(painted, expected);
  assert.ok(![...painted].some((p) => { const [x, y] = p.split(",").map(Number); return x! < 2 || y! < 2 || x! >= size - 2 || y! >= size - 2; }),
    "the quiet zone of 2 modules stays white");
});

test("qrSvgHasOnlyNumbersAndFixedTextWhateverTheInput", () => {
  const svg = qrSvg('"><script>alert(1)</script>&');
  assert.ok(!/<script|alert/.test(svg), "the encoded text never appears in the markup");
  assert.ok(!svg.includes("\u0000"));
  for (const r of rectsOf(svg)) for (const [k, v] of Object.entries(r.attrs)) {
    if (k !== "fill") assert.match(v, /^\d+$/, k);
  }
});

test("qrSvgRejectsTextThatDoesNotFit", () => {
  assert.throws(() => qrSvg("x".repeat(3000)));
});
