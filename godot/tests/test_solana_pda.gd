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
