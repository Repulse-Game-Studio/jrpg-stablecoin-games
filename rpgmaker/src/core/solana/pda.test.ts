import { test } from "node:test";
import assert from "node:assert/strict";
import { ASSOCIATED_TOKEN_PROGRAM_ID, MAX_SEEDS, MAX_SEED_LEN, associatedTokenAddress, createProgramAddress, findProgramAddress } from "./pda.ts";
import { b58decode, b58encode, decodeFixed } from "./base58.ts";
import { isOnCurve } from "./ed25519.ts";
import { ValueError } from "../errors.ts";

const USDC = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";

test("test_associated_token_address_known", () => {
  assert.equal(associatedTokenAddress("FJaqG9WaVnkX5UFbgXvUiuahgqHT5Fyv7mgyqMXs3Scr", USDC), "8u372zUCNt1A6vX2HJYo5hQ9VH9gamchSKt7kJnaupBU");
  assert.equal(associatedTokenAddress("EJiXmX9t48wXGHTZ2Fb63q9KWMR23yi2BzDs4XREtg3i", USDC), "74u5ULr2EEwtA7zpSUjbWQWK9cT1ByoyXdyQvGGBG7tZ");
});

test("test_find_program_address_known", () => {
  const [addr, bump] = findProgramAddress([new Uint8Array(0)], decodeFixed(ASSOCIATED_TOKEN_PROGRAM_ID, 32));
  assert.equal(b58encode(addr), "DzQr5rR32D2de4ugfqgNmKboRoBK5eid5K7WpRCxeRBY");
  assert.equal(bump, 254);
});

test("test_token_accounts_are_off_curve", () => {
  const ata = associatedTokenAddress("FJaqG9WaVnkX5UFbgXvUiuahgqHT5Fyv7mgyqMXs3Scr", USDC);
  assert.equal(isOnCurve(b58decode(ata)), false);
});

test("test_seed_limits", () => {
  const prog = decodeFixed(ASSOCIATED_TOKEN_PROGRAM_ID, 32);
  assert.equal(MAX_SEEDS, 16);
  assert.equal(MAX_SEED_LEN, 32);
  assert.throws(() => createProgramAddress(Array.from({ length: 17 }, () => new Uint8Array(1)), prog), ValueError);
  assert.throws(() => createProgramAddress([new Uint8Array(33)], prog), ValueError);
  assert.throws(() => findProgramAddress(Array.from({ length: 16 }, () => new Uint8Array(1)), prog), ValueError);
});
