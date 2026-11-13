/**
 * `Dec`: a finite Python `decimal.Decimal` under the default context (28 digits, round half
 * even), limited to the operations the payment core uses. `String(dec)` equals Python's
 * `str(Decimal)`. No Python twin.
 *
 * Differences from CPython, all outside any realistic amount: exponents beyond +-2^53 are a
 * ValueError (Python accepts up to +-999999999999999999), `add` refuses operands whose
 * exponents are more than 100000 apart, and the context's Emin/Emax overflow traps are not
 * modelled by `add`, `mulInt` and `quantize`. `addChecked` and `scaleb` are the exceptions: they
 * are Python's context arithmetic exactly (Emax overflow, subnormal rounding at Etiny), for stored,
 * untrusted amounts.
 */
import { Overflow, ValueError } from "./errors.ts";
import { pyStrip, toAsciiDigits } from "./pystr.ts";

const PREC = 28;
const MAX_EXP_GAP = 100000;
// The default context's exponent limits.
const EMAX = 999999;
const EMIN = -999999;
const ETINY = EMIN - PREC + 1; // -1000026
const ETOP = EMAX - PREC + 1; // 999972
// libmpdec's limits for a literal: the exponent, and the adjusted exponent.
const MIN_LITERAL_EXP = -1999999999999999997n;
const MAX_LITERAL_ADJUSTED = 999999999999999999n;
// `parse(text, { anyExponent: true })` clamps exponents to +-2^50: every such value is far outside
// [Etiny, Emax], so comparisons and `addChecked` give Python's results, and exp + digits stays exact.
const EXP_CLAMP = 2 ** 50;

const pow10 = (n: number): bigint => 10n ** BigInt(n);
const digitCount = (n: bigint): number => n.toString().length; // 0n has one digit, like Python

/** Divide by 10^drop rounding half to even. */
function roundDrop(coef: bigint, drop: number): bigint {
  const p = pow10(drop);
  let q = coef / p;
  const r = coef % p;
  const twice = r * 2n;
  if (twice > p || (twice === p && (q & 1n) === 1n)) q += 1n;
  return q;
}

// Python's Decimal(str) grammar, applied after whitespace stripping and underscore removal.
const NUMBER = /^([+-]?)(?:(?:([0-9]+)\.?([0-9]*)|\.([0-9]+))(?:[eE]([+-]?[0-9]+))?|(inf(?:inity)?)|(s?nan[0-9]*))$/i;

export class Dec {
  readonly coef: bigint; // magnitude, >= 0
  readonly exp: number;
  readonly neg: boolean;

  constructor(coef: bigint, exp: number, neg: boolean) {
    this.coef = coef;
    this.exp = exp;
    this.neg = neg;
  }

  /**
   * Python `Decimal(str)`; non-finite values come back as "nan" / "inf" so callers can reject them.
   *
   * `anyExponent`: accept every exponent libmpdec accepts (beyond its limits: ValueError, Python's
   * InvalidOperation) instead of refusing those beyond +-2^53. Exponents beyond +-2^50 are clamped
   * to +-2^50, so comparison, `addChecked` and a small `scaleb` match Python but `toString`,
   * `quantize` and `integralValue` do not: only for values that are compared, summed or scaled.
   */
  static parse(text: string, opts: { anyExponent?: boolean } = {}): Dec | "nan" | "inf" {
    if (typeof text !== "string") throw new ValueError("Decimal() needs a string");
    // CPython's C decimal strips whitespace, maps Unicode digits to ASCII and drops every underscore.
    const m = NUMBER.exec(toAsciiDigits(pyStrip(text)).replace(/_/g, ""));
    if (m === null) throw new ValueError(`invalid decimal literal: ${JSON.stringify(text)}`);
    if (m[7] !== undefined) return "nan";
    if (m[6] !== undefined) return "inf";
    const intPart = m[2] ?? "";
    const frac = m[2] !== undefined ? m[3]! : m[4]!;
    const coef = BigInt(intPart + frac);
    let exp = BigInt(m[5] ?? "0") - BigInt(frac.length);
    if (opts.anyExponent) {
      if (exp < MIN_LITERAL_EXP || exp + BigInt(digitCount(coef)) - 1n > MAX_LITERAL_ADJUSTED) {
        throw new ValueError(`exponent out of range: ${JSON.stringify(text)}`);
      }
      const clamp = BigInt(EXP_CLAMP);
      if (exp > clamp) exp = clamp;
      if (exp < -clamp) exp = -clamp;
    } else if (exp > BigInt(Number.MAX_SAFE_INTEGER) || exp < -BigInt(Number.MAX_SAFE_INTEGER)) {
      throw new ValueError("exponent out of range");
    }
    return new Dec(coef, Number(exp), m[1] === "-");
  }

  static fromInt(n: bigint | number): Dec {
    const v = BigInt(n);
    return new Dec(v < 0n ? -v : v, 0, v < 0n);
  }

  /** Python `Decimal(x)` for an int or float (exact, no rounding). Integral numbers are ints. */
  static fromNumber(n: number): Dec {
    if (!Number.isFinite(n)) throw new ValueError("cannot convert a non-finite float");
    if (Number.isInteger(n)) return Dec.fromInt(BigInt(n));
    const buf = new DataView(new ArrayBuffer(8));
    buf.setFloat64(0, n);
    const bits = buf.getBigUint64(0);
    const neg = (bits >> 63n) === 1n;
    const biased = Number((bits >> 52n) & 0x7ffn);
    let mant = bits & ((1n << 52n) - 1n);
    let e2 = biased === 0 ? -1074 : biased - 1075;
    if (biased !== 0) mant |= 1n << 52n;
    while ((mant & 1n) === 0n) { // reduce to lowest terms (odd numerator over 2^k)
      mant >>= 1n;
      e2 += 1;
    }
    // Not an integer, so e2 < 0 here.
    return new Dec(mant * 5n ** BigInt(-e2), e2, neg);
  }

  isZero(): boolean {
    return this.coef === 0n;
  }

  /** Python `adjusted()`: the exponent of the leading digit (exp + digits - 1; a zero's is its exp). */
  adjusted(): number {
    return this.exp + digitCount(this.coef) - 1;
  }

  /** The value as an integer if it is integral, else null (Python `q == q.to_integral_value()` then `int(q)`). */
  integralValue(): bigint | null {
    if (this.coef === 0n) return 0n; // whatever the exponent (0E+999999): no power of ten to build
    let mag: bigint;
    if (this.exp >= 0) {
      mag = this.coef * pow10(this.exp);
    } else {
      const p = pow10(-this.exp);
      if (this.coef % p !== 0n) return null;
      mag = this.coef / p;
    }
    return this.neg ? -mag : mag;
  }

  /** Round to the default context's 28 digits (Python's `_fix`). */
  private fixed(): Dec {
    let { coef, exp } = this;
    let digits = digitCount(coef);
    while (digits > PREC) {
      coef = roundDrop(coef, digits - PREC);
      exp += digits - PREC;
      digits = digitCount(coef); // a carry (99..9 -> 100..0) can add a digit
    }
    return new Dec(coef, exp, this.neg);
  }

  /** Python `quantize(Decimal(1).scaleb(exp))`: ValueError where Python raises InvalidOperation. */
  quantize(exp: number): Dec {
    const shift = this.exp - exp;
    let coef: bigint;
    if (shift >= 0) {
      if (this.coef !== 0n && digitCount(this.coef) + shift > PREC) throw new ValueError("quantize: result needs more than 28 digits");
      coef = this.coef === 0n ? 0n : this.coef * pow10(shift);
    } else if (-shift > digitCount(this.coef)) {
      coef = 0n; // below a tenth of the last place: rounds to zero
    } else {
      coef = roundDrop(this.coef, -shift);
    }
    if (digitCount(coef) > PREC) throw new ValueError("quantize: result needs more than 28 digits");
    return new Dec(coef, exp, this.neg);
  }

  /**
   * Python `scaleb(n)` for an int `n` under the default context (`_fix`): 28-digit rounding,
   * subnormal rounding at Etiny, and an Overflow (the trapped decimal.Overflow) above Emax.
   */
  scaleb(n: number): Dec {
    return new Dec(this.coef, this.exp + n, this.neg).fixContext();
  }

  add(other: Dec): Dec {
    if (this.coef === 0n || other.coef === 0n) {
      const exp = Math.min(this.exp, other.exp);
      if (this.coef === 0n && other.coef === 0n) return new Dec(0n, exp, this.neg && other.neg);
      const kept = this.coef === 0n ? other : this; // the zero operand only decides the exponent
      const target = Math.max(exp, kept.exp - PREC - 1);
      return new Dec(kept.coef * pow10(kept.exp - target), target, kept.neg).fixed();
    }
    if (Math.abs(this.exp - other.exp) > MAX_EXP_GAP) throw new ValueError("exponents too far apart");
    const exp = Math.min(this.exp, other.exp);
    const a = (this.neg ? -1n : 1n) * this.coef * pow10(this.exp - exp);
    const b = (other.neg ? -1n : 1n) * other.coef * pow10(other.exp - exp);
    const sum = a + b;
    return new Dec(sum < 0n ? -sum : sum, exp, sum < 0n).fixed();
  }

  /**
   * Python `self + other` under the default context, exactly (`_pydecimal.Decimal.__add__`): the
   * operand normalisation (far-apart exponents cost nothing), 28-digit rounding half even,
   * subnormal rounding at Etiny, a zero's exponent clamped into [Etiny, Emax], and an Overflow
   * (Python's trapped decimal.Overflow) for a result above Emax.
   */
  addChecked(other: Dec): Dec {
    const exp = Math.min(this.exp, other.exp);
    if (this.coef === 0n && other.coef === 0n) return new Dec(0n, exp, this.neg && other.neg).fixContext();
    if (this.coef === 0n || other.coef === 0n) {
      const kept = this.coef === 0n ? other : this;
      const target = Math.max(exp, kept.exp - PREC - 1); // <= kept.exp: padding only, at most 29 digits
      return new Dec(kept.coef * pow10(kept.exp - target), target, kept.neg).fixContext();
    }
    // _normalize: an operand entirely below the other's rounding range becomes a sticky digit.
    const [big, lower] = this.exp < other.exp ? [other, this] : [this, other];
    let small = lower;
    const stickyExp = big.exp + Math.min(-1, digitCount(big.coef) - PREC - 2);
    if (digitCount(small.coef) + small.exp - 1 < stickyExp) small = new Dec(1n, stickyExp, small.neg);
    const work = small.exp;
    const a = (big.neg ? -1n : 1n) * big.coef * pow10(big.exp - work);
    const b = (small.neg ? -1n : 1n) * small.coef;
    const sum = a + b;
    if (sum === 0n) return new Dec(0n, exp, false).fixContext(); // ROUND_HALF_EVEN: +0
    return new Dec(sum < 0n ? -sum : sum, work, sum < 0n).fixContext();
  }

  /** `_pydecimal.Decimal._fix` for the default context, raising Overflow as the trapped signal does. */
  private fixContext(): Dec {
    if (this.coef === 0n) return new Dec(0n, Math.min(Math.max(this.exp, ETINY), EMAX), this.neg);
    const digits = digitCount(this.coef);
    let expMin = digits + this.exp - PREC;
    if (expMin > ETOP) throw new Overflow("above Emax");
    if (expMin < ETINY) expMin = ETINY; // subnormal
    if (this.exp >= expMin) return this;
    const keep = digits + this.exp - expMin;
    // keep < 0: the value is below half of the last place and rounds to zero (Python's sticky '1').
    let coef = keep < 0 ? 0n : roundDrop(this.coef, digits - keep);
    if (digitCount(coef) > PREC) { // the rounding carried into a 29th digit
      coef /= 10n;
      expMin += 1;
    }
    if (expMin > ETOP) throw new Overflow("above Emax");
    return new Dec(coef, expMin, this.neg);
  }

  sub(other: Dec): Dec {
    return this.add(new Dec(other.coef, other.exp, !other.neg));
  }

  /** Multiply by an integer (Python `Decimal * int`). */
  mulInt(n: bigint | number): Dec {
    const v = BigInt(n);
    return new Dec(this.coef * (v < 0n ? -v : v), this.exp, this.neg !== (v < 0n)).fixed();
  }

  /** Compare by value: -1, 0 or 1. Zeros of either sign are equal. */
  cmp(other: Dec): -1 | 0 | 1 {
    const sa = this.coef === 0n ? 0 : this.neg ? -1 : 1;
    const sb = other.coef === 0n ? 0 : other.neg ? -1 : 1;
    if (sa !== sb) return sa < sb ? -1 : 1;
    if (sa === 0) return 0;
    const adjA = this.exp + digitCount(this.coef);
    const adjB = other.exp + digitCount(other.coef);
    let mag: number;
    if (adjA !== adjB) {
      mag = adjA > adjB ? 1 : -1;
    } else {
      const exp = Math.min(this.exp, other.exp);
      const a = this.coef * pow10(this.exp - exp);
      const b = other.coef * pow10(other.exp - exp);
      mag = a === b ? 0 : a > b ? 1 : -1;
    }
    return (mag * sa) as -1 | 0 | 1;
  }

  /** Python `str(Decimal)`: plain notation when exp <= 0 and adjusted >= -6, else scientific. */
  toString(): string {
    const digits = this.coef.toString();
    const leftdigits = this.exp + digits.length;
    const dotplace = this.exp <= 0 && leftdigits > -6 ? leftdigits : 1;
    let intpart: string;
    let fracpart: string;
    if (dotplace <= 0) {
      intpart = "0";
      fracpart = "." + "0".repeat(-dotplace) + digits;
    } else if (dotplace >= digits.length) {
      intpart = digits + "0".repeat(dotplace - digits.length);
      fracpart = "";
    } else {
      intpart = digits.slice(0, dotplace);
      fracpart = "." + digits.slice(dotplace);
    }
    const e = leftdigits - dotplace;
    const expPart = e === 0 ? "" : "E" + (e > 0 ? "+" : "-") + Math.abs(e);
    return (this.neg ? "-" : "") + intpart + fracpart + expPart;
  }
}
