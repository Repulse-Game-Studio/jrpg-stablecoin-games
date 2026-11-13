import { test } from "node:test";
import assert from "node:assert/strict";
import { bytesToHex } from "./bytes.ts";
import { sha256 } from "./sha256.ts";

const enc = (s: string) => new TextEncoder().encode(s);

test("sha256 NIST vectors", () => {
  assert.equal(bytesToHex(sha256(enc(""))), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert.equal(bytesToHex(sha256(enc("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert.equal(
    bytesToHex(sha256(enc("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
  );
});

test("sha256 matches node:crypto across block boundaries", async () => {
  const { createHash } = await import("node:crypto");
  for (const n of [1, 55, 56, 63, 64, 65, 119, 120, 128, 1000]) {
    const data = new Uint8Array(n).map((_, i) => (i * 7 + n) & 0xff);
    assert.equal(bytesToHex(sha256(data)), createHash("sha256").update(data).digest("hex"), String(n));
  }
});
