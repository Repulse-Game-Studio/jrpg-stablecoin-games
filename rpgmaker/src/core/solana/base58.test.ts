import { test } from "node:test";
import assert from "node:assert/strict";
import { b58decode, b58encode, decodeFixed, normalizePubkey, normalizeSignature } from "./base58.ts";
import { ValueError } from "../errors.ts";

const TOKEN = "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA";
const u8 = (...n: number[]) => new Uint8Array(n);

test("test_known_vectors", () => {
  const cases: [Uint8Array, string][] = [
    [u8(), ""], [u8(0), "1"], [u8(0, 0, 1), "112"], [new Uint8Array(32), "1".repeat(32)],
    [new TextEncoder().encode("hello world"), "StV1DL6CwTryKyV"],
  ];
  for (const [raw, text] of cases) {
    assert.equal(b58encode(raw), text);
    if (text) assert.deepEqual(b58decode(text), raw);
  }
});

test("test_decode_rejects_bad_input", () => {
  for (const bad of ["", "0", "O", "I", "l", "abc def", null, 5]) {
    assert.throws(() => b58decode(bad as any), ValueError, String(bad));
  }
});

test("test_fixed_lengths", () => {
  assert.equal(decodeFixed(TOKEN, 32).length, 32);
  assert.throws(() => decodeFixed(TOKEN, 64), ValueError);
  assert.equal(normalizePubkey("  " + TOKEN + "\n"), TOKEN);
  assert.throws(() => normalizePubkey("112"), ValueError);
  assert.throws(() => normalizePubkey(5 as any), ValueError);
  const sig = b58encode(Uint8Array.from({ length: 64 }, (_, i) => i));
  assert.equal(normalizeSignature(" " + sig), sig);
  assert.throws(() => normalizeSignature(TOKEN), ValueError);
});

test("normalizePubkey strips Python whitespace, not JS-only", () => {
  assert.equal(normalizePubkey("\x1c" + TOKEN + "\u00a0\u3000"), TOKEN);
  assert.throws(() => normalizePubkey("\ufeff" + TOKEN), ValueError);
});
