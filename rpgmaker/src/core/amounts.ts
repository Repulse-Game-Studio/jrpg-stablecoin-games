/**
 * Money handling: parsing developer/player amounts and the price|code|nonce codec.
 *
 * On-chain USDC/USDT amounts are integers in base units (6 decimals). The last four
 * decimals of every payment carry an item code (2 digits) and a random nonce
 * (2 digits), so a transaction can be traced back to what it bought:
 *
 *     4.99 | 37 | 42   ->   4.993742 USDC   (units = 4_993_742)
 *
 * Python `int` is `bigint` or an integral `number`; a non-integral `number` stands for a
 * Python `float`.
 *
 * Token amounts in base units are the branded `Units` bigint (decision D16), so the compiler
 * refuses a plain `number` or an unnormalised bigint (a raw 18-decimal Arc log value, a JSON
 * field) where token units are expected. A value becomes `Units` only through `asUnits` (or a
 * function returning `Units`), at the point where it is known to be 6-decimal base units.
 */
import { Dec } from "./decimal.ts";
import { ValueError } from "./errors.ts";
import { pyStrip } from "./pystr.ts";

export const TOKEN_DECIMALS = 6;

/** A token amount in base units (6 decimals). Erased at runtime: it is a plain bigint. */
export type Units = bigint & { readonly __units: unique symbol };

/**
 * Brand a bigint known to be 6-decimal base units. Like Python's `int` it may be negative or
 * huge (`format_units` and `units_to_decimal` accept those); `decode` does the range check.
 * Anything but a bigint is a TypeError, so an untyped caller cannot slip a `number` through.
 */
export function asUnits(v: bigint): Units {
  if (typeof v !== "bigint") throw new TypeError(`token units must be a bigint, got ${typeof v}`);
  return v as Units;
}
export const MAX_AMOUNT: Dec = Dec.fromInt(1000000);
const CENT_EXP = -2;

/**
 * Validate a price/tip amount and return it quantized to cents.
 *
 * Accepts string, integer (`bigint` or integral `number`) or `Dec`. Floats are refused:
 * binary floating point and money do not mix.
 */
export function parseAmount(value: unknown): Dec {
  if (typeof value === "boolean" || (typeof value === "number" && !Number.isInteger(value))) {
    throw new TypeError("amounts must be str, int or Decimal (never float)");
  }
  let d: Dec;
  if (typeof value === "bigint" || typeof value === "number") {
    d = Dec.fromInt(value);
  } else if (value instanceof Dec) {
    d = value;
  } else if (typeof value === "string") {
    let parsed: Dec | "nan" | "inf";
    try {
      parsed = Dec.parse(pyStrip(value));
    } catch (e) {
      if (e instanceof ValueError) throw new ValueError(`not a number: ${JSON.stringify(value)}`);
      throw e;
    }
    if (parsed === "nan" || parsed === "inf") throw new ValueError("amount must be finite");
    d = parsed;
  } else {
    throw new TypeError(`unsupported amount type: ${value === null ? "NoneType" : typeof value}`);
  }
  if (d.cmp(Dec.fromInt(0)) <= 0) throw new ValueError("amount must be positive");
  if (d.cmp(MAX_AMOUNT) > 0) throw new ValueError("amount too large");
  const q = d.quantize(CENT_EXP);
  if (q.cmp(d) !== 0) throw new ValueError("amount must have at most 2 decimals");
  return q;
}

function checkField(name: string, value: unknown): number {
  const n = typeof value === "bigint" ? Number(value) : value;
  if (typeof value === "boolean" || (typeof value !== "bigint" && typeof value !== "number")
    || !Number.isInteger(n) || (n as number) < 0 || (n as number) > 99) {
    throw new ValueError(`${name} must be an int in 0..99`);
  }
  return n as number;
}

/** price (2 decimals) + code (0 = tip, 1..99 = item) + nonce -> base units. */
export function encode(price: unknown, code: unknown, nonce: unknown): Units {
  const p = parseAmount(price);
  const c = checkField("code", code);
  const n = checkField("nonce", nonce);
  return asUnits(p.mulInt(100).integralValue()! * 10000n + BigInt(c * 100 + n));
}

/** Inverse of encode: base units -> (price, code, nonce). */
export function decode(units: Units): [Dec, number, number] {
  if (units < 0n) throw new ValueError("units must be >= 0");
  // Python: (Decimal(units // 10_000) / 100).quantize(_CENT) raises InvalidOperation once the
  // result needs more than 28 significant digits, i.e. from units // 10_000 >= 10**28.
  const cents = units / 10000n;
  if (cents >= 10n ** 28n) throw new ValueError("price needs more than 28 digits");
  const price = new Dec(cents, CENT_EXP, false);
  return [price, Number((units / 100n) % 100n), Number(units % 100n)];
}

export function unitsToDecimal(units: Units): Dec {
  return Dec.fromInt(units).scaleb(-TOKEN_DECIMALS);
}

export function decimalToUnits(amount: Dec): Units {
  const q = amount.scaleb(TOKEN_DECIMALS).integralValue();
  if (q === null) throw new ValueError("more than 6 decimals");
  return asUnits(q);
}

/** Human display of a token amount with all 6 decimals: 4993742 -> '4.993742'. */
export function formatUnits(units: Units): string {
  const mag = units < 0n ? -units : units;
  const s = mag.toString().padStart(TOKEN_DECIMALS + 1, "0");
  const cut = s.length - TOKEN_DECIMALS;
  return (units < 0n ? "-" : "") + s.slice(0, cut) + "." + s.slice(cut);
}
