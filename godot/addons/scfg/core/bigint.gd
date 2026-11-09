## A non-negative arbitrary-precision integer.
##
## GDScript's int is signed 64-bit, which overflows on uint256 values, 18-decimal token amounts
## and field elements. ScfgBigInt stores the value as little-endian base-2^24 limbs in a
## PackedInt64Array, so a limb product (< 2^48) plus carries always fits in an int.
## Values are immutable: every operation returns a new ScfgBigInt. Parsers return null for
## malformed input; violated preconditions (negative results, oversized conversions) assert.
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


static func from_int(n: int) -> ScfgBigInt:
	assert(n >= 0, "ScfgBigInt.from_int: negative value")
	var l := PackedInt64Array()
	while n > 0:
		l.append(n & MASK)
		n >>= BITS
	return _of(l)


## Decimal digits only (no sign, spaces or separators); leading zeros are fine.
static func from_decimal(s: String) -> ScfgBigInt:
	var digits := s.to_ascii_buffer()
	if digits.is_empty() or digits.size() != s.length():
		return null
	for c in digits:
		if c < 48 or c > 57:
			return null
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


## Hex digits, either case, with an optional 0x prefix.
static func from_hex(s: String) -> ScfgBigInt:
	if s.begins_with("0x") or s.begins_with("0X"):
		s = s.substr(2)
	var chars := s.to_ascii_buffer()
	if chars.is_empty() or chars.size() != s.length():
		return null
	for c in chars:
		if not ((c >= 48 and c <= 57) or (c >= 97 and c <= 102) or (c >= 65 and c <= 70)):
			return null
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


## Exactly `length` bytes, little-endian, zero-padded. Asserts that the value fits.
func to_bytes_le(length: int) -> PackedByteArray:
	assert(bit_length() <= length * 8, "ScfgBigInt.to_bytes_le: value does not fit")
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


## Asserts the value is below 2^63.
func to_int() -> int:
	assert(bit_length() <= 63, "ScfgBigInt.to_int: value is 2^63 or more")
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


## Asserts self >= o.
func sub(o: ScfgBigInt) -> ScfgBigInt:
	assert(cmp(o) >= 0, "ScfgBigInt.sub: negative result")
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


## k in [0, 2^38): keeps limb * k + carry inside an int.
func mul_small(k: int) -> ScfgBigInt:
	assert(k >= 0 and k < (1 << 38), "ScfgBigInt.mul_small: k out of range")
	return _of(_mul_add_small(limbs, k, 0))


## [quotient, remainder]. Asserts o != 0.
func divmod(o: ScfgBigInt) -> Array:
	assert(not o.is_zero(), "ScfgBigInt.divmod: division by zero")
	if cmp(o) < 0:
		return [ScfgBigInt.new(), _of(limbs.duplicate())]
	var qr := _divmod_limbs(limbs, o.limbs)
	return [_of(qr[0]), _of(qr[1])]


func mod(o: ScfgBigInt) -> ScfgBigInt:
	if cmp(o) < 0:
		return _of(limbs.duplicate())
	return divmod(o)[1]


## self^e mod m (left-to-right square and multiply). Asserts m != 0.
func pow_mod(e: ScfgBigInt, m: ScfgBigInt) -> ScfgBigInt:
	assert(not m.is_zero(), "ScfgBigInt.pow_mod: zero modulus")
	var result := ScfgBigInt.from_int(1).mod(m)
	var base := mod(m)
	for i in range(e.bit_length() - 1, -1, -1):
		result = result.mul(result).mod(m)
		if (e.limbs[i / BITS] >> (i % BITS)) & 1:
			result = result.mul(base).mod(m)
	return result


## self mod 2^n.
func low_bits(n: int) -> ScfgBigInt:
	assert(n >= 0, "ScfgBigInt.low_bits: negative n")
	var full := n / BITS
	if full >= limbs.size():
		return _of(limbs.duplicate())
	var out := limbs.slice(0, full + 1)
	out[full] = out[full] & ((1 << (n % BITS)) - 1)
	return _of(out)


# --- limb helpers ---------------------------------------------------------------------------


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
