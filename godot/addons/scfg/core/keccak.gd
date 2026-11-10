## Keccak-256 as used by Ethereum: the original Keccak padding (0x01), not NIST SHA3-256 (0x06).
##
## Mirrors scfg_core/keccak.py (XKCP's CompactFIPS202 sponge, rate 1088, capacity 512). The
## 25 lanes of Keccak-f[1600] live in signed 64-bit ints, lane index x + 5*y. Left shifts wrap,
## which is what a lane wants, but GDScript's `>>` is arithmetic, so the bits a rotation brings
## in from the right are masked: rol(x, n) = (x << n) | ((x >> (64 - n)) & ((1 << n) - 1)).
class_name ScfgKeccak
extends RefCounted

const RATE_BYTES := 136
const _SUFFIX := 0x01

const _RC: Array[int] = [
	1, 32898, -9223372036854742902, -9223372034707259392, 32907, 2147483649,
	-9223372034707259263, -9223372036854743031, 138, 136, 2147516425, 2147483658,
	2147516555, -9223372036854775669, -9223372036854742903, -9223372036854743037,
	-9223372036854743038, -9223372036854775680, 32778, -9223372034707292150,
	-9223372034707259263, -9223372036854742912, 2147483649, -9223372034707259384,
]
# rho offsets and pi destinations, walked from lane 1 (as in XKCP's (x, y) = (1, 0) loop)
const _ROTC: Array[int] = [1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62,
	18, 39, 61, 20, 44]
const _PILN: Array[int] = [10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20,
	14, 22, 9, 6, 1]


## The 32-byte Keccak-256 digest of `data`. Cannot fail: any byte string is valid input, so
## there is no ScfgErr path (and no `## @fallible`).
static func keccak256(data: PackedByteArray) -> PackedByteArray:
	# pad: data, 0x01, zeros, then 0x80 on the last byte of the block (0x81 if they coincide)
	var msg := data.duplicate()
	var pad := RATE_BYTES - (msg.size() % RATE_BYTES)
	var start := msg.size()
	msg.resize(start + pad)
	for i in range(start, msg.size()):
		msg[i] = 0
	msg[start] = _SUFFIX
	msg[msg.size() - 1] = msg[msg.size() - 1] | 0x80

	var st := PackedInt64Array()
	st.resize(25)
	st.fill(0)
	for off in range(0, msg.size(), RATE_BYTES):
		for i in RATE_BYTES / 8:
			st[i] = st[i] ^ msg.decode_s64(off + 8 * i)
		_permute(st)

	var out := PackedByteArray()
	out.resize(32)
	for i in 4:
		out.encode_s64(8 * i, st[i])
	return out


static func _permute(st: PackedInt64Array) -> void:
	for rnd in 24:
		# theta
		var c0: int = st[0] ^ st[5] ^ st[10] ^ st[15] ^ st[20]
		var c1: int = st[1] ^ st[6] ^ st[11] ^ st[16] ^ st[21]
		var c2: int = st[2] ^ st[7] ^ st[12] ^ st[17] ^ st[22]
		var c3: int = st[3] ^ st[8] ^ st[13] ^ st[18] ^ st[23]
		var c4: int = st[4] ^ st[9] ^ st[14] ^ st[19] ^ st[24]
		var d0: int = c4 ^ ((c1 << 1) | ((c1 >> 63) & 1))
		var d1: int = c0 ^ ((c2 << 1) | ((c2 >> 63) & 1))
		var d2: int = c1 ^ ((c3 << 1) | ((c3 >> 63) & 1))
		var d3: int = c2 ^ ((c4 << 1) | ((c4 >> 63) & 1))
		var d4: int = c3 ^ ((c0 << 1) | ((c0 >> 63) & 1))
		for y in range(0, 25, 5):
			st[y] = st[y] ^ d0
			st[y + 1] = st[y + 1] ^ d1
			st[y + 2] = st[y + 2] ^ d2
			st[y + 3] = st[y + 3] ^ d3
			st[y + 4] = st[y + 4] ^ d4
		# rho and pi
		var cur: int = st[1]
		for t in 24:
			var j: int = _PILN[t]
			var n: int = _ROTC[t]
			var nxt: int = st[j]
			st[j] = (cur << n) | ((cur >> (64 - n)) & ((1 << n) - 1))
			cur = nxt
		# chi
		for y in range(0, 25, 5):
			var a0: int = st[y]
			var a1: int = st[y + 1]
			var a2: int = st[y + 2]
			var a3: int = st[y + 3]
			var a4: int = st[y + 4]
			st[y] = a0 ^ (~a1 & a2)
			st[y + 1] = a1 ^ (~a2 & a3)
			st[y + 2] = a2 ^ (~a3 & a4)
			st[y + 3] = a3 ^ (~a4 & a0)
			st[y + 4] = a4 ^ (~a0 & a1)
		# iota
		st[0] = st[0] ^ _RC[rnd]
