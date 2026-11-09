import { test } from "node:test";
import assert from "node:assert/strict";
import { ConfigError, ValueError } from "../../rpgmaker/src/core/errors.ts";
import { FUNCS, IMPLEMENTED, runCase } from "./tier1.ts";
import { loadTier1 } from "./vectors.ts";

const topics = loadTier1();
test("the vectors directory has Tier 1 topics", () => {
  assert.ok(topics.length >= 10);
});

for (const { topic, cases } of topics) {
  cases.forEach((c, i) => {
    test(`${topic} #${i} ${c.fn}`, { skip: !IMPLEMENTED.has(topic) }, () => runCase(c));
  });
}

test("test_runner_rejects_wrong_error_class", () => {
  FUNCS["__plain"] = () => { throw new Error("boom"); };
  FUNCS["__value"] = () => { throw new ValueError("bad"); };
  FUNCS["__type"] = () => { throw new TypeError("bad"); };
  FUNCS["__config"] = () => { throw new ConfigError("bad", { code: "a.b" }); };
  try {
    assert.throws(() => runCase({ fn: "__plain", in: {}, error: "invalid" }));
    runCase({ fn: "__value", in: {}, error: "invalid" });
    runCase({ fn: "__type", in: {}, error: "invalid" });
    runCase({ fn: "__config", in: {}, error: "a.b" });
    assert.throws(() => runCase({ fn: "__config", in: {}, error: "a.c" }));
    assert.throws(() => runCase({ fn: "__config", in: {}, error: "invalid" }));
    assert.throws(() => runCase({ fn: "__value", in: {}, error: "a.b" }));
  } finally {
    for (const k of ["__plain", "__value", "__type", "__config"]) delete FUNCS[k];
  }
});

test("an unimplemented fn is reported, not passed", () => {
  assert.throws(() => runCase({ fn: "__nope", in: {}, out: 1 }), /no JS adapter/);
});

test("test_every_topic_implemented", () => {
  assert.equal(topics.length, 17);
  assert.deepEqual([...IMPLEMENTED].sort(), topics.map((t) => t.topic).sort());
});
