import { test } from "node:test";
import assert from "node:assert/strict";
import { asciiBytes, base64Encode, bytesToHex, hexToBytes } from "./bytes.ts";
import { ValueError } from "./errors.ts";

test("hex round trip, lowercase, optional 0x", () => {
  assert.equal(bytesToHex(new Uint8Array([0, 1, 0xab, 0xff])), "0001abff");
  assert.deepEqual(hexToBytes("0001ABff"), new Uint8Array([0, 1, 0xab, 0xff]));
  assert.deepEqual(hexToBytes("0x0a"), new Uint8Array([10]));
  assert.equal(hexToBytes("").length, 0);
});

test("test_hex_rejects_odd_and_non_hex", () => {
  for (const bad of ["0", "abc", "zz", "0g", "0x0", "a b", "-1"]) {
    assert.throws(() => hexToBytes(bad), ValueError, bad);
  }
});

test("test_base64_encode_is_standard_and_padded", () => {
  // Python base64.b64encode(...).decode("ascii")
  const cases: [number[], string][] = [
    [[], ""], [[0], "AA=="], [[0, 0], "AAA="], [[0, 0, 0], "AAAA"], [[0xff], "/w=="], [[0xfb, 0xff], "+/8="],
    [[...asciiBytes("Man")], "TWFu"], [[...asciiBytes("hello world")], "aGVsbG8gd29ybGQ="],
  ];
  for (const [raw, want] of cases) assert.equal(base64Encode(new Uint8Array(raw)), want);
  const all = new Uint8Array(1000).map((_, i) => (i * 37 + 11) & 0xff);
  for (let n = 0; n <= all.length; n += 7) {
    assert.equal(base64Encode(all.subarray(0, n)), Buffer.from(all.subarray(0, n)).toString("base64"));
  }
});
