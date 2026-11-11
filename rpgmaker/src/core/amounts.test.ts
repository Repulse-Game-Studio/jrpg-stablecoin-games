import { test } from "node:test";
import assert from "node:assert/strict";
import {
  MAX_AMOUNT, TOKEN_DECIMALS, asUnits, decimalToUnits, decode, encode, formatUnits, parseAmount, storedAmount, unitsToDecimal,
  type Units,
} from "./amounts.ts";
import { Dec } from "./decimal.ts";
import { ArithmeticError, InvalidOperation, ValueError } from "./errors.ts";
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
    [" 5", "5.00"], ["+5", "5.00"], ["1E+1", "10.00"], ["5E-1", "0.50"], ["1000000.00", "1000000.00"],
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
    "0.005", "1e-30", "-0", "1,000", ".", 0, -1, 1_000_001, 0n, 2_000_000n,
    "1_000", "4_99", "1_0.00", "_5", "5_", "1__000"]) {
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
  // exact past Decimal's default 28 significant digits: no rounding (Python builds it from the digits)
  assert.equal(String(unitsToDecimal(asUnits(0n))), "0.000000");
  assert.equal(String(unitsToDecimal(asUnits(10n ** 28n + 1n))), "10000000000000000000000.000001");
  assert.equal(String(unitsToDecimal(asUnits(2n ** 256n - 1n))),
    "115792089237316195423570985008687907853269984665640564039457584007913129.639935");
  assert.equal(String(unitsToDecimal(asUnits(-(10n ** 30n) - 7n))), "-1000000000000000000000000.000007");
});

test("test_decimal_to_units_rejects_amounts_out_of_range", () => {
  // a nonzero amount's adjusted exponent must be within +-40: a ValueError, at once
  for (const x of ["1E+41", "-1E+41", "1E+999993", "1E+999999", "-1E+999995", "1E-41", "9.99E+41"]) {
    const start = performance.now();
    assert.throws(() => decimalToUnits(dec(x)), ValueError, x);
    assert.ok(performance.now() - start < 1000, `${x} is refused quickly`);
  }
  assert.equal(decimalToUnits(dec("1E+40")), 10n ** 46n);
  assert.equal(decimalToUnits(dec("-9.999999E+40")), -9_999_999n * 10n ** 40n);
  assert.equal(decimalToUnits(Dec.parse("0E+999999", { anyExponent: true }) as Dec), 0n);
  assert.equal(decimalToUnits(dec("1E+20")), 10n ** 26n);
});

test("test_stored_amount_reads_numbers_in_range", () => {
  const cases: [unknown, string][] = [["4.990000", "4.990000"], [" 5 ", "5"], [5, "5"], ["0", "0"],
    ["0E-999999", "0E-999999"], ["1E+40", "1E+40"], ["-1.5E-40", "-1.5E-40"], [1.5, "1.5"], [true, "1"]];
  for (const [v, want] of cases) assert.equal(String(storedAmount(v)), want, String(v));
});

test("test_amount_text_is_bounded", () => {
  // AMOUNT_TEXT_LIMIT = 100 characters: longer text is refused before it is parsed
  assert.equal(String(storedAmount("4.99" + "0".repeat(96))), "4.99" + "0".repeat(96));
  assert.equal(String(parseAmount("4.99" + "0".repeat(96))), "4.99");
  for (const text of ["4.99" + "0".repeat(97), "9".repeat(100_000), "0." + "9".repeat(100_000), " ".repeat(99) + "5 "]) {
    const start = performance.now();
    assert.throws(() => storedAmount(text), ValueError);
    assert.throws(() => parseAmount(text), ValueError);
    assert.ok(performance.now() - start < 500);
  }
});

test("test_stored_amount_refuses_lists_and_tuples", () => {
  // Python: Decimal() reads a (sign, digits, exponent) list; a stored amount never is one: TypeError
  for (const v of [[0, [2, 5], -1], [1], []]) assert.throws(() => storedAmount(v), TypeError, JSON.stringify(v));
});

test("test_decimal_to_units_is_exact", () => {
  assert.equal(decimalToUnits(dec("12345678901234567890123456789012")), 12345678901234567890123456789012n * 10n ** 6n);
  assert.equal(decimalToUnits(dec("1.2345678901234567890123456789012345E+30")), 1234567890123456789012345678901234500n);
  assert.equal(decimalToUnits(dec("4.9900000000000000000000000000000000")), 4_990_000n);
  assert.throws(() => decimalToUnits(dec("0.000001000000000000000000000000001")), ValueError, "7th decimal nonzero");
});

test("test_stored_amount_refuses_others", () => {
  for (const v of ["1E+41", "1E-41", "1E+999999", "1E+9999999999999999", "Infinity", "NaN", "sNaN", "abc", "",
    null, undefined, [1], {}, 1e300, Infinity]) {
    const start = performance.now();
    assert.throws(() => storedAmount(v), (e: unknown) => e instanceof ValueError || e instanceof TypeError, String(v));
    assert.ok(performance.now() - start < 1000);
  }
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
  assert.throws(() => decode(asUnits(10n ** 32n)), InvalidOperation);
  assert.throws(() => decode(asUnits(10n ** 32n + 12_345n)), InvalidOperation);
  assert.throws(() => decode(asUnits(10n ** 40n)), InvalidOperation);
});

test("test_decode_past_the_decimal_range_is_invalid_operation", () => {
  // Carried from the M3 review. Python: decimal.InvalidOperation, whose MRO is DecimalException,
  // ArithmeticError, Exception: not a ValueError, so `except ValueError` does not catch it.
  const e = (() => { try { decode(asUnits(10n ** 32n)); } catch (x) { return x; } return null; })();
  assert.ok(e instanceof InvalidOperation && e instanceof ArithmeticError, String(e));
  assert.ok(!(e instanceof ValueError));
  assert.equal(e.name, "InvalidOperation");
  assert.throws(() => decode(asUnits(-1n)), ValueError); // the sign check stays a ValueError
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
