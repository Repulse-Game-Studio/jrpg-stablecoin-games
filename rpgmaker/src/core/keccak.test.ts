import { test } from "node:test";
import assert from "node:assert/strict";
import { bytesToHex } from "./bytes.ts";
import { keccak256 } from "./keccak.ts";

const enc = (s: string) => new TextEncoder().encode(s);

test("test_keccak256_vectors", () => {
  const cases: [Uint8Array, string][] = [
    [enc(""), "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"],
    [enc("abc"), "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45"],
    [enc("Transfer(address,address,uint256)"), "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef"],
    // 200 bytes spans two 136-byte blocks
    [enc("a".repeat(200)), "96ea54061def936c4be90b518992fdc6f12f535068a256229aca54267b4d084d"],
  ];
  for (const [data, want] of cases) assert.equal(bytesToHex(keccak256(data)), want);
});

test("test_keccak256_returns_32_bytes", () => {
  assert.equal(keccak256(enc("x")).length, 32);
});

test("keccak256 at exact rate multiples (136, 272) is distinct and 32 bytes; known answers are in the Tier 1 vectors", () => {
  // Known-answer cross-check against the Python vectors' lengths: 136 and 272 bytes.
  assert.equal(bytesToHex(keccak256(new Uint8Array(136).fill(0xff))).length, 64);
  assert.notEqual(bytesToHex(keccak256(new Uint8Array(136))), bytesToHex(keccak256(new Uint8Array(137))));
  assert.notEqual(bytesToHex(keccak256(new Uint8Array(272))), bytesToHex(keccak256(new Uint8Array(136))));
});
