import { test } from "node:test";
import assert from "node:assert/strict";
import { ConfigError, IndexError, KeyError, ScfgError, ValueError, isScfgError } from "./errors.ts";

test("test_config_error_carries_code", () => {
  const e = new ConfigError("x", { code: "a.b" });
  assert.equal(e.message, "x");
  assert.equal(e.code, "a.b");
  assert.equal(e.name, "ConfigError");
  assert.ok(e instanceof ScfgError);
  assert.ok(isScfgError(e));
  assert.ok(e instanceof Error);
  // Python: class ConfigError(ScfgError, ValueError)
  assert.ok(e instanceof ValueError);
});

test("test_scfg_error_is_not_a_value_error", () => {
  const e = new ScfgError("x");
  assert.ok(e instanceof ScfgError && isScfgError(e));
  assert.ok(!(e instanceof ValueError));
  assert.ok(!(e instanceof ConfigError));
  assert.ok(!(new ValueError("v") instanceof ScfgError));
  assert.ok(!isScfgError(new Error("e")) && !isScfgError(null) && !isScfgError("ScfgError"));
});

test("test_subclasses_of_scfg_error_keep_their_own_instanceof", () => {
  class Sub extends ScfgError {}
  assert.ok(new Sub("s") instanceof Sub);
  assert.ok(new Sub("s") instanceof ScfgError);
  assert.equal(new Sub("s").name, "Sub");
  assert.ok(!(new ScfgError("x") instanceof Sub));
  assert.ok(!(new ConfigError("c", { code: "a.b" }) instanceof Sub));
});

test("test_python_builtin_errors", () => {
  const k = new KeyError("value");
  assert.ok(k instanceof KeyError && k instanceof Error && !(k instanceof ValueError));
  assert.equal(k.name, "KeyError");
  assert.equal(k.message, '"value"');
  assert.equal(new KeyError(0).message, "0");
  const i = new IndexError();
  assert.ok(i instanceof IndexError && i.name === "IndexError");
  assert.equal(i.message, "list index out of range");
});
