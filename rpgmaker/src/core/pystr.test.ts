import { test } from "node:test";
import assert from "node:assert/strict";
import {
  hasOwn, pyFormat1, pyGet, pyInt, pyIntNumber, pyStrip, pyTruthy, toAsciiDigits, urlQuote, utf8Bytes,
} from "./pystr.ts";
import { IndexError, KeyError, ValueError } from "./errors.ts";

test("test_py_strip_matches_str_strip", () => {
  assert.equal(pyStrip("\x1c  x 　\n"), "x");
  assert.equal(pyStrip("﻿x"), "﻿x"); // the BOM is not whitespace in Python
  assert.equal(pyStrip("\x85x "), "x");
});

test("test_py_int", () => {
  assert.equal(pyInt(" 12 "), 12n);
  assert.equal(pyInt("-5"), -5n);
  assert.equal(pyInt("+5"), 5n);
  assert.equal(pyInt("1_000"), 1000n);
  assert.equal(pyInt("１２"), 12n);
  assert.equal(pyInt("1" + "0".repeat(30)), 10n ** 30n);
  for (const bad of ["", "abc", "1.5", "1__0", "_1", "1_", "--1", "0x10", "1e3"]) {
    assert.throws(() => pyInt(bad), ValueError, bad);
  }
  // int() of the other JSON values
  assert.equal(pyInt(5), 5n);
  assert.equal(pyInt(-1.9), -1n);
  assert.equal(pyInt(true), 1n);
  assert.equal(pyInt(2n ** 70n), 2n ** 70n);
  assert.equal(pyIntNumber(" 7 "), 7);
  for (const bad of [null, undefined, [], {}]) assert.throws(() => pyInt(bad), TypeError);
  assert.throws(() => pyInt(Infinity), ValueError);
  assert.throws(() => pyInt(NaN), ValueError);
});

test("test_py_get_and_has_own", () => {
  assert.equal(pyGet({ a: 1 }, "a"), 1);
  assert.equal(pyGet({ a: null }, "a"), null);
  assert.throws(() => pyGet({}, "a"), KeyError);
  assert.throws(() => pyGet({}, "toString"), KeyError); // own keys only
  for (const bad of [null, [], "s", 1]) assert.throws(() => pyGet(bad, "a"), TypeError);
  assert.ok(hasOwn({ a: undefined }, "a") && !hasOwn({}, "constructor"));
});

test("test_py_truthy", () => {
  for (const f of [null, undefined, false, 0, 0n, "", [], {}]) assert.equal(pyTruthy(f), false);
  for (const t of [true, 1, -1, 0.5, NaN, 1n, "0", [0], { a: 0 }]) assert.equal(pyTruthy(t), true);
});

test("test_to_ascii_digits", () => {
  assert.equal(toAsciiDigits("a٠٩b𝟘𝟡"), "a09b09"); // math double-struck digits
});

test("test_url_quote_matches_urllib", () => {
  // urllib.parse.quote(s, safe='') for each input
  assert.equal(urlQuote("a b~_.-/é"), "a%20b~_.-%2F%C3%A9");
  assert.equal(urlQuote("日本"), "%E6%97%A5%E6%9C%AC");
  assert.equal(urlQuote("🎮"), "%F0%9F%8E%AE");
  assert.equal(urlQuote("!*'()[]#%&?=+\t\n"), "%21%2A%27%28%29%5B%5D%23%25%26%3F%3D%2B%09%0A");
  assert.deepEqual(utf8Bytes("€"), [0xe2, 0x82, 0xac]);
  assert.throws(() => urlQuote("\ud800"), ValueError);
});

test("test_py_format1_raises_what_str_format_raises", () => {
  assert.equal(pyFormat1("https://x/tx/{}", "0xab"), "https://x/tx/0xab");
  assert.equal(pyFormat1("{0}{{}}", "a"), "a{}");
  assert.equal(pyFormat1("{!r}", "a"), "'a'");
  assert.throws(() => pyFormat1("{1}", "a"), IndexError);
  assert.throws(() => pyFormat1("{name}", "a"), KeyError);
  assert.throws(() => pyFormat1("{", "a"), ValueError);
  assert.throws(() => pyFormat1("}", "a"), ValueError);
});

// CPython's int() maps non-ASCII whitespace to a space but leaves ASCII
// characters as they are, then skips only ASCII whitespace (Py_ISSPACE). So \x1c-\x1f, which
// str.strip() removes, make int() fail: int("\x1c5") is a ValueError while Decimal("\x1c5") is 5.
// The table is str.isspace() for every code point (CPython 3.9.25, `uv run python`).
const PY_ISSPACE = [
  0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x85, 0xa0, 0x1680, 0x2000, 0x2001, 0x2002, 0x2003,
  0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000,
];

test("test_py_int_whitespace_matches_python_int", () => {
  for (const bad of ["\x1c5", "5\x1f", " \x1d 5", "\x1e٥", "﻿5"]) assert.throws(() => pyInt(bad), ValueError, JSON.stringify(bad));
  assert.equal(pyInt("\x855"), 5n);
  assert.equal(pyInt("　 5 "), 5n);
  for (const cp of PY_ISSPACE) {
    const ws = String.fromCodePoint(cp);
    const intRejects = cp >= 0x1c && cp <= 0x1f; // python3 -c "int(chr(cp) + '5')"
    for (const s of [ws + "5", "5" + ws]) {
      if (intRejects) assert.throws(() => pyInt(s), ValueError, `U+${cp.toString(16)}`);
      else assert.equal(pyInt(s), 5n, `U+${cp.toString(16)}`);
    }
    assert.equal(pyStrip(ws + "5" + ws), "5"); // str.strip() still removes all of them
  }
});

test("test_py_int_and_strip_are_linear_on_whitespace_runs", () => {
  // A hostile RPC answer: a long whitespace run inside a value must not backtrack quadratically.
  const time = (fn: () => void): number => {
    let best = Infinity;
    for (let i = 0; i < 3; i++) {
      const t0 = performance.now();
      fn();
      best = Math.min(best, performance.now() - t0);
    }
    return best;
  };
  const inside = (n: number) => "1" + " ".repeat(n) + "x";
  const intOf = (n: number) => () => assert.throws(() => pyInt(inside(n)), ValueError);
  const stripOf = (n: number) => () => assert.equal(pyStrip(inside(n)), inside(n));
  for (const [name, make] of [["pyInt", intOf], ["pyStrip", stripOf]] as const) {
    const small = time(make(16 * 1024));
    const large = time(make(64 * 1024));
    assert.ok(large < 200, `${name}: 64 KiB took ${large.toFixed(1)} ms`);
    // linear is 4x for 4x the input; quadratic is 16x (a 1 ms floor keeps timer noise out)
    assert.ok(large <= 10 * Math.max(small, 1), `${name}: 16 KiB ${small.toFixed(2)} ms, 64 KiB ${large.toFixed(2)} ms`);
  }
});

test("test_py_int_whitespace_edges_are_unchanged", () => {
  assert.equal(pyInt("\x85\xa0　 12\t\n"), 12n);
  assert.equal(pyInt(" ".repeat(100000) + "7" + " ".repeat(100000)), 7n);
  assert.throws(() => pyInt("\x1c5"), ValueError);
  assert.throws(() => pyInt("5\x1f"), ValueError);
  assert.throws(() => pyInt("   "), ValueError);
  assert.throws(() => pyInt("1 2"), ValueError);
  assert.equal(pyStrip("\x1c\x1d x \x1e\x1f"), "x"); // strip() removes \x1c-\x1f, int() does not
});

test("test_py_int_digit_limit", () => {
  // CPython's int(str) refuses more than 4300 digits (sys.get_int_max_str_digits()): a ValueError, at once
  assert.equal(pyInt("9".repeat(4300)), 10n ** 4300n - 1n);
  assert.equal(pyInt(" " + "1_".repeat(2000) + "1 "), BigInt("1".repeat(2001)));
  for (const text of ["9".repeat(4301), "-" + "9".repeat(4301), "1_".repeat(4300) + "1", "9".repeat(100_000)]) {
    const start = performance.now();
    assert.throws(() => pyInt(text), ValueError, `${text.length} characters`);
    assert.ok(performance.now() - start < 500);
  }
});

// The digits are CPython 3.9's (Unicode 13.0.0, the project's interpreter), not the JS engine's
// \p{Nd}: the sets Unicode added later (Garay, Myanmar Pa-O, Kirat Rai, Tangsa, Kawi, Nag Mundari, ...)
// are not digits for Python 3.9's int() and re's \d (tests/unit/test_digit_tables.py checks the table).
test("test_digits_are_python_39s", () => {
  for (const zero of [0x10d40, 0x116d0, 0x16d70, 0x16ac0, 0x11f50, 0x1e4f0, 0x1ccf0]) {
    const five = String.fromCodePoint(zero + 5);
    assert.equal(toAsciiDigits(five), five, zero.toString(16));
    assert.throws(() => pyInt(five), ValueError, zero.toString(16));
  }
  assert.equal(toAsciiDigits("\u0665\u{1d7ff}\u{1fbf9}\uff10"), "5990");
  assert.equal(pyInt("\u{11d57}"), 7n);
});
