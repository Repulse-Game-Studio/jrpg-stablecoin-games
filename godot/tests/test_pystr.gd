extends RefCounted

const T := preload("res://tests/t.gd")


# Every expectation below was checked against CPython 3.9 (the project's interpreter, Unicode 13)
# with `python3 -c "print(repr(...))"`.
func test_py_strip_matches_python() -> bool:
	var cases := [
		["\u001c a 　", "a"],  # \x1c-\x1f are whitespace to str.strip()
		["﻿a", "﻿a"],  # the BOM is not
		["\u0085 x ", "x"],
		["​x", "​x"],  # nor is ZERO WIDTH SPACE
		["\t\n\u000b\u000c\r x       ", "x"],
		["", ""],
		["   ", ""],
		[" a b ", "a b"],
	]
	for c: Array in cases:
		T.eq(ScfgPy.py_strip(c[0]), c[1], "strip(%s)" % ScfgPy.py_repr(c[0]))
	return T.done()


func test_py_get_errors() -> bool:
	var d := {"a": 1, "n": null}
	T.eq(ScfgPy.py_get(d, "a"), 1, "present key")
	T.eq(ScfgPy.py_get(d, "n"), null, "a stored None is a value, not an error")
	T.is_err(ScfgPy.py_get(d, "b"), "KeyError", "missing key")
	T.eq((ScfgPy.py_get(d, "b") as ScfgErr).message, "'b'", "KeyError message is the key's repr")
	T.is_err(ScfgPy.py_get(null, "a"), "TypeError", "None['a']")
	T.is_err(ScfgPy.py_get("abc", "a"), "TypeError", "'abc'['a']")
	T.is_err(ScfgPy.py_get(5, "a"), "TypeError", "5['a']")
	T.is_err(ScfgPy.py_get([1, 2], "a"), "TypeError", "[1, 2]['a']")
	T.eq(ScfgPy.py_get([1, 2], 1), 2, "list index")
	T.eq(ScfgPy.py_get([1, 2], -1), 2, "negative list index")
	T.is_err(ScfgPy.py_get([1, 2], 2), "IndexError", "list index out of range")
	T.is_err(ScfgPy.py_get([1, 2], -3), "IndexError", "negative index out of range")
	T.is_err(ScfgPy.py_get([1, 2], true), "TypeError", "a bool is not a list index here")
	return T.done()


func test_json_int_rules() -> bool:
	var parsed: Variant = JSON.parse_string('{"n": 5}')
	T.eq(typeof(parsed["n"]), TYPE_FLOAT, "(JSON numbers arrive as float)")
	T.eq(ScfgPy.json_int(parsed["n"]), 5, "parsed 5 becomes the int 5")
	T.eq(ScfgPy.json_int(5.0), 5, "5.0 -> 5")
	T.eq(ScfgPy.json_int(-7.0), -7, "-7.0 -> -7")
	T.eq(ScfgPy.json_int(0.0), 0, "0.0 -> 0")
	T.eq(ScfgPy.json_int(float(9007199254740991)), 9007199254740991, "2**53 - 1 is exact")
	T.eq(ScfgPy.json_int(12), 12, "an int passes through")
	T.is_err(ScfgPy.json_int(5.5), "ValueError", "5.5")
	T.is_err(ScfgPy.json_int(float(1 << 53)), "ValueError", "2.0**53 may already be rounded")
	T.is_err(ScfgPy.json_int(-float(1 << 53)), "ValueError", "-2.0**53")
	T.is_err(ScfgPy.json_int(9007199254740992), "ValueError", "an int of 2**53 is out of the JSON range too")
	T.is_err(ScfgPy.json_int(INF), "ValueError", "inf")
	T.is_err(ScfgPy.json_int(NAN), "ValueError", "nan")
	T.is_err(ScfgPy.json_int(true), "TypeError", "true is not a number")
	T.is_err(ScfgPy.json_int("5"), "TypeError", "\"5\" is not a number")
	T.is_err(ScfgPy.json_int(null), "TypeError", "null is not a number")
	return T.done()


func test_from_json_reads_numbers_as_python_json_load_does() -> bool:
	var parsed: Variant = JSON.parse_string('{"a": 5, "b": [0, -7, 2.5, {"c": 9007199254740991}], "s": "5", "t": true, "n": null}')
	var v: Variant = ScfgPy.from_json(parsed)
	T.eq([typeof(v["a"]), typeof(v["b"][0]), typeof(v["b"][1]), typeof(v["b"][3]["c"])], [TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT],
		"integral floats become ints, nested")
	T.eq([v["a"], v["b"][1], v["b"][3]["c"]], [5, -7, 9007199254740991], "the same values (2**53 - 1 exact)")
	T.eq(typeof(v["b"][2]), TYPE_FLOAT, "a fractional float stays a float")
	T.eq([v["s"], v["t"], v["n"]], ["5", true, null], "strings, bools and null unchanged")
	T.eq((v as Dictionary).keys(), ["a", "b", "s", "t", "n"], "key order kept")
	T.ok(not is_same(v, parsed) and not is_same(v["b"], parsed["b"]), "new containers")
	T.eq(typeof(parsed["a"]), TYPE_FLOAT, "the input is not touched")
	T.eq(typeof(ScfgPy.from_json(float(1 << 53))), TYPE_FLOAT, "2.0**53 stays a float (json_int refuses it)")
	T.eq(typeof(ScfgPy.from_json(-float(1 << 53))), TYPE_FLOAT, "-2.0**53 too")
	T.eq(typeof(ScfgPy.from_json(INF)), TYPE_FLOAT, "inf stays a float")
	T.ok(is_nan(ScfgPy.from_json(NAN)), "nan stays nan")
	T.eq(ScfgPy.from_json(1 << 60), 1 << 60, "an int passes through, even a large one")
	var big: Variant = ScfgBigInt.from_int(3)
	T.ok(is_same(ScfgPy.from_json([big])[0], big), "an object passes through")
	var typed: Array[String] = ["x"]
	T.ok((ScfgPy.from_json(typed) as Array).is_typed(), "a typed Array keeps its type")
	return T.done()


func test_py_int_matches_python() -> bool:
	var ok := [
		["1_0", 10], [" 7 ", 7], ["٣", 3], ["١٢", 12], ["１２", 12],
		["+5", 5], ["-5", -5], ["007", 7], ["\u00855\u0085", 5], [" 5", 5], ["　5", 5],
		["\t5\n", 5], ["\u000b5\u000c", 5], [String.chr(0x1d7d9) + "0", 10],
		["9223372036854775807", 9223372036854775807], ["-9223372036854775808", -9223372036854775807 - 1],
		[true, 1], [false, 0], [7, 7], [1.9, 1], [-1.9, -1], [-0.0, 0], [5.0, 5],
	]
	for c: Array in ok:
		T.eq(ScfgPy.py_int(c[0]), c[1], "int(%s)" % ScfgPy.py_repr(c[0]))
	var bad := [
		"0x10", "\u001c5", "5\u001f", "​5", "﻿5", "1__0", "_1", "1_", "- 5", "", " ", "1.5",
		"1e3", "\u007f5", "ａ", "5 5",
	]
	for s: String in bad:
		T.is_err(ScfgPy.py_int(s), "ValueError", "int(%s)" % ScfgPy.py_repr(s))
	var big: Variant = ScfgPy.py_int("9223372036854775808")
	T.ok(big is ScfgBigInt and (big as ScfgBigInt).to_decimal() == "9223372036854775808",
		"2**63 comes back as an ScfgBigInt")
	big = ScfgPy.py_int(" 1_000000000000000000000000000000 ")
	T.ok(big is ScfgBigInt and (big as ScfgBigInt).to_decimal() == "1" + "0".repeat(30), "10**30")
	big = ScfgPy.py_int(pow(2.0, 70))
	T.ok(big is ScfgBigInt and (big as ScfgBigInt).to_decimal() == "1180591620717411303424",
		"int(2.0**70) is exact")
	T.eq(ScfgPy.py_int(ScfgBigInt.from_decimal("42")), 42, "a small ScfgBigInt normalises to int")
	T.is_err(ScfgPy.py_int("-9223372036854775809"), "OverflowError",
		"below -2**63 is outside what the port can hold")
	T.is_err(ScfgPy.py_int(INF), "OverflowError", "int(inf)")
	T.is_err(ScfgPy.py_int(NAN), "ValueError", "int(nan)")
	T.is_err(ScfgPy.py_int(null), "TypeError", "int(None)")
	T.is_err(ScfgPy.py_int([]), "TypeError", "int([])")
	T.is_err(ScfgPy.py_int({}), "TypeError", "int({})")
	return T.done()


func test_py_hex_int_matches_python() -> bool:
	var ok := [
		["0x10", 16], ["0X1f", 31], ["0x_1f", 31], ["ff", 255], [" ff ", 255], ["-0x10", -16],
		["٣", 3], ["f_f", 255], ["0b1", 177], ["FF", 255], ["+0xa", 10],
	]
	for c: Array in ok:
		T.eq(ScfgPy.py_hex_int(c[0]), c[1], "int(%s, 16)" % ScfgPy.py_repr(c[0]))
	for s: String in ["x10", "0x", "ａ", "", "0x__1", "1__0", "g", "0x 1"]:
		T.is_err(ScfgPy.py_hex_int(s), "ValueError", "int(%s, 16)" % ScfgPy.py_repr(s))
	T.is_err(ScfgPy.py_hex_int(5), "TypeError", "int(5, 16)")
	T.is_err(ScfgPy.py_hex_int(null), "TypeError", "int(None, 16)")
	var big: Variant = ScfgPy.py_hex_int("0x" + "f".repeat(64))
	T.ok(big is ScfgBigInt and (big as ScfgBigInt).to_hex() == "f".repeat(64), "uint256 max")
	T.eq(ScfgPy.py_hex_int("7fffffffffffffff"), 9223372036854775807, "2**63 - 1 stays an int")
	return T.done()


func test_py_truthy() -> bool:
	for v: Variant in [null, false, 0, 0.0, "", [], {}, PackedByteArray(), ScfgBigInt.from_decimal("0")]:
		T.ok(not ScfgPy.py_truthy(v), "%s is falsy" % ScfgPy.py_repr(v))
	for v: Variant in [true, 1, -1, 0.5, NAN, " ", "0", [0], {"a": null}, ScfgBigInt.from_decimal("1")]:
		T.ok(ScfgPy.py_truthy(v), "%s is truthy" % ScfgPy.py_repr(v))
	return T.done()


func test_has() -> bool:
	T.ok(ScfgPy.has({"a": null}, "a"), "a key holding None is present")
	T.ok(not ScfgPy.has({}, "a"), "missing key")
	T.ok(not ScfgPy.has(null, "a"), "None has no keys")
	T.ok(not ScfgPy.has(["a"], "a"), "has() is for dicts only")
	return T.done()


func test_py_repr() -> bool:
	T.eq(ScfgPy.py_repr("x"), "'x'", "str")
	T.eq(ScfgPy.py_repr("a'b"), "\"a'b\"", "str with a single quote")
	T.eq(ScfgPy.py_repr("a\"b'c"), "'a\"b\\'c'", "str with both quotes")
	T.eq(ScfgPy.py_repr("é\n"), "'é\\n'", "escapes")
	T.eq(ScfgPy.py_repr(null), "None", "None")
	T.eq(ScfgPy.py_repr(true), "True", "True")
	T.eq(ScfgPy.py_repr(5), "5", "int")
	T.eq(ScfgPy.py_repr(5.0), "5.0", "integral float")
	T.eq(ScfgPy.py_repr(0.1), "0.1", "float")
	T.eq(ScfgPy.py_repr({"a": [1, null]}), "{'a': [1, None]}", "dict and list")
	T.eq(ScfgPy.py_repr(ScfgBigInt.from_decimal("18446744073709551616")), "18446744073709551616", "bigint")
	return T.done()


## `template.format("0xab")` as CPython 3.12 has it. Every row was produced by CPython; the
## outcome is the output or the exception class.
func test_py_format1_matches_python() -> bool:
	var rows := [
		["https://s.io/{}", "https://s.io/0xab"], ["{{}}", "{}"], ["{}{{", "0xab{"], ["{}}}", "0xab}"],
		["}}{}{{", "}0xab{"], ["{}}", "ValueError"], ["{}{", "ValueError"], ["}{}", "ValueError"],
		["{}{}", "IndexError"], ["{0}{}", "ValueError"], ["{}{0}", "ValueError"], ["{0}", "0xab"],
		["{00}", "0xab"], ["{1}", "IndexError"], ["{x}", "KeyError"], ["{a.b}", "KeyError"],
		["{ 0}", "KeyError"], ["{0 }", "KeyError"], ["{!r}", "'0xab'"], ["{!s}", "0xab"], ["{!a}", "'0xab'"],
		["{!x}", "ValueError"], ["{!}", "ValueError"], ["{!r", "ValueError"], ["{!rr}", "ValueError"],
		["{!r:>8}", "  '0xab'"], ["{!s:>6}", "  0xab"], ["{!r:}", "'0xab'"], ["{:", "ValueError"],
		["{:>6}", "  0xab"], ["{:*<6}", "0xab**"], ["{:^7}", " 0xab  "], ["{:5}", "0xab "], ["{:08}", "0xab0000"],
		["{:.1}", "0"], ["{:.0}", ""], ["{:s}", "0xab"], ["{:10.2}", "0x        "], ["{:é>6}", "éé0xab"],
		["{:<}", "0xab"], ["{:>>6}", ">>0xab"], ["{:x}", "ValueError"], ["{:b}", "ValueError"],
		["{:+}", "ValueError"], ["{:-}", "ValueError"], ["{: }", "ValueError"], ["{:,}", "ValueError"],
		["{:_}", "ValueError"], ["{:=5}", "ValueError"], ["{:#}", "ValueError"], ["{:z}", "ValueError"],
		["{:.}", "ValueError"], ["{:010000000000000000000}", "ValueError"], ["{:{}}", "IndexError"],
		["{:{:{}}}", "IndexError"], ["{0:{0}}", "ValueError"], ["{:{{}}", "ValueError"],
		["{[0]}", "0"], ["{[5]}", "IndexError"], ["{[x]}", "TypeError"], ["{.nope}", "AttributeError"],
		["{[}", "ValueError"], ["{[0}", "ValueError"], ["{.}", "ValueError"], ["{0.}", "ValueError"],
		["{.upper[0]}", "TypeError"], ["{.upper.x}", "AttributeError"], ["{.upper:>3}", "TypeError"],
		["{.upper}{}", "IndexError"], ["{.__class__}", "ValueError"],
		["{{", "{"], ["{", "ValueError"], ["}", "ValueError"], ["", ""], ["plain", "plain"],
	]
	for row: Array in rows:
		var got: Variant = ScfgPy.py_format1(row[0], "0xab")
		var want: String = row[1]
		if got is ScfgErr:
			T.eq((got as ScfgErr).kind, want, "format(%s) raises" % ScfgPy.py_repr(row[0]))
		else:
			T.eq(got, want, "format(%s)" % ScfgPy.py_repr(row[0]))
	# the argument is what is substituted
	T.eq(ScfgPy.py_format1("https://x/tx/{}?a=1", "0xdeadbeef"), "https://x/tx/0xdeadbeef?a=1", "a real hash")
	T.eq(ScfgPy.py_format1("{[5]}", "0xdeadbeef"), "d", "index 5 of a longer argument")
	T.eq(ScfgPy.py_format1("{!a}", "é"), "'\\xe9'", "!a escapes non-ASCII")
	T.eq(ScfgPy.py_format1("{.upper}", "x"), "<built-in method upper of str object at 0x7f0000000000>",
		"a str method formats as a bound method (Python prints its real address)")
	return T.done()


## Python `a == b` for JSON-like values: never a runtime error (GDScript's `==` is one between,
## say, a float and a String).
func test_py_eq_is_python_equality() -> bool:
	var rows := [
		[5, 5.0, true], [5, "5", false], ["0x0", 0.0, false], [null, "x", false], [null, null, true],
		[true, 1, true], [false, 0.0, true], ["a", &"a", true], [[1, "a"], [1, "a"], true], [[1, "a"], [1, 2], false],
		[{"a": 1}, {"a": 1.0}, true], [{"a": 1}, {"b": 1}, false], [{"a": 1}, [1], false],
		[ScfgBigInt.from_decimal("123456789012345678901"), ScfgBigInt.from_decimal("123456789012345678901"), true],
		[ScfgBigInt.from_decimal("7"), 7, true], [7, ScfgBigInt.from_decimal("7"), true],
		[ScfgBigInt.from_decimal("7"), 7.5, false], [1.5, 1.5, true], [NAN, NAN, false],
	]
	for r: Array in rows:
		T.eq(ScfgPy.py_eq(r[0], r[1]), r[2], "%s == %s" % [r[0], r[1]])
	return T.done()


## Python's `for x in v` over a JSON value.
func test_py_iter_is_python_iteration() -> bool:
	T.eq(ScfgPy.py_iter([1, 2]), [1, 2], "a list")
	T.eq(ScfgPy.py_iter({"a": 1, "b": 2}), ["a", "b"], "a dict's keys")
	T.eq(ScfgPy.py_iter("ab"), ["a", "b"], "a str's characters")
	T.is_err(ScfgPy.py_iter(5), "TypeError", "an int")
	T.is_err(ScfgPy.py_iter(null), "TypeError", "None")
	T.is_err(ScfgPy.py_iter(true), "TypeError", "a bool")
	return T.done()
