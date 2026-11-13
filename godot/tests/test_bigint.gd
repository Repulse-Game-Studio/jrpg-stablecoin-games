extends RefCounted

const T := preload("res://tests/t.gd")

# [decimal, hex] pairs: 0, 1, 2^24-1, 2^24, 2^63, 2^256-1, 10**18 * 9999999
const ROUND_TRIPS := [
	["0", "0"],
	["1", "1"],
	["16777215", "ffffff"],
	["16777216", "1000000"],
	["9223372036854775808", "8000000000000000"],
	["115792089237316195423570985008687907853269984665640564039457584007913129639935",
		"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"],
	["9999999000000000000000000", "8459508334a94a29c0000"],
]
const P_HEX := "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffed"


func test_decimal_and_hex_round_trips() -> bool:
	for pair: Array in ROUND_TRIPS:
		var dec: String = pair[0]
		var hex: String = pair[1]
		var from_dec: ScfgBigInt = ScfgBigInt.from_decimal(dec)
		var from_hex: ScfgBigInt = ScfgBigInt.from_hex(hex)
		T.eq(from_dec.to_decimal(), dec, "decimal round trip of " + dec)
		T.eq(from_dec.to_hex(), hex, "decimal -> hex of " + dec)
		T.eq(from_hex.to_hex(), hex, "hex round trip of " + hex)
		T.eq(from_hex.to_decimal(), dec, "hex -> decimal of " + hex)
		T.eq(ScfgBigInt.from_hex("0x" + hex).to_decimal(), dec, "0x prefix on " + hex)
		T.eq(from_dec.cmp(from_hex), 0, "cmp of equal values " + dec)
	T.eq(ScfgBigInt.from_hex("0").is_zero(), true, "zero is zero")
	T.eq(ScfgBigInt.from_int(1).is_zero(), false, "one is not zero")
	return T.done()


func test_parsing_edges() -> bool:
	T.eq(ScfgBigInt.from_decimal("0007").to_decimal(), "7", "leading zeros (decimal)")
	T.eq(ScfgBigInt.from_hex("0x00FF").to_hex(), "ff", "leading zeros and capitals (hex)")
	for bad: String in ["", "-1", "12a", " 1", "1.5", "+1", "1_0", "\u0663"]:
		T.is_err(ScfgBigInt.from_decimal(bad), "ValueError", "from_decimal rejects %s" % bad)
	for bad: String in ["", "0x", "xyz", "0xg1", "-ff", " ff", "f_f", "0x\u0663"]:
		T.is_err(ScfgBigInt.from_hex(bad), "ValueError", "from_hex rejects %s" % bad)
	return T.done()


func test_ints_and_bytes() -> bool:
	for n: int in [0, 1, 255, 16777215, 16777216, 9007199254740993, 9223372036854775807]:
		T.eq(ScfgBigInt.from_int(n).to_int(), n, "from_int/to_int %d" % n)
		T.eq(ScfgBigInt.from_int(n).to_decimal(), str(n), "from_int decimal %d" % n)
	var be := PackedByteArray(range(1, 34))  # 33 bytes 01..21, big-endian
	var v := ScfgBigInt.from_bytes_be(be)
	T.eq(v.to_hex(), "102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2021", "from_bytes_be")
	var le := be.duplicate()
	le.reverse()
	T.eq(ScfgBigInt.from_bytes_le(le).cmp(v), 0, "from_bytes_le of the reversed bytes")
	T.eq(v.to_bytes_le(33), le, "to_bytes_le(33)")
	var padded := le.duplicate()
	padded.resize(40)
	T.eq(v.to_bytes_le(40), padded, "to_bytes_le pads with zeros")
	T.eq(ScfgBigInt.from_int(0).to_bytes_le(4), PackedByteArray([0, 0, 0, 0]), "zero to 4 bytes")
	T.eq(ScfgBigInt.from_bytes_le(PackedByteArray()).is_zero(), true, "no bytes is zero")
	T.eq(ScfgBigInt.from_bytes_be(PackedByteArray([0, 0, 1])).to_int(), 1, "leading zero bytes")
	return T.done()


func test_arithmetic_basics() -> bool:
	var a: ScfgBigInt = ScfgBigInt.from_hex("1" + "0".repeat(46) + "3039")  # 2^200 + 12345
	T.eq(a.to_decimal(), "1606938044258990275541962092341162602522202993782792835313721", "2^200+12345")
	T.eq(a.add(ScfgBigInt.from_int(1)).sub(a).to_int(), 1, "add then sub")
	T.eq(a.sub(a).is_zero(), true, "a - a")
	T.eq(a.low_bits(61).to_int(), 12345, "low_bits(61)")
	T.eq(a.low_bits(0).is_zero(), true, "low_bits(0)")
	T.eq(a.low_bits(400).cmp(a), 0, "low_bits wider than the value")
	T.eq(a.mul_small(999999).to_decimal(),
		"1606936437320946016551686550379070261359600471579799052520885686279", "mul_small")
	T.eq(a.mul_small(0).is_zero(), true, "mul_small(0)")
	T.eq(a.cmp(a.add(ScfgBigInt.from_int(1))), -1, "cmp less")
	T.eq(a.add(ScfgBigInt.from_int(1)).cmp(a), 1, "cmp greater")
	var x: ScfgBigInt = ScfgBigInt.from_hex("1" + "0".repeat(31) + "1")  # 2^128 + 1
	var y: ScfgBigInt = ScfgBigInt.from_hex("f".repeat(16))  # 2^64 - 1
	T.eq(x.mul(y).to_decimal(), "6277101735386680763495507056286727952657427581105975853055", "mul")
	var n: ScfgBigInt = ScfgBigInt.from_decimal(
		"265613988875874769338781322035779626829233452653394495974574961739092490901302182994384699044001")
	var d: ScfgBigInt = ScfgBigInt.from_decimal("1798465042647412146620280340569649349263594")
	var qr: Array = n.divmod(d)
	T.eq((qr[0] as ScfgBigInt).to_decimal(),
		"147689269781346654697366079240021362540968891926345127", "3^200 // (7^50+12345)")
	T.eq((qr[1] as ScfgBigInt).to_decimal(),
		"1480513908709133232415680991351079358637563", "3^200 % (7^50+12345)")
	T.eq(n.mod(d).cmp(qr[1]), 0, "mod equals divmod remainder")
	var small: Array = ScfgBigInt.from_int(5).divmod(n)
	T.eq([(small[0] as ScfgBigInt).to_int(), (small[1] as ScfgBigInt).to_int()], [0, 5], "a < b")
	return T.done()


func test_mul_divmod_identities() -> bool:
	var rng := RandomNumberGenerator.new()
	rng.seed = 20261109
	for k in 50:
		var a := ScfgBigInt.from_bytes_be(_random_bytes(rng, rng.randi_range(1, 64), k))
		# mostly no longer than a, so Algorithm D (not the a < b shortcut) runs
		var b_len := rng.randi_range(1, 40) if k % 5 == 4 else rng.randi_range(1, maxi(1, (a.bit_length() + 7) / 8))
		var b := ScfgBigInt.from_bytes_be(_random_bytes(rng, b_len, k + 1))
		if b.is_zero():
			b = ScfgBigInt.from_int(k + 1)
		var qr: Array = a.divmod(b)
		var q: ScfgBigInt = qr[0]
		var r: ScfgBigInt = qr[1]
		var label := "pair %d (%s / %s)" % [k, a.to_hex(), b.to_hex()]
		T.eq(q.mul(b).add(r).cmp(a), 0, "q*b + r == a for " + label)
		T.eq(r.cmp(b), -1, "r < b for " + label)
		T.eq(a.mul(b).divmod(b)[0].cmp(a), 0, "(a*b) // b == a for " + label)
		T.eq(a.mul(b).cmp(b.mul(a)), 0, "a*b == b*a for " + label)
		T.eq(a.add(b).sub(b).cmp(a), 0, "(a + b) - b == a for " + label)
		T.eq(a.sub(r).add(r).cmp(a), 0, "(a - r) + r == a for " + label)
	return T.done()


func test_sub_borrows() -> bool:
	var one: ScfgBigInt = ScfgBigInt.from_int(1)
	T.eq(ScfgBigInt.from_hex("1" + "0".repeat(60)).sub(one).to_hex(), "f".repeat(60),
		"2^240 - 1: a borrow through every limb")
	var max256: ScfgBigInt = ScfgBigInt.from_hex("f".repeat(64))
	T.eq(max256.add(one).to_hex(), "1" + "0".repeat(64), "2^256 - 1 + 1")
	T.eq(max256.add(one).sub(one).to_hex(), "f".repeat(64), "(2^256 - 1 + 1) - 1")
	T.eq(ScfgBigInt.from_hex("5000000000000").sub(one).to_hex(), "4ffffffffffff",
		"a borrow past the subtrahend's length that stops below the top limb")
	T.eq(ScfgBigInt.from_hex("1000000000001").sub(ScfgBigInt.from_int(2)).to_hex(), "ffffffffffff",
		"a borrow out of the low limb that empties the top limb")
	T.eq(ScfgBigInt.from_hex("123000000").sub(ScfgBigInt.from_hex("ffffff")).to_hex(), "122000001",
		"a full-limb subtrahend with a borrow")
	T.eq(ScfgBigInt.from_hex("10000000000000000").sub(ScfgBigInt.from_hex("ffffffffffffffff")).to_hex(),
		"1", "2^64 - (2^64 - 1)")
	return T.done()


# Divisions that take Algorithm D's rare branches (found by simulating it in Python): the
# first three need the add-back step, the last starts with a quotient estimate >= 2^24.
const KNUTH_EDGES := [
	["7fffff800000000000000000", "800000000000000001", "16777214", "2361183241434805829634"],
	["800000000000000003", "200000000000000001", "3", "590295810358705651712"],
	["7fff008000000000000000", "8000000000000001", "16776704", "9223372036837999104"],
	["800000000000fffffe000000", "800000ffffff", "281474943156231", "140737303805959"],
]


func test_divmod_add_back_and_qhat_edges() -> bool:
	for row: Array in KNUTH_EDGES:
		var qr: Array = ScfgBigInt.from_hex(row[0]).divmod(ScfgBigInt.from_hex(row[1]))
		T.eq((qr[0] as ScfgBigInt).to_decimal(), row[2], "quotient of %s / %s" % [row[0], row[1]])
		T.eq((qr[1] as ScfgBigInt).to_decimal(), row[3], "remainder of %s / %s" % [row[0], row[1]])
	return T.done()


func test_pow_mod_inverts_two_mod_p() -> bool:
	var p: ScfgBigInt = ScfgBigInt.from_hex(P_HEX)
	T.eq(p.to_decimal(),
		"57896044618658097711785492504343953926634992332820282019728792003956564819949", "p")
	var two: ScfgBigInt = ScfgBigInt.from_int(2)
	var inv2: ScfgBigInt = two.pow_mod(p.sub(two), p)
	T.eq(inv2.mul(two).mod(p).to_decimal(), "1", "pow_mod(2, p-2, p) * 2 mod p == 1")
	T.eq(inv2.to_decimal(),
		"28948022309329048855892746252171976963317496166410141009864396001978282409975", "(p+1)/2")
	T.eq(two.pow_mod(ScfgBigInt.from_int(0), p).to_int(), 1, "x^0")
	T.eq(ScfgBigInt.from_int(3).pow_mod(ScfgBigInt.from_int(5), ScfgBigInt.from_int(7)).to_int(), 5,
		"3^5 mod 7")
	return T.done()


# Random bytes, with some all-0xff, sparse and leading-zero shapes mixed in for the edges.
func _random_bytes(rng: RandomNumberGenerator, n: int, k: int) -> PackedByteArray:
	var out := PackedByteArray()
	out.resize(n)
	for i in n:
		match k % 5:
			0:
				out[i] = 0xff
			1:
				out[i] = 0 if i % 4 else rng.randi_range(0, 255)
			_:
				out[i] = rng.randi_range(0, 255)
	return out


func test_bigint_errors_are_values() -> bool:
	var one: ScfgBigInt = ScfgBigInt.from_int(1)
	var zero := ScfgBigInt.new()
	var big: ScfgBigInt = ScfgBigInt.from_hex("8000000000000000")  # 2^63
	T.is_err(zero.sub(one), "OverflowError", "0 - 1 has no unsigned result")
	T.is_err(ScfgBigInt.from_int(-1), "OverflowError", "from_int(-1)")
	T.is_err(big.to_int(), "OverflowError", "to_int(2^63)")
	T.eq(big.sub(one).to_int(), 9223372036854775807, "to_int(2^63 - 1)")
	T.is_err(big.to_bytes_le(7), "OverflowError", "2^63 does not fit 7 bytes")
	T.eq(big.to_bytes_le(8).size(), 8, "2^63 fits 8 bytes")
	T.is_err(one.to_bytes_le(-1), "ValueError", "negative length")
	T.is_err(one.divmod(zero), "ZeroDivisionError", "divmod by zero")
	T.is_err(one.mod(zero), "ZeroDivisionError", "mod by zero")
	T.is_err(zero.mod(zero), "ZeroDivisionError", "0 mod 0")
	T.is_err(one.pow_mod(one, zero), "ValueError", "pow() 3rd argument cannot be 0")
	T.is_err(one.low_bits(-1), "ValueError", "negative bit count")
	T.is_err(one.mul_small(-1), "OverflowError", "a negative factor")
	T.eq(one.mul_small(1 << 40).to_hex(), "10000000000", "a factor above 2^38 falls back to mul")
	T.eq(big.mul_small(9223372036854775807).to_decimal(), "85070591730234615856620279821087277056",
		"2^63 * (2^63 - 1)")
	return T.done()
