## Assertion helper for the in-repo test runner (res://tests/run.gd).
##
## Test scripts `const T := preload("res://tests/t.gd")` and call `T.eq(...)` / `T.ok(...)`.
## A failed check is recorded and the test carries on; the runner reports every recorded
## failure. Each `test_*` method must end with `return T.done()`: a method that a script error
## aborted returns null instead, and the runner counts that as a failure.
##
## The same goes for any function: a script error makes it return null and the caller carries
## on. So a null from an aborted call can pass for an expected null (`T.ok(f() == null)`).
## Keep helpers returning values the test checks, and prefer checks that a null cannot satisfy.
extends RefCounted

static var vectors_dir: String = ""
static var failures: Array[String] = []
static var checks: int = 0


static func reset() -> void:
	failures = []
	checks = 0


static func done() -> bool:
	return true


## Strict equality: the Variant types must match too (so 1 never equals 1.0 or "1").
static func eq(a: Variant, b: Variant, msg: String = "") -> bool:
	checks += 1
	if typeof(a) == typeof(b) and a == b:
		return true
	failures.append("%s: expected %s (%s), got %s (%s)" % [
		msg, _show(b), type_string(typeof(b)), _show(a), type_string(typeof(a))])
	return false


static func ok(cond: bool, msg: String = "") -> bool:
	checks += 1
	if not cond:
		failures.append(msg if msg != "" else "condition is false")
	return cond


static func _show(v: Variant) -> String:
	var s := str(v)
	return s if s.length() <= 200 else s.substr(0, 200) + "..."
