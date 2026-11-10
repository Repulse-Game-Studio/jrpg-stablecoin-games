## Headless test runner for the SCFG GDScript core.
##
##   godot --headless --path godot --import        # once per checkout: registers class_name globals
##   godot --headless --path godot --script res://tests/run.gd -- --vectors=<abs path> [--bench]
##       [--tests=<res:// dir>] [--test-timeout=<seconds>]
##
## Loads every test_*.gd directly in the tests directory (default res://tests; each extends
## RefCounted), runs each of its `test_*` methods in order, one at a time, and prints
## `PASS <name>`, `FAIL <name>: <message>` or `SKIP <name>: <reason>` plus the summary
## `<n> passed, <n> failed, <n> skipped`.
##
## Fail-closed rules (the exit code is 0 only when at least one test passed and none failed):
## - a test method must end with `return T.done()`: a script error aborts the method, which then
##   returns null (also after an `await`), and that counts as a failure;
## - a test that made no checks fails, unless it recorded a skip (then it is only a SKIP);
## - a SKIP never counts as a pass, so a run of nothing but skips exits 1;
## - a test file that does not load, or has no test_* methods, is a failure;
## - zero tests, an unknown argument, a missing --vectors or tests directory, or class_name
##   globals that are not registered yet (run --import) exit 1;
## - a coroutine test still pending after --test-timeout seconds (default 600) fails the run
##   (exit 1 at once: a hung test cannot be cancelled);
## - the process exits only through quit(code) after _main returned an int; if the runner itself
##   aborts, the watchdog in _process exits 1.
## `--bench` also runs res://tests/bench_spike.gd, which prints medians.
extends SceneTree

const T := preload("res://tests/t.gd")
const DEFAULT_TESTS_DIR := "res://tests"
const DEFAULT_TEST_TIMEOUT := 600.0

var _started := false  # _run is past its first line
var _done := false  # _run has called quit()
var _test_label := ""  # the test being awaited, for the timeout message
var _test_started_ms := 0
var _test_timeout_s := DEFAULT_TEST_TIMEOUT


func _initialize() -> void:
	_run()  # a coroutine: the main loop keeps turning while tests await


func _process(_delta: float) -> bool:
	if _done:
		return false
	if not _started:
		printerr("run.gd: the runner aborted before it started (see the errors above)")
		_finish(1)
	elif _test_label != "" and Time.get_ticks_msec() - _test_started_ms > _test_timeout_s * 1000.0:
		print("FAIL %s: timed out after %s s" % [_test_label, _fmt_seconds(_test_timeout_s)])
		_finish(1)
	return false


func _run() -> void:
	_started = true
	var code: Variant = await _main()
	_finish(code if code is int else 1)


func _finish(code: int) -> void:
	if not _done:
		_done = true
		quit(code)


func _main() -> int:
	var vectors := ""
	var bench := false
	var tests_dir := DEFAULT_TESTS_DIR
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--vectors="):
			vectors = arg.substr("--vectors=".length())
		elif arg.begins_with("--tests="):
			tests_dir = arg.substr("--tests=".length())
		elif arg.begins_with("--test-timeout="):
			var t := arg.substr("--test-timeout=".length())
			if not t.is_valid_float() or float(t) <= 0.0:
				printerr("run.gd: --test-timeout needs a positive number of seconds (got %s)" % t)
				return 1
			_test_timeout_s = float(t)
		elif arg == "--bench":
			bench = true
		else:
			printerr("run.gd: unknown argument %s" % arg)
			return 1
	if vectors == "" or not DirAccess.dir_exists_absolute(vectors.path_join("tier1")):
		printerr("run.gd: pass --vectors=<absolute path to conformance/vectors> (got %s)" % vectors)
		return 1
	if not DirAccess.dir_exists_absolute(tests_dir):
		printerr("run.gd: tests directory %s does not exist" % tests_dir)
		return 1
	T.vectors_dir = vectors
	var registered := ProjectSettings.get_global_class_list().any(
		func(c: Dictionary) -> bool: return c["class"] == &"ScfgBigInt")
	if not registered:
		printerr("run.gd: class_name globals are not registered; run "
			+ "`godot --headless --path godot --import` once first")
		return 1

	var passed := 0
	var failed := 0
	var skipped := 0
	var files := Array(DirAccess.get_files_at(tests_dir))
	files.sort()
	for file: String in files:
		if not (file.begins_with("test_") and file.ends_with(".gd")):
			continue
		var path := tests_dir.path_join(file)
		var script := load(path) as GDScript
		if script == null or not script.can_instantiate():
			print("FAIL %s: script did not load (see the errors above)" % file)
			failed += 1
			continue
		var suite: Object = script.new()
		var ran_in_file := 0
		for method: Dictionary in script.get_script_method_list():
			var method_name: String = method["name"]
			if not method_name.begins_with("test_"):
				continue
			ran_in_file += 1
			var label := "%s::%s" % [file.get_basename(), method_name]
			T.reset()
			_test_label = label
			_test_started_ms = Time.get_ticks_msec()
			# a plain method returns its value; a coroutine is awaited until it returns
			var result: Variant = await suite.call(method_name)
			_test_label = ""
			if not (result is bool and result == true):
				T.failures.append("did not return T.done() (aborted by a script error?)")
			elif T.checks == 0 and T.skips.is_empty():
				T.failures.append("made no checks")
			if T.failures.is_empty():
				if T.checks > 0:
					print("PASS %s (%d checks)" % [label, T.checks])
					passed += 1
			else:
				for msg in T.failures:
					print("FAIL %s: %s" % [label, msg])
				failed += 1
			for reason in T.skips:
				print("SKIP %s: %s" % [label, reason])
				skipped += 1
		if ran_in_file == 0:
			print("FAIL %s: has no test_* methods" % file)
			failed += 1

	print("%d passed, %d failed, %d skipped" % [passed, failed, skipped])
	var code := 0 if failed == 0 and passed > 0 else 1

	if bench:
		var bench_script := load(DEFAULT_TESTS_DIR.path_join("bench_spike.gd")) as GDScript
		if bench_script == null or not bench_script.can_instantiate():
			print("FAIL bench_spike.gd: script did not load")
			return 1
		var ran: Variant = bench_script.new().call("run", vectors)
		if not (ran is bool and ran == true):
			print("FAIL bench_spike.gd: run() did not finish")
			return 1
	return code


static func _fmt_seconds(s: float) -> String:
	return str(int(s)) if s == floorf(s) else str(s)
