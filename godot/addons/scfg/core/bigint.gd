## A non-negative arbitrary-precision integer.
##
## GDScript's int is signed 64-bit, which overflows on uint256 values, 18-decimal token amounts
## and field elements. ScfgBigInt stores the value as little-endian base-2^24 limbs in a
## PackedInt64Array, so a limb product (< 2^48) plus carries always fits in an int.
## Values are immutable: every operation returns a new ScfgBigInt.
##
## Errors are values (see ScfgErr; the fallible functions are marked `@fallible`): a malformed
## string is a ValueError, a result that an unsigned value cannot hold (a negative difference,
## a negative from_int, a conversion that does not fit) an OverflowError, as Python's
## int.to_bytes raises, and a zero divisor a ZeroDivisionError (a zero pow_mod modulus is
## Python's ValueError). Operations that cannot fail return a typed ScfgBigInt.
class_name ScfgBigInt
extends RefCounted

const BITS := 24
const BASE := 1 << BITS
const MASK := BASE - 1
const _DEC_CHUNK := 7  # 10^7 < 2^24: one short multiply/divide per 7 decimal digits
const _DEC_BASE := 10000000

## Little-endian limbs, each in [0, 2^24), without trailing zero limbs (zero is empty).
## Read-only: Godot shares packed arrays by reference, so writing here changes every alias.
var limbs: PackedInt64Array = PackedInt64Array()


static func _of(l: PackedInt64Array) -> ScfgBigInt:
	var n := l.size()
	while n > 0 and l[n - 1] == 0:
		n -= 1
	if n != l.size():
		l.resize(n)
	var out := ScfgBigInt.new()
	out.limbs = l
	return out


# --- constructors ---------------------------------------------------------------------------


## A negative n is an OverflowError.
## @fallible
static func from_int(n: int) -> Variant:
	if n < 0:
		return ScfgErr.make("OverflowError", "ScfgBigInt.from_int: negative value %d" % n)
	return _from_nonneg(n)


static func _from_nonneg(n: int) -> ScfgBigInt:
	var l := PackedInt64Array()
	while n > 0:
		l.append(n & MASK)
		n >>= BITS
	return _of(l)


## ASCII decimal digits only (no sign, spaces, underscores or other scripts); leading zeros
## are fine. Anything else is a ValueError (ScfgPy.py_int has Python's int() rules).
## @fallible
static func from_decimal(s: String) -> Variant:
	if s.is_empty():
		return ScfgErr.make("ValueError", _bad_literal(s, 10))
	for i in s.length():
		var c := s.unicode_at(i)
		if c < 48 or c > 57:
			return ScfgErr.make("ValueError", _bad_literal(s, 10))
	var digits := s.to_ascii_buffer()
	var l := PackedInt64Array()
	var first := digits.size() % _DEC_CHUNK
	if first == 0:
		first = _DEC_CHUNK
	var pos := 0
	var width := first
	while pos < digits.size():
		var chunk := 0
		var mult := 1
		for i in range(pos, pos + width):
			chunk = chunk * 10 + (digits[i] - 48)
			mult *= 10
		l = _mul_add_small(l, mult, chunk)
		pos += width
		width = _DEC_CHUNK
	return _of(l)


## ASCII hex digits, either case, with an optional 0x or 0X prefix (a call site that must
## reject `0X` checks that itself). Anything else is a ValueError.
## @fallible
static func from_hex(s: String) -> Variant:
	var text := s
	if s.begins_with("0x") or s.begins_with("0X"):
		s = s.substr(2)
	if s.is_empty():
		return ScfgErr.make("ValueError", _bad_literal(text, 16))
	for i in s.length():
		var c := s.unicode_at(i)
		if not ((c >= 48 and c <= 57) or (c >= 97 and c <= 102) or (c >= 65 and c <= 70)):
			return ScfgErr.make("ValueError", _bad_literal(text, 16))
	var l := PackedInt64Array()
	var end := s.length()
	while end > 0:
		var start := maxi(0, end - 6)
		l.append(s.substr(start, end - start).hex_to_int())
		end = start
	return _of(l)


static func from_bytes_le(b: PackedByteArray) -> ScfgBigInt:
	var n := b.size()
	var l := PackedInt64Array()
	l.resize((n + 2) / 3)
	for i in l.size():
		var j := i * 3
		var v: int = b[j]
		if j + 1 < n:
			v |= b[j + 1] << 8
		if j + 2 < n:
			v |= b[j + 2] << 16
		l[i] = v
	return _of(l)


static func from_bytes_be(b: PackedByteArray) -> ScfgBigInt:
	var r := b.duplicate()
	r.reverse()
	return from_bytes_le(r)


# --- conversions ----------------------------------------------------------------------------


func to_decimal() -> String:
	if limbs.is_empty():
		return "0"
	var work := limbs.duplicate()
	var chunks := PackedInt64Array()
	while not work.is_empty():
		var rem := 0
		for i in range(work.size() - 1, -1, -1):
			var cur: int = (rem << BITS) | work[i]
			work[i] = cur / _DEC_BASE
			rem = cur % _DEC_BASE
		chunks.append(rem)
		var n := work.size()
		while n > 0 and work[n - 1] == 0:
			n -= 1
		work.resize(n)
	var out := str(chunks[chunks.size() - 1])
	for i in range(chunks.size() - 2, -1, -1):
		out += "%07d" % chunks[i]
	return out


## Lowercase, no 0x prefix, "0" for zero.
func to_hex() -> String:
	if limbs.is_empty():
		return "0"
	var out := "%x" % limbs[limbs.size() - 1]
	for i in range(limbs.size() - 2, -1, -1):
		out += "%06x" % limbs[i]
	return out


## Exactly `length` bytes, little-endian, zero-padded. A value that does not fit is an
## OverflowError and a negative length a ValueError, as in Python's int.to_bytes.
## @fallible
func to_bytes_le(length: int) -> Variant:
	if length < 0:
		return ScfgErr.make("ValueError", "length argument must be non-negative")
	if bit_length() > length * 8:
		return ScfgErr.make("OverflowError", "int too big to convert")
	var out := PackedByteArray()
	out.resize(length)
	out.fill(0)
	for i in limbs.size():
		var v: int = limbs[i]
		for k in 3:
			var j := i * 3 + k
			if j < length:
				out[j] = (v >> (8 * k)) & 0xff
	return out


## The value as an int; 2^63 or more is an OverflowError.
## @fallible
func to_int() -> Variant:
	if bit_length() > 63:
		return ScfgErr.make("OverflowError", "ScfgBigInt.to_int: %s is 2^63 or more" % to_decimal())
	var v := 0
	for i in range(limbs.size() - 1, -1, -1):
		v = (v << BITS) | limbs[i]
	return v


func bit_length() -> int:
	if limbs.is_empty():
		return 0
	var top: int = limbs[limbs.size() - 1]
	var bits := 0
	while top > 0:
		bits += 1
		top >>= 1
	return (limbs.size() - 1) * BITS + bits


func is_zero() -> bool:
	return limbs.is_empty()


## -1, 0 or 1.
func cmp(o: ScfgBigInt) -> int:
	return _cmp_limbs(limbs, o.limbs)


# --- arithmetic -----------------------------------------------------------------------------


func add(o: ScfgBigInt) -> ScfgBigInt:
	var a := limbs
	var b := o.limbs
	if a.size() < b.size():
		var t := a
		a = b
		b = t
	var out := PackedInt64Array()
	out.resize(a.size() + 1)
	var carry := 0
	for i in a.size():
		var s: int = a[i] + carry
		if i < b.size():
			s += b[i]
		out[i] = s & MASK
		carry = s >> BITS
	out[a.size()] = carry
	return _of(out)


## self - o; a negative result is an OverflowError.
## @fallible
func sub(o: ScfgBigInt) -> Variant:
	if cmp(o) < 0:
		return ScfgErr.make("OverflowError", "ScfgBigInt.sub: negative result")
	return _sub(o)


## sub() for self >= o.
func _sub(o: ScfgBigInt) -> ScfgBigInt:
	var out := limbs.duplicate()
	var b := o.limbs
	var borrow := 0
	for i in out.size():
		var d: int = out[i] - borrow
		if i < b.size():
			d -= b[i]
		elif borrow == 0:
			break
		if d < 0:
			d += BASE
			borrow = 1
		else:
			borrow = 0
		out[i] = d
	return _of(out)


func mul(o: ScfgBigInt) -> ScfgBigInt:
	return _of(_mul_limbs(limbs, o.limbs))


## self * k for an int k >= 0 (fast below 2^38, where limb * k + carry fits an int); a
## negative k is an OverflowError.
## @fallible
func mul_small(k: int) -> Variant:
	if k < 0:
		return ScfgErr.make("OverflowError", "ScfgBigInt.mul_small: negative factor %d" % k)
	if k >= (1 << 38):
		return mul(_from_nonneg(k))
	return _of(_mul_add_small(limbs, k, 0))


## [quotient, remainder]; a zero divisor is a ZeroDivisionError.
## @fallible
func divmod(o: ScfgBigInt) -> Variant:
	if o.is_zero():
		return ScfgErr.make("ZeroDivisionError", "integer division or modulo by zero")
	if cmp(o) < 0:
		return [ScfgBigInt.new(), _of(limbs.duplicate())]
	var qr := _divmod_limbs(limbs, o.limbs)
	return [_of(qr[0]), _of(qr[1])]


## self mod o; a zero divisor is a ZeroDivisionError.
## @fallible
func mod(o: ScfgBigInt) -> Variant:
	if o.is_zero():
		return ScfgErr.make("ZeroDivisionError", "integer division or modulo by zero")
	return _mod(o)


## mod() for o != 0.
func _mod(o: ScfgBigInt) -> ScfgBigInt:
	if cmp(o) < 0:
		return _of(limbs.duplicate())
	return _of(_divmod_limbs(limbs, o.limbs)[1])


## self^e mod m (left-to-right square and multiply); a zero m is a ValueError, as Python's
## pow(x, e, 0) raises.
## @fallible
func pow_mod(e: ScfgBigInt, m: ScfgBigInt) -> Variant:
	if m.is_zero():
		return ScfgErr.make("ValueError", "pow() 3rd argument cannot be 0")
	var result := _from_nonneg(1)._mod(m)
	var base := _mod(m)
	for i in range(e.bit_length() - 1, -1, -1):
		result = result.mul(result)._mod(m)
		if (e.limbs[i / BITS] >> (i % BITS)) & 1:
			result = result.mul(base)._mod(m)
	return result


## self mod 2^n; a negative n is a ValueError.
## @fallible
func low_bits(n: int) -> Variant:
	if n < 0:
		return ScfgErr.make("ValueError", "negative shift count")
	var full := n / BITS
	if full >= limbs.size():
		return _of(limbs.duplicate())
	var out := limbs.slice(0, full + 1)
	out[full] = out[full] & ((1 << (n % BITS)) - 1)
	return _of(out)


# --- limb helpers ---------------------------------------------------------------------------


static func _bad_literal(s: String, base: int) -> String:
	return "invalid literal for ScfgBigInt (base %d): %s" % [base, ScfgPy.py_repr(s)]


static func _cmp_limbs(a: PackedInt64Array, b: PackedInt64Array) -> int:
	if a.size() != b.size():
		return 1 if a.size() > b.size() else -1
	for i in range(a.size() - 1, -1, -1):
		if a[i] != b[i]:
			return 1 if a[i] > b[i] else -1
	return 0


## a * k + add as new limbs (k < 2^38, add < 2^24 + 2^38).
static func _mul_add_small(a: PackedInt64Array, k: int, add: int) -> PackedInt64Array:
	var out := PackedInt64Array()
	out.resize(a.size())
	var carry := add
	for i in a.size():
		var t: int = a[i] * k + carry
		out[i] = t & MASK
		carry = t >> BITS
	while carry > 0:
		out.append(carry & MASK)
		carry >>= BITS
	return out


static func _mul_limbs(a: PackedInt64Array, b: PackedInt64Array) -> PackedInt64Array:
	var na := a.size()
	var nb := b.size()
	var out := PackedInt64Array()
	if na == 0 or nb == 0:
		return out
	out.resize(na + nb)
	out.fill(0)
	for i in na:
		var ai: int = a[i]
		if ai == 0:
			continue
		var carry := 0
		for j in nb:
			var t: int = out[i + j] + ai * b[j] + carry
			out[i + j] = t & MASK
			carry = t >> BITS
		out[i + nb] = carry
	return out


## Knuth's Algorithm D (TAOCP 4.3.1) in base 2^24. Requires u >= v > 0.
static func _divmod_limbs(u_in: PackedInt64Array, v_in: PackedInt64Array) -> Array:
	var n := v_in.size()
	if n == 1:
		var d: int = v_in[0]
		var q1 := PackedInt64Array()
		q1.resize(u_in.size())
		var rem := 0
		for i in range(u_in.size() - 1, -1, -1):
			var cur: int = (rem << BITS) | u_in[i]
			q1[i] = cur / d
			rem = cur % d
		return [q1, PackedInt64Array([rem])]
	# normalize so the divisor's top limb has its high bit set
	var shift := 0
	var top: int = v_in[n - 1]
	while top < (BASE >> 1):
		top <<= 1
		shift += 1
	var v := _shl_bits(v_in, shift, n)
	var u := _shl_bits(u_in, shift, u_in.size() + 1)
	var m := u.size() - n
	var q := PackedInt64Array()
	q.resize(m)
	var v1: int = v[n - 1]
	var v2: int = v[n - 2]
	for j in range(m - 1, -1, -1):
		var num: int = (u[j + n] << BITS) | u[j + n - 1]
		var qhat: int = num / v1
		var rhat: int = num % v1
		while qhat >= BASE or qhat * v2 > ((rhat << BITS) | u[j + n - 2]):
			qhat -= 1
			rhat += v1
			if rhat >= BASE:
				break
		# u[j .. j+n] -= qhat * v
		var borrow := 0
		var carry := 0
		for i in n:
			var p: int = qhat * v[i] + carry
			carry = p >> BITS
			var t: int = u[i + j] - (p & MASK) - borrow
			if t < 0:
				u[i + j] = t + BASE
				borrow = 1
			else:
				u[i + j] = t
				borrow = 0
		var t2: int = u[j + n] - carry - borrow
		if t2 < 0:
			# qhat was one too large: add v back (the carry out cancels the borrow)
			u[j + n] = t2 + BASE
			qhat -= 1
			var c := 0
			for i in n:
				var s: int = u[i + j] + v[i] + c
				u[i + j] = s & MASK
				c = s >> BITS
			u[j + n] = (u[j + n] + c) & MASK
		else:
			u[j + n] = t2
		q[j] = qhat
	var r := _shr_bits(u.slice(0, n), shift)
	return [q, r]


## a << shift (shift < 24) into `size` limbs.
static func _shl_bits(a: PackedInt64Array, shift: int, size: int) -> PackedInt64Array:
	var out := PackedInt64Array()
	out.resize(size)
	out.fill(0)
	var carry := 0
	for i in a.size():
		var t: int = (a[i] << shift) | carry
		out[i] = t & MASK
		carry = t >> BITS
	if a.size() < size:
		out[a.size()] = carry
	return out


## a >> shift (shift < 24).
static func _shr_bits(a: PackedInt64Array, shift: int) -> PackedInt64Array:
	if shift == 0:
		return a
	var out := PackedInt64Array()
	out.resize(a.size())
	for i in a.size():
		var hi: int = a[i + 1] if i + 1 < a.size() else 0
		out[i] = ((a[i] >> shift) | (hi << (BITS - shift))) & MASK
	return out
