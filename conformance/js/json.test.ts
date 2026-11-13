import { test } from "node:test";
import assert from "node:assert/strict";
import { firstDiff, jsonEqual } from "./json.ts";

test("test_json_equal_rules", () => {
  assert.equal(jsonEqual({ a: 1, b: [1, 2] }, { b: [1, 2], a: 1 }), true);
  assert.equal(jsonEqual(true, 1), false);
  assert.equal(jsonEqual(1, 1.0), true);
  assert.equal(jsonEqual([1, 2], [2, 1]), false);
  assert.equal(jsonEqual(null, undefined), false);
  assert.equal(jsonEqual({ a: 1 }, { a: 1, b: 2 }), false);
  assert.equal(jsonEqual("1", 1), false);
  assert.equal(jsonEqual([], {}), false);
  assert.equal(jsonEqual(null, {}), false);
  assert.equal(firstDiff({ a: { b: [1, 2] } }, { a: { b: [1, 3] } }), "a.b[1]: 2 != 3");
  assert.ok(firstDiff({ a: 1 }, { a: 1 }) === "");
});

test("firstDiff renders bigint instead of throwing", () => {
  assert.equal(firstDiff({ v: 1n }, { v: 2n }), 'v: "1n" != "2n"');
  assert.equal(firstDiff({ v: 1n }, { v: 1n }), "");
});
