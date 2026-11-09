import { test } from "node:test";
import assert from "node:assert/strict";
import { D, P, isOnCurve } from "./ed25519.ts";
import { b58decode } from "./base58.ts";

test("curve constants", () => {
  assert.equal(P, 2n ** 255n - 19n);
  assert.equal(D, 37095705934669439343138083508754565189542113879843219016388785533085940283555n);
});

test("test_wrong_length_is_not_on_curve", () => {
  assert.equal(isOnCurve(new Uint8Array(31).fill(1)), false);
  assert.equal(isOnCurve(new Uint8Array(33)), false);
});

test("wallet and program keys", () => {
  assert.equal(isOnCurve(b58decode("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v")), true);
  assert.equal(isOnCurve(new Uint8Array(32)), true); // y = 0: x^2 = -1, a square since p = 1 mod 4
});
