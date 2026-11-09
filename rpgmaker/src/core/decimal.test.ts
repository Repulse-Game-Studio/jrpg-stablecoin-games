import { test } from "node:test";
import assert from "node:assert/strict";
import { Dec } from "./decimal.ts";
import { ValueError } from "./errors.ts";
import { asUnits, unitsToDecimal } from "./amounts.ts";

const d = (s: string): Dec => {
  const v = Dec.parse(s);
  assert.ok(v instanceof Dec, `${s} should be finite`);
  return v;
};

// Every expected string below was produced by CPython's `str(Decimal(...))`.
test("test_dec_str_matches_python_forms", () => {
  const forms: [Dec, string][] = [
    [d("4.99"), "4.99"],
    [unitsToDecimal(asUnits(4990000n)), "4.990000"],
    [unitsToDecimal(asUnits(0n)), "0.000000"],
    [d("1.5").add(d("0")), "1.5"],
    [d("1E+1"), "1E+1"],
    [d("0.0000001"), "1E-7"],
    [d("1_000"), "1000"],
    [d("-0"), "-0"],
    [d("0E-7"), "0E-7"],
    [d("0E+3"), "0E+3"],
    [d("123E5"), "1.23E+7"],
    [d("0.000001"), "0.000001"],
    [d("-0.00"), "-0.00"],
    [d("1.000000").add(d("0.5")), "1.500000"],
    [d("0.00").add(Dec.fromInt(0)), "0.00"],
    [d("-0").add(d("-0")), "-0"],
    [d("1").sub(Dec.fromInt(1)), "0"],
    [unitsToDecimal(asUnits(-5000000n)), "-5.000000"],
    [Dec.fromInt(0).mulInt(-1), "-0"],
    [d("1.10").mulInt(100), "110.00"],
    [d("007"), "7"],
    [d("5."), "5"],
    [d("+.5"), "0.5"],
    [d("1_0.5_5"), "10.55"],
  ];
  for (const [value, text] of forms) assert.equal(String(value), text);
});

test("test_parse_follows_python_grammar", () => {
  assert.equal(String(d(" 5")), "5");
  assert.equal(String(d(" 5\n")), "5");
  assert.equal(String(d("５")), "5"); // fullwidth digit
  assert.equal(String(d("١٢")), "12"); // Arabic-Indic digits
  assert.equal(String(d("1e5_0")), "1E+50");
  assert.equal(String(d("1__0")), "10"); // the C decimal drops every underscore
  assert.equal(String(d("_1")), "1");
  for (const nan of ["nan", "-NaN", "sNaN", "NaN12", "snan7"]) assert.equal(Dec.parse(nan), "nan");
  for (const inf of ["inf", "+Inf", "-Infinity", "INFINITY"]) assert.equal(Dec.parse(inf), "inf");
  for (const bad of ["", " ", ".", "1e", "1E+", "abc", "0x10", "infin", "1.2.3", "--1", "1 2", "e5", "NaN1x", "_"]) {
    assert.throws(() => Dec.parse(bad), ValueError, JSON.stringify(bad));
  }
  assert.throws(() => Dec.parse(5 as any), ValueError);
});

test("test_float_conversion_is_exact_like_python", () => {
  assert.equal(String(Dec.fromNumber(0.1)), "0.1000000000000000055511151231257827021181583404541015625");
  assert.equal(String(Dec.fromNumber(1.5)), "1.5");
  assert.equal(String(Dec.fromNumber(-2.5)), "-2.5");
  assert.equal(String(Dec.fromNumber(4)), "4");
  assert.equal(String(Dec.fromNumber(-7)), "-7");
  assert.throws(() => Dec.fromNumber(NaN), ValueError);
});

test("test_quantize_rounds_half_even_and_keeps_sign", () => {
  const cent = d("0.01");
  const q = (s: string, to = cent) => String(d(s).quantize(to.exp));
  assert.equal(q("-0.001"), "-0.00");
  assert.equal(q("0.005"), "0.00");
  assert.equal(q("0.015"), "0.02");
  assert.equal(q("0.025"), "0.02");
  assert.equal(q("1E+1"), "10.00");
  assert.equal(q("0.999"), "1.00");
  assert.equal(q("1E-30"), "0.00");
  assert.equal(String(d("0.5").quantize(0)), "0");
  assert.equal(String(d("1.5").quantize(0)), "2");
  assert.equal(String(d("2.5").quantize(0)), "2");
  assert.equal(String(d("15").quantize(1)), "2E+1");
  assert.equal(String(d("25").quantize(1)), "2E+1");
  assert.equal(String(d("9".repeat(27) + ".5").quantize(0)), "1000000000000000000000000000");
});

test("test_quantize_is_invalid_operation_when_digits_exceed_precision", () => {
  assert.throws(() => d("1E+30").quantize(-2), ValueError); // 33 digits, Python: InvalidOperation
  assert.throws(() => d("9".repeat(27)).quantize(-2), ValueError);
  assert.equal(String(d("0E+30").quantize(-2)), "0.00");
});

test("test_arithmetic_rounds_to_28_digits_like_the_default_context", () => {
  assert.equal(String(Dec.fromInt(10n ** 28n).add(Dec.fromInt(1))), "1.000000000000000000000000000E+28");
  assert.equal(String(Dec.fromInt(10n ** 28n).add(Dec.fromInt(5))), "1.000000000000000000000000000E+28");
  assert.equal(String(Dec.fromInt(10n ** 28n).add(Dec.fromInt(15))), "1.000000000000000000000000002E+28");
  assert.equal(String(d("9".repeat(28)).add(Dec.fromInt(5))), "1.000000000000000000000000000E+28");
  assert.equal(String(d("0E-100").add(Dec.fromInt(1))), "1.000000000000000000000000000");
  assert.equal(String(Dec.fromInt(2n ** 64n).scaleb(-6)), "18446744073709.551616");
  assert.equal(String(Dec.fromInt(10n ** 30n).scaleb(-6)), "1000000000000000000000000.000");
});

test("test_cmp_compares_by_value", () => {
  assert.equal(d("1.0").cmp(d("1.00")), 0);
  assert.equal(d("-0").cmp(d("0")), 0);
  assert.equal(d("-1").cmp(d("0")), -1);
  assert.equal(d("1E+1").cmp(d("9.99")), 1);
  assert.equal(d("1E-7").cmp(d("0")), 1);
  assert.equal(d("1E+30").cmp(d("1E+29")), 1);
  assert.equal(d("-2").cmp(d("-10")), 1);
  assert.equal(d("4.99").cmp(d("4.980000")), 1);
  assert.ok(d("0.00").isZero() && d("-0E+5").isZero() && !d("1E-9").isZero());
});

test("test_integral_value", () => {
  assert.equal(d("5.000").integralValue(), 5n);
  assert.equal(d("1E+3").integralValue(), 1000n);
  assert.equal(d("-12.0").integralValue(), -12n);
  assert.equal(d("5.5").integralValue(), null);
  assert.equal(d("-0").integralValue(), 0n);
});
