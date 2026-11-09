import { test } from "node:test";
import assert from "node:assert/strict";
import {
  MAX_AMOUNT, TOKEN_DECIMALS, asUnits, decimalToUnits, decode, encode, formatUnits, parseAmount, unitsToDecimal,
  type Units,
} from "./amounts.ts";
import { Dec } from "./decimal.ts";
import { ValueError } from "./errors.ts";
import type { RawTransferLog, TransferLog } from "./evm.ts";
import { eip681TransferUri } from "./paylinks.ts";
import type { Transfer } from "./transfers.ts";

const dec = (s: string): Dec => Dec.parse(s) as Dec;

test("test_constants", () => {
  assert.equal(TOKEN_DECIMALS, 6);
  assert.equal(String(MAX_AMOUNT), "1000000");
});

test("test_parse_amount_accepts", () => {
  const cases: [unknown, string][] = [
    ["4.99", "4.99"], ["3", "3.00"], [" 2.5 ", "2.50"], ["4.990", "4.99"], [dec("1.10"), "1.10"], [7, "7.00"], [7n, "7.00"],
    ["1_000", "1000.00"], [" 5", "5.00"], ["+5", "5.00"], ["1E+1", "10.00"], ["5E-1", "0.50"], ["1000000.00", "1000000.00"],
    ["5\n", "5.00"], ["　5　", "5.00"],
  ];
  for (const [value, want] of cases) assert.equal(String(parseAmount(value)), want, String(value));
});

test("test_parse_amount_rejects_types", () => {
  // a non-integral JS number stands for a Python float; booleans, null and arrays are not amounts
  for (const v of [4.99, true, false, null, undefined, [1], {}]) assert.throws(() => parseAmount(v), TypeError, String(v));
});

test("test_parse_amount_rejects_values", () => {
  for (const v of ["", " ", "abc", "4.999", "0", "-1", "0.00", "NaN", "Infinity", "sNaN", "-Infinity", "1000000.01",
    "0.005", "1e-30", "-0", "1,000", ".", 0, -1, 1_000_001, 0n, 2_000_000n]) {
    assert.throws(() => parseAmount(v), ValueError, String(v));
  }
});

test("test_encode_decode_spec_example", () => {
  assert.equal(encode(dec("4.99"), 37, 42), 4_993_742n);
  assert.equal(encode("3.00", 0, 5), 3_000_005n);
  const [price, code, nonce] = decode(asUnits(4_993_742n));
  assert.deepEqual([String(price), code, nonce], ["4.99", 37, 42]);
  assert.equal(String(decode(asUnits(0n))[0]), "0.00");
  assert.equal(encode("1000000", 99, 99), 1_000_000_009_999n);
});

test("test_encode_rejects_bad_fields", () => {
  for (const [code, nonce] of [[-1, 0], [100, 0], [0, -1], [0, 100], [true, 0], [0, true], ["1", 0], [0, null], [1.5, 0]]) {
    assert.throws(() => encode("1.00", code, nonce), ValueError, `${code} ${nonce}`);
  }
  assert.throws(() => encode("4.999", 1, 1), ValueError);
  assert.throws(() => encode(4.99, 1, 1), TypeError);
  assert.throws(() => decode(asUnits(-1n)), ValueError);
});

test("test_encode_decode_round_trip", () => {
  for (const cents of [1, 99, 100, 499, 1234567, 100_000_000]) {
    for (const [code, nonce] of [[0, 0], [37, 42], [99, 99]]) {
      const price = new Dec(BigInt(cents), -2, false);
      const [p, c, n] = decode(encode(price, code!, nonce!));
      assert.equal(p.cmp(price), 0);
      assert.deepEqual([c, n], [code, nonce]);
    }
  }
});

test("test_units_conversions", () => {
  assert.equal(String(unitsToDecimal(asUnits(4_993_742n))), "4.993742");
  assert.equal(decimalToUnits(dec("4.993742")), 4_993_742n);
  assert.equal(decimalToUnits(dec("5")), 5_000_000n);
  assert.equal(decimalToUnits(dec("1E+1")), 10_000_000n);
  assert.equal(decimalToUnits(dec("-0")), 0n);
  assert.equal(decimalToUnits(dec("0.000001")), 1n);
  assert.equal(decimalToUnits(dec("-1")), -1_000_000n);
  assert.throws(() => decimalToUnits(dec("0.0000001")), ValueError);
});

test("test_format_units_keeps_six_decimals", () => {
  assert.equal(formatUnits(asUnits(4_993_742n)), "4.993742");
  assert.equal(formatUnits(asUnits(5_000_000n)), "5.000000");
  assert.equal(formatUnits(asUnits(0n)), "0.000000");
  assert.equal(formatUnits(asUnits(2n ** 53n)), "9007199254.740992");
  assert.equal(formatUnits(asUnits(-5n)), "-0.000005");
  assert.equal(formatUnits(asUnits(10n ** 18n)), "1000000000000.000000");
});

test("test_decode_boundary_matches_python_decimal_precision", () => {
  // Python: (Decimal(units // 10_000) / 100).quantize(Decimal("0.01")) -> InvalidOperation from 10**28 cents.
  const [price, code, nonce] = decode(asUnits(10n ** 32n - 1n));
  assert.equal(String(price), "99999999999999999999999999.99");
  assert.deepEqual([code, nonce], [99, 99]);
  assert.equal(String(decode(asUnits(10n ** 31n))[0]), "10000000000000000000000000.00");
  assert.throws(() => decode(asUnits(10n ** 32n)), ValueError);
  assert.throws(() => decode(asUnits(10n ** 32n + 12_345n)), ValueError);
  assert.throws(() => decode(asUnits(10n ** 40n)), ValueError);
});

test("test_units_is_a_brand_the_compiler_enforces", () => {
  // Type-level checks: `npm run typecheck` fails if any @ts-expect-error line stops being an error.
  const u: Units = asUnits(4_993_742n);
  const raw: bigint = u; // Units is still a bigint for arithmetic and comparison
  assert.equal(raw, 4_993_742n);
  // @ts-expect-error a plain number is not token units
  formatUnits(4_993_742);
  // @ts-expect-error neither is an unbranded bigint (e.g. a raw 18-decimal Arc log value)
  formatUnits(4_993_742_000_000_000_000n);
  // @ts-expect-error decode and unitsToDecimal take Units too
  decode(1n);
  // @ts-expect-error
  unitsToDecimal(1);
  // @ts-expect-error a Transfer's value must be normalised units
  const t: Pick<Transfer, "value"> = { value: 5n };
  // @ts-expect-error paylinks take Units
  eip681TransferUri({ token_address: "0xT", chain_id: 1, recipient: "0xR", units: 5n });
  // @ts-expect-error a raw log value cannot flow into a TransferLog unbranded
  const log: TransferLog = { ...({} as RawTransferLog) };
  void t; void log;
  // At runtime the brand is erased; asUnits still refuses a non-bigint from untyped callers.
  assert.equal(formatUnits(asUnits(5n)), "0.000005");
  assert.throws(() => asUnits(5 as unknown as bigint), TypeError);
});
