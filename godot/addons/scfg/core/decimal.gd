## A finite Python `decimal.Decimal` under the default context (28 digits, ROUND_HALF_EVEN,
## Emin -999999, Emax 999999), limited to the operations the payment core uses. No Python twin.
## `str(dec)` (`_to_string`) equals Python's `str(Decimal)`.
##
## A value is sign, coefficient (an ScfgBigInt, so 32-digit and longer coefficients are exact)
## and exponent (an int). Values are immutable: every operation returns a new ScfgDec (or self).
## NaN and Infinity are not ScfgDec values: `parse` and `from_value` return the Strings "nan"
## and "inf" for them (of either sign, signalling NaN included) so that callers can reject them.
##
## Fidelity to CPython (the C `_decimal`; `_pydecimal` is the readable reference):
## - `Decimal(str)`: Python's whitespace strip, Unicode decimal digits read as ASCII, every
##   underscore dropped (`"1__0"` and `"_1"` are 10 and 1), case-insensitive `inf`, `infinity`,
##   `nan`, `snan` (with diagnostic digits); an exponent below -1999999999999999997 or an
##   adjusted exponent above 999999999999999999 is an InvalidOperation, as in libmpdec.
## - Arithmetic results are rounded to 28 digits (`_fix`), including the subnormal rounding at
##   Etiny (-1000026) and the clamping of a zero's exponent into [Etiny, Emax].
## - `add`/`sub` use Python's operand normalisation, so operands with far-apart exponents cost
##   no more than near ones.
## - Python raises decimal.Overflow when a result's adjusted exponent is above Emax (999999).
##   `add_checked`, `scaleb` and `quantize` report it; the plain `add`, `sub`, `mul` and
##   `mul_int` do not (such a result keeps its exponent), so they stay infallible: use them only
##   where the operands are known to be small (code paths that read stored or player-supplied
##   amounts use add_checked).
##
## Errors are the Python classes: InvalidOperation and Overflow (decimal's own, ArithmeticErrors
## that are not ValueErrors), ValueError and TypeError for the constructors' type checks.
class_name ScfgDec
extends RefCounted

const PREC := 28
const EMAX := 999999
const EMIN := -999999
const ETINY := EMIN - PREC + 1  # -1000026
## libmpdec's bounds for a literal: the exponent and the adjusted exponent.
const MIN_LITERAL_EXP := -1999999999999999997
const MAX_LITERAL_ADJUSTED := 999999999999999999
## scaleb's limit on its argument: 2 * (Emax + prec).
const SCALEB_LIMIT := 2 * (EMAX + PREC)

const _NUMBER := "^([+-]?)(?:(?:([0-9]+)\\.?([0-9]*)|\\.([0-9]+))(?:[eE]([+-]?[0-9]+))?|((?i)inf(?:inity)?)|((?i)s?nan[0-9]*))\\z"

## The coefficient (magnitude, >= 0). Read-only.
var coef: ScfgBigInt
## The exponent: the value is (-1)^neg * coef * 10^exponent. Read-only.
var exponent: int = 0
## The sign (a zero has one too: `-0`). Read-only.
var neg: bool = false
var _digits: int = -1  # cached digit count of coef

static var _number_re: RegEx = null
static var _pow10_cache: Dictionary = {}


static func _make(c: ScfgBigInt, e: int, negative: bool) -> ScfgDec:
	var d := ScfgDec.new()
	d.coef = c
	d.exponent = e
	d.neg = negative
	return d


# --- constructors ---------------------------------------------------------------------------


## The value (-1)^negative * c * 10^e, as it is (no rounding).
static func from_parts(c: ScfgBigInt, e: int, negative: bool) -> ScfgDec:
	return _make(c, e, negative)


## Python `Decimal(text)` for a str: an ScfgDec, "nan" or "inf", or an InvalidOperation for a
## string that is not a decimal literal. A non-string is a TypeError.
## @fallible
static func parse(text: Variant) -> Variant:
	if not (text is String or text is StringName):
		return ScfgErr.make("TypeError", "conversion from %s to Decimal is not supported" % ScfgPy.py_type(text))
	var src := str(text)
	var bad := "[<class 'decimal.ConversionSyntax'>] %s" % ScfgPy.py_repr(src)
	var stripped := ScfgPy.py_strip(src)
	var ascii := ""
	for i in stripped.length():
		var c := stripped.unicode_at(i)
		if c == 0x5f:  # "_": the C decimal drops every underscore
			continue
		if c < 0x80:
			ascii += String.chr(c)
			continue
		var digit := ScfgPy.decimal_digit(c)
		if digit < 0:
			return ScfgErr.make("InvalidOperation", bad)
		ascii += String.chr(0x30 + digit)
	if _number_re == null:
		_number_re = RegEx.create_from_string(_NUMBER)
	var m := _number_re.search(ascii)
	if m == null:
		return ScfgErr.make("InvalidOperation", bad)
	if m.get_start(7) >= 0:
		return "nan"
	if m.get_start(6) >= 0:
		return "inf"
	var int_part := m.get_string(2)
	var frac := m.get_string(3) if m.get_start(2) >= 0 else m.get_string(4)
	var e := 0
	if m.get_start(5) >= 0:
		var parsed: Variant = _literal_exponent(m.get_string(5))
		if parsed == null:
			return ScfgErr.make("InvalidOperation", bad)
		e = parsed
	e -= frac.length()
	var c: ScfgBigInt = ScfgBigInt.from_decimal(int_part + frac)  # err-ok: ASCII digits, matched above
	var d := _make(c, e, m.get_string(1) == "-")
	if e < MIN_LITERAL_EXP or d.adjusted() > MAX_LITERAL_ADJUSTED:
		return ScfgErr.make("InvalidOperation", bad)
	return d


## Python `Decimal(n)` for an int (an int, an ScfgBigInt or a bool): exact. Anything else is a
## TypeError.
## @fallible
static func from_int(n: Variant) -> Variant:
	match typeof(n):
		TYPE_BOOL:
			return _make(_small(1 if n else 0), 0, false)
		TYPE_INT:
			var v: int = n
			if v == -9223372036854775807 - 1:
				return _make(ScfgBigInt.from_hex("8000000000000000"), 0, true)  # err-ok: a valid literal
			return _make(_small(absi(v)), 0, v < 0)
		TYPE_OBJECT:
			if n is ScfgBigInt:
				return _make(n, 0, false)
	return ScfgErr.make("TypeError", "expected an int, not '%s'" % ScfgPy.py_type(n))


## Python `Decimal(f)` for a float: the exact binary value (`Decimal(0.1)` has 55 digits);
## "nan" or "inf" for a non-finite float.
static func from_float(f: float) -> Variant:
	if is_nan(f):
		return "nan"
	if is_inf(f):
		return "inf"
	var raw := PackedByteArray()
	raw.resize(8)
	raw.encode_double(0, f)
	var bits := raw.decode_s64(0)
	var negative := bits < 0
	var biased := (bits >> 52) & 0x7ff
	var mant := bits & ((1 << 52) - 1)
	var e2 := -1074 if biased == 0 else biased - 1075
	if biased != 0:
		mant |= 1 << 52
	if mant == 0:
		return _make(_small(0), 0, negative)
	while (mant & 1) == 0:
		mant >>= 1
		e2 += 1
	if e2 >= 0:
		return _make(_small(mant).mul(_pow2(e2)), 0, negative)
	# mant / 2^k == mant * 5^k / 10^k
	var c := _small(mant)
	var five := _small(5)
	for _i in -e2:
		c = c.mul(five)
	return _make(c, e2, negative)


## Python `Decimal(v)` for any value the core meets: a str (parse), an int or bool, a float
## (exact), an ScfgDec (itself), or a list as Python's `(sign, digits, exponent)` tuple form.
## Returns an ScfgDec, "nan" or "inf"; an invalid literal is an InvalidOperation, a malformed
## list a ValueError, and any other type (None, dict, bytes) a TypeError.
## @fallible
static func from_value(v: Variant) -> Variant:
	match typeof(v):
		TYPE_STRING, TYPE_STRING_NAME:
			return parse(v)
		TYPE_BOOL, TYPE_INT:
			return from_int(v)
		TYPE_FLOAT:
			return from_float(v)
		TYPE_ARRAY:
			return _from_tuple(v)
		TYPE_OBJECT:
			if v is ScfgDec:
				return v
			if v is ScfgBigInt:
				return from_int(v)
	return ScfgErr.make("TypeError", "conversion from %s to Decimal is not supported" % ScfgPy.py_type(v))


# --- queries --------------------------------------------------------------------------------


func is_zero() -> bool:
	return coef.is_zero()


## The number of digits of the coefficient (1 for zero), as Python's `len(d._int)`.
func digits() -> int:
	if _digits < 0:
		_digits = 1 if coef.is_zero() else coef.to_decimal().length()
	return _digits


## Python `d.adjusted()`: the exponent of the most significant digit.
func adjusted() -> int:
	return exponent + digits() - 1


## Python `d == d.to_integral_value()`: whether the value is a whole number.
func is_integral() -> bool:
	if exponent >= 0 or coef.is_zero():
		return true
	if -exponent >= digits():
		return false
	var qr: Array = coef.divmod(_pow10(-exponent))  # err-ok: 10^k is not zero
	return (qr[1] as ScfgBigInt).is_zero()


## Python `int(d)`: truncated toward zero, an int when it fits in 64 bits signed, else an
## ScfgBigInt; a negative value below -2^63 is an OverflowError (a port limit: ScfgBigInt is
## unsigned).
## @fallible
func to_py_int() -> Variant:
	var mag: ScfgBigInt
	if exponent >= 0:
		mag = coef.mul(_pow10(exponent))
	elif -exponent >= digits():
		mag = _small(0)
	else:
		var qr: Array = coef.divmod(_pow10(-exponent))  # err-ok: 10^k is not zero
		mag = qr[0]
	if mag.bit_length() <= 63:
		var v: int = mag.to_int()  # err-ok: bit_length() <= 63
		return -v if neg else v
	if not neg:
		return mag
	if mag.to_hex() == "8000000000000000":
		return -9223372036854775807 - 1
	return ScfgErr.make("OverflowError", "int() below -2**63 is not supported by the port")


## Python's comparison: -1, 0 or 1 by value (zeros of either sign and exponent are equal).
func cmp(o: ScfgDec) -> int:
	var a_zero := coef.is_zero()
	var b_zero := o.coef.is_zero()
	if a_zero or b_zero:
		if a_zero and b_zero:
			return 0
		if a_zero:
			return 1 if o.neg else -1
		return -1 if neg else 1
	if neg != o.neg:
		return -1 if neg else 1
	var sign := -1 if neg else 1
	var adj_a := adjusted()
	var adj_b := o.adjusted()
	if adj_a != adj_b:
		return sign if adj_a > adj_b else -sign
	# same adjusted exponent: the exponent gap equals the digit-count gap, so this is small
	var e := mini(exponent, o.exponent)
	var c := coef.mul(_pow10(exponent - e)).cmp(o.coef.mul(_pow10(o.exponent - e)))
	return c * sign


# --- arithmetic -----------------------------------------------------------------------------


## Python `self + o` (rounded to 28 digits). See the class doc for the one thing not modelled.
func add(o: ScfgDec) -> ScfgDec:
	var e := mini(exponent, o.exponent)
	var a_zero := coef.is_zero()
	var b_zero := o.coef.is_zero()
	if a_zero and b_zero:
		return _make(coef, e, neg and o.neg)._fix()
	if a_zero or b_zero:
		var kept: ScfgDec = o if a_zero else self
		e = maxi(e, kept.exponent - PREC - 1)
		return kept._rescale(e)._fix()
	# _pydecimal's _normalize: an operand far below the other's last digit becomes 1 just below it
	var big: ScfgDec = self
	var small: ScfgDec = o
	if exponent < o.exponent:
		big = o
		small = self
	var floor_exp := big.exponent + mini(-1, big.digits() - PREC - 2)
	var small_coef := small.coef
	var small_exp := small.exponent
	if small.digits() + small.exponent - 1 < floor_exp:
		small_coef = _small(1)
		small_exp = floor_exp
	var big_coef := big.coef.mul(_pow10(big.exponent - small_exp))
	var r_exp := small_exp
	if big.neg == small.neg:
		return _make(big_coef.add(small_coef), r_exp, big.neg)._fix()
	var c := big_coef.cmp(small_coef)
	if c == 0:
		return _make(_small(0), e, false)._fix()  # equal and opposite: +0 under ROUND_HALF_EVEN
	if c > 0:
		return _make(big_coef._sub(small_coef), r_exp, big.neg)._fix()
	return _make(small_coef._sub(big_coef), r_exp, small.neg)._fix()


## Python `self + o` including the Overflow check `add` leaves out: a result whose adjusted
## exponent is above Emax (999999) is Python's decimal.Overflow (kind "Overflow").
## @fallible
func add_checked(o: ScfgDec) -> Variant:
	var sum := add(o)
	return sum._fix_checked()


## Python `self - o`.
func sub(o: ScfgDec) -> ScfgDec:
	return add(_make(o.coef, o.exponent, not o.neg))


## Python `self * o` (rounded to 28 digits).
func mul(o: ScfgDec) -> ScfgDec:
	return _make(coef.mul(o.coef), exponent + o.exponent, neg != o.neg)._fix()


## Python `self * n` for an int n.
func mul_int(n: int) -> ScfgDec:
	var other: ScfgDec = from_int(n)  # err-ok: an int is always accepted
	return mul(other)


## Python `self.scaleb(n)`: the exponent moved by n, then rounded to 28 digits. An n beyond
## +-2000054 (2 * (Emax + prec)) is an InvalidOperation and a result above Emax an Overflow.
## @fallible
func scaleb(n: int) -> Variant:
	if n < -SCALEB_LIMIT or n > SCALEB_LIMIT:
		return ScfgErr.make("InvalidOperation", "[<class 'decimal.InvalidOperation'>] scaleb argument out of range")
	var moved := _make(coef, exponent + n, neg)
	return moved._fix_checked()


## Python `self.quantize(Decimal(1).scaleb(target))` (ROUND_HALF_EVEN): the value with exponent
## `target`. An InvalidOperation where Python raises one: `target` outside [Etiny, Emax], or a result
## that needs more than 28 digits (`Decimal("1E+30").quantize(Decimal("0.01"))`).
## @fallible
func quantize(target: int) -> Variant:
	var invalid := "[<class 'decimal.InvalidOperation'>] quantize result has too many digits for current context"
	if target < ETINY or target > EMAX:
		return ScfgErr.make("InvalidOperation", "[<class 'decimal.InvalidOperation'>] target exponent out of bounds in quantize")
	if coef.is_zero():
		var z := _make(coef, target, neg)
		return z._fix_checked()
	if adjusted() > EMAX or adjusted() - target + 1 > PREC:
		return ScfgErr.make("InvalidOperation", invalid)
	var ans := _rescale(target)
	if ans.adjusted() > EMAX or ans.digits() > PREC:
		return ScfgErr.make("InvalidOperation", invalid)
	return ans._fix_checked()


# --- formatting -----------------------------------------------------------------------------


## Python `str(d)`: plain notation when the exponent is <= 0 and the adjusted exponent >= -6,
## scientific otherwise (`"4.990000"`, `"1E+1"`, `"1E-7"`, `"-0"`, `"0E-7"`).
func _to_string() -> String:
	var ds := coef.to_decimal()
	var leftdigits := exponent + ds.length()
	var dotplace := leftdigits if exponent <= 0 and leftdigits > -6 else 1
	var intpart: String
	var fracpart: String
	if dotplace <= 0:
		intpart = "0"
		fracpart = "." + "0".repeat(-dotplace) + ds
	elif dotplace >= ds.length():
		intpart = ds + "0".repeat(dotplace - ds.length())
		fracpart = ""
	else:
		intpart = ds.substr(0, dotplace)
		fracpart = "." + ds.substr(dotplace)
	var e := leftdigits - dotplace
	var exp_part := "" if e == 0 else ("E%s%d" % ["+" if e > 0 else "-", absi(e)])
	return ("-" if neg else "") + intpart + fracpart + exp_part


## Python `format(d, ".<places>f")`: rounded half even to `places` decimals, never scientific.
func format_fixed(places: int) -> String:
	var ds: String
	var negative := neg
	var e := -places
	if coef.is_zero():
		ds = "0"
	elif exponent >= e:  # pad with zeros as text: a large exponent would make a huge ScfgBigInt
		ds = coef.to_decimal() + "0".repeat(exponent - e)
	else:
		ds = _rescale(e).coef.to_decimal()
	var dotplace := e + ds.length()
	var intpart: String
	var fracpart: String
	if dotplace < 0:
		intpart = "0"
		fracpart = "0".repeat(-dotplace) + ds
	elif dotplace > ds.length():
		intpart = ds + "0".repeat(dotplace - ds.length())
		fracpart = ""
	else:
		intpart = ds.substr(0, dotplace) if dotplace > 0 else "0"
		fracpart = ds.substr(dotplace)
	return ("-" if negative else "") + intpart + ("." + fracpart if fracpart != "" else "")


# --- internals ------------------------------------------------------------------------------


## _pydecimal's `_rescale(target, ROUND_HALF_EVEN)`: pad with zeros or round to that exponent.
func _rescale(target: int) -> ScfgDec:
	if coef.is_zero():
		return _make(coef, target, neg)
	if exponent >= target:
		return _make(coef.mul(_pow10(exponent - target)), target, neg)
	return _make(_round_drop(coef, digits(), target - exponent), target, neg)


## _pydecimal's `_fix` for the default context, without the Overflow check (see the class doc):
## at most 28 digits (rounding half even), subnormal results rounded at Etiny, and a zero's
## exponent clamped into [Etiny, Emax].
func _fix() -> ScfgDec:
	if coef.is_zero():
		var clamped := clampi(exponent, ETINY, EMAX)
		return self if clamped == exponent else _make(coef, clamped, neg)
	var exp_min := digits() + exponent - PREC
	if exp_min < ETINY:
		exp_min = ETINY
	if exponent >= exp_min:
		return self
	var c := _round_drop(coef, digits(), exp_min - exponent)
	var out := _make(c, exp_min, neg)
	if out.digits() > PREC:  # 99..9 rounded up to 10^28: drop the trailing zero
		out = _make(_pow10(PREC - 1), exp_min + 1, neg)
	return out


## _fix() plus Python's Overflow: a result whose adjusted exponent is above Emax.
## @fallible
func _fix_checked() -> Variant:
	var out := _fix()
	if not out.coef.is_zero() and out.adjusted() > EMAX:
		return ScfgErr.make("Overflow", "[<class 'decimal.Overflow'>] above Emax")
	return out


## c / 10^drop rounded half even, for c with `nd` digits and drop >= 1.
static func _round_drop(c: ScfgBigInt, nd: int, drop: int) -> ScfgBigInt:
	if drop > nd:  # c < 10^(drop - 1): below half of the last kept place
		return _small(0)
	var p := _pow10(drop)
	var qr: Array = c.divmod(p)  # err-ok: 10^k is not zero
	var q: ScfgBigInt = qr[0]
	var r: ScfgBigInt = qr[1]
	var half := r.add(r).cmp(p)
	var odd := not q.limbs.is_empty() and (q.limbs[0] & 1) == 1
	if half > 0 or (half == 0 and odd):
		q = q.add(_small(1))
	return q


## Python `Decimal((sign, digits, exponent))` for a list, following the C `_decimal` checks.
## A JSON number arrives as a float: an integral one stands for Python's int.
## @fallible
static func _from_tuple(t: Array) -> Variant:
	if t.size() != 3:
		return ScfgErr.make("ValueError", "argument must be a sequence of length 3")
	var sign: Variant = _tuple_int(t[0])
	if not (sign is int and (sign == 0 or sign == 1)):
		return ScfgErr.make("ValueError", "sign must be an integer with the value 0 or 1")
	var special := ""
	var e: Variant = null
	if t[2] is String or t[2] is StringName:
		special = {"F": "inf", "n": "nan", "N": "nan"}.get(str(t[2]), "")
		if special == "":
			return ScfgErr.make("ValueError", "string argument in the third position must be 'F', 'n' or 'N'")
	else:
		e = _tuple_int(t[2])
		if not e is int:
			return ScfgErr.make("ValueError", "exponent must be an integer")
	if not t[1] is Array:
		return ScfgErr.make("ValueError", "coefficient must be a tuple of digits")
	var ds := ""
	for x: Variant in t[1]:
		var digit: Variant = _tuple_int(x)
		if not digit is int or digit < 0 or digit > 9:
			return ScfgErr.make("ValueError", "coefficient must be a tuple of digits")
		ds += str(digit)
	if special != "":
		return special
	return parse(("-" if sign == 1 else "") + (ds if ds != "" else "0") + "E" + str(e))


## A tuple element as Python's int: a bool, an int, or an integral float (a JSON number);
## null for anything else.
static func _tuple_int(v: Variant) -> Variant:
	if v is bool:
		return 1 if v else 0
	if v is int:
		return v
	if v is float:
		var i: Variant = ScfgPy.json_int(v)
		if i is ScfgErr:
			return null
		return i
	return null


## A literal's exponent digits (with sign) as an int, or null when it is so large that no
## literal can carry it (libmpdec's bounds are below 2 * 10^18).
static func _literal_exponent(s: String) -> Variant:
	var negative := s.begins_with("-")
	var body := s.trim_prefix("-").trim_prefix("+")
	var a := 0
	while a < body.length() - 1 and body[a] == "0":
		a += 1
	body = body.substr(a)
	if body.length() > 19 or (body.length() == 19 and body > "9000000000000000000"):
		return null
	var v := body.to_int()
	return -v if negative else v


static func _small(n: int) -> ScfgBigInt:
	var b: ScfgBigInt = ScfgBigInt.from_int(n)  # err-ok: callers pass n >= 0
	return b


static func _pow2(n: int) -> ScfgBigInt:
	var out := _small(1)
	var two := _small(2)
	for _i in n:
		out = out.mul(two)
	return out


## 10^n for n >= 0 (cached).
static func _pow10(n: int) -> ScfgBigInt:
	if n == 0:
		return _small(1)
	if _pow10_cache.has(n):
		return _pow10_cache[n]
	var p: ScfgBigInt = ScfgBigInt.from_decimal("1" + "0".repeat(n))  # err-ok: ASCII digits
	if _pow10_cache.size() < 256:
		_pow10_cache[n] = p
	return p
