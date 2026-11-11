extends RefCounted

const T := preload("res://tests/t.gd")


func _d(s: String) -> ScfgDec:
	var v: Variant = ScfgDec.parse(s)
	return v if v is ScfgDec else null


func _s(v: Variant) -> String:
	if v is ScfgErr:
		return "<%s>" % str(v)
	if v is ScfgBigInt:
		return (v as ScfgBigInt).to_decimal()
	return str(v)


# tests/unit/test_amounts.py::test_parse_amount_accepts
func test_parse_amount_accepts() -> bool:
	for row: Array in [["4.99", "4.99"], ["3", "3.00"], [" 2.5 ", "2.50"], ["4.990", "4.99"],
			[_d("1.10"), "1.10"], [7, "7.00"]]:
		var got: Variant = ScfgAmounts.parse_amount(row[0])
		T.eq(_s(got), row[1], "parse_amount(%s)" % ScfgPy.py_repr(row[0]))
		if got is ScfgDec:
			T.eq((got as ScfgDec).exponent, -2, "always quantized to cents")
	T.eq(_s(ScfgAmounts.parse_amount(ScfgBigInt.from_decimal("12"))), "12.00", "an ScfgBigInt is an int")
	T.eq(_s(ScfgAmounts.parse_amount(" 5　")), "5.00", "str.strip() whitespace")
	T.eq(_s(ScfgAmounts.parse_amount(&"2")), "2.00", "a StringName is a str")
	return T.done()


# tests/unit/test_amounts.py::test_parse_amount_rejects_types
func test_parse_amount_rejects_types() -> bool:
	for value: Variant in [4.99, 5.0, true, null, [1], {}]:
		T.is_err(ScfgAmounts.parse_amount(value), "TypeError", "parse_amount(%s)" % ScfgPy.py_repr(value))
	return T.done()


# tests/unit/test_amounts.py::test_parse_amount_rejects_values
func test_parse_amount_rejects_values() -> bool:
	for value: Variant in ["", "abc", "4.999", "0", "-1", "0.00", "NaN", "Infinity", "1000000.01", "sNaN",
			"-Infinity", "1,000", 0, -3, 1000001, ScfgBigInt.from_decimal("100000000000000000000"),
			"1_000", "4_99", "1_0.00", "_5", "5_", "1__000", &"1_000"]:
		T.is_err(ScfgAmounts.parse_amount(value), "ValueError", "parse_amount(%s)" % ScfgPy.py_repr(value))
	return T.done()


# tests/unit/test_amounts.py::test_encode_spec_example
func test_encode_spec_example() -> bool:
	T.eq(ScfgAmounts.encode(_d("4.99"), 37, 42), 4993742, "4.99 | 37 | 42")
	return T.done()


# tests/unit/test_amounts.py::test_encode_tip_uses_code_zero
func test_encode_tip_uses_code_zero() -> bool:
	T.eq(ScfgAmounts.encode("3.00", 0, 5), 3000005, "a tip")
	T.eq(ScfgAmounts.encode("1000000", 99, 99), 1000000009999, "the largest amount")
	return T.done()


# tests/unit/test_amounts.py::test_decode_spec_example
func test_decode_spec_example() -> bool:
	var got: Variant = ScfgAmounts.decode(4993742)
	T.ok(got is Array and got.size() == 3, "a (price, code, nonce) triple")
	if got is Array:
		T.eq([_s(got[0]), got[1], got[2]], ["4.99", 37, 42], "decode(4993742)")
	got = ScfgAmounts.decode(true)
	T.eq([_s(got[0]), got[1], got[2]], ["0.00", 0, 1], "a bool is an int")
	return T.done()


# tests/unit/test_amounts.py::test_encode_rejects_bad_fields
func test_encode_rejects_bad_fields() -> bool:
	for row: Array in [[-1, 0], [100, 0], [0, -1], [0, 100], [true, 0], ["1", 0], [0, null], [1.0, 0]]:
		T.is_err(ScfgAmounts.encode("1.00", row[0], row[1]), "ValueError", "encode(1.00, %s, %s)" % [
			ScfgPy.py_repr(row[0]), ScfgPy.py_repr(row[1])])
	return T.done()


# tests/unit/test_amounts.py::test_encode_validates_price
func test_encode_validates_price() -> bool:
	T.is_err(ScfgAmounts.encode("4.999", 1, 1), "ValueError", "3 decimals")
	T.is_err(ScfgAmounts.encode(4.99, 1, 1), "TypeError", "a float price")
	return T.done()


# tests/unit/test_amounts.py::test_encode_decode_round_trip (a fixed sample of the hypothesis space)
func test_encode_decode_round_trip() -> bool:
	for cents: int in [1, 2, 99, 100, 101, 499, 12345, 99999999, 100000000]:
		var price := _d("%d.%02d" % [cents / 100, cents % 100])
		for cn: Array in [[0, 0], [99, 99], [37, 42], [1, 0], [0, 1], [50, 7]]:
			var units: Variant = ScfgAmounts.encode(price, cn[0], cn[1])
			var back: Variant = ScfgAmounts.decode(units)
			T.ok(back is Array and (back[0] as ScfgDec).cmp(price) == 0 and back[1] == cn[0] and back[2] == cn[1],
				"round trip %s %s" % [str(price), cn])
	return T.done()


# tests/unit/test_amounts.py::test_encoded_amount_never_exceeds_price_by_a_cent (fixed sample)
func test_encoded_amount_never_exceeds_price_by_a_cent() -> bool:
	var cent := _d("0.01")
	for cents: int in [1, 99, 100, 499, 100000000]:
		var price := _d("%d.%02d" % [cents / 100, cents % 100])
		for cn: Array in [[0, 0], [99, 99], [37, 42]]:
			var paid: Variant = ScfgAmounts.units_to_decimal(ScfgAmounts.encode(price, cn[0], cn[1]))
			T.ok(paid is ScfgDec and price.cmp(paid) <= 0 and (paid as ScfgDec).cmp(price.add(cent)) < 0,
				"%s with %s pays %s" % [str(price), cn, _s(paid)])
	return T.done()


# tests/unit/test_amounts.py::test_units_conversions
func test_units_conversions() -> bool:
	T.eq(_s(ScfgAmounts.units_to_decimal(4993742)), "4.993742", "units_to_decimal")
	T.eq(ScfgAmounts.decimal_to_units(_d("4.993742")), 4993742, "decimal_to_units")
	T.eq(ScfgAmounts.decimal_to_units(_d("5")), 5000000, "a whole amount")
	T.eq(_s(ScfgAmounts.units_to_decimal(0)), "0.000000", "zero keeps 6 decimals")
	T.eq(_s(ScfgAmounts.units_to_decimal(ScfgBigInt.from_decimal("18446744073709551616"))),
		"18446744073709.551616", "2^64 units are exact")
	T.eq(ScfgAmounts.format_units(4993742), "4.993742", "format_units")
	T.eq(ScfgAmounts.format_units(-1), "-0.000001", "a negative amount")
	T.eq(ScfgAmounts.format_units(ScfgBigInt.from_decimal("10000000000000000000000000000000000000001")),
		"10000000000000000000000000000000000.000001", "exact at any size")
	# exact past Decimal's default 28 significant digits: no rounding (Python builds it from the digits)
	T.eq(_s(ScfgAmounts.units_to_decimal(ScfgBigInt.from_decimal("10000000000000000000000000001"))),
		"10000000000000000000000.000001", "29 digits, exact")
	var max256: ScfgBigInt = ScfgBigInt.from_decimal("115792089237316195423570985008687907853269984665640564039457584007913129639935")
	T.eq(_s(ScfgAmounts.units_to_decimal(max256)),
		"115792089237316195423570985008687907853269984665640564039457584007913129.639935", "2^256 - 1, exact")
	T.eq(_s(ScfgAmounts.units_to_decimal(-123)), "-0.000123", "a negative int")
	# tests/unit/test_amounts.py::test_decimal_to_units_is_exact: no rounding to 28 digits
	T.eq(_s(ScfgAmounts.decimal_to_units(_d("12345678901234567890123456789012"))),
		"12345678901234567890123456789012000000", "a 32-digit amount, exact")
	T.eq(_s(ScfgAmounts.decimal_to_units("1.2345678901234567890123456789012345E+30")),
		"1234567890123456789012345678901234500", "35 significant digits, exact")
	T.eq(ScfgAmounts.decimal_to_units("4.9900000000000000000000000000000000"), 4990000, "trailing zeros")
	T.is_err(ScfgAmounts.decimal_to_units("0.000001000000000000000000000000001"), "ValueError", "7th decimal nonzero")
	T.eq(_s(ScfgAmounts.decimal_to_units("1E+30")), "1000000000000000000000000000000000000", "Decimal(str)")
	T.is_err(ScfgAmounts.units_to_decimal("5"), "TypeError", "units are an int")
	return T.done()


# tests/unit/test_amounts.py::test_decimal_to_units_rejects_sub_unit_precision
func test_decimal_to_units_rejects_sub_unit_precision() -> bool:
	T.is_err(ScfgAmounts.decimal_to_units(_d("0.0000001")), "ValueError", "7 decimals")
	T.is_err(ScfgAmounts.decimal_to_units("NaN"), "ValueError", "NaN != NaN: 'more than 6 decimals'")
	T.is_err(ScfgAmounts.decimal_to_units("Infinity"), "ValueError", "not finite: out of range")
	T.is_err(ScfgAmounts.decimal_to_units("abc"), "InvalidOperation", "Decimal('abc')")
	return T.done()


# tests/unit/test_amounts.py::test_decimal_to_units_rejects_amounts_out_of_range
# A nonzero amount's adjusted exponent must be within +-40: refused at once, never a huge integer.
func test_decimal_to_units_rejects_amounts_out_of_range() -> bool:
	for amount: String in ["1E+41", "-1E+41", "1E+999993", "1E+999999", "Infinity", "-Infinity", "NaN", "1E-41",
			"9.99E+41"]:
		var start := Time.get_ticks_msec()
		T.is_err(ScfgAmounts.decimal_to_units(amount), "ValueError", amount)
		T.ok(Time.get_ticks_msec() - start < 2000, "%s is refused quickly" % amount)
	return T.done()


# tests/unit/test_amounts.py::test_decimal_to_units_range_edges
func test_decimal_to_units_range_edges() -> bool:
	T.eq(_s(ScfgAmounts.decimal_to_units("1E+40")), "1" + "0".repeat(46), "1E+40")
	T.eq(_s(ScfgAmounts.decimal_to_units("0E+999999")), "0", "a zero has no magnitude to bound")
	T.eq(ScfgAmounts.decimal_to_units("0.000001"), 1, "one unit")
	return T.done()


# tests/unit/test_amounts.py::test_stored_amount_reads_numbers_in_range
func test_stored_amount_reads_numbers_in_range() -> bool:
	for row: Array in [["4.990000", "4.990000"], [" 5 ", "5"], [5, "5"], ["0", "0"], ["0E-999999", "0E-999999"],
			["1E+40", "1E+40"], ["-1.5E-40", "-1.5E-40"], [1.5, "1.5"], [true, "1"]]:
		T.eq(_s(ScfgAmounts.stored_amount(row[0])), row[1], "stored_amount(%s)" % ScfgPy.py_repr(row[0]))
	return T.done()


# tests/unit/test_amounts.py::test_amount_text_is_bounded: AMOUNT_TEXT_LIMIT (100 characters) is
# checked before the digits are parsed (GDScript parses them in quadratic time).
func test_amount_text_is_bounded() -> bool:
	T.eq(_s(ScfgAmounts.stored_amount("4.99" + "0".repeat(96))), "4.99" + "0".repeat(96), "100 characters")
	T.eq(_s(ScfgAmounts.parse_amount("4.99" + "0".repeat(96))), "4.99", "100 characters")
	for text: String in ["4.99" + "0".repeat(97), "9".repeat(100000), "0." + "9".repeat(100000), " ".repeat(99) + "5 "]:
		var start := Time.get_ticks_msec()
		T.is_err(ScfgAmounts.stored_amount(text), "ValueError", "stored_amount, %d characters" % text.length())
		T.is_err(ScfgAmounts.parse_amount(text), "ValueError", "parse_amount, %d characters" % text.length())
		T.ok(Time.get_ticks_msec() - start < 2000, "refused at once")
	return T.done()


# tests/unit/test_amounts.py::test_stored_amount_refuses_lists_and_tuples
func test_stored_amount_refuses_lists_and_tuples() -> bool:
	for value: Variant in [[0, [2, 5], -1], [1], []]:
		T.is_err(ScfgAmounts.stored_amount(value), "TypeError", ScfgPy.py_repr(value))
	return T.done()


# tests/unit/test_amounts.py::test_stored_amount_refuses_others
func test_stored_amount_refuses_others() -> bool:
	for value: Variant in ["1E+41", "1E-41", "1E+999999", "1E+9999999999999999", "Infinity", "NaN", "sNaN", "abc", "",
			null, [1], {}, 1e300, INF]:
		var start := Time.get_ticks_msec()
		var got: Variant = ScfgAmounts.stored_amount(value)
		T.ok(got is ScfgErr and ((got as ScfgErr).is_a("ValueError") or (got as ScfgErr).is_a("TypeError")),
			"stored_amount(%s): %s" % [ScfgPy.py_repr(value), str(got)])
		T.ok(Time.get_ticks_msec() - start < 2000, "quickly")
	return T.done()


## Python's decode raises decimal.InvalidOperation (from quantize, not a ValueError) once the
## price needs more than 28 digits: from units >= 10**32 (checked with CPython).
func test_decode_beyond_precision_is_invalid_operation() -> bool:
	var below: Variant = ScfgAmounts.decode(ScfgBigInt.from_decimal("99999999999999999999999999999999"))
	T.ok(below is Array, "10**32 - 1 decodes")
	if below is Array:
		T.eq([_s(below[0]), below[1], below[2]], ["99999999999999999999999999.99", 99, 99], "Python's answer")
	var over: Variant = ScfgAmounts.decode(ScfgBigInt.from_decimal("100000000000000000000000000000000"))
	T.is_err(over, "InvalidOperation", "10**32")
	T.ok(over is ScfgErr and not (over as ScfgErr).is_a("ValueError"), "InvalidOperation is not a ValueError")
	T.is_err(ScfgAmounts.decode(-1), "ValueError", "negative units")
	T.is_err(ScfgAmounts.decode("5"), "TypeError", "units are an int")
	return T.done()
