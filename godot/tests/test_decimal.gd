extends RefCounted

const T := preload("res://tests/t.gd")

## Every row is [op, args..., what CPython gives] and was produced by CPython 3.12's decimal
## module (the generator: for each row, run the op on `Decimal(arg)` and print `str()` of the
## result, "InvalidOperation"/"Overflow" when Python raises that, an int for cmp, a bool for
## integral). The ops:
##   str s | add a b | sub a b | mul a b | mul_int a n | scaleb a n | quantize a exp
##   units n (Decimal(int(n)).scaleb(-6)) | float repr (Decimal(float(repr)))
##   float_bits n (Decimal of the double whose IEEE-754 bits are the int n)
##   fmt a places (format(a, ".<places>f")) | cmp a b | int a (int(Decimal(a))) | integral a
const ROWS := [
		["str", "4.99", "4.99"],
		["units", "4990000", "4.990000"],
		["units", "0", "0.000000"],
		["add", "1.5", "0", "1.5"],
		["str", "1E+1", "1E+1"],
		["str", "0.0000001", "1E-7"],
		["str", "1_000", "1000"],
		["str", "-0", "-0"],
		["str", "0E-7", "0E-7"],
		["str", "0E+3", "0E+3"],
		["str", "123E5", "1.23E+7"],
		["str", "0.000001", "0.000001"],
		["str", "-0.00", "-0.00"],
		["add", "1.000000", "0.5", "1.500000"],
		["add", "0.00", "0", "0.00"],
		["add", "-0", "-0", "-0"],
		["sub", "1", "1", "0"],
		["units", "-5000000", "-5.000000"],
		["mul_int", "0", -1, "-0"],
		["mul_int", "1.10", 100, "110.00"],
		["str", "007", "7"],
		["str", "5.", "5"],
		["str", "+.5", "0.5"],
		["str", "1_0.5_5", "10.55"],
		["str", " 5", "5"],
		["str", " 5\n", "5"],
		["str", "\uff15", "5"],
		["str", "\u0661\u0662", "12"],
		["str", "1e5_0", "1E+50"],
		["str", "1__0", "10"],
		["str", "_1", "1"],
		["str", "1_", "1"],
		["str", "\u001c1", "1"],
		["str", "\u00a05\u3000", "5"],
		["str", "5_\n", "5"],
		["str", "\u00a0_5", "5"],
		["str", "1e\u0663", "1E+3"],
		["str", "-.5e-3", "-0.0005"],
		["str", "1_e1", "1E+1"],
		["str", "1e0000000000000000000000005", "1E+5"],
		["str", "1e-0000000000000000000000005", "0.00001"],
		["str", "\u0663.\u0665", "3.5"],
		["str", "1E+999999999999999999", "1E+999999999999999999"],
		["str", "1234e999999999999999996", "1.234E+999999999999999999"],
		["str", "0010e999999999999999998", "1.0E+999999999999999999"],
		["str", "00e999999999999999999", "0E+999999999999999999"],
		["str", "0.0000e1000000000000000003", "0E+999999999999999999"],
		["str", "1E-1999999999999999997", "1E-1999999999999999997"],
		["str", "0.00100e-1999999999999999990", "1.00E-1999999999999999993"],
		["str", "12345678901234567890123456789012", "12345678901234567890123456789012"],
		["str", "1.2345678901234567890123456789012", "1.2345678901234567890123456789012"],
		["str", "-000.000", "-0.000"],
		["str", "5\n_", "InvalidOperation"],
		["str", "5 _", "InvalidOperation"],
		["str", "_ 1", "InvalidOperation"],
		["str", "1 0", "InvalidOperation"],
		["str", "", "InvalidOperation"],
		["str", " ", "InvalidOperation"],
		["str", ".", "InvalidOperation"],
		["str", "1e", "InvalidOperation"],
		["str", "1E+", "InvalidOperation"],
		["str", "e5", "InvalidOperation"],
		["str", ".e1", "InvalidOperation"],
		["str", "abc", "InvalidOperation"],
		["str", "0x10", "InvalidOperation"],
		["str", "infin", "InvalidOperation"],
		["str", "1.2.3", "InvalidOperation"],
		["str", "--1", "InvalidOperation"],
		["str", "NaN1x", "InvalidOperation"],
		["str", "_", "InvalidOperation"],
		["str", "\u200b1", "InvalidOperation"],
		["str", "\ufeff1", "InvalidOperation"],
		["str", "1,000", "InvalidOperation"],
		["str", "1e1000000000000000000", "InvalidOperation"],
		["str", "10e999999999999999999", "InvalidOperation"],
		["str", "1234e999999999999999997", "InvalidOperation"],
		["str", "0e1000000000000000000", "InvalidOperation"],
		["str", "0.000e1000000000000000003", "InvalidOperation"],
		["str", "1.5e-1999999999999999997", "InvalidOperation"],
		["str", "1e-1999999999999999998", "InvalidOperation"],
		["str", "1e99999999999999999999", "InvalidOperation"],
		["str", "1e+9000000000000000000", "InvalidOperation"],
		["str", "1e-9000000000000000000", "InvalidOperation"],
		["str", "\u00e9", "InvalidOperation"],
		["add", "10000000000000000000000000000", "1", "1.000000000000000000000000000E+28"],
		["add", "10000000000000000000000000000", "5", "1.000000000000000000000000000E+28"],
		["add", "10000000000000000000000000000", "15", "1.000000000000000000000000002E+28"],
		["add", "9999999999999999999999999999", "5", "1.000000000000000000000000000E+28"],
		["add", "0E-100", "1", "1.000000000000000000000000000"],
		["add", "0E-40", "5", "5.000000000000000000000000000"],
		["add", "1E+40", "1E-40", "1.000000000000000000000000000E+40"],
		["sub", "1E+40", "1E-40", "1.000000000000000000000000000E+40"],
		["add", "9999999999999999999999999999", "0.5", "1.000000000000000000000000000E+28"],
		["add", "9999999999999999999999999998", "0.5", "9999999999999999999999999998"],
		["add", "9999999999999999999999999997", "0.5", "9999999999999999999999999998"],
		["add", "1.5", "-1.5", "0.0"],
		["add", "-1.5", "1.5", "0.0"],
		["add", "-1.5", "-1.5", "-3.0"],
		["add", "-1.5", "0.25", "-1.25"],
		["add", "0.25", "-1.5", "-1.25"],
		["add", "-0", "0", "0"],
		["add", "-0.0", "-0.00", "-0.00"],
		["add", "-0E+5", "5", "5"],
		["add", "0E+5", "5.5", "5.5"],
		["sub", "0", "0", "0"],
		["sub", "-0", "0", "-0"],
		["sub", "1.00", "1", "0.00"],
		["sub", "5", "7.25", "-2.25"],
		["add", "1", "1E-1999999999999999997", "1.000000000000000000000000000"],
		["add", "1E-1999999999999999997", "2E-1999999999999999997", "0E-1000026"],
		["add", "1E-1000030", "0", "0E-1000026"],
		["add", "-1E-1000030", "0", "-0E-1000026"],
		["add", "1.5E-1000026", "0", "2E-1000026"],
		["add", "0E-1000030", "0", "0E-1000026"],
		["add", "123456789E-1000030", "0", "1.2346E-1000022"],
		["add", "12345678901234567890123456789012", "0", "1.234567890123456789012345679E+31"],
		["add", "1.2345678901234567890123456789012", "1E-40", "1.234567890123456789012345679"],
		["add", "4.990000", "3.000000", "7.990000"],
		["add", "0.5", "10.25", "10.75"],
		["mul_int", "-7", 3, "-21"],
		["mul_int", "-2.5", -2, "5.0"],
		["mul_int", "1E+1", 0, "0E+1"],
		["mul_int", "2E-1", 0, "0.0"],
		["mul_int", "-0", 5, "-0"],
		["mul_int", "1234567890123456789012345", 10000, "1.234567890123456789012345000E+28"],
		["mul_int", "1234567890123456789012345678", 3, "3703703670370370367037037034"],
		["mul_int", "4.99", 100, "499.00"],
		["mul", "0E+1000000", "1", "0E+999999"],
		["mul", "1.5", "1.5", "2.25"],
		["mul", "-1", "0.00", "-0.00"],
		["scaleb", "18446744073709551616", -6, "18446744073709.551616"],
		["scaleb", "1000000000000000000000000000000", -6, "1000000000000000000000000.000"],
		["scaleb", "10000000000000000000000000000000000000000", -6, "1.000000000000000000000000000E+34"],
		["scaleb", "10000000000000000000000000000000000000001", -6, "1.000000000000000000000000000E+34"],
		["scaleb", "5", -1000027, "0E-1000026"],
		["scaleb", "15", -1000027, "2E-1000026"],
		["scaleb", "25", -1000027, "2E-1000026"],
		["scaleb", "251", -1000028, "3E-1000026"],
		["scaleb", "1", 0, "1"],
		["scaleb", "12345678901234567890123456789012", 6, "1.234567890123456789012345679E+37"],
		["scaleb", "0.12345678901234567890123456789012", 6, "123456.7890123456789012345679"],
		["scaleb", "1", 2000055, "InvalidOperation"],
		["scaleb", "1", -2000055, "InvalidOperation"],
		["scaleb", "1", 1000000, "Overflow"],
		["scaleb", "1E+999999", 1, "Overflow"],
		["scaleb", "0", 2000054, "0E+999999"],
		["scaleb", "0", -2000054, "0E-1000026"],
		["scaleb", "999999999999999999999999999999", 999970, "Overflow"],
		["quantize", "-0.001", -2, "-0.00"],
		["quantize", "0.005", -2, "0.00"],
		["quantize", "0.015", -2, "0.02"],
		["quantize", "0.025", -2, "0.02"],
		["quantize", "1E+1", -2, "10.00"],
		["quantize", "0.999", -2, "1.00"],
		["quantize", "1E-30", -2, "0.00"],
		["quantize", "0.5", 0, "0"],
		["quantize", "1.5", 0, "2"],
		["quantize", "2.5", 0, "2"],
		["quantize", "15", 1, "2E+1"],
		["quantize", "25", 1, "2E+1"],
		["quantize", "999999999999999999999999999.5", 0, "1000000000000000000000000000"],
		["quantize", "0E+30", -2, "0.00"],
		["quantize", "-0E+30", -2, "-0.00"],
		["quantize", "0.0049999", -2, "0.00"],
		["quantize", "0.0050001", -2, "0.01"],
		["quantize", "1E-1999999999999999997", -2, "0.00"],
		["quantize", "4.990", -2, "4.99"],
		["quantize", "1E+30", -2, "InvalidOperation"],
		["quantize", "999999999999999999999999999", -2, "InvalidOperation"],
		["quantize", "1E+999999", -2, "InvalidOperation"],
		["quantize", "1", -1000027, "InvalidOperation"],
		["quantize", "1", 1000000, "InvalidOperation"],
		["quantize", "99999999999999999999999999.995", -2, "InvalidOperation"],
		["quantize", "1E+999999999999999999", 0, "InvalidOperation"],
		["quantize", "0", -1000026, "0E-1000026"],
		["quantize", "5", 999999, "0E+999999"],
		["float", "0.1", "0.1000000000000000055511151231257827021181583404541015625"],
		["float", "1.5", "1.5"],
		["float", "-2.5", "-2.5"],
		["float", "4.0", "4"],
		["float", "-7.0", "-7"],
		["float", "-0.0", "-0"],
		["float", "0.0", "0"],
		["float", "1180591620717411303424.0", "1180591620717411303424"],
		["float_bits", "1", "4.940656458412465441765687928682213723650598026143247644255856825006755072702087518652998363616359923797965646954457177309266567103559397963987747960107818781263007131903114045278458171678489821036887186360569987307230500063874091535649843873124733972731696151400317153853980741262385655911710266585566867681870395603106249319452715914924553293054565444011274801297099995419319894090804165633245247571478690147267801593552386115501348035264934720193790268107107491703332226844753335720832431936092382893458368060106011506169809753078342277318329247904982524730776375927247874656084778203734469699533647017972677717585125660551199131504891101451037862738167250955837389733598993664809941164205702637090279242767544565229087538682506419718265533447265625E-324"],
		["float", "1e-07", "9.99999999999999954748111825886258685613938723690807819366455078125E-8"],
		["float", "123.456", "123.4560000000000030695446184836328029632568359375"],
		["fmt", "4.993742", 6, "4.993742"],
		["fmt", "-0.000005", 6, "-0.000005"],
		["fmt", "0E-6", 6, "0.000000"],
		["fmt", "1E+34", 6, "10000000000000000000000000000000000.000000"],
		["fmt", "1.0000005", 6, "1.000000"],
		["fmt", "1.0000015", 6, "1.000002"],
		["fmt", "-0.0000001", 6, "-0.000000"],
		["fmt", "5", 0, "5"],
		["fmt", "0.5", 0, "0"],
		["fmt", "123.456", 2, "123.46"],
		["fmt", "0E+5", 2, "0.00"],
		["cmp", "1.0", "1.00", 0],
		["cmp", "-0", "0", 0],
		["cmp", "-1", "0", -1],
		["cmp", "1E+1", "9.99", 1],
		["cmp", "1E-7", "0", 1],
		["cmp", "1E+30", "1E+29", 1],
		["cmp", "-2", "-10", 1],
		["cmp", "4.99", "4.980000", 1],
		["cmp", "0", "-5", 1],
		["cmp", "-5", "0E+9", -1],
		["cmp", "-1.5", "-1.50", 0],
		["cmp", "2.000001", "2", 1],
		["cmp", "1E+999999999999999999", "1E-1999999999999999997", 1],
		["cmp", "12.3", "1.23E+1", 0],
		["int", "5.000", "5"],
		["int", "1E+3", "1000"],
		["int", "-12.0", "-12"],
		["int", "5.5", "5"],
		["int", "-5.5", "-5"],
		["int", "-0", "0"],
		["int", "0.0000001", "0"],
		["int", "123456789012345678901234567890", "123456789012345678901234567890"],
		["int", "-9223372036854775808", "-9223372036854775808"],
		["int", "9223372036854775807", "9223372036854775807"],
		["int", "1E+20", "100000000000000000000"],
		["integral", "5.000", true],
		["integral", "5.5", false],
		["integral", "1E+3", true],
		["integral", "0E-5", true],
		["integral", "1E-1999999999999999997", false],
		["integral", "123.000", true]
]


func _d(s: String) -> ScfgDec:
	var v: Variant = ScfgDec.parse(s)
	return v if v is ScfgDec else null


func _show(v: Variant) -> Variant:
	if v is ScfgErr:
		return (v as ScfgErr).kind
	if v is ScfgDec or v is ScfgBigInt:
		return str(v) if v is ScfgDec else (v as ScfgBigInt).to_decimal()
	if v is int:
		return str(v)
	return v


func _run(row: Array) -> Variant:
	var op: String = row[0]
	match op:
		"str":
			return _show(ScfgDec.parse(row[1]))
		"add":
			return _show(_d(row[1]).add(_d(row[2])))
		"sub":
			return _show(_d(row[1]).sub(_d(row[2])))
		"mul":
			return _show(_d(row[1]).mul(_d(row[2])))
		"mul_int":
			return _show(_d(row[1]).mul_int(int(row[2])))
		"scaleb":
			return _show(_d(row[1]).scaleb(int(row[2])))
		"quantize":
			return _show(_d(row[1]).quantize(int(row[2])))
		"units":
			var units: Variant = ScfgDec.from_int(ScfgPy.py_int(row[1]))
			return _show((units as ScfgDec).scaleb(-6))
		"float":
			return _show(ScfgDec.from_float((row[1] as String).to_float()))
		"float_bits":  # the double with these raw bits (String.to_float flushes 5e-324 to 0)
			var raw := PackedByteArray()
			raw.resize(8)
			raw.encode_s64(0, int(row[1]))
			return _show(ScfgDec.from_float(raw.decode_double(0)))
		"fmt":
			return _d(row[1]).format_fixed(int(row[2]))
		"cmp":
			return _d(row[1]).cmp(_d(row[2]))
		"int":
			return _show(_d(row[1]).to_py_int())
		"integral":
			return _d(row[1]).is_integral()
	return "unknown op " + op


## The table above: Python's str(Decimal) forms, the grammar, rounding, quantize, scaleb,
## float conversion, fixed formatting, comparison and int().
func test_dec_str_matches_python_forms() -> bool:
	for row: Array in ROWS:
		var want: Variant = row[row.size() - 1]
		if want is float:
			want = int(want)
		var got: Variant = _run(row)
		T.eq(got, want, "%s -> %s" % [ScfgPy.py_repr(row.slice(0, row.size() - 1)), ScfgPy.py_repr(want)])
	return T.done()


func test_parse_recognises_nan_and_infinity() -> bool:
	for nan: String in ["nan", "-NaN", "sNaN", "NaN12", "snan7", " nan ", "n_an", "NaN_1", "-sNaN"]:
		T.eq(ScfgDec.parse(nan), "nan", nan)
	for inf: String in ["inf", "+Inf", "-Infinity", "INFINITY", "infinity\n"]:
		T.eq(ScfgDec.parse(inf), "inf", inf)
	for bad: Variant in [5, 1.5, null, [], {}, true]:
		T.is_err(ScfgDec.parse(bad), "TypeError", "parse(%s)" % ScfgPy.py_repr(bad))
	T.is_err(ScfgDec.parse("in"), "InvalidOperation", "in")
	T.eq(str(ScfgDec.parse(&"4.5")), "4.5", "a StringName is a str")
	return T.done()


## Python's Decimal(v) for the other constructor types (values checked with CPython).
func test_from_value_follows_the_decimal_constructor() -> bool:
	var table := [
		[true, "1"], [false, "0"], [4.0, "4"], [-0.0, "-0"], [0.5, "0.5"], [7, "7"], [-9223372036854775807 - 1, "-9223372036854775808"],
		[[0, [1, 2], -1], "1.2"], [[1, [2], 0], "-2"], [[0, [], 5], "0E+5"], [[true, [1], 0], "-1"],
		[[1, [0], 0], "-0"], [[0, [1], "F"], "inf"], [[0, [1], "n"], "nan"], [[0, [1], "N"], "nan"],
		["  12.50 ", "12.50"], [ScfgDec.parse("3.25"), "3.25"],
	]
	for row: Array in table:
		var got: Variant = ScfgDec.from_value(row[0])
		T.eq(got if got is String else _show(got), row[1], "Decimal(%s)" % ScfgPy.py_repr(row[0]))
	for bad: Variant in [[], [1, 2, 3], [2, [1], 0], [0, [10], 0], [0, ["1"], 0], [0, [1], "x"],
			[0, [1], 1.5], [0, "12", 0], [0, [1], null], [0.5, [1], 0]]:
		T.is_err(ScfgDec.from_value(bad), "ValueError", "Decimal(%s)" % ScfgPy.py_repr(bad))
	T.is_err(ScfgDec.from_value([0, [1], 1000000000000000000]), "InvalidOperation", "exponent too large")
	for bad: Variant in [null, {}, PackedByteArray([49]), Vector2.ZERO]:
		T.is_err(ScfgDec.from_value(bad), "TypeError", "Decimal(%s)" % ScfgPy.py_repr(bad))
	T.eq(ScfgDec.from_value(NAN), "nan", "Decimal(float('nan'))")
	T.eq(ScfgDec.from_value(-INF), "inf", "Decimal(float('-inf'))")
	var huge: Variant = ScfgDec.from_value(ScfgBigInt.from_decimal("123456789012345678901234567890123"))
	T.eq(_show(huge), "123456789012345678901234567890123", "an ScfgBigInt is exact (33 digits)")
	return T.done()


## A port limit, not Python: int() of a Decimal below -2^63 (Python gives the int
## -9223372036854775809) is an OverflowError, because ScfgBigInt is unsigned.
func test_int_below_minus_2_63_is_a_port_limit() -> bool:
	T.eq(_show(_d("-9223372036854775808").to_py_int()), "-9223372036854775808", "-2^63 still fits")
	T.is_err(_d("-9223372036854775809").to_py_int(), "OverflowError", "one below -2^63")
	return T.done()


## add_checked is add plus Python's decimal.Overflow (values checked with CPython).
func test_add_checked_reports_overflow() -> bool:
	T.eq(str(_d("0").add_checked(_d("9E+999999"))), "9.000000000000000000000000000E+999999",
		"rounded to 28 digits, still within Emax")
	T.eq(str(_d("0").add_checked(_d("1E-1000030"))), "0E-1000026", "a subnormal rounds to zero, no error")
	T.is_err(_d("0").add_checked(_d("1E+1000000")), "Overflow", "adjusted exponent 1000000 > Emax")
	T.is_err(_d("9E+999999").add_checked(_d("9E+999999")), "Overflow", "the carry passes Emax")
	T.eq(str(_d("1.5").add_checked(_d("0"))), "1.5", "an ordinary sum")
	return T.done()


func test_from_int_accepts_python_ints_only() -> bool:
	T.eq(str(ScfgDec.from_int(-42)), "-42", "a negative int")
	T.eq(str(ScfgDec.from_int(true)), "1", "a bool is an int")
	for bad: Variant in ["5", 5.0, null, []]:
		T.is_err(ScfgDec.from_int(bad), "TypeError", "from_int(%s)" % ScfgPy.py_repr(bad))
	return T.done()


func test_is_zero_and_errors_are_decimal_classes() -> bool:
	T.ok(_d("0.00").is_zero() and _d("-0E+5").is_zero() and not _d("1E-9").is_zero(), "is_zero")
	var e: Variant = ScfgDec.parse("abc")
	T.ok(e is ScfgErr and (e as ScfgErr).is_a("ArithmeticError") and not (e as ScfgErr).is_a("ValueError"),
		"InvalidOperation is decimal's ArithmeticError, not a ValueError (callers convert it)")
	var o: Variant = _d("1E+999999").scaleb(1)
	T.ok(o is ScfgErr and (o as ScfgErr).is_a("ArithmeticError") and (o as ScfgErr).kind == "Overflow",
		"decimal.Overflow")
	return T.done()


func test_values_are_immutable() -> bool:
	var a := _d("1.5")
	var b := a.add(_d("2"))
	a.mul_int(10)
	a.quantize(-4)
	a.scaleb(3)
	T.eq(str(a), "1.5", "operands are unchanged")
	T.eq(str(b), "3.5", "the sum")
	return T.done()
