## Python built-in semantics the port needs on JSON-like values: `str.strip()`, `int()`,
## `int(s, 16)`, `d[key]`, `key in d`, truthiness and `repr()`. No Python twin.
##
## Values are what JSON.parse_string produces (null, bool, float, String, Array, Dictionary)
## plus GDScript ints and ScfgBigInt. An integer result is an `int` when it fits in 64 bits
## signed and an ScfgBigInt (non-negative only) above that: every function here returns the
## smallest of the two that holds the value, so callers can test `v is int`.
class_name ScfgPy
extends RefCounted

## CPython's default `sys.get_int_max_str_digits()`: int(str) refuses more decimal digits.
const INT_MAX_STR_DIGITS := 4300

## 2^53: integers below it in magnitude are exact as a float, a JSON number and a stored int. The
## core's one copy (watcher, evm and the Solana backend use it).
const TWO_53 := 9007199254740992
## 2^63 as a float, built from ints: the GDScript tokenizer misreads some large float literals
## (9007199254740991.0 becomes 9007199254740990.0), so the core never writes one.
const _TWO_63_F := (1 << 62) * 2.0

## The zero of every run of ten Unicode decimal digits (category Nd), from CPython 3.9's
## unicodedata (Unicode 13.0.0), the project's interpreter. Python's int() and Decimal() read
## any of them as the ASCII digit.
const _ND_ZEROS: Array[int] = [
	0x30, 0x660, 0x6f0, 0x7c0, 0x966, 0x9e6, 0xa66, 0xae6, 0xb66, 0xbe6, 0xc66, 0xce6, 0xd66,
	0xde6, 0xe50, 0xed0, 0xf20, 0x1040, 0x1090, 0x17e0, 0x1810, 0x1946, 0x19d0, 0x1a80, 0x1a90,
	0x1b50, 0x1bb0, 0x1c40, 0x1c50, 0xa620, 0xa8d0, 0xa900, 0xa9d0, 0xa9f0, 0xaa50, 0xabf0,
	0xff10, 0x104a0, 0x10d30, 0x11066, 0x110f0, 0x11136, 0x111d0, 0x112f0, 0x11450, 0x114d0,
	0x11650, 0x116c0, 0x11730, 0x118e0, 0x11950, 0x11c50, 0x11d50, 0x11da0, 0x16a60, 0x16b50,
	0x1d7ce, 0x1d7d8, 0x1d7e2, 0x1d7ec, 0x1d7f6, 0x1e140, 0x1e2f0, 0x1e950, 0x1fbf0,
]


## Python's `str.isspace()` for one code point: the set `str.strip()` removes. It is wider than
## GDScript's strip_edges() (\x1c-\x1f, \x85, U+00A0, U+1680, U+2000-U+200A, U+2028, U+2029,
## U+202F, U+205F, U+3000) and does not include the BOM U+FEFF or U+200B.
static func is_py_space(c: int) -> bool:
	if c <= 0x20:
		return c == 0x20 or (c >= 0x09 and c <= 0x0d) or (c >= 0x1c and c <= 0x1f)
	if c < 0x85:
		return false
	return (c == 0x85 or c == 0xa0 or c == 0x1680 or (c >= 0x2000 and c <= 0x200a) or c == 0x2028
		or c == 0x2029 or c == 0x202f or c == 0x205f or c == 0x3000)


## Exactly Python's `s.strip()` with no argument.
static func py_strip(s: String) -> String:
	var start := 0
	var end := s.length()
	while start < end and is_py_space(s.unicode_at(start)):
		start += 1
	while end > start and is_py_space(s.unicode_at(end - 1)):
		end -= 1
	return s.substr(start, end - start)


## A digit run read out of a node's error message (an EVM range limit, a Solana transaction
## version) is a number only up to this many significant digits (scfg_core/ints.py
## DIGIT_RUN_LIMIT): every range or version a request can carry is below 10**18.
const DIGIT_RUN_LIMIT := 18


## Python's ints.digit_run_value for the run of decimal digits (any script, Python's `\d+`) that
## starts at `start` in `s`: `[end, value]`, `end` the index after the run and `value` its int, or
## null beyond DIGIT_RUN_LIMIT significant digits. Linear: no text is built.
static func digit_run(s: String, start: int) -> Array:
	var n := s.length()
	var i := start
	var value := 0
	var significant := 0
	while i < n:
		var d := decimal_digit(s.unicode_at(i))
		if d < 0:
			break
		if significant > 0 or d > 0:
			significant += 1
			if significant <= DIGIT_RUN_LIMIT:
				value = value * 10 + d
		i += 1
	return [i, value if significant <= DIGIT_RUN_LIMIT else null]


## The ASCII digit value of a Unicode decimal digit (any script), or -1.
static func decimal_digit(c: int) -> int:
	if c >= 0x30 and c <= 0x39:
		return c - 0x30
	if c < 0x660:
		return -1
	for z in _ND_ZEROS:
		if c >= z and c < z + 10:
			return c - z
	return -1


## Python `int(v)`:
## - a bool is 0 or 1, an int or ScfgBigInt is itself;
## - a float is truncated toward zero (NaN is a ValueError, an infinity an OverflowError);
## - a str may have surrounding whitespace, a sign, decimal digits of any script and single
##   underscores between digits (anything else is a ValueError, `"0x10"` included);
## - any other type is a TypeError.
## A result below -2^63 is an OverflowError: ScfgBigInt is unsigned (a port limit, not Python).
## @fallible
static func py_int(v: Variant) -> Variant:
	match typeof(v):
		TYPE_BOOL:
			return 1 if v else 0
		TYPE_INT:
			return v
		TYPE_FLOAT:
			return _float_to_int(v)
		TYPE_STRING, TYPE_STRING_NAME:
			return _parse_int(str(v), 10)
		TYPE_OBJECT:
			if v is ScfgBigInt:
				return _normalize(v, false)
	return ScfgErr.make("TypeError",
		"int() argument must be a string, a bytes-like object or a real number, not '%s'" % py_type(v))


## Python `int(s, 16)`: like py_int on a str, plus an optional `0x`/`0X` after the sign, which
## may be followed by one underscore (`"0x_1f"`). A non-str is a TypeError.
## @fallible
static func py_hex_int(s: Variant) -> Variant:
	if not (s is String or s is StringName):
		return ScfgErr.make("TypeError", "int() can't convert non-string with explicit base")
	return _parse_int(str(s), 16)


## A JSON number that Python holds as an int. JSON.parse_string makes every number a float, so
## an integral float below 2^53 in magnitude becomes that int; an int below 2^53 passes through.
## A fractional, non-finite or larger number is a ValueError (a float of 2^53 or more may
## already be rounded), and a non-number (bool and numeric strings included) a TypeError.
## @fallible
static func json_int(v: Variant) -> Variant:
	if typeof(v) == TYPE_INT:
		if v > -TWO_53 and v < TWO_53:
			return v
		return ScfgErr.make("ValueError", "integer %d is outside the JSON-safe range" % v)
	if typeof(v) != TYPE_FLOAT:
		return ScfgErr.make("TypeError", "expected a JSON number, not '%s'" % py_type(v))
	var f: float = v
	if is_nan(f) or is_inf(f) or f != floorf(f):
		return ScfgErr.make("ValueError", "expected an integral JSON number, got %s" % py_repr(f))
	if f <= -float(TWO_53) or f >= float(TWO_53):
		return ScfgErr.make("ValueError", "%s is outside the JSON-safe integer range" % py_repr(f))
	return int(f)


## A value from JSON.parse_string as Python's json.load reads the same text: Godot's reader makes
## every number a float, so an integral float below 2^53 in magnitude becomes that int, recursively
## through Arrays and Dictionaries (new copies; keys kept). Everything else is unchanged, so a
## fractional, non-finite or too-large float still fails validation as Python's would.
static func from_json(v: Variant) -> Variant:
	match typeof(v):
		TYPE_FLOAT:
			var f: float = v
			if is_nan(f) or is_inf(f) or f != floorf(f) or absf(f) >= float(TWO_53):
				return v
			return int(f)
		TYPE_ARRAY:
			var out: Array = (v as Array).duplicate()  # (keeps a typed Array's type)
			for i in out.size():
				out[i] = from_json(out[i])
			return out
		TYPE_DICTIONARY:
			var out: Dictionary = (v as Dictionary).duplicate()  # (keeps the key order)
			for k: Variant in out:
				out[k] = from_json(out[k])
			return out
	return v


## Python `key in d` for a dict; false for anything that is not a Dictionary.
static func has(d: Variant, key: Variant) -> bool:
	return d is Dictionary and (d as Dictionary).has(key)


## Python `d[key]`: the value (None included) or a KeyError for a missing key of a dict
## (message: the key's repr), an int index of a list (negative counts from the end; out of
## range is an IndexError), and a TypeError for anything else (None, str, numbers, a list
## indexed by a non-int).
## @fallible
static func py_get(d: Variant, key: Variant) -> Variant:
	if d is Dictionary:
		if (d as Dictionary).has(key):
			return d[key]
		return ScfgErr.make("KeyError", py_repr(key))
	if d is Array:
		if typeof(key) != TYPE_INT:
			return ScfgErr.make("TypeError", "list indices must be integers or slices, not %s" % py_type(key))
		var a: Array = d
		var i: int = key
		if i < 0:
			i += a.size()
		if i < 0 or i >= a.size():
			return ScfgErr.make("IndexError", "list index out of range")
		return a[i]
	return ScfgErr.make("TypeError", "'%s' object is not subscriptable" % py_type(d)
		if not (d is String or d is StringName) else "string indices must be integers")


## Python `a == b` for the values the core compares (JSON values, ScfgBigInt, ScfgDec), without
## GDScript's runtime error on mismatched types (`5.0 == "x"` is one): numbers compare by exact
## value across int, float, bool and ScfgBigInt; a str equals a str (or StringName) with the same
## text; lists and dicts compare element by element; two ScfgDec compare by value; anything else
## of different types is not equal (Python's default), and of the same type GDScript's `==`.
static func py_eq(a: Variant, b: Variant) -> bool:
	var na := _is_number(a)
	var nb := _is_number(b)
	if na or nb:
		return na and nb and _num_eq(a, b)
	var sa := a is String or a is StringName
	var sb := b is String or b is StringName
	if sa or sb:
		return sa and sb and str(a) == str(b)
	if a is Array or b is Array:
		if not (a is Array and b is Array) or (a as Array).size() != (b as Array).size():
			return false
		for i in (a as Array).size():
			if not py_eq(a[i], b[i]):
				return false
		return true
	if a is Dictionary or b is Dictionary:
		if not (a is Dictionary and b is Dictionary) or (a as Dictionary).size() != (b as Dictionary).size():
			return false
		for k: Variant in a:
			if not (b as Dictionary).has(k) or not py_eq(a[k], b[k]):
				return false
		return true
	if a is ScfgDec and b is ScfgDec:
		return (a as ScfgDec).cmp(b) == 0
	if typeof(a) != typeof(b):
		return false
	return a == b


static func _is_number(v: Variant) -> bool:
	return typeof(v) in [TYPE_BOOL, TYPE_INT, TYPE_FLOAT] or v is ScfgBigInt


## Exact numeric equality (Python's int/float comparison: 2^53 + 1 != 2.0^53).
static func _num_eq(a: Variant, b: Variant) -> bool:
	if a is bool:
		a = 1 if a else 0
	if b is bool:
		b = 1 if b else 0
	if a is float or b is float:
		if a is float and b is float:
			return a == b
		var f: float = a if a is float else b
		var other: Variant = b if a is float else a
		if is_nan(f) or is_inf(f) or f != floorf(f):
			return false
		var exact: Variant = py_int(f)
		if exact is ScfgErr:
			return false
		return _num_eq(exact, other)
	if a is int and b is int:
		return a == b
	if a is ScfgBigInt and b is ScfgBigInt:
		return (a as ScfgBigInt).cmp(b) == 0
	var big: ScfgBigInt = a if a is ScfgBigInt else b
	var n: int = b if a is ScfgBigInt else a
	if n < 0 or big.bit_length() > 63:
		return false  # an ScfgBigInt is non-negative
	var small: Variant = big.to_int()
	if small is ScfgErr:
		return false
	return small == n


## Python's `for x in v` over a JSON-like value: a list's items, a dict's keys, a str's
## characters; anything else (None, numbers, bools) is a TypeError.
## @fallible
static func py_iter(v: Variant) -> Variant:
	if v is Array:
		return v
	if v is Dictionary:
		return (v as Dictionary).keys()
	if v is String or v is StringName:
		var out := []
		var text := str(v)
		for i in text.length():
			out.append(text[i])
		return out
	if v is PackedStringArray:
		return Array(v)
	return ScfgErr.make("TypeError", "'%s' object is not iterable" % py_type(v))


## Python truthiness: None, False, 0, 0.0, "", [], {}, empty bytes and a zero ScfgBigInt are
## falsy; everything else (NaN included) is truthy.
static func py_truthy(v: Variant) -> bool:
	match typeof(v):
		TYPE_NIL:
			return false
		TYPE_BOOL:
			return v
		TYPE_INT:
			return v != 0
		TYPE_FLOAT:
			return v != 0.0  # NaN != 0.0, so NaN is truthy as in Python
		TYPE_STRING, TYPE_STRING_NAME:
			return str(v) != ""
		TYPE_ARRAY, TYPE_DICTIONARY, TYPE_PACKED_BYTE_ARRAY:
			return not v.is_empty()
		TYPE_OBJECT:
			if v is ScfgBigInt:
				return not (v as ScfgBigInt).is_zero()
	return true


## Python's `type(v).__name__` for a JSON-like value (for messages).
static func py_type(v: Variant) -> String:
	match typeof(v):
		TYPE_NIL:
			return "NoneType"
		TYPE_BOOL:
			return "bool"
		TYPE_INT:
			return "int"
		TYPE_FLOAT:
			return "float"
		TYPE_STRING, TYPE_STRING_NAME:
			return "str"
		TYPE_ARRAY:
			return "list"
		TYPE_DICTIONARY:
			return "dict"
		TYPE_PACKED_BYTE_ARRAY:
			return "bytes"
		TYPE_OBJECT:
			if v is ScfgBigInt:
				return "int"
			if v is ScfgErr:
				return (v as ScfgErr).kind
			if v != null:
				return (v as Object).get_class()
	return type_string(typeof(v))


## Python's `repr(v)`, for messages only (never compared): str with Python's quoting and
## escapes, None/True/False, `5.0` for an integral float, dict and list as Python prints them,
## bytes as `b'..'`, an ScfgBigInt as its decimal digits, an ScfgErr as `Kind('message')`.
## Exotic non-printable characters are approximated.
static func py_repr(v: Variant) -> String:
	match typeof(v):
		TYPE_NIL:
			return "None"
		TYPE_BOOL:
			return "True" if v else "False"
		TYPE_INT:
			return str(v)
		TYPE_FLOAT:
			var f: float = v
			if is_nan(f):
				return "nan"
			if is_inf(f):
				return "inf" if f > 0 else "-inf"
			if f == floorf(f) and absf(f) < 1e16:
				return ("-" if f == 0.0 and 1.0 / f < 0 else "") + "%d.0" % int(f)
			return str(f)
		TYPE_STRING, TYPE_STRING_NAME:
			return _repr_str(str(v))
		TYPE_ARRAY:
			var parts := PackedStringArray()
			for x: Variant in v:
				parts.append(py_repr(x))
			return "[" + ", ".join(parts) + "]"
		TYPE_DICTIONARY:
			var parts := PackedStringArray()
			for k: Variant in v:
				parts.append(py_repr(k) + ": " + py_repr(v[k]))
			return "{" + ", ".join(parts) + "}"
		TYPE_PACKED_BYTE_ARRAY:
			return _repr_bytes(v)
		TYPE_OBJECT:
			if v is ScfgBigInt:
				return (v as ScfgBigInt).to_decimal()
			if v is ScfgErr:
				return "%s(%s)" % [(v as ScfgErr).kind, _repr_str((v as ScfgErr).message)]
	return str(v)


## Python's `str(v)` (as an f-string formats it): a str as it is, anything else as its repr (an
## int, None, True, an ScfgBigInt's digits).
static func py_str(v: Variant) -> String:
	if v is String or v is StringName:
		return str(v)
	return py_repr(v)


## Python `template.format(arg)` for ONE positional str argument (no keyword arguments), for
## the one template the core formats: a chain's `explorer_tx`, which build_chain accepts only with
## a single `{}` (ScfgConfig.build_chain). As CPython 3.12:
## - `{{` and `}}` are literal braces; a lone brace, an unmatched `{` or a bad conversion (`!r`,
##   `!s`, `!a` only) is a ValueError;
## - `{}` and `{0}` are the argument (mixing the two is a ValueError), a second `{}` or another
##   index is an IndexError, a name is a KeyError.
## Port limit (stricter than Python, as the TS core's pyFormat1): a field with a non-empty format
## spec (`{:>8}`, a nested `{:{}}`), an index (`{0[1]}`) or an attribute (`{0.upper}`) is a
## ValueError once its argument is found, where Python formats it or raises a TypeError or
## AttributeError. build_chain reports a ValueError as `chain.bad_explorer_tx_braces`.
## @fallible
static func py_format1(template: String, arg: String) -> Variant:
	var out := ""
	var auto := 0  # 0: no field yet, 1: automatic numbering ({}), 2: manual ({0})
	var next_index := 0
	var i := 0
	var n := template.length()
	while i < n:
		var c := template[i]
		if c != "{" and c != "}":
			out += c
			i += 1
			continue
		if i + 1 < n and template[i + 1] == c:
			out += c
			i += 2
			continue
		if c == "}" or i + 1 >= n:
			return ScfgErr.make("ValueError", "Single '%s' encountered in format string" % c)
		var close := template.find("}", i + 1)
		if close < 0:
			return ScfgErr.make("ValueError", "expected '}' before end of string")
		var field := template.substr(i + 1, close - i - 1)
		i = close + 1
		if field.contains("{"):
			return ScfgErr.make("ValueError", "unexpected '{' in field name")
		# the field is name[!conversion][:spec]
		var name := field
		var rest := ""
		for k in field.length():
			if field[k] == "!" or field[k] == ":":
				name = field.substr(0, k)
				rest = field.substr(k)
				break
		var first := name
		for k in name.length():
			if name[k] == "." or name[k] == "[":
				first = name.substr(0, k)
				break
		var index := 0
		if first == "":
			if auto == 2:
				return ScfgErr.make("ValueError", "cannot switch from manual field specification to automatic field numbering")
			auto = 1
			index = next_index
			next_index += 1
		elif _is_ascii_digits(first):
			if auto == 1:
				return ScfgErr.make("ValueError", "cannot switch from automatic field numbering to manual field specification")
			auto = 2
			index = first.to_int() if first.length() < 18 else 1 << 40
		else:
			return ScfgErr.make("KeyError", py_repr(first))
		if index >= 1:
			return ScfgErr.make("IndexError", "Replacement index %d out of range for positional args tuple" % index)
		if first != name:
			return ScfgErr.make("ValueError", "format field %s: indexing and attributes are not supported by the port" % py_repr(name))
		var text := arg
		if rest.begins_with("!"):
			if rest.length() < 2:
				return ScfgErr.make("ValueError", "end of string while looking for conversion specifier")
			if rest.length() > 2 and rest[2] != ":":
				return ScfgErr.make("ValueError", "expected ':' after conversion specifier")
			match rest[1]:
				"r":
					text = py_repr(arg)
				"s":
					pass
				"a":
					text = _ascii_escape(py_repr(arg))
				_:
					return ScfgErr.make("ValueError", "Unknown conversion specifier %s" % rest[1])
			rest = rest.substr(2)
		if rest.length() > 1:
			return ScfgErr.make("ValueError", "format spec %s: format specs are not supported by the port" % py_repr(rest.substr(1)))
		out += text
	return out


static func _is_ascii_digits(s: String) -> bool:
	for k in s.length():
		var c := s.unicode_at(k)
		if c < 0x30 or c > 0x39:
			return false
	return true


## Python's `ascii()` of a repr: every non-ASCII character as \xNN, \uNNNN or \UNNNNNNNN.
static func _ascii_escape(s: String) -> String:
	var out := ""
	for i in s.length():
		var c := s.unicode_at(i)
		if c < 0x80:
			out += String.chr(c)
		elif c <= 0xff:
			out += "\\x%02x" % c
		elif c <= 0xffff:
			out += "\\u%04x" % c
		else:
			out += "\\U%08x" % c
	return out

# --- helpers ----------------------------------------------------------------------------------


## @fallible
static func _float_to_int(f: float) -> Variant:
	if is_nan(f):
		return ScfgErr.make("ValueError", "cannot convert float NaN to integer")
	if is_inf(f):
		return ScfgErr.make("OverflowError", "cannot convert float infinity to integer")
	var t := f - fmod(f, 1.0)  # toward zero, exact for every double
	if t >= -_TWO_63_F and t < _TWO_63_F:
		return int(t)
	if t < 0:
		return ScfgErr.make("OverflowError", "int() below -2**63 is not supported by the port")
	# t >= 2^63 is an integer: m * 2^k with m < 2^53, both exact
	var k := 0
	while t >= float(TWO_53):
		t /= 2.0
		k += 1
	var m := int(t)
	var bytes := PackedByteArray()
	while m > 0:
		bytes.append(m & 0xff)
		m >>= 8
	var out := ScfgBigInt.from_bytes_le(bytes)
	for i in k:
		out = out.add(out)
	return out


## Python's int(s, base) for base 10 or 16, after CPython's own steps: every non-ASCII
## whitespace character becomes a space and every non-ASCII decimal digit its ASCII digit
## (any other non-ASCII character, and DEL, make the literal invalid); then only ASCII
## whitespace (space, \t \n \v \f \r) is stripped, so `"\x1c5"` is invalid although
## `"\x1c5".strip()` is `"5"`.
## @fallible
static func _parse_int(s: String, base: int) -> Variant:
	if base == 10 and s.length() > INT_MAX_STR_DIGITS:
		# CPython's digit limit, counted first: the text is then never copied or converted
		var count := 0
		for i in s.length():
			if decimal_digit(s.unicode_at(i)) >= 0:
				count += 1
		if count > INT_MAX_STR_DIGITS:
			return ScfgErr.make("ValueError", "Exceeds the limit (%d digits) for integer string conversion" % INT_MAX_STR_DIGITS)
	var bad := ScfgErr.make("ValueError", "invalid literal for int() with base %d: %s" % [base, _repr_str(s)])
	var parts := PackedStringArray()  # joined once: String appends are not amortized
	for i in s.length():
		var c := s.unicode_at(i)
		if c < 0x7f:
			parts.append(String.chr(c))
		elif is_py_space(c):
			parts.append(" ")
		else:
			var d := decimal_digit(c)
			if d < 0:
				return bad
			parts.append(String.chr(0x30 + d))
	var t := "".join(parts)
	var a := 0
	var b := t.length()
	while a < b and _is_ascii_space(t.unicode_at(a)):
		a += 1
	while b > a and _is_ascii_space(t.unicode_at(b - 1)):
		b -= 1
	var body := t.substr(a, b - a)
	if body == "":
		return bad
	var negative := false
	if body[0] == "+" or body[0] == "-":
		negative = body[0] == "-"
		body = body.substr(1)
	var prefixed := false
	if base == 16 and (body.begins_with("0x") or body.begins_with("0X")):
		body = body.substr(2)
		prefixed = true
		if body.begins_with("_"):
			body = body.substr(1)
	var digits := "0123456789" if base == 10 else "0123456789abcdefABCDEF"
	var kept := PackedStringArray()
	var prev_digit := false
	for i in body.length():
		var ch := body[i]
		if ch == "_":
			if not prev_digit or i == body.length() - 1:
				return bad
			prev_digit = false
		elif digits.contains(ch):
			kept.append(ch)
			prev_digit = true
		else:
			return bad
	var clean := "".join(kept)
	if clean == "" or (prefixed and not prev_digit):
		return bad
	if base == 10 and clean.length() > INT_MAX_STR_DIGITS:
		return ScfgErr.make("ValueError", "Exceeds the limit (%d digits) for integer string conversion" % INT_MAX_STR_DIGITS)
	var big: ScfgBigInt = (ScfgBigInt.from_decimal(clean) if base == 10
		else ScfgBigInt.from_hex(clean))  # err-ok: ASCII digits of the base, checked above
	return _normalize(big, negative)


## CPython's Py_ISSPACE: space, \t \n \v \f \r (not \x1c-\x1f).
static func _is_ascii_space(c: int) -> bool:
	return c == 0x20 or (c >= 0x09 and c <= 0x0d)


## An int when the (signed) value fits in 64 bits, else the ScfgBigInt; a negative value below
## -2^63 is an OverflowError.
## @fallible
static func _normalize(big: ScfgBigInt, negative: bool) -> Variant:
	var bits := big.bit_length()
	if not negative:
		if bits <= 63:
			return big.to_int()  # err-ok: bit_length() <= 63
		return big
	if bits <= 63:
		return -int(big.to_int())  # err-ok: bit_length() <= 63
	if big.to_hex() == "8000000000000000":
		return -9223372036854775807 - 1
	return ScfgErr.make("OverflowError", "int() below -2**63 is not supported by the port")


static func _repr_str(s: String) -> String:
	# Built as parts and joined once: appending to a String is not amortized (long input would
	# make this quadratic, and error messages repr untrusted text).
	var q := "\"" if s.contains("'") and not s.contains("\"") else "'"
	var out := PackedStringArray([q])
	for i in s.length():
		var c := s.unicode_at(i)
		var ch := String.chr(c)
		if ch == "\\" or ch == q:
			out.append("\\" + ch)
		elif c == 0x0a:
			out.append("\\n")
		elif c == 0x0d:
			out.append("\\r")
		elif c == 0x09:
			out.append("\\t")
		elif c < 0x20 or (c >= 0x7f and c <= 0xa0) or c == 0xad:
			out.append("\\x%02x" % c)
		elif (c == 0x1680 or (c >= 0x2000 and c <= 0x200f) or (c >= 0x2028 and c <= 0x202f)
				or (c >= 0x205f and c <= 0x2064) or c == 0x3000 or c == 0xfeff):
			out.append("\\u%04x" % c)
		else:
			out.append(ch)
	out.append(q)
	return "".join(out)


static func _repr_bytes(b: PackedByteArray) -> String:
	var q := "\"" if b.has(0x27) and not b.has(0x22) else "'"
	var out := "b" + q
	for c in b:
		var ch := String.chr(c)
		if ch == "\\" or ch == q:
			out += "\\" + ch
		elif c == 0x0a:
			out += "\\n"
		elif c == 0x0d:
			out += "\\r"
		elif c == 0x09:
			out += "\\t"
		elif c < 0x20 or c >= 0x7f:
			out += "\\x%02x" % c
		else:
			out += ch
	return out + q
