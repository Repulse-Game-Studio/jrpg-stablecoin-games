## Runner fixture (run by test_runner.gd, never by the main pass): passes and skips.
extends RefCounted

const T := preload("res://tests/t.gd")


func test_passes() -> bool:
	T.ok(true, "a check")
	return T.done()


func test_skips() -> bool:
	T.skip("not yet")
	return T.done()


func test_passes_and_skips() -> bool:
	await (Engine.get_main_loop() as SceneTree).process_frame
	T.ok(true, "a check after an await")
	T.skip("one part is not implemented")
	return T.done()
