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
