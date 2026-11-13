## Runner fixture: a coroutine test that never finishes (run with a short --test-timeout).
extends RefCounted

signal never

const T := preload("res://tests/t.gd")


func test_passes() -> bool:
	T.ok(true, "a check")
	return T.done()


func test_hangs() -> bool:
	await never
	return T.done()
