## Self-tests of the assertion helpers in t.gd that compare JSON values.
extends RefCounted

const T := preload("res://tests/t.gd")


func test_json_eq_semantics() -> bool:
	var same := [
		[{"a": 1, "b": 2}, {"b": 2, "a": 1}],  # objects ignore key order
		[5, 5.0], [-0.0, 0], [9007199254740991, float(9007199254740991)],  # numbers by value
		[null, null], ["x", "x"], [true, true], [[], []], [{}, {}],
		[[1, {"k": [null, "s"]}], [1.0, {"k": [null, "s"]}]],
		[JSON.parse_string('{"n": 5, "l": [1, 2]}'), {"l": [1, 2], "n": 5}],
		[&"name", "name"],  # a StringName is a string
	]
	for p: Array in same:
		T.eq(T.first_diff(p[0], p[1]), "", "%s equals %s" % [p[0], p[1]])
		T.eq(T.first_diff(p[1], p[0]), "", "%s equals %s (swapped)" % [p[1], p[0]])
	var different := [
		[true, 1], [false, 0], [true, 1.0],  # bool is never a number
		[null, 0], [null, ""], [null, false], [null, []], [null, {}],  # null is distinct
		["1", 1], [[1, 2], [2, 1]], [[1], [1, 1]], [{"a": 1}, {"a": 1, "b": 2}],
		[{"a": null}, {}], [5, 5.5], [[], {}],
		# 2**53 + 1 as an int is not the float 2**53, though GDScript's == says it is
		[9007199254740993, float(1 << 53)],
		[9223372036854775807, (1 << 62) * 2.0],
	]
	for p: Array in different:
		T.ok(T.first_diff(p[0], p[1]) != "", "%s differs from %s" % [p[0], p[1]])
		T.ok(T.first_diff(p[1], p[0]) != "", "%s differs from %s (swapped)" % [p[1], p[0]])
	return T.done()


func test_first_diff_names_the_path() -> bool:
	T.eq(T.first_diff({"a": [1, {"b": 2}]}, {"a": [1, {"b": 3}]}), "a[1].b: 2 != 3", "nested path")
	T.eq(T.first_diff({"a": 1}, {}), "a: missing on the right", "missing on the right")
	T.eq(T.first_diff({}, {"z": 1}), "z: missing on the left", "missing on the left")
	T.eq(T.first_diff([1, 2], [1]), "length 2 != 1", "array length")
	T.eq(T.first_diff(true, 1), "true != 1", "top-level difference")
	return T.done()


func test_json_eq_records_a_check() -> bool:
	var before := T.checks
	var ok := T.json_eq({"a": 5.0}, {"a": 5}, "json_eq passes")
	T.ok(ok, "json_eq returns true when equal")
	T.eq(T.checks - before, 2, "json_eq counts as one check")
	var failures_before := T.failures.size()
	var bad := T.json_eq(true, 1, "deliberate")
	T.ok(not bad, "json_eq returns false when different")
	T.eq(T.failures.size() - failures_before, 1, "and records one failure")
	if T.failures.size() > failures_before:
		T.ok(T.failures[-1].begins_with("deliberate: "), "the failure carries the message")
		T.failures.resize(failures_before)  # the deliberate failure is not this test's verdict
	return T.done()
