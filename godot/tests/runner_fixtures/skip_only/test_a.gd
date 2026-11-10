## Runner fixture: only skipped work, which must not make the run pass.
extends RefCounted

const T := preload("res://tests/t.gd")


func test_skips() -> bool:
	T.skip("nothing implemented")
	return T.done()
