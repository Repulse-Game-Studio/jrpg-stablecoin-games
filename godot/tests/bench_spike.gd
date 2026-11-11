## Spike benchmarks (run with `--bench`): medians over 21 runs, printed against the budgets below.
## A missed budget is reported, not failed.
extends RefCounted

const Vectors := preload("res://tests/vectors.gd")

const RUNS := 21
const KECCAK_BUDGET_MS := 5.0
const ON_CURVE_BUDGET_MS := 150.0
const USDC_MINT := "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
const TOKEN_PROGRAM := "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
const ATA_PROGRAM := "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL"
const B58 := "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


func run(vectors_dir: String) -> bool:
	print("BENCH Godot %s, %d runs each" % [Engine.get_version_info()["string"], RUNS])

	var data := PackedByteArray()
	data.resize(136)
	for i in 136:
		data[i] = (7 + i) % 256
	var keccak_ms := _median_ms(func() -> void: ScfgKeccak.keccak256(data))
	_report("keccak256 of 136 bytes", keccak_ms, KECCAK_BUDGET_MS)

	var mint := _b58decode32(USDC_MINT)
	var on_curve_ms := _median_ms(func() -> void: ScfgEd25519.is_on_curve(mint))
	_report("is_on_curve(USDC mint)", on_curve_ms, ON_CURVE_BUDGET_MS)

	# The ATA for the first USDC mint/owner pair in the pda vectors: count the bump attempts
	# (each one is a SHA-256 and an is_on_curve) until the address is off the curve.
	var owner := ""
	var expected := PackedByteArray()
	for c: Dictionary in Vectors.load_topic(vectors_dir, "pda"):
		if c["fn"] == "associated_token_address" and c["in"]["mint"] == USDC_MINT and c.has("out"):
			owner = c["in"]["owner"]
			expected = _b58decode32(c["out"])
			break
	if owner == "":
		printerr("bench: no associated_token_address case for the USDC mint in pda.json")
		return false
	var seeds := _b58decode32(owner) + _b58decode32(TOKEN_PROGRAM) + mint
	var tail := _b58decode32(ATA_PROGRAM) + "ProgramDerivedAddress".to_ascii_buffer()
	var found := _find_ata(seeds, tail)
	if found[0] != expected:
		printerr("bench: derived ATA does not match the pda vector for owner %s" % owner)
		return false
	var attempts: int = 256 - int(found[1])
	print("BENCH ATA %s/%s: bump %d, %d attempt(s)" % [owner, USDC_MINT, found[1], attempts])
	print("BENCH associated_token_address estimate: %.2f ms (is_on_curve x %d)" % [
		on_curve_ms * attempts, attempts])
	var ata_ms := _median_ms(func() -> void: _find_ata(seeds, tail))
	print("BENCH associated_token_address measured (SHA-256 + is_on_curve per bump): %.2f ms" % ata_ms)
	return true


## [address, bump] as find_program_address does it: the first bump from 255 down whose
## SHA-256(seeds + [bump] + program_id + marker) is off the curve.
func _find_ata(seeds: PackedByteArray, tail: PackedByteArray) -> Array:
	for bump in range(255, -1, -1):
		var ctx := HashingContext.new()
		ctx.start(HashingContext.HASH_SHA256)
		ctx.update(seeds + PackedByteArray([bump]) + tail)
		var digest := ctx.finish()
		if not ScfgEd25519.is_on_curve(digest):
			return [digest, bump]
	return [PackedByteArray(), -1]


func _median_ms(f: Callable) -> float:
	f.call()  # warm-up (lazy constants)
	var times: Array[float] = []
	for i in RUNS:
		var t0 := Time.get_ticks_usec()
		f.call()
		times.append((Time.get_ticks_usec() - t0) / 1000.0)
	times.sort()
	return times[RUNS / 2]


func _report(label: String, ms: float, budget: float) -> void:
	print("BENCH %s: median %.3f ms (budget <= %.0f ms) %s" % [
		label, ms, budget, "OK" if ms <= budget else "OVER BUDGET"])


## Base58 (Bitcoin alphabet) to exactly 32 bytes, big-endian. Bench-only helper: the real
## decoder is core/solana/base58.gd, the port of scfg_core/solana/base58.py.
func _b58decode32(s: String) -> PackedByteArray:
	var n := ScfgBigInt.new()
	for ch in s:
		n = n.mul_small(58).add(ScfgBigInt.from_int(B58.find(ch)))  # a valid base58 string
	var out: PackedByteArray = n.to_bytes_le(32)  # 32-byte keys
	out.reverse()
	return out
