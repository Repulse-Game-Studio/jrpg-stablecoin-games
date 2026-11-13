extends RefCounted

const T := preload("res://tests/t.gd")


func test_config_error_is_a_value_error() -> bool:
	var e := ScfgErr.make("ConfigError", "no chains configured", "chains.empty")
	T.eq(e.kind, "ConfigError", "kind")
	T.eq(e.code, "chains.empty", "code")
	T.eq(e.message, "no chains configured", "message")
	T.eq(e.status, null, "status defaults to null")
	for k: String in ["ConfigError", "ValueError", "ScfgError", "Exception", "BaseException"]:
		T.ok(e.is_a(k), "ConfigError is a %s" % k)
	for k: String in ["TypeError", "KeyError", "LookupError", "RestoreError"]:
		T.ok(not e.is_a(k), "ConfigError is not a %s" % k)
	return T.done()


func test_python_hierarchy() -> bool:
	# KeyError and IndexError are LookupErrors, not ValueErrors (Python's tree, not a port's guess)
	for k: String in ["KeyError", "IndexError"]:
		var e := ScfgErr.make(k, "x")
		T.ok(e.is_a("LookupError") and e.is_a("Exception"), "%s is a LookupError" % k)
		T.ok(not e.is_a("ValueError") and not e.is_a("TypeError"), "%s is not a ValueError/TypeError" % k)
	T.ok(ScfgErr.make("OverflowError", "x").is_a("ArithmeticError"), "OverflowError is an ArithmeticError")
	T.ok(ScfgErr.make("ZeroDivisionError", "x").is_a("ArithmeticError"), "ZeroDivisionError too")
	T.ok(not ScfgErr.make("AttributeError", "x").is_a("ValueError"), "AttributeError is not a ValueError")
	T.ok(not ScfgErr.make("RestoreError", "x", "tx_not_found").is_a("ScfgError"),
		"RestoreError extends Exception directly, as in scfg_core/restore.py")
	# asyncio.CancelledError is a BaseException, so `except Exception` does not contain it
	var c := ScfgErr.make("CancelledError", "cancelled")
	T.ok(c.is_a("BaseException") and not c.is_a("Exception"), "CancelledError is only a BaseException")
	var t := ScfgErr.make("TransportError", "HTTP 503", null, 503)
	T.eq(t.status, 503, "TransportError keeps its HTTP status")
	T.ok(t.is_a("TransportError") and not t.is_a("RpcError"), "TransportError is not an RpcError")
	T.eq(ScfgErr.make("RpcError", "boom", -32000).code, -32000, "RpcError keeps an int code")
	T.ok(ScfgErr.make("ValueError", "x").is_a("ValueError"), "a kind is itself")
	return T.done()


func test_is_err() -> bool:
	var e := ScfgErr.make("ValueError", "bad")
	T.ok(ScfgErr.is_err(e), "an ScfgErr is an error")
	T.ok(e is ScfgErr, "`is ScfgErr` works as the propagation check")
	for v: Variant in [null, 0, "", "ValueError", {}, [], false, RefCounted.new(), ScfgBigInt.new()]:
		T.ok(not ScfgErr.is_err(v), "%s is not an error" % [v])
	return T.done()


func test_err_to_string() -> bool:
	T.eq(str(ScfgErr.make("ValueError", "bad hex")), "ValueError: bad hex", "plain kind")
	T.eq(str(ScfgErr.make("ConfigError", "no chains", "chains.empty")),
		"ConfigError(chains.empty): no chains", "a code is shown")
	T.eq(str(ScfgErr.make("TransportError", "HTTP 503", null, 503)),
		"TransportError(status=503): HTTP 503", "a status is shown")
	return T.done()


## A typo in a kind name (`is_a("ValueEror")`) would silently be false in an `except` translation,
## so it is a programming error reported with push_error, like `make()` with an unknown kind.
## The tests silence the report and read it back instead (a passing run prints no ERROR lines).
func test_unknown_kind_is_a_programming_error() -> bool:
	var e := ScfgErr.make("ValueError", "x")
	ScfgErr.silence_misuse = true
	ScfgErr.misuse.clear()
	T.eq(e.is_a("ValueEror"), false, "a typo is not a base")
	T.eq(ScfgErr.misuse.size(), 1, "is_a reports an unknown kind")
	T.ok(ScfgErr.misuse.size() == 1 and "ValueEror" in ScfgErr.misuse[0], "the report names the typo")
	var odd := ScfgErr.make("NoSuchError", "y")
	T.eq(ScfgErr.misuse.size(), 2, "make reports an unknown kind")
	T.eq(odd.kind, "NoSuchError", "and still returns the error")
	ScfgErr.misuse.clear()
	for k: String in ScfgErr.BASES:
		e.is_a(k)
		odd.is_a(k)
	T.eq(ScfgErr.misuse.size(), 0, "every known kind is accepted")
	ScfgErr.silence_misuse = false
	return T.done()


## `ScfgErr.printed` collects only while a test sets `silence_printed`: otherwise report()'s
## fallback prints the line (one expected line in the runner's output) and keeps nothing, so a
## game never accumulates its contained errors in memory.
func test_report_keeps_nothing_when_not_silenced() -> bool:
	var was := ScfgErr.silence_printed
	ScfgErr.silence_printed = false
	ScfgErr.printed.clear()
	ScfgErr.report(Callable(), ScfgErr.make("RuntimeError",
		"boom (expected: test_err::test_report_keeps_nothing_when_not_silenced)"))
	T.eq(ScfgErr.printed.size(), 0, "printed, not kept (%s)" % [ScfgErr.printed])
	# a valid handler is called and nothing is printed or kept either
	var told := []
	ScfgErr.report(func(x: Variant) -> void: told.append(x), ScfgErr.make("RuntimeError", "told"))
	T.eq(told.size(), 1, "the handler is called")
	T.eq(ScfgErr.printed.size(), 0, "still nothing kept (%s)" % [ScfgErr.printed])
	ScfgErr.silence_printed = was
	return T.done()
