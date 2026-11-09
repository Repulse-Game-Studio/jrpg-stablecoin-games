import { test } from "node:test";
import assert from "node:assert/strict";
import { deepCopy } from "./copy.ts";

test("test_deep_copy_detaches_nested_json", () => {
  const src = { a: { b: [1, { c: "x" }] }, n: null, t: true, big: 5n };
  const out = deepCopy(src);
  assert.deepEqual(out, src);
  (out.a.b[1] as { c: string }).c = "y";
  assert.equal((src.a.b[1] as { c: string }).c, "x");
  assert.notEqual(out.a, src.a);
});

test("test_deep_copy_keeps_key_order_and_own_keys_only", () => {
  const out = deepCopy({ z: 1, a: 2, __proto__x: 3 });
  assert.deepEqual(Object.keys(out), ["z", "a", "__proto__x"]);
});

test("test_deep_copy_refuses_non_json_objects", () => {
  assert.throws(() => deepCopy({ d: new Date() }), TypeError);
  assert.throws(() => deepCopy({ s: new Set([1]) }), TypeError);
  assert.throws(() => deepCopy({ f: () => 1 }), TypeError);
});

test("test_deep_copy_keeps_a_proto_key_as_an_own_property", () => {
  const src = JSON.parse('{"__proto__": 1, "n": {"__proto__": [2]}}'); // JSON.parse makes it an own key
  const out = deepCopy(src);
  assert.deepEqual(Object.keys(out), ["__proto__", "n"]);
  assert.equal(Object.getPrototypeOf(out), Object.prototype);
  assert.equal(Object.getOwnPropertyDescriptor(out, "__proto__")!.value, 1);
  assert.deepEqual(Object.getOwnPropertyDescriptor(out.n, "__proto__")!.value, [2]);
  assert.equal(({} as any).polluted, undefined);
});
