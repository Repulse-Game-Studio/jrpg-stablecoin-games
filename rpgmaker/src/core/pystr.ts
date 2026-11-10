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

const ND = /^\p{Nd}$/u;

/**
 * Replace every Unicode decimal digit (category Nd) by its ASCII digit, as Python's `int()` and
 * `Decimal()` do. Nd digits come in contiguous runs of ten starting at zero.
 */
export function toAsciiDigits(s: string): string {
  let out = "";
  for (const ch of s) {
    const cp = ch.codePointAt(0)!;
    if (cp < 0x80 || !ND.test(ch)) {
      out += ch;
      continue;
    }
    let start = cp;
    while (ND.test(String.fromCodePoint(start - 1))) start--;
    out += String((cp - start) % 10);
  }
  return out;
}

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
  return BigInt(t.replace(/_/g, "").replace(/^\+/, ""));
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
