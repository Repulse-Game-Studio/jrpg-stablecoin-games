## Runner fixture: one pass and every way a test can fail.
extends RefCounted

const T := preload("res://tests/t.gd")


func test_passes() -> bool:
	T.ok(true, "a check")
	return T.done()


func test_fails_a_check() -> bool:
	T.eq(1, 2, "one is two")
	return T.done()


func test_aborts() -> bool:
	T.ok(true, "a check before the script error")
	var d := {}
	var _x: Variant = d["missing"]
	return T.done()


func test_aborts_after_await() -> bool:
	T.ok(true, "a check before the await")
	await (Engine.get_main_loop() as SceneTree).process_frame
	var d := {}
	var _x: Variant = d["missing"]
	return T.done()


func test_makes_no_checks() -> bool:
	return T.done()


func test_fails_and_skips() -> bool:
	T.skip("a skip does not hide a failure")
	T.ok(false, "a failed check")
	return T.done()
