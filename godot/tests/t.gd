## Assertion helper for the in-repo test runner (res://tests/run.gd).
##
## Test scripts `const T := preload("res://tests/t.gd")` and call `T.eq(...)` / `T.ok(...)` /
## `T.json_eq(...)` / `T.is_err(...)`. A failed check is recorded and the test carries on; the
## runner reports every recorded failure. Each `test_*` method must end with `return T.done()`:
## a method that a script error aborted returns null instead, and the runner counts that as a
## failure. A test method may be a coroutine (`await` inside it); the runner awaits it.
##
## `T.skip(reason)` records work that was deliberately not done. The runner prints one
## `SKIP <test>: <reason>` line per call and counts them. A skip is never a pass: a test that
## made no checks but skipped is reported as skipped only, and a run of nothing but skips fails.
##
## The same goes for any function: a script error makes it return null and the caller carries
## on. So a null from an aborted call can pass for an expected null (`T.ok(f() == null)`).
## Keep helpers returning values the test checks, and prefer checks that a null cannot satisfy.
extends RefCounted

## 2^63 as a float. (Large float literals are avoided: the GDScript tokenizer can misread them;
## 9007199254740991.0 parses as 9007199254740990 on 4.3 and 4.7.)
const _TWO_63_F := (1 << 62) * 2.0

static var vectors_dir: String = ""
static var failures: Array[String] = []
static var skips: Array[String] = []
static var checks: int = 0


static func reset() -> void:
	failures = []
	skips = []
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


## Records skipped work (see the header). Not a check.
static func skip(reason: String) -> void:
	skips.append(reason)


## `v` is an ScfgErr whose kind is (a subclass of) `kind`, as Python's `isinstance`.
static func is_err(v: Variant, kind: String, msg: String = "") -> bool:
	checks += 1
	if v is ScfgErr and (v as ScfgErr).is_a(kind):
		return true
	failures.append("%s: expected an ScfgErr %s, got %s (%s)" % [msg, kind, _show(v), type_string(typeof(v))])
	return false


## JSON equality, as conformance/README.md defines it for outputs and request params:
## objects ignore key order, arrays are ordered, numbers compare by exact value (an int equals
## a float of the same value: JSON.parse_string makes every number a float), a bool is never
## a number, null equals only null, and a StringName is a string. Any other Variant must have
## the same type and be ==.
static func json_eq(a: Variant, b: Variant, msg: String = "") -> bool:
	checks += 1
	var diff := first_diff(a, b)
	if diff == "":
		return true
	failures.append("%s: %s (got %s, want %s)" % [msg, diff, _show(a), _show(b)])
	return false


## "" when json_eq would hold, otherwise `<path>: <left> != <right>` for the first difference
## (`a[1].b: 2 != 3`, `k: missing on the right`, `length 2 != 1`). Keys are visited sorted.
static func first_diff(a: Variant, b: Variant, path: String = "") -> String:
	var here := func(detail: String) -> String: return detail if path == "" else "%s: %s" % [path, detail]
	var ta := typeof(a)
	var tb := typeof(b)
	if ta == TYPE_ARRAY or tb == TYPE_ARRAY:
		if ta != tb:
			return here.call("%s != %s" % [_json(a), _json(b)])
		var n := mini(a.size(), b.size())
		for i in n:
			var d := first_diff(a[i], b[i], "%s[%d]" % [path, i])
			if d != "":
				return d
		if a.size() != b.size():
			return here.call("length %d != %d" % [a.size(), b.size()])
		return ""
	if ta == TYPE_DICTIONARY or tb == TYPE_DICTIONARY:
		if ta != tb:
			return here.call("%s != %s" % [_json(a), _json(b)])
		var keys: Array = a.keys()
		keys.sort()
		for k: Variant in keys:
			var sub := str(k) if path == "" else "%s.%s" % [path, k]
			if not (b as Dictionary).has(k):
				return "%s: missing on the right" % sub
			var d := first_diff(a[k], b[k], sub)
			if d != "":
				return d
		var bkeys: Array = b.keys()
		bkeys.sort()
		for k: Variant in bkeys:
			if not (a as Dictionary).has(k):
				return "%s: missing on the left" % (str(k) if path == "" else "%s.%s" % [path, k])
		return ""
	if _json_scalar_eq(a, b):
		return ""
	return here.call("%s != %s" % [_json(a), _json(b)])


static func _json_scalar_eq(a: Variant, b: Variant) -> bool:
	var ta := typeof(a)
	var tb := typeof(b)
	var na := ta == TYPE_INT or ta == TYPE_FLOAT
	var nb := tb == TYPE_INT or tb == TYPE_FLOAT
	if na or nb:
		if not (na and nb):
			return false
		if ta == tb:
			return a == b
		# int against float: equal only if the float is integral, in int range, and the same value
		var i: int = a if ta == TYPE_INT else b
		var f: float = b if ta == TYPE_INT else a
		if is_nan(f) or is_inf(f) or f != floorf(f) or absf(f) >= _TWO_63_F:
			return false
		return int(f) == i
	var sa := ta == TYPE_STRING or ta == TYPE_STRING_NAME
	var sb := tb == TYPE_STRING or tb == TYPE_STRING_NAME
	if sa or sb:
		return sa and sb and str(a) == str(b)
	return ta == tb and a == b


## JSON-ish rendering for diffs (JSON.stringify writes floats differently across Godot
## versions; this keeps ints as ints and marks floats with a decimal point).
static func _json(v: Variant) -> String:
	match typeof(v):
		TYPE_NIL:
			return "null"
		TYPE_FLOAT:
			var f: float = v
			return ("%d.0" % int(f)) if f == floorf(f) and absf(f) < 1e15 else str(f)
		TYPE_STRING, TYPE_STRING_NAME:
			return JSON.stringify(str(v))
		TYPE_ARRAY, TYPE_DICTIONARY:
			var s := JSON.stringify(v)
			return s if s.length() <= 120 else s.substr(0, 117) + "..."
	return str(v)


static func _show(v: Variant) -> String:
	var s := str(v)
	return s if s.length() <= 200 else s.substr(0, 200) + "..."
