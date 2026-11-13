extends RefCounted

const T := preload("res://tests/t.gd")


func test_hex_to_bytes() -> bool:
	T.eq(ScfgBytes.hex_to_bytes("0a0B"), PackedByteArray([10, 11]), "either case")
	T.eq(ScfgBytes.hex_to_bytes("0x0aff"), PackedByteArray([10, 255]), "optional 0x")
	T.eq(ScfgBytes.hex_to_bytes(""), PackedByteArray(), "empty")
	T.eq(ScfgBytes.hex_to_bytes("0x"), PackedByteArray(), "0x alone is empty")
	for bad: String in ["abc", "0x0", "zz", "0g", "0X0a", " 0a", "0a ", "0a 0b", "٣٣"]:
		T.is_err(ScfgBytes.hex_to_bytes(bad), "ValueError", "hex_to_bytes(%s)" % ScfgPy.py_repr(bad))
	T.is_err(ScfgBytes.hex_to_bytes(null), "TypeError", "hex_to_bytes(None)")
	T.is_err(ScfgBytes.hex_to_bytes(10), "TypeError", "hex_to_bytes(10)")
	return T.done()


func test_hex_rejects_odd_and_non_hex() -> bool:
	for odd: String in ["a", "abc", "0x0", "0x123", "0123456"]:
		var r: Variant = ScfgBytes.hex_to_bytes(odd)
		T.is_err(r, "ValueError", "odd length %s" % ScfgPy.py_repr(odd))
		T.ok(r is ScfgErr and "odd" in (r as ScfgErr).message, "the message says odd length")
	for bad: String in ["zz", "0xzz", "0g", "g0", "12 34", "12\n", "\t12", "0x0x", "0xx1", "--", "1z", "é1", "١٢"]:
		var r: Variant = ScfgBytes.hex_to_bytes(bad)
		T.is_err(r, "ValueError", "non-hex %s" % ScfgPy.py_repr(bad))
	for not_str: Variant in [null, 5, 1.5, true, [], {}, PackedByteArray([1])]:
		T.is_err(ScfgBytes.hex_to_bytes(not_str), "TypeError", "non-string %s" % ScfgPy.py_repr(not_str))
	return T.done()


func test_bytes_to_hex_and_back() -> bool:
	var b := PackedByteArray([0, 1, 127, 128, 255])
	T.eq(ScfgBytes.bytes_to_hex(b), "00017f80ff", "lowercase, no prefix")
	T.eq(ScfgBytes.bytes_to_hex(PackedByteArray()), "", "empty")
	T.eq(ScfgBytes.hex_to_bytes(ScfgBytes.bytes_to_hex(b)), b, "round trip")
	return T.done()


func test_utf8_and_base64() -> bool:
	T.eq(ScfgBytes.utf8("aé€"), PackedByteArray([0x61, 0xc3, 0xa9, 0xe2, 0x82, 0xac]), "utf8")
	T.eq(ScfgBytes.utf8(""), PackedByteArray(), "utf8 of empty")
	T.eq(ScfgBytes.utf8(String.chr(0x1f600)), PackedByteArray([0xf0, 0x9f, 0x98, 0x80]), "astral plane")
	# python3 -c "import base64; print(base64.b64encode(b'hello'), base64.b64encode(b'\xff\x00'))"
	T.eq(ScfgBytes.base64("hello".to_ascii_buffer()), "aGVsbG8=", "base64")
	T.eq(ScfgBytes.base64(PackedByteArray([255, 0])), "/wA=", "base64 of binary")
	T.eq(ScfgBytes.base64(PackedByteArray()), "", "base64 of empty")
	T.eq(ScfgBytes.concat([PackedByteArray([1]), PackedByteArray(), PackedByteArray([2, 3])]),
		PackedByteArray([1, 2, 3]), "concat")
	T.eq(ScfgBytes.concat([]), PackedByteArray(), "concat of nothing")
	return T.done()
