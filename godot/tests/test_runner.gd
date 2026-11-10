## Self-tests of the runner (res://tests/run.gd): coroutine tests in-process, and the fail-closed
## rules by running the runner on the fixture directories in res://tests/runner_fixtures/ in a
## child Godot process (those fixtures are never part of the main pass: it does not recurse).
extends RefCounted

signal _value_ready(v: int)

const T := preload("res://tests/t.gd")
const FIXTURES := "res://tests/runner_fixtures"


func test_runner_awaits_coroutines() -> bool:
	var v: Variant = await _next_frame_plus_one(41)
	T.eq(v, 42, "a coroutine helper's value after a frame")
	# a value that only exists once a deferred call has run
	_emit_later.call_deferred(7)
	var got: Variant = await _value_ready
	T.eq(got, 7, "a deferred signal's value")
	return T.done()


func test_runner_fail_closed_rules() -> bool:
	var r := _run_fixture("pass_skip")
	T.eq(r[0], 0, "passes plus skips exit 0\n" + r[1])
	_has_line(r[1], "PASS test_a::test_passes (1 checks)")
	_has_line(r[1], "SKIP test_a::test_skips: not yet")
	_has_line(r[1], "PASS test_a::test_passes_and_skips (1 checks)")
	_has_line(r[1], "SKIP test_a::test_passes_and_skips: one part is not implemented")
	_has_line(r[1], "2 passed, 0 failed, 2 skipped")

	r = _run_fixture("skip_only")
	T.eq(r[0], 1, "only skips exit 1\n" + r[1])
	_has_line(r[1], "0 passed, 0 failed, 1 skipped")

	r = _run_fixture("failures")
	T.eq(r[0], 1, "failures exit 1\n" + r[1])
	_has_line(r[1], "PASS test_a::test_passes (1 checks)")
	_has_line(r[1], "FAIL test_a::test_fails_a_check: one is two: expected 2 (int), got 1 (int)")
	_has_line(r[1], "FAIL test_a::test_aborts: did not return T.done() (aborted by a script error?)")
	_has_line(r[1], "FAIL test_a::test_aborts_after_await: did not return T.done() (aborted by a script error?)")
	_has_line(r[1], "FAIL test_a::test_makes_no_checks: made no checks")
	_has_line(r[1], "FAIL test_a::test_fails_and_skips: a failed check")
	_has_line(r[1], "SKIP test_a::test_fails_and_skips: a skip does not hide a failure")
	_has_line(r[1], "1 passed, 5 failed, 1 skipped")

	r = _run_fixture("no_methods")
	T.eq(r[0], 1, "a file without test_* methods exits 1\n" + r[1])
	_has_line(r[1], "FAIL test_a.gd: has no test_* methods")

	r = _run_fixture("no_tests")
	T.eq(r[0], 1, "zero tests exit 1\n" + r[1])
	_has_line(r[1], "0 passed, 0 failed, 0 skipped")

	r = _run_fixture("hang", ["--test-timeout=1"])
	T.eq(r[0], 1, "a hung coroutine test exits 1\n" + r[1])
	_has_line(r[1], "FAIL test_a::test_hangs: timed out after 1 s")

	r = _run_fixture("pass_skip", ["--bogus"])
	T.eq(r[0], 1, "an unknown argument exits 1\n" + r[1])
	r = _run_fixture("pass_skip", ["--test-timeout=0"])
	T.eq(r[0], 1, "a non-positive timeout exits 1\n" + r[1])
	r = _run_fixture("does_not_exist")
	T.eq(r[0], 1, "a missing tests directory exits 1\n" + r[1])
	return T.done()


func _next_frame_plus_one(x: int) -> int:
	await (Engine.get_main_loop() as SceneTree).process_frame
	return x + 1


func _emit_later(v: int) -> void:
	_value_ready.emit(v)


## [exit code, stdout] of the runner on one fixture directory, in a child Godot process.
func _run_fixture(name: String, extra: Array = []) -> Array:
	var args := ["--headless", "--path", ProjectSettings.globalize_path("res://"),
		"--script", "res://tests/run.gd", "--", "--vectors=" + T.vectors_dir,
		"--tests=" + FIXTURES.path_join(name)]
	args.append_array(extra)
	var out := []
	var code := OS.execute(OS.get_executable_path(), args, out, true)
	return [code, "\n".join(out)]


func _has_line(output: String, line: String) -> void:
	T.ok(line in output.split("\n"), "runner output has the line %s\n--- output ---\n%s" % [line, output])
