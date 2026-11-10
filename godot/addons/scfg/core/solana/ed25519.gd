## Is a 32-byte value a valid ed25519 public key (a point on the curve)?
##
## Solana uses this to tell wallet addresses (on the curve) from program-derived addresses such
## as token accounts (deliberately off the curve). Mirrors scfg_core/solana/ed25519.py, which
## follows curve25519-dalek's CompressedEdwardsY::decompress: y is read little-endian with the
## sign bit masked and reduced mod p (non-canonical encodings accepted), and the point exists
## iff (y^2 - 1) / (d*y^2 + 1) is a square mod p (Euler's criterion).
class_name ScfgEd25519
extends RefCounted

const P_HEX := "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffed"  # 2^255 - 19
const D_HEX := "52036cee2b6ffe738cc740797779e89800700a4d4141d8ab75eb4dca135978a3"  # -121665/121666

static var _p: ScfgBigInt
static var _d: ScfgBigInt
static var _one: ScfgBigInt
static var _p_minus_2: ScfgBigInt
static var _half: ScfgBigInt  # (p - 1) / 2


## True when the 32 bytes decompress to a curve point; any other length is false (Python: the
## same). Cannot fail, so it is not `## @fallible`: the ScfgBigInt calls inside are waived with
## `# err-ok:` where their preconditions hold.
static func is_on_curve(point: PackedByteArray) -> bool:
	if point.size() != 32:
		return false
	_init_constants()
	var masked := point.duplicate()
	masked[31] = masked[31] & 0x7f
	# every modulus below is p != 0 and every difference is non-negative, so the fallible
	# ScfgBigInt calls cannot fail here
	var y: ScfgBigInt = ScfgBigInt.from_bytes_le(masked).mod(_p)  # err-ok: p != 0
	var yy: ScfgBigInt = y.mul(y).mod(_p)  # err-ok: p != 0
	var u: ScfgBigInt = yy.add(_p).sub(_one).mod(_p)  # err-ok: yy + p >= 1, p != 0
	var v: ScfgBigInt = _d.mul(yy).add(_one).mod(_p)  # err-ok: p != 0 (v is never 0: d is not a square mod p)
	var x2: ScfgBigInt = u.mul(v.pow_mod(_p_minus_2, _p)).mod(_p)  # err-ok: p != 0
	return x2.is_zero() or x2.pow_mod(_half, _p).cmp(_one) == 0  # err-ok: p != 0


static func _init_constants() -> void:
	if _p != null:
		return
	# constants: the literals parse and p > 2
	_one = ScfgBigInt.from_int(1)  # err-ok: literal
	_d = ScfgBigInt.from_hex(D_HEX)  # err-ok: literal
	_p_minus_2 = ScfgBigInt.from_hex(P_HEX).sub(ScfgBigInt.from_int(2))  # err-ok: literals, p > 2
	_half = ScfgBigInt.from_hex(P_HEX).sub(_one).divmod(ScfgBigInt.from_int(2))[0]  # err-ok: literals
	_p = ScfgBigInt.from_hex(P_HEX)  # err-ok: literal
