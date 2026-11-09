## Headless test runner for the SCFG GDScript core.
##
##   godot --headless --path godot --import        # once per checkout: registers class_name globals
##   godot --headless --path godot --script res://tests/run.gd -- --vectors=<abs path> [--bench]
##
## Loads every res://tests/test_*.gd (each extends RefCounted), calls each of its `test_*`
## methods, prints `PASS <name>` or `FAIL <name>: <message>` and a summary, and exits 0 only
## when at least one test ran and none failed. A test method must end with `return T.done()`:
## a script error aborts the method (it then returns null) and that counts as a failure.
## `--bench` also runs res://tests/bench_spike.gd, which prints medians.
extends SceneTree

const T := preload("res://tests/t.gd")
const TESTS_DIR := "res://tests"


func _init() -> void:
	# Exit 1 unless _main finishes: a script error aborts _main and leaves this code in place
	# (it would otherwise abort _init before quit() and hang the process).
	quit(1)
	var code: Variant = _main()
	if code is int:
		quit(code)


func _main() -> int:
	var vectors := ""
	var bench := false
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--vectors="):
			vectors = arg.substr("--vectors=".length())
		elif arg == "--bench":
			bench = true
		else:
			printerr("run.gd: unknown argument %s" % arg)
			return 1
	if vectors == "" or not DirAccess.dir_exists_absolute(vectors.path_join("tier1")):
		printerr("run.gd: pass --vectors=<absolute path to conformance/vectors> (got %s)" % vectors)
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
	var files := Array(DirAccess.get_files_at(TESTS_DIR))
	files.sort()
	for file: String in files:
		if not (file.begins_with("test_") and file.ends_with(".gd")):
			continue
		var path := TESTS_DIR.path_join(file)
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
			T.reset()
			var result: Variant = suite.call(method_name)
			var label := "%s::%s" % [file.get_basename(), method_name]
			if not (result is bool and result == true):
				T.failures.append("did not return T.done() (aborted by a script error?)")
			elif T.checks == 0:
				T.failures.append("made no checks")
			if T.failures.is_empty():
				print("PASS %s (%d checks)" % [label, T.checks])
				passed += 1
			else:
				for msg in T.failures:
					print("FAIL %s: %s" % [label, msg])
				failed += 1
		if ran_in_file == 0:
			print("FAIL %s: has no test_* methods" % file)
			failed += 1

	print("%d passed, %d failed" % [passed, failed])
	var code := 0 if failed == 0 and passed > 0 else 1

	if bench:
		var bench_script := load(TESTS_DIR.path_join("bench_spike.gd")) as GDScript
		if bench_script == null or not bench_script.can_instantiate():
			print("FAIL bench_spike.gd: script did not load")
			return 1
		var ran: Variant = bench_script.new().call("run", vectors)
		if not (ran is bool and ran == true):
			print("FAIL bench_spike.gd: run() did not finish")
			return 1
	return code
