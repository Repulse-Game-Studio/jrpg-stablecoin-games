## Program-derived addresses and associated token accounts (classic SPL Token program). Mirrors
## scfg_core/solana/pda.py. Addresses and seeds are PackedByteArrays; `associated_token_address`
## takes and returns base58 text.
class_name ScfgSolanaPda
extends RefCounted

const TOKEN_PROGRAM_ID := "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"
const ASSOCIATED_TOKEN_PROGRAM_ID := "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL"
const MAX_SEED_LEN := 32
const MAX_SEEDS := 16
const _MARKER := "ProgramDerivedAddress"


## sha256(seeds + program_id + "ProgramDerivedAddress") as a 32-byte address, or null (Python's
## None) when that hash is a valid curve point (then it could have a private key and is not a
## program address). More than 16 seeds or a seed longer than 32 bytes is a ValueError; a seed
## that is not a PackedByteArray is a TypeError.
## @fallible
static func create_program_address(seeds: Array, program_id: PackedByteArray) -> Variant:
	for s: Variant in seeds:
		if not s is PackedByteArray:
			return ScfgErr.make("TypeError", "a seed must be bytes, not '%s'" % ScfgPy.py_type(s))
	if seeds.size() > MAX_SEEDS:
		return ScfgErr.make("ValueError", "too many seeds or a seed longer than 32 bytes")
	for s: PackedByteArray in seeds:
		if s.size() > MAX_SEED_LEN:
			return ScfgErr.make("ValueError", "too many seeds or a seed longer than 32 bytes")
	var parts: Array = seeds.duplicate()
	parts.append(program_id)
	parts.append(_MARKER.to_ascii_buffer())
	var digest := ScfgSha256.sha256(ScfgBytes.concat(parts))
	return null if ScfgEd25519.is_on_curve(digest) else digest


## The first bump seed, counting down from 255, that makes `create_program_address` succeed:
## `[address, bump]`. A ValueError from the seed limits (the bump counts as a seed), or when no
## bump works.
## @fallible
static func find_program_address(seeds: Array, program_id: PackedByteArray) -> Variant:
	for bump in range(255, -1, -1):
		var with_bump: Array = seeds.duplicate()
		with_bump.append(PackedByteArray([bump]))
		var address: Variant = create_program_address(with_bump, program_id)
		if address is ScfgErr:
			return address
		if address != null:
			return [address, bump]
	return ScfgErr.make("ValueError", "no viable bump seed")


## The developer's (or player's) token account for `mint`, as wallets create it: base58 text.
## An owner or mint that is not a base58 32-byte key is a ValueError.
## @fallible
static func associated_token_address(owner: Variant, mint: Variant) -> Variant:
	var owner_raw: Variant = ScfgSolanaBase58.decode_fixed(owner, 32)
	if owner_raw is ScfgErr:
		return owner_raw
	var token_raw: Variant = ScfgSolanaBase58.decode_fixed(TOKEN_PROGRAM_ID, 32)
	if token_raw is ScfgErr:
		return token_raw
	var mint_raw: Variant = ScfgSolanaBase58.decode_fixed(mint, 32)
	if mint_raw is ScfgErr:
		return mint_raw
	var program_raw: Variant = ScfgSolanaBase58.decode_fixed(ASSOCIATED_TOKEN_PROGRAM_ID, 32)
	if program_raw is ScfgErr:
		return program_raw
	var found: Variant = find_program_address([owner_raw, token_raw, mint_raw], program_raw)
	if found is ScfgErr:
		return found
	return ScfgSolanaBase58.b58encode(found[0])
