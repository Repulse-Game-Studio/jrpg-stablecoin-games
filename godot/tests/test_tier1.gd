extends RefCounted

const T := preload("res://tests/t.gd")
const Vectors := preload("res://tests/vectors.gd")
const Tier1 := preload("res://tests/tier1.gd")


## Every Tier 1 case of every implemented topic, through the harness. Each other topic is one
## SKIP line (never a pass); an implemented topic without a vector file fails.
func test_tier1_vectors() -> bool:
	var topics := Vectors.load_tier1(T.vectors_dir)
	T.ok(topics.size() > 0, "the tier1 files load")
	for topic: String in Tier1.IMPLEMENTED:
		T.ok(topics.has(topic), "implemented topic %s has a vector file" % topic)
	for topic: String in topics:
		var cases: Array = topics[topic]
		if not topic in Tier1.IMPLEMENTED:
			T.skip("tier1/%s.json: %d cases (topic not implemented yet)" % [topic, cases.size()])
			continue
		T.ok(cases.size() > 0, "tier1/%s.json has cases" % topic)
		for c: Variant in cases:
			var msg := Tier1.run_case(c)
			T.ok(msg == "", "tier1/%s: %s" % [topic, msg])
	return T.done()


## Every Tier 1 vector file has a GDScript adapter set: nothing is skipped any more (Task 4 closed
## the last five topics). A new topic in the vectors fails here until its adapters exist.
func test_every_topic_implemented() -> bool:
	var topics := Vectors.load_tier1(T.vectors_dir)
	T.eq(topics.size(), 17, "17 Tier 1 topics")
	for topic: String in topics:
		T.ok(topic in Tier1.IMPLEMENTED, "tier1/%s.json is implemented" % topic)
	for topic: String in Tier1.IMPLEMENTED:
		T.ok(topics.has(topic), "implemented topic %s has a vector file" % topic)
	T.eq(Tier1.IMPLEMENTED.size(), 17, "IMPLEMENTED lists 17 topics")
	var total := 0
	for topic: String in topics:
		total += (topics[topic] as Array).size()
	T.eq(total, 1074, "1,074 Tier 1 cases")
	var missing: Array[String] = []
	for topic: String in topics:
		for c: Variant in topics[topic]:
			if c is Dictionary and not Tier1.FUNCS.has(c.get("fn")):
				missing.append(str(c.get("fn")))
	T.eq(missing, [], "every vector fn has an adapter")
	return T.done()


func test_run_case_buckets_errors() -> bool:
	var fake := {
		"__value_error": func(_i: Dictionary) -> Variant: return ScfgErr.make("ValueError", "bad"),
		"__type_error": func(_i: Dictionary) -> Variant: return ScfgErr.make("TypeError", "bad"),
		"__config_error": func(_i: Dictionary) -> Variant:
			return ScfgErr.make("ConfigError", "no chains", "chains.empty"),
		"__key_error": func(_i: Dictionary) -> Variant: return ScfgErr.make("KeyError", "'x'"),
		"__index_error": func(_i: Dictionary) -> Variant: return ScfgErr.make("IndexError", "out of range"),
		"__attribute_error": func(_i: Dictionary) -> Variant:
			return ScfgErr.make("AttributeError", "'NoneType' object has no attribute 'strip'"),
		"__rpc_error": func(_i: Dictionary) -> Variant: return ScfgErr.make("RpcError", "boom", -32000),
		"__null": func(_i: Dictionary) -> Variant: return null,
		"__none": func(_i: Dictionary) -> Variant: return Tier1.NONE,
		"__dict": func(i: Dictionary) -> Variant: return {"b": [1, true], "a": i.get("n")},
	}
	for k: String in fake:
		Tier1.FUNCS[k] = fake[k]
	var case_ := func(fn: String, want: Dictionary, input: Variant = {}) -> Dictionary:
		var c := {"fn": fn, "in": input}
		c.merge(want)
		return c
	# ValueError and TypeError (ConfigError's code aside) are "invalid", as in tier1.py's _compute
	T.eq(Tier1.run_case(case_.call("__value_error", {"error": "invalid"})), "", "ValueError is invalid")
	T.eq(Tier1.run_case(case_.call("__type_error", {"error": "invalid"})), "", "TypeError is invalid")
	T.eq(Tier1.run_case(case_.call("__config_error", {"error": "chains.empty"})), "",
		"a ConfigError is compared by code")
	T.ok(Tier1.run_case(case_.call("__config_error", {"error": "invalid"})) != "",
		"a ConfigError is not \"invalid\"")
	T.ok(Tier1.run_case(case_.call("__config_error", {"error": "chains.other"})) != "", "wrong code")
	# Python's _compute lets these propagate (the generator would crash), so they fail the case
	for fn: String in ["__key_error", "__index_error", "__attribute_error", "__rpc_error"]:
		var msg := Tier1.run_case(case_.call(fn, {"error": "invalid"}))
		T.ok(msg != "" and "raised" in msg, "%s fails the case: %s" % [fn, msg])
	T.ok("did not return" in Tier1.run_case(case_.call("__null", {"out": null})),
		"a bare null (an aborted adapter?) fails, even where None is expected")
	T.eq(Tier1.run_case(case_.call("__none", {"out": null})), "", "Tier1.NONE is Python's None")
	T.eq(Tier1.run_case(case_.call("__dict", {"out": {"a": 5.0, "b": [1, true]}}, {"n": 5})), "",
		"outputs compare as JSON: key order and int/float do not matter")
	T.ok(Tier1.run_case(case_.call("__dict", {"out": {"a": 5, "b": [1, 1]}}, {"n": 5})) != "",
		"true is not 1")
	T.ok(Tier1.run_case(case_.call("__dict", {"error": "invalid"}, {"n": 5})) != "",
		"an output where the vector wants an error")
	T.ok(Tier1.run_case(case_.call("__value_error", {"out": null})) != "",
		"an error where the vector wants an output")
	T.ok("no GDScript adapter" in Tier1.run_case(case_.call("__missing", {"out": null})), "unknown fn")
	T.ok(Tier1.run_case(case_.call("__dict", {"out": null}, [])) != "", "`in` must be an object")
	T.ok(Tier1.run_case({"fn": "__dict", "in": {}}) != "", "a case without out or error")
	T.ok(Tier1.run_case({"fn": "__dict", "in": {}, "out": 1, "error": "invalid"}) != "",
		"a case with both out and error")
	for k: String in fake:
		Tier1.FUNCS.erase(k)
	return T.done()


func test_tier1_runner_reads_bigints_as_strings() -> bool:
	var keccak := Vectors.load_topic(T.vectors_dir, "keccak")
	T.ok(keccak.size() > 0, "keccak.json has cases")
	if keccak.size() > 0:
		T.eq(typeof(keccak[0]["in"]["data_hex"]), TYPE_STRING, "*_hex field arrives as a String")
	# a decimal-string field above 2**53 (units = 2**64 in amounts.json) stays exact
	var found := false
	for c: Dictionary in Vectors.load_topic(T.vectors_dir, "amounts"):
		if c["fn"] == "units_to_decimal" and str(c["in"].get("units")) == "18446744073709551616":
			found = true
			T.eq(typeof(c["in"]["units"]), TYPE_STRING, "big units arrive as a String")
			var units: Variant = ScfgBigInt.from_decimal(c["in"]["units"])
			T.ok(units is ScfgBigInt and units.to_hex() == "10000000000000000", "2**64 exactly")
	T.ok(found, "amounts.json has the units_to_decimal case for 2**64")
	var big: Variant = ScfgBigInt.from_decimal("9007199254740993")
	T.ok(big is ScfgBigInt and big.to_decimal() == "9007199254740993", "2**53 + 1, which a float cannot hold")
	T.ok(float("9007199254740993") == float(1 << 53), "(sanity: a float would round it)")
	return T.done()


func test_vector_loaders() -> bool:
	var topics := Vectors.load_tier1(T.vectors_dir)
	var names := DirAccess.get_files_at(T.vectors_dir.path_join("tier1"))
	T.eq(topics.size(), names.size(), "one topic per tier1 file")
	for topic: String in topics:
		T.ok(topics[topic] is Array and (topics[topic] as Array).size() > 0, "tier1/%s.json has cases" % topic)
	var keys := topics.keys()
	var sorted := keys.duplicate()
	sorted.sort()
	T.eq(keys, sorted, "topics come in file-name order")

	var tier2 := Vectors.load_tier2(T.vectors_dir)
	T.ok(tier2.size() >= 100, "the tier2 scenarios load (%d)" % tier2.size())
	var paths := []
	for s: Variant in tier2:
		var path: String = s["path"]
		var v: Dictionary = s["vector"]
		T.ok(path.begins_with("tier2/") and path.ends_with(".json"), "path %s" % path)
		T.eq(v.get("id"), path.trim_prefix("tier2/").trim_suffix(".json"), "id matches the path")
		T.ok(v.get("steps") is Array, "%s has steps" % path)
		paths.append(path)
	var sorted_paths := paths.duplicate()
	sorted_paths.sort()
	T.eq(paths, sorted_paths, "scenarios come in path order")
	# quiet: these failures are deliberate, and a passing run must print no ERROR lines
	T.eq(Vectors.load_tier1("/nonexistent/vectors", true), {}, "a missing directory loads nothing")
	T.eq(Vectors.load_tier2("/nonexistent/vectors", true), [], "a missing tier2 directory loads nothing")
	T.eq(Vectors.load_topic("/nonexistent/vectors", "keccak", true), [], "a missing topic file loads nothing")
	T.eq(Vectors.load_topic(T.vectors_dir, "no_such_topic", true), [], "an unknown topic loads nothing")
	return T.done()


## `bytes.fromhex` as Python 3.12 has it: ASCII whitespace (space, \t \n \r \v \f) is skipped
## between bytes, never inside a pair; no `0x` prefix; non-ASCII anything is an error. Every
## expectation below was checked against CPython.
func test_bytes_fromhex_mirrors_python() -> bool:
	var good := {
		"": [], "  ": [], "0a0b": [10, 11], "0A0b": [10, 11], "0a 0b": [10, 11], " 0a0b ": [10, 11],
		"\n0a\t0b\r": [10, 11], "0a\u000b0b": [10, 11], "0a\u000c": [10], "00ff": [0, 255],
	}
	for text: String in good:
		T.eq(Tier1.bytes_fromhex(text), PackedByteArray(good[text]), "fromhex(%s)" % ScfgPy.py_repr(text))
	for bad: String in ["0x0a", "0X0a", "0a0", "0 a", "a b", "\u001f0a", "0a\u00a00b", "0a\u00850b", "zz", "٣٣", "0a-1"]:
		T.is_err(Tier1.bytes_fromhex(bad), "ValueError", "fromhex(%s)" % ScfgPy.py_repr(bad))
	for not_str: Variant in [null, 5, 1.5, true, [], {}]:
		T.is_err(Tier1.bytes_fromhex(not_str), "TypeError", "fromhex(%s)" % ScfgPy.py_repr(not_str))
	# the adapters use it: a prefix is invalid for the Python adapter, so for this one too
	T.is_err(Tier1.FUNCS["keccak256"].call({"data_hex": "0x00"}), "ValueError", "keccak256 rejects 0x")
	T.eq(Tier1.FUNCS["keccak256"].call({"data_hex": "61 62 63"}),
		"4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45", "keccak256 skips whitespace")
	T.is_err(Tier1.FUNCS["keccak256"].call({}), "KeyError", "a missing field is a KeyError, as in Python")
	T.is_err(Tier1.FUNCS["keccak256"].call({"data_hex": 5}), "TypeError", "a non-string field")
	return T.done()


## The compact_u16 adapter: every number in a vector is a Python int (the canonical writer emits
## integral numbers only, as ints), so an integral float is that int; a fractional number would
## be a Python float, which the core rejects as Python does (TypeError in range, else ValueError).
func test_compact_u16_adapter_mirrors_python_numbers() -> bool:
	var f: Callable = Tier1.FUNCS["compact_u16"]
	T.eq(f.call({"n": 2.0}), "02", "a JSON 2 is Python's int 2")
	T.eq(f.call({"n": 65535.0}), "ffff03", "the largest")
	T.is_err(f.call({"n": 2.5}), "TypeError", "a fractional number is a float: 2.5 & 0x7F")
	T.is_err(f.call({"n": 70000.5}), "ValueError", "a float out of range fails the range check first")
	T.is_err(f.call({"n": float(1 << 60)}), "ValueError", "a huge integral number is a huge int: out of range")
	T.is_err(f.call({"n": "1"}), "TypeError", "a str")
	return T.done()


## The configuration adapters' input rules (tier1_config.py): `{"float": "<repr>"}` rebuilds a
## Python float, `dict(kwargs)` accepts what dict() accepts, and to_jsonable enforces the vector
## rules.
func test_config_adapters_mirror_python_input_rules() -> bool:
	T.eq(Tier1._value({"float": "4.99"}), 4.99, "a float")
	T.eq(typeof(Tier1._value({"float": "5.0"})), TYPE_FLOAT, "an integral float stays a float, as in Python")
	T.eq(Tier1._value({"float": "1e2"}), 100.0, "exponent")
	T.eq(Tier1._value({"float": " 1_0.5 "}), 10.5, "whitespace and underscores")
	T.eq(Tier1._value({"float": "\u0663.5"}), 3.5, "any script's digits")
	T.eq(Tier1._value({"float": "-inf"}), -INF, "-inf")
	T.ok(is_nan(Tier1._value({"float": "NaN"})), "nan")
	T.eq(Tier1._value({"float": 9}), 9.0, "float(9)")
	T.eq(Tier1._value({"float": true}), 1.0, "float(True)")
	for bad: Variant in ["", "abc", "1__0", "_1", "1_", "1.5.5", "0x10", "1e", ".", "e5", "--1"]:
		T.is_err(Tier1._value({"float": bad}), "ValueError", "float(%s)" % ScfgPy.py_repr(bad))
	T.is_err(Tier1._value({"float": null}), "TypeError", "float(None)")
	T.is_err(Tier1._value({"float": [1]}), "TypeError", "float([1])")
	T.eq(Tier1._value({"float": "1", "x": 1}), {"float": "1", "x": 1}, "only a lone `float` key is the encoding")
	T.eq(Tier1._value("x"), "x", "everything else is unchanged")
	T.eq(Tier1._dict_of([]), {}, "dict([])")
	T.eq(Tier1._dict_of(""), {}, "dict('')")
	T.eq(Tier1._dict_of([["a", 1]]), {"a": 1}, "dict([['a', 1]])")
	T.is_err(Tier1._dict_of(["base"]), "ValueError", "dict(['base']): length 4")
	T.is_err(Tier1._dict_of([5]), "TypeError", "dict([5])")
	T.is_err(Tier1._dict_of("ab"), "ValueError", "dict('ab')")
	T.is_err(Tier1._dict_of(5), "TypeError", "dict(5)")
	# to_jsonable
	var out: Variant = Tier1.to_jsonable({"a": ScfgDec.parse("4.990"), "b": [1, 2.0, "x", null, true], "c": {"d": 3}})  # err-ok: literal
	T.eq(str(out), str({"a": "4.990", "b": [1, 2, "x", null, true], "c": {"d": 3}}), "Decimals are strings, integral floats ints")
	T.eq(typeof((out as Dictionary)["b"][1]), TYPE_INT, "2.0 is the int 2")
	T.is_err(Tier1.to_jsonable(1.5), "ValueError", "a non-integral float")
	T.is_err(Tier1.to_jsonable([NAN]), "ValueError", "nan")
	T.is_err(Tier1.to_jsonable(1 << 53), "ValueError", "2**53")
	T.eq(Tier1.to_jsonable((1 << 53) - 1), (1 << 53) - 1, "2**53 - 1")
	T.is_err(Tier1.to_jsonable(ScfgBigInt.from_decimal("18446744073709551616")), "ValueError", "a bigint")  # err-ok: literal
	T.is_err(Tier1.to_jsonable({1: 2}), "TypeError", "a non-string key")
	T.is_err(Tier1.to_jsonable(PackedByteArray([1])), "TypeError", "bytes")
	T.is_err(Tier1.to_jsonable(Vector2(1, 2)), "TypeError", "any other type")
	return T.done()
