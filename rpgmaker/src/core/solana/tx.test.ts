import { test } from "node:test";
import assert from "node:assert/strict";
import { bytesToHex, hexToBytes } from "../bytes.ts";
import { ValueError } from "../errors.ts";
import {
  COMPUTE_BUDGET_PROGRAM_ID, compactU16, compileMessage, setComputeUnitLimit, setComputeUnitPrice,
  transferChecked, unsignedTransaction,
} from "./tx.ts";

test("test_compact_u16", () => {
  const cases: [number, string][] = [[0, "00"], [127, "7f"], [128, "8001"], [16383, "ff7f"], [16384, "808001"], [65535, "ffff03"]];
  for (const [n, hex] of cases) assert.equal(bytesToHex(compactU16(n)), hex);
  for (const bad of [-1, 65536]) assert.throws(() => compactU16(bad), ValueError);
  assert.throws(() => compactU16("1" as any), TypeError);
});

test("test_instruction_data", () => {
  assert.equal(COMPUTE_BUDGET_PROGRAM_ID, "ComputeBudget111111111111111111111111111111");
  assert.equal(bytesToHex(setComputeUnitLimit(30_000).data), "02" + "30750000");
  assert.equal(bytesToHex(setComputeUnitPrice(5_000).data), "03" + "8813000000000000");
  assert.equal(bytesToHex(setComputeUnitPrice(2n ** 64n - 1n).data), "03" + "ff".repeat(8));
  const k = [1, 2, 3, 4, 5].map((n) => "1".repeat(n));
  const ix = transferChecked({ source: k[0]!, mint: k[1]!, destination: k[2]!, owner: k[3]!, amount: 4_993_742n, decimals: 6, extra: [k[4]!] });
  assert.equal(bytesToHex(ix.data), "0c" + "ce324c0000000000" + "06");
  assert.deepEqual(ix.accounts.map((a) => [a.pubkey, a.is_signer, a.is_writable]), [
    [k[0], false, true], [k[1], false, false], [k[2], false, true], [k[3], true, false], [k[4], false, false],
  ]);
});

test("test_payment_message_known_answer", () => {
  const msgHex = "010002059f74edcb8da724445d08aaf5946968c16ac8a030a9e45dad96d832f73694f9842069361b5b957cf9989b7dfed2144dcb9bf9e48cc819e2db2815e3daaec92ddca1fabe9873560832abe0cf5ee51e911ae811e7f6d51fa5aaff0a9677a63c7fc506ddf6e1d765a193d9cbe146ceeb79ac1cb485ed5f5b37913a8cf5857eff00a9c6fa7af3bedbad3a3d65f36aabc97431b1bbe4c2d2f6e0e47ca60203452f5d61bd8915ffd31fbae911393b663b0f8aa378232689dd9bbf2c043818b7492a503f010304020401000a0c30244c000000000006";
  const m = compileMessage("BjTEiQC3P1mUEAYu8Nxn7RS4A1qY5X2Pz2n38bbmAcUK", [{
    program_id: "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA",
    accounts: [
      { pubkey: "BuJQ9S87hR4RMzBQNrXKgpu8sEdFjSQ3FPMZ5S88xA9A", is_signer: false, is_writable: true },
      { pubkey: "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v", is_signer: false, is_writable: false },
      { pubkey: "3BX6ZDcbmdhcextrAHrv9fhQomQxJsNfVrz5pRsG2bAs", is_signer: false, is_writable: true },
      { pubkey: "BjTEiQC3P1mUEAYu8Nxn7RS4A1qY5X2Pz2n38bbmAcUK", is_signer: true, is_writable: false },
    ],
    data: hexToBytes("0c30244c000000000006"),
  }], "DksJ5Gd5ZW3L3eEAnayCX6hYodyRbxcyZT1LLmjcv34J");
  assert.equal(bytesToHex(m), msgHex);
  const t = unsignedTransaction(m);
  assert.equal(bytesToHex(t), "01" + "00".repeat(64) + msgHex);
});
