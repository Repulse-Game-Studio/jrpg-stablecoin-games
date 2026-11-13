/**
 * Python built-in behaviour shared by the ports: `str.strip()`, `int()`, `d[key]`, truthiness,
 * `urllib.parse.quote`, `str.format`. No Python twin.
 */
import { IndexError, KeyError, ValueError } from "./errors.ts";

/** `Object.hasOwn` (ES2022) for ES2020: Python's `key in d` on a parsed JSON dict. */
export const hasOwn = (o: object, key: string): boolean => Object.prototype.hasOwnProperty.call(o, key);

/** Python's type name of a parsed JSON value, for error messages. */
export const pyType = (v: unknown): string =>
  v === null || v === undefined ? "NoneType" : Array.isArray(v) ? "list" : typeof v === "object" ? "dict"
    : typeof v === "string" ? "str" : typeof v === "boolean" ? "bool" : typeof v === "number" ? (Number.isInteger(v) ? "int" : "float")
    : typeof v === "bigint" ? "int" : typeof v;

/**
 * Python `d[key]` on a parsed JSON value: KeyError for a missing key of a dict, TypeError when
 * `d` is not a dict (None, list, str, number are not indexable by a str key).
 */
export function pyGet(d: unknown, key: string): any {
  if (typeof d !== "object" || d === null || Array.isArray(d)) {
    throw new TypeError(`cannot index ${d === null ? "None" : typeof d} with ${JSON.stringify(key)}`);
  }
  if (!hasOwn(d, key)) throw new KeyError(key);
  return (d as Record<string, unknown>)[key];
}

// Python's str.strip() whitespace set (differs from JS trim at \x1c-\x1f, \x85, and the BOM U+FEFF).
const PY_WS = "\\t\\n\\v\\f\\r\\x1c-\\x20\\x85\\xa0\\u1680\\u2000-\\u200a\\u2028\\u2029\\u202f\\u205f\\u3000";
const PY_WS_CHAR = new RegExp(`^[${PY_WS}]$`);

/**
 * `s` without the leading and trailing characters of `ws`: one scan from each end, so linear
 * (`s.replace(/^[ws]+|[ws]+$/g, "")` retries the end anchor from every start of a whitespace run
 * inside the string: quadratic on a hostile value).
 */
function stripWith(s: string, ws: RegExp): string {
  let start = 0;
  let end = s.length;
  while (start < end && ws.test(s[start]!)) start++;
  while (end > start && ws.test(s[end - 1]!)) end--;
  return s.slice(start, end);
}

/** Exactly Python's `str.strip()` with no argument. */
export const pyStrip = (s: string): string => stripWith(s, PY_WS_CHAR);

// What int() skips around the digits: CPython maps every non-ASCII whitespace character to a space
// but leaves ASCII ones as they are, then skips only ASCII whitespace (Py_ISSPACE), so the four
// separators \x1c-\x1f that str.strip() removes make int() fail: int("\x1c5") is a ValueError.
const INT_WS = "\\t\\n\\v\\f\\r\\x20\\x85\\xa0\\u1680\\u2000-\\u200a\\u2028\\u2029\\u202f\\u205f\\u3000";
const INT_WS_CHAR = new RegExp(`^[${INT_WS}]$`);

/**
 * The zero of every run of ten Unicode decimal digits (Nd) in CPython 3.9's unicodedata (Unicode
 * 13.0.0, the project's interpreter), as pystr.gd _ND_ZEROS. Not the JS engine's `\p{Nd}`: Node 24
 * (Unicode 17) has 11 more digit sets that Python 3.9 refuses, and an older NW.js has fewer.
 */
const ND_ZEROS: readonly number[] = [
  0x30, 0x660, 0x6f0, 0x7c0, 0x966, 0x9e6, 0xa66, 0xae6, 0xb66, 0xbe6, 0xc66, 0xce6, 0xd66,
  0xde6, 0xe50, 0xed0, 0xf20, 0x1040, 0x1090, 0x17e0, 0x1810, 0x1946, 0x19d0, 0x1a80, 0x1a90,
  0x1b50, 0x1bb0, 0x1c40, 0x1c50, 0xa620, 0xa8d0, 0xa900, 0xa9d0, 0xa9f0, 0xaa50, 0xabf0,
  0xff10, 0x104a0, 0x10d30, 0x11066, 0x110f0, 0x11136, 0x111d0, 0x112f0, 0x11450, 0x114d0,
  0x11650, 0x116c0, 0x11730, 0x118e0, 0x11950, 0x11c50, 0x11d50, 0x11da0, 0x16a60, 0x16b50,
  0x1d7ce, 0x1d7d8, 0x1d7e2, 0x1d7ec, 0x1d7f6, 0x1e140, 0x1e2f0, 0x1e950, 0x1fbf0,
];

/**
 * A regex character class (for a `u`-flag RegExp) of exactly those digits: Python 3.9's `\d`.
 * Use it instead of `\p{Nd}`.
 */
export const PY_DIGIT =
  "[" + ND_ZEROS.map((z) => "\\u{" + z.toString(16) + "}-\\u{" + (z + 9).toString(16) + "}").join("") + "]";

/** The value of a Python 3.9 decimal digit (any script) at code point `cp`, or -1. */
export function decimalDigit(cp: number): number {
  if (cp >= 0x30 && cp <= 0x39) return cp - 0x30;
  if (cp < 0x660) return -1;
  for (const z of ND_ZEROS) if (cp >= z && cp < z + 10) return cp - z;
  return -1;
}

/**
 * Replace every Unicode decimal digit (Python 3.9's set, ND_ZEROS) by its ASCII digit, as
 * Python's `int()` and `Decimal()` do. Nd digits come in contiguous runs of ten starting at zero.
 */
export function toAsciiDigits(s: string): string {
  let out = "";
  for (const ch of s) {
    const d = decimalDigit(ch.codePointAt(0)!);
    out += d < 0 ? ch : String(d);
  }
  return out;
}

/** CPython's default `sys.get_int_max_str_digits()`. */
export const INT_MAX_STR_DIGITS = 4300;

/**
 * Python `int(x)` for a JSON-like value, as a bigint (u64 token amounts and uint256 values can
 * exceed 2^53):
 *   - bool -> 0/1, bigint as is, a finite number truncated toward zero (`int(1.9) == 1`);
 *   - a string: surrounding whitespace (int()'s set: str.strip()'s minus \x1c-\x1f), an optional
 *     sign, digits (any script) with single underscores between digits;
 *   - anything else is a TypeError; a malformed string or a non-finite number is a ValueError.
 * Only ValueError and TypeError are thrown.
 */
export function pyInt(x: unknown): bigint {
  if (typeof x === "boolean") return x ? 1n : 0n;
  if (typeof x === "bigint") return x;
  if (typeof x === "number") {
    if (!Number.isFinite(x)) throw new ValueError("cannot convert a non-finite float to integer");
    return BigInt(Math.trunc(x));
  }
  if (typeof x !== "string") throw new TypeError(`int() argument must be a string or a number, not '${pyType(x)}'`);
  const t = toAsciiDigits(stripWith(x, INT_WS_CHAR));
  if (!/^[+-]?[0-9]+(_[0-9]+)*$/.test(t)) throw new ValueError(`invalid literal for int(): ${JSON.stringify(x)}`);
  const body = t.replace(/_/g, "").replace(/^\+/, "");
  // More digits than CPython's int(str) limit is a ValueError.
  if (body.replace(/^-/, "").length > INT_MAX_STR_DIGITS) {
    throw new ValueError(`Exceeds the limit (${INT_MAX_STR_DIGITS} digits) for integer string conversion`);
  }
  return BigInt(body);
}

/**
 * A digit run read out of a node's error message is a number only up to this many significant
 * digits (scfg_core/ints.py DIGIT_RUN_LIMIT): every range or version a request can
 * carry is below 10**18.
 */
export const DIGIT_RUN_LIMIT = 18;

/**
 * Python's `ints.digit_run_value`: a run of Unicode decimal digits (what `PY_DIGIT+` matches, as
 * Python's `\d+`) as a number, or null beyond DIGIT_RUN_LIMIT significant digits. Linear, and
 * it stops at the 19th significant digit. Above 2**53 the number is the nearest float (a port
 * limit: no real range or version gets there).
 */
export function digitRunValue(run: string): number | null {
  const memo = new Map<string, string>();
  let digits = "";
  for (const ch of run) {
    let d = memo.get(ch);
    if (d === undefined) {
      d = toAsciiDigits(ch);
      memo.set(ch, d);
    }
    if (digits === "" && d === "0") continue;
    if (digits.length === DIGIT_RUN_LIMIT) return null;
    digits += d;
  }
  return digits === "" ? 0 : Number(digits);
}

/**
 * Python's `json.loads(text)` as the core calls it (`ints.json_loads`): an integer
 * literal of more than INT_MAX_STR_DIGITS digits makes the text invalid JSON (a SyntaxError),
 * where JSON.parse alone would read it as Infinity. Other numbers are JSON.parse's. Linear.
 */
export function jsonLoads(text: string): unknown {
  if (text.length > INT_MAX_STR_DIGITS && hasOverlongJsonInt(text)) {
    throw new SyntaxError(`a JSON integer has more than ${INT_MAX_STR_DIGITS} digits`);
  }
  return JSON.parse(text);
}

/**
 * True when JSON text holds an integer literal (no fraction, no exponent) of more than
 * INT_MAX_STR_DIGITS digits outside strings. Only meaningful for text JSON.parse accepts.
 */
function hasOverlongJsonInt(text: string): boolean {
  const n = text.length;
  const isDigit = (c: number) => c >= 0x30 && c <= 0x39;
  let i = 0;
  while (i < n) {
    const c = text.charCodeAt(i);
    if (c === 0x22) { // a string: skip to its closing quote, past escapes
      i++;
      while (i < n && text.charCodeAt(i) !== 0x22) i += text.charCodeAt(i) === 0x5c ? 2 : 1;
      i++;
    } else if (isDigit(c)) {
      const start = i;
      while (i < n && isDigit(text.charCodeAt(i))) i++;
      const next = text.charCodeAt(i);
      if (next === 0x2e || next === 0x65 || next === 0x45) { // . e E: a float, skip the rest of it
        i++;
        while (i < n && (isDigit(text.charCodeAt(i)) || "+-eE".includes(text[i]!))) i++;
      } else if (i - start > INT_MAX_STR_DIGITS) {
        return true;
      }
    } else {
      i++;
    }
  }
  return false;
}

/** `pyInt(x)` as a JS number, for values a JSON number holds (slots, blocks, log indexes). */
export const pyIntNumber = (x: unknown): number => Number(pyInt(x));

/** UTF-8 bytes of a string. A lone surrogate is a ValueError, like Python's UnicodeEncodeError. */
export function utf8Bytes(s: string): number[] {
  const out: number[] = [];
  for (const ch of s) {
    const cp = ch.codePointAt(0)!;
    if (cp >= 0xd800 && cp <= 0xdfff) throw new ValueError("surrogates not allowed");
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    else out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
  }
  return out;
}

const UNRESERVED = /^[A-Za-z0-9_.\-~]$/;

/** Python `urllib.parse.quote(s, safe='')`: UTF-8 percent-encoding keeping only `A-Za-z0-9_.-~`. */
export function urlQuote(s: string): string {
  let out = "";
  for (const b of utf8Bytes(s)) {
    const c = String.fromCharCode(b);
    out += b < 0x80 && UNRESERVED.test(c) ? c : "%" + (b < 16 ? "0" : "") + b.toString(16).toUpperCase();
  }
  return out;
}

/** Python truthiness of a JSON-like value (None/False/0/""/[]/{} are falsy). */
export function pyTruthy(v: unknown): boolean {
  if (v === null || v === undefined || v === false || v === 0 || v === 0n || v === "") return false;
  if (typeof v === "number") return true;
  if (Array.isArray(v)) return v.length > 0;
  if (typeof v === "object") return Object.keys(v as object).length > 0;
  return true;
}

/**
 * `template.format(arg)` for ONE positional argument that is a string. Raises what CPython
 * raises: IndexError for `{1}`, KeyError for `{name}`, ValueError for malformed braces.
 * Supported fields: `{}`, `{0}`, `{!r}`/`{!s}`/`{!a}`; attribute access, indexing and format
 * specs are rejected (ValueError), which is stricter than Python.
 */
export function pyFormat1(template: string, arg: string): string {
  let out = "";
  let auto: boolean | null = null; // true: automatic numbering, false: manual
  let nextIndex = 0;
  let i = 0;
  while (i < template.length) {
    const ch = template[i]!;
    if (ch === "}") {
      if (template[i + 1] === "}") { out += "}"; i += 2; continue; }
      throw new ValueError("Single '}' encountered in format string");
    }
    if (ch !== "{") { out += ch; i++; continue; }
    if (template[i + 1] === "{") { out += "{"; i += 2; continue; }
    const close = template.indexOf("}", i + 1);
    if (close < 0) throw new ValueError("expected '}' before end of string");
    const field = template.slice(i + 1, close);
    if (field.includes("{")) throw new ValueError("unsupported nested replacement field");
    const m = /^([^.[!:]*)(?:!([^:]*))?(?::(.*))?$/s.exec(field);
    if (m === null || (m[3] !== undefined && m[3] !== "") || /[.[]/.test(field.split(/[!:]/)[0]!)) {
      throw new ValueError("unsupported replacement field");
    }
    const name = m[1]!;
    if (name === "") {
      if (auto === false) throw new ValueError("cannot switch from manual field specification to automatic field numbering");
      auto = true;
      if (nextIndex++ !== 0) throw new IndexError("Replacement index out of range");
    } else if (/^[0-9]+$/.test(name)) {
      if (auto === true) throw new ValueError("cannot switch from automatic field numbering to manual field specification");
      auto = false;
      if (Number(name) !== 0) throw new IndexError("Replacement index out of range");
    } else {
      throw new KeyError(name);
    }
    if (m[2] !== undefined && !/^[rsa]$/.test(m[2])) throw new ValueError("Unknown conversion specifier");
    out += m[2] === "r" || m[2] === "a" ? `'${arg}'` : arg;
    i = close + 1;
  }
  return out;
}
