## Money handling: parsing developer/player amounts and the price|code|nonce codec.
## Mirrors scfg_core/amounts.py.
##
## On-chain USDC/USDT amounts are integers in base units (6 decimals). The last four
## decimals of every payment carry an item code (2 digits) and a random nonce
## (2 digits), so a transaction can be traced back to what it bought:
##
##     4.99 | 37 | 42   ->   4.993742 USDC   (units = 4_993_742)
##
## Python's `int` is a GDScript int or an ScfgBigInt (a bool counts as an int, as in Python);
## a GDScript float is a Python float. Decimal values are ScfgDec.
class_name ScfgAmounts
extends RefCounted

const TOKEN_DECIMALS := 6
const _CENT_EXP := -2
## Python's MAX_AMOUNT = Decimal("1000000").
static var MAX_AMOUNT: ScfgDec = ScfgDec.parse("1000000")  # err-ok: a valid literal
## The magnitude bound of every amount the core reads (Python's AMOUNT_EXPONENT_LIMIT): a nonzero
## amount's adjusted exponent must lie in [-40, 40], far beyond any token amount, so a near-Emax
## value (1E+999999) can neither overflow nor build a huge ScfgBigInt.
const AMOUNT_EXPONENT_LIMIT := 40
## The longest amount text parse_amount and stored_amount parse (Python's AMOUNT_TEXT_LIMIT): far
## beyond any amount, and it keeps the (quadratic) digit parsing cheap.
const AMOUNT_TEXT_LIMIT := 100
## 10^28: decode's limit on units // 10000 (Python's 28-digit precision).
static var _TEN_28: ScfgBigInt = ScfgBigInt.from_decimal("1" + "0".repeat(28))  # err-ok: ASCII digits


## Validate a price/tip amount and return it quantized to cents.
##
## Accepts str, int (an int or ScfgBigInt) or ScfgDec. Floats are refused: binary floating
## point and money do not mix. A bool or float is a TypeError (as are other types); an
## unparsable, non-finite, non-positive, too large or sub-cent amount a ValueError.
## @fallible
static func parse_amount(value: Variant) -> Variant:
	var d: ScfgDec
	match typeof(value):
		TYPE_BOOL, TYPE_FLOAT:
			return ScfgErr.make("TypeError", "amounts must be str, int or Decimal (never float)")
		TYPE_INT:
			d = ScfgDec.from_int(value)  # err-ok: an int is always accepted
		TYPE_STRING, TYPE_STRING_NAME:
			if str(value).length() > AMOUNT_TEXT_LIMIT:
				return ScfgErr.make("ValueError", "amount text longer than %d characters" % AMOUNT_TEXT_LIMIT)
			if str(value).contains("_"):  # Decimal's digit-group underscores ("1_000") are refused
				return ScfgErr.make("ValueError", "not a number: %s" % ScfgPy.py_repr(str(value)))
			var parsed: Variant = ScfgDec.parse(ScfgPy.py_strip(str(value)))
			if parsed is ScfgErr:
				return ScfgErr.make("ValueError", "not a number: %s" % ScfgPy.py_repr(str(value)))
			if not parsed is ScfgDec:
				return ScfgErr.make("ValueError", "amount must be finite")
			d = parsed
		TYPE_OBJECT:
			if value is ScfgBigInt:
				d = ScfgDec.from_int(value)  # err-ok: an ScfgBigInt is always accepted
			elif value is ScfgDec:
				d = value
	if d == null:
		return ScfgErr.make("TypeError", "unsupported amount type: %s" % ScfgPy.py_type(value))
	if not _in_range(d):
		return ScfgErr.make("ValueError", "amount out of range")
	if d.cmp(ScfgDec.from_int(0)) <= 0:  # err-ok: an int is always accepted
		return ScfgErr.make("ValueError", "amount must be positive")
	if d.cmp(MAX_AMOUNT) > 0:
		return ScfgErr.make("ValueError", "amount too large")
	var q: Variant = d.quantize(_CENT_EXP)
	if q is ScfgErr:
		return q
	if (q as ScfgDec).cmp(d) != 0:
		return ScfgErr.make("ValueError", "amount must have at most 2 decimals")
	return q


## price (2 decimals) + code (0 = tip, 1..99 = item) + nonce -> base units (an int).
## @fallible
static func encode(price: Variant, code: Variant, nonce: Variant) -> Variant:
	var p: Variant = parse_amount(price)
	if p is ScfgErr:
		return p
	var bad: Variant = _check_field("code", code)
	if bad is ScfgErr:
		return bad
	bad = _check_field("nonce", nonce)
	if bad is ScfgErr:
		return bad
	var cents: int = (p as ScfgDec).mul_int(100).to_py_int()  # err-ok: at most 10^8, an int
	return cents * 10000 + int(code) * 100 + int(nonce)


## Inverse of encode: base units (an int or ScfgBigInt, >= 0) -> [price: ScfgDec, code, nonce].
## Once the price needs more than 28 digits (units >= 10**32) Python's quantize raises
## decimal.InvalidOperation, and so does the port (kind "InvalidOperation", not a ValueError).
## A non-int is a TypeError here; Python would accept a float (`5.0 // 10_000`) and return
## floats for code and nonce, which no caller can reach (units are always ints).
## @fallible
static func decode(units: Variant) -> Variant:
	if units is bool:
		units = 1 if units else 0
	if units is int:
		if units < 0:
			return ScfgErr.make("ValueError", "units must be >= 0")
		var u: int = units
		var c: ScfgBigInt = ScfgBigInt.from_int(u / 10000)  # err-ok: u >= 0
		return [ScfgDec.from_parts(c, _CENT_EXP, false), (u / 100) % 100, u % 100]
	if not units is ScfgBigInt:
		return ScfgErr.make("TypeError", "'<' not supported between instances of '%s' and 'int'" % ScfgPy.py_type(units))
	var big: ScfgBigInt = units
	var qr: Array = big.divmod(ScfgBigInt.from_int(10000))  # err-ok: a non-zero divisor
	var cents: ScfgBigInt = qr[0]
	if cents.cmp(_TEN_28) >= 0:
		return ScfgErr.make("InvalidOperation",
			"[<class 'decimal.InvalidOperation'>] the price of %s units needs more than 28 digits" % big.to_decimal())
	var low: int = (qr[1] as ScfgBigInt).to_int()  # err-ok: below 10000
	return [ScfgDec.from_parts(cents, _CENT_EXP, false), low / 100, low % 100]


## Base units (an int, ScfgBigInt or bool) -> token amount, exactly at any size: 4990000 ->
## 4.990000 (Python builds it from the digits, no 28-digit rounding). A non-int is a TypeError.
## @fallible
static func units_to_decimal(units: Variant) -> Variant:
	var d: Variant = ScfgDec.from_int(units)
	if d is ScfgErr:
		return d
	var dec: ScfgDec = d
	return ScfgDec.from_parts(dec.coef, dec.exponent - TOKEN_DECIMALS, dec.neg)


## Whether a finite amount is within the amount range (AMOUNT_EXPONENT_LIMIT).
static func _in_range(d: ScfgDec) -> bool:
	return d.is_zero() or absi(d.adjusted()) <= AMOUNT_EXPONENT_LIMIT


## Python `stored_amount`: an amount read back from storage (a receipt's amount, a session's
## price), `Decimal(value)` (ScfgDec.from_value) when it is a finite number within the amount
## range, else a ValueError (a TypeError or InvalidOperation as Decimal() raises for the value:
## Python turns an InvalidOperation into a ValueError here, and so does the port).
## @fallible
static func stored_amount(value: Variant) -> Variant:
	if value is Array:  # Decimal() reads a (sign, digits, exponent) list; a stored amount never is one
		return ScfgErr.make("TypeError", "a stored amount is a number, not a list")
	if (value is String or value is StringName) and str(value).length() > AMOUNT_TEXT_LIMIT:
		return ScfgErr.make("ValueError", "amount text longer than %d characters" % AMOUNT_TEXT_LIMIT)
	var d: Variant = ScfgDec.from_value(value)
	if d is ScfgErr:
		if (d as ScfgErr).is_a("InvalidOperation"):
			return ScfgErr.make("ValueError", "not a number: %s" % ScfgPy.py_repr(value))
		return d
	if not d is ScfgDec or not _in_range(d):
		return ScfgErr.make("ValueError", "amount out of range: %s" % ScfgPy.py_repr(value))
	return d


## Python `decimal_to_units`: the amount times 10^6 exactly (no rounding to 28 digits) when that is
## a whole number, else a ValueError ("more than 6 decimals"), and a ValueError for an amount that
## is not finite or out of the amount range. `amount` is anything Python's Decimal() takes (see ScfgDec.from_value):
## an ScfgDec, a str, an int. The result is an int or ScfgBigInt.
## @fallible
static func decimal_to_units(amount: Variant) -> Variant:
	var d: Variant = ScfgDec.from_value(amount)
	if d is ScfgErr:
		return d
	if not d is ScfgDec or not _in_range(d):
		return ScfgErr.make("ValueError", "amount out of range: %s" % ScfgPy.py_repr(amount))
	var dec: ScfgDec = d
	var scaled := ScfgDec.from_parts(dec.coef, dec.exponent + TOKEN_DECIMALS, dec.neg)  # exact: no rounding
	if not scaled.is_integral():
		return ScfgErr.make("ValueError", "more than 6 decimals")
	return scaled.to_py_int()


## Human display of a token amount with all 6 decimals: 4993742 -> '4.993742'
## (Python: f"{units_to_decimal(units):.6f}").
## @fallible
static func format_units(units: Variant) -> Variant:
	var d: Variant = units_to_decimal(units)
	if d is ScfgErr:
		return d
	return (d as ScfgDec).format_fixed(TOKEN_DECIMALS)


## @fallible
static func _check_field(name: String, value: Variant) -> Variant:
	if not (value is int) or value < 0 or value > 99:
		return ScfgErr.make("ValueError", "%s must be an int in 0..99" % name)
	return null
