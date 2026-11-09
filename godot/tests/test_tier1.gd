extends RefCounted

const T := preload("res://tests/t.gd")
const Vectors := preload("res://tests/vectors.gd")


func test_keccak_vectors() -> bool:
	var cases := Vectors.load_topic(T.vectors_dir, "keccak")
	T.ok(cases.size() > 0, "keccak.json has cases")
	var ran := 0
	for c: Dictionary in cases:
		T.eq(c["fn"], "keccak256", "keccak case fn")
		var data_hex: String = c["in"]["data_hex"]
		var got := ScfgKeccak.keccak256(data_hex.hex_decode()).hex_encode()
		T.eq(got, c["out"], "keccak256(%s)" % data_hex)
		ran += 1
	T.eq(ran, cases.size(), "every keccak case ran")
	return T.done()


func test_ed25519_vectors() -> bool:
	var cases := Vectors.load_topic(T.vectors_dir, "ed25519")
	T.ok(cases.size() > 0, "ed25519.json has cases")
	var ran := 0
	for c: Dictionary in cases:
		T.eq(c["fn"], "is_on_curve", "ed25519 case fn")
		var point_hex: String = c["in"]["point_hex"]
		T.eq(ScfgEd25519.is_on_curve(point_hex.hex_decode()), c["out"], "is_on_curve(%s)" % point_hex)
		ran += 1
	T.eq(ran, cases.size(), "every ed25519 case ran")
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
			T.eq(ScfgBigInt.from_decimal(c["in"]["units"]).to_hex(), "10000000000000000", "2**64 exactly")
	T.ok(found, "amounts.json has the units_to_decimal case for 2**64")
	T.eq(ScfgBigInt.from_decimal("9007199254740993").to_decimal(), "9007199254740993",
		"2**53 + 1, which a float cannot hold")
	T.ok(float("9007199254740993") == 9007199254740992.0, "(sanity: a float would round it)")
	return T.done()
