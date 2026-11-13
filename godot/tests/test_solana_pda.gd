extends RefCounted

const T := preload("res://tests/t.gd")

const USDC := "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
const OWNER := "AKnL4NNf3DGWZJS6cPknBuEGnVsV4A4m5tgebLHaRSZ9"


func _prog() -> PackedByteArray:
	return ScfgSolanaBase58.b58decode(ScfgSolanaPda.ASSOCIATED_TOKEN_PROGRAM_ID)  # err-ok: constant


func test_constants() -> bool:
	T.eq(ScfgSolanaPda.TOKEN_PROGRAM_ID, "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", "token program")
	T.eq(ScfgSolanaPda.ASSOCIATED_TOKEN_PROGRAM_ID, "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL", "ATA program")
	T.eq(ScfgSolanaPda.MAX_SEED_LEN, 32, "max seed length")
	T.eq(ScfgSolanaPda.MAX_SEEDS, 16, "max seeds")
	return T.done()


func test_wrong_length_is_not_on_curve() -> bool:
	var p := PackedByteArray()
	p.resize(31)
	p.fill(1)
	T.eq(ScfgEd25519.is_on_curve(p), false, "31 bytes")
	T.eq(ScfgEd25519.is_on_curve(PackedByteArray()), false, "empty")
	return T.done()


## scfg_core/solana/ed25519.py as written: invert v, then Euler's criterion on u/v (two powers).
func _python_is_on_curve(point: PackedByteArray) -> bool:
	var p: ScfgBigInt = ScfgBigInt.from_hex(ScfgEd25519.P_HEX)  # err-ok: literal
	var d: ScfgBigInt = ScfgBigInt.from_hex(ScfgEd25519.D_HEX)  # err-ok: literal
	var one: ScfgBigInt = ScfgBigInt.from_int(1)  # err-ok: literal
	var two: ScfgBigInt = ScfgBigInt.from_int(2)  # err-ok: literal
	var masked := point.duplicate()
	masked[31] = masked[31] & 0x7f
	var y: ScfgBigInt = ScfgBigInt.from_bytes_le(masked).mod(p)  # err-ok: p != 0
	var yy: ScfgBigInt = y.mul(y).mod(p)  # err-ok: p != 0
	var u: ScfgBigInt = yy.add(p).sub(one).mod(p)  # err-ok: p != 0
	var v: ScfgBigInt = d.mul(yy).add(one).mod(p)  # err-ok: p != 0
	var x2: ScfgBigInt = u.mul(v.pow_mod(p.sub(two), p)).mod(p)  # err-ok: p != 0
	return x2.is_zero() or x2.pow_mod(p.sub(one).divmod(two)[0], p).cmp(one) == 0  # err-ok: p != 0


## The port tests u*v with one modular power where Python tests u/v with two: the same answer for
## the edge encodings (y = 0, 1, p - 1, the non-canonical p, p + 1 and 2^255 - 1, the sign bit) and
## for hashes, which land on and off the curve.
func test_is_on_curve_matches_pythons_two_power_formula() -> bool:
	var points: Array[PackedByteArray] = []
	for hex: String in [
			"00".repeat(32), "01" + "00".repeat(31), "01" + "00".repeat(30) + "80",
			"ec" + "ff".repeat(30) + "7f", "ed" + "ff".repeat(30) + "7f", "ee" + "ff".repeat(30) + "7f",
			"ff".repeat(31) + "7f", "ff".repeat(32), "02" + "00".repeat(31), "ec" + "ff".repeat(31)]:
		points.append(ScfgBytes.hex_to_bytes(hex))  # err-ok: test literals
	var digest := "is_on_curve".to_ascii_buffer()
	for i in 24:
		digest = ScfgSha256.sha256(digest)
		points.append(digest)
	var on := 0
	for point: PackedByteArray in points:
		var want := _python_is_on_curve(point)
		T.eq(ScfgEd25519.is_on_curve(point), want, "is_on_curve(%s)" % point.hex_encode())
		on += int(want)
	T.ok(on >= 8 and points.size() - on >= 8, "both outcomes are covered (%d on the curve of %d)" % [on, points.size()])
	T.eq(ScfgEd25519.is_on_curve(points[1]), true, "y = 1 (u = 0) is on the curve")
	return T.done()


## The result is kept per (owner, mint): the same answer again, errors never kept, and the cache
## never grows past its bound.
func test_associated_token_address_is_memoised_and_bounded() -> bool:
	ScfgSolanaPda._ata_cache.clear()
	var first: Variant = ScfgSolanaPda.associated_token_address(OWNER, USDC)
	T.eq(first, "3wvJdyFnGvaMWpbq93NU91SggiVRveULUXL6iX5VZDGP", "computed")
	T.eq(ScfgSolanaPda._ata_cache, {[OWNER, USDC]: first}, "kept under [owner, mint]")
	var start := Time.get_ticks_usec()
	for i in 200:
		T.eq(ScfgSolanaPda.associated_token_address(OWNER, USDC), first, "the same address again")
	T.ok(Time.get_ticks_usec() - start < 200 * 1000, "200 repeats take no search")
	T.is_err(ScfgSolanaPda.associated_token_address("abc", USDC), "ValueError", "bad owner")
	T.is_err(ScfgSolanaPda.associated_token_address(OWNER, 5), "ValueError", "a mint that is not text")
	T.eq(ScfgSolanaPda._ata_cache.size(), 1, "errors are not kept")
	T.ok(ScfgSolanaPda.associated_token_address(USDC, OWNER) is String, "the swapped pair")
	T.ok(ScfgSolanaPda._ata_cache.has([USDC, OWNER]) and ScfgSolanaPda._ata_cache[[USDC, OWNER]] != first, "(mint, owner) is another entry")
	for i in ScfgSolanaPda._ATA_CACHE_MAX:
		ScfgSolanaPda._ata_cache[["owner %d" % i, USDC]] = "x"
	T.ok(ScfgSolanaPda._ata_cache.size() > ScfgSolanaPda._ATA_CACHE_MAX, "(overfilled by the test)")
	T.eq(ScfgSolanaPda.associated_token_address(USDC, USDC), ScfgSolanaPda.associated_token_address(USDC, USDC), "a new pair")
	T.eq(ScfgSolanaPda._ata_cache.keys(), [[USDC, USDC]], "a full cache is emptied before the new pair is kept")
	ScfgSolanaPda._ata_cache.clear()
	T.eq(ScfgSolanaPda.associated_token_address(OWNER, USDC), first, "and recomputed the same")
	return T.done()


# solders: Pubkey.find_program_address([seed], ATA program) for each seed
func test_find_program_address_matches_solders() -> bool:
	var table := [
		["", "DzQr5rR32D2de4ugfqgNmKboRoBK5eid5K7WpRCxeRBY", 254],
		["61", "GjMJe8fchUnHcjNA1qQUnTZWEXVQ8gRsvJ6xv1nSg3JY", 254],
		["73636667", "HEV3HNtqZkL8MuPCMk4oscJvtmrDUxjXbUrWCo9A9v25", 255],
		["00".repeat(32), "H8oGb4DUSpX9wKJBhr8Jia3eiAPvwcywRsgz57DusNi7", 252],
	]
	for row: Array in table:
		var seed_bytes: PackedByteArray = ScfgBytes.hex_to_bytes(row[0])  # err-ok: test literals
		var r: Variant = ScfgSolanaPda.find_program_address([seed_bytes], _prog())
		T.ok(r is Array and r.size() == 2, "a pair")
		if r is Array and r.size() == 2:
			T.eq(ScfgSolanaBase58.b58encode(r[0]), row[1], "address for seed %s" % row[0])
			T.eq(r[1], row[2], "bump for seed %s" % row[0])
	return T.done()


# solders: find_program_address([owner, TOKEN_PROGRAM, mint], ATA program) with owner = Keypair.from_seed([1]*32)
func test_associated_token_address_matches_solders() -> bool:
	T.eq(ScfgSolanaPda.associated_token_address(OWNER, USDC), "3wvJdyFnGvaMWpbq93NU91SggiVRveULUXL6iX5VZDGP", "ATA")
	T.is_err(ScfgSolanaPda.associated_token_address("abc", USDC), "ValueError", "bad owner")
	T.is_err(ScfgSolanaPda.associated_token_address(OWNER, "112"), "ValueError", "bad mint")
	T.is_err(ScfgSolanaPda.associated_token_address(null, USDC), "ValueError", "None owner")
	return T.done()


func test_token_accounts_are_off_curve() -> bool:
	var ata: Variant = ScfgSolanaPda.associated_token_address(OWNER, USDC)
	T.ok(ata is String, "an address")
	if ata is String:
		var raw: PackedByteArray = ScfgSolanaBase58.b58decode(ata)  # err-ok: we just encoded it
		T.eq(ScfgEd25519.is_on_curve(raw), false, "a token account is deliberately off the curve")
	return T.done()


func test_seed_limits() -> bool:
	var zero32 := PackedByteArray()
	zero32.resize(32)
	var too_long := PackedByteArray()
	too_long.resize(33)
	T.is_err(ScfgSolanaPda.find_program_address([too_long], zero32), "ValueError", "a 33-byte seed")
	T.is_err(ScfgSolanaPda.create_program_address([too_long], zero32), "ValueError", "a 33-byte seed (create)")
	var sixteen := []
	for i in 16:
		sixteen.append(PackedByteArray([120]))
	# find appends the bump seed, which makes 17
	T.is_err(ScfgSolanaPda.find_program_address(sixteen, zero32), "ValueError", "16 seeds plus the bump")
	var fifteen := sixteen.slice(0, 15)
	T.ok(ScfgSolanaPda.find_program_address(fifteen, zero32) is Array, "15 seeds plus the bump fit")
	T.ok(not ScfgErr.is_err(ScfgSolanaPda.create_program_address(sixteen, zero32)), "16 seeds are allowed")
	T.is_err(ScfgSolanaPda.create_program_address(sixteen + [PackedByteArray()], zero32), "ValueError", "17 seeds")
	var thirty_two := PackedByteArray()
	thirty_two.resize(32)
	T.ok(not ScfgErr.is_err(ScfgSolanaPda.create_program_address([thirty_two], zero32)), "a 32-byte seed is allowed")
	T.is_err(ScfgSolanaPda.create_program_address(["x"], zero32), "TypeError", "a seed that is not bytes")
	return T.done()


func test_create_program_address_returns_null_when_on_curve() -> bool:
	# seeds from the vectors: some hash onto the curve (None), some off it
	var on := 0
	var off := 0
	for i in 12:
		var r: Variant = ScfgSolanaPda.create_program_address([PackedByteArray([i])], _prog())
		if r == null:
			on += 1
		else:
			T.ok(r is PackedByteArray and r.size() == 32, "a 32-byte address")
			off += 1
	T.ok(on > 0 and off > 0, "both outcomes occur (%d on the curve, %d off)" % [on, off])
	return T.done()
