## Legacy Solana transactions, serialized without any library. Mirrors scfg_core/solana/tx.py.
##
## Message layout: header (3 bytes) | compact-u16 #keys | keys (32 bytes each) | recent blockhash
## (32) | compact-u16 #instructions | per instruction: program index, compact-u16 #accounts,
## account indexes, compact-u16 #data, data.
## Account order (as the Solana SDK compiles it): fee payer, then writable signers, readonly
## signers, writable non-signers, readonly non-signers, each group sorted by raw key bytes.
## Program ids are readonly non-signers.
##
## An AccountMeta is a Dictionary {pubkey, is_signer, is_writable} and an Instruction one
## {program_id, accounts: Array of AccountMeta, data: PackedByteArray}, Python's field order
## (build them with `account_meta` and `instruction`). Like Python's dataclasses, the builders
## take any values: a key that is not base58 text fails later, in compile_message (ValueError).
class_name ScfgSolanaTx
extends RefCounted

const COMPUTE_BUDGET_PROGRAM_ID := "ComputeBudget111111111111111111111111111111"


static func account_meta(pubkey: Variant, is_signer: Variant, is_writable: Variant) -> Dictionary:
	return {"pubkey": pubkey, "is_signer": is_signer, "is_writable": is_writable}


static func instruction(program_id: Variant, accounts: Variant, data: Variant) -> Dictionary:
	return {"program_id": program_id, "accounts": accounts, "data": data}


## The compact-u16 (shortvec) encoding of `n`: 7 bits per byte, low group first, high bit set on
## all but the last byte. A number outside 0..65535 is a ValueError and a non-number a TypeError.
## A float is a TypeError too, as in Python (`2.0 & 0x7f` fails), after the range check (so is a
## fractional one). A bool is the int 0 or 1.
## @fallible
static func compact_u16(n: Variant) -> Variant:
	var v: int
	match typeof(n):
		TYPE_INT:
			v = n
		TYPE_BOOL:
			v = 1 if n else 0
		TYPE_FLOAT:
			if not (n >= 0.0 and n <= 65535.0):
				return ScfgErr.make("ValueError", "compact-u16 out of range: %s" % ScfgPy.py_repr(n))
			return ScfgErr.make("TypeError", "unsupported operand type(s) for &: 'float' and 'int'")
		_:
			return ScfgErr.make("TypeError", "'<=' not supported between instances of 'int' and '%s'" % ScfgPy.py_type(n))
	if v < 0 or v > 0xFFFF:
		return ScfgErr.make("ValueError", "compact-u16 out of range: %d" % v)
	var out := PackedByteArray()
	while true:
		var byte := v & 0x7F
		v >>= 7
		if v == 0:
			out.append(byte)
			break
		out.append(byte | 0x80)
	return out


## The message to be signed. Accounts are deduplicated (a key keeps every flag any use gave it),
## the payer leads as a writable signer, and the other keys follow in four groups, each sorted
## by raw key bytes. A key or blockhash that is not base58 for 32 bytes is a ValueError; so is a
## count that does not fit its field (a header byte over 255, a compact-u16 over 65535).
## `instructions` that is not a list, or holds anything but Instruction Dictionaries with an
## Array of AccountMeta Dictionaries and PackedByteArray data, is a TypeError (Python: an
## AttributeError or TypeError on the malformed object).
## @fallible
static func compile_message(payer: Variant, instructions: Variant, recent_blockhash: Variant) -> Variant:
	var malformed: Variant = _check_instructions(instructions)
	if malformed is ScfgErr:
		return malformed
	# lowercase hex of the raw key -> [signer, writable]; hex order is raw byte order
	var flags := {}
	for ix: Dictionary in instructions:
		for meta: Dictionary in ix["accounts"]:
			var added: Variant = _add(flags, meta["pubkey"], meta["is_signer"], meta["is_writable"])
			if added is ScfgErr:
				return added
		var prog: Variant = _add(flags, ix["program_id"], false, false)
		if prog is ScfgErr:
			return prog
	var payer_raw: Variant = ScfgSolanaBase58.decode_fixed(payer, 32)
	if payer_raw is ScfgErr:
		return payer_raw
	flags.erase(ScfgBytes.bytes_to_hex(payer_raw))

	var ws := _group(flags, true, true)
	var rs := _group(flags, true, false)
	var wu := _group(flags, false, true)
	var ru := _group(flags, false, false)
	var keys: Array = [ScfgBytes.bytes_to_hex(payer_raw)]
	keys.append_array(ws)
	keys.append_array(rs)
	keys.append_array(wu)
	keys.append_array(ru)
	var index := {}
	for i in keys.size():
		index[keys[i]] = i

	var header := [1 + ws.size() + rs.size(), rs.size(), ru.size()]
	for h: int in header:
		if h > 255:
			return ScfgErr.make("ValueError", "byte must be in range(0, 256)")
	var out := PackedByteArray(header)
	var key_count: Variant = compact_u16(keys.size())
	if key_count is ScfgErr:
		return key_count
	out.append_array(key_count)
	for k: String in keys:
		out.append_array(k.hex_decode())
	var blockhash: Variant = ScfgSolanaBase58.decode_fixed(recent_blockhash, 32)
	if blockhash is ScfgErr:
		return blockhash
	out.append_array(blockhash)
	var ix_count: Variant = compact_u16((instructions as Array).size())
	if ix_count is ScfgErr:
		return ix_count
	out.append_array(ix_count)
	for ix: Dictionary in instructions:
		# every key was added above, so these decodes cannot fail and every key has an index
		var program_hex := ScfgBytes.bytes_to_hex(ScfgSolanaBase58.decode_fixed(ix["program_id"], 32))  # err-ok: decoded by _add
		if index[program_hex] > 255:
			return ScfgErr.make("ValueError", "byte must be in range(0, 256)")
		out.append(index[program_hex])
		var accounts: Array = ix["accounts"]
		var n_accounts: Variant = compact_u16(accounts.size())
		if n_accounts is ScfgErr:
			return n_accounts
		out.append_array(n_accounts)
		for meta: Dictionary in accounts:
			var i: int = index[ScfgBytes.bytes_to_hex(ScfgSolanaBase58.decode_fixed(meta["pubkey"], 32))]  # err-ok: decoded by _add
			if i > 255:
				return ScfgErr.make("ValueError", "byte must be in range(0, 256)")
			out.append(i)
		var data: PackedByteArray = ix["data"]
		var n_data: Variant = compact_u16(data.size())
		if n_data is ScfgErr:
			return n_data
		out.append_array(n_data)
		out.append_array(data)
	return out


## Empty signature slots + message, ready for a wallet to sign. An empty message is an
## IndexError (Python: `message[0]`).
## @fallible
static func unsigned_transaction(message: PackedByteArray) -> Variant:
	if message.is_empty():
		return ScfgErr.make("IndexError", "index out of range")
	var signers: int = message[0]
	var count: Variant = compact_u16(signers)
	if count is ScfgErr:
		return count
	var out: PackedByteArray = (count as PackedByteArray).duplicate()
	var slots := PackedByteArray()
	slots.resize(64 * signers)
	slots.fill(0)
	out.append_array(slots)
	out.append_array(message)
	return out


## ComputeBudget SetComputeUnitLimit: `units` as an unsigned 32-bit little-endian number.
## @fallible
static func set_compute_unit_limit(units: Variant) -> Variant:
	var le: Variant = _uint_le(units, 4)
	if le is ScfgErr:
		return le
	return instruction(COMPUTE_BUDGET_PROGRAM_ID, [], ScfgBytes.concat([PackedByteArray([2]), le]))


## ComputeBudget SetComputeUnitPrice: `micro_lamports` as an unsigned 64-bit little-endian number.
## @fallible
static func set_compute_unit_price(micro_lamports: Variant) -> Variant:
	var le: Variant = _uint_le(micro_lamports, 8)
	if le is ScfgErr:
		return le
	return instruction(COMPUTE_BUDGET_PROGRAM_ID, [], ScfgBytes.concat([PackedByteArray([3]), le]))


## SPL Token TransferChecked. Keyword arguments (Python's `*`) travel in `opts`: source, mint,
## destination, owner, amount (an int or ScfgBigInt), decimals, and optionally extra (an Array of
## keys, Python's `()`). `extra` keys (e.g. a Solana Pay reference) are appended read-only so the
## payment can be found with getSignaturesForAddress(extra key). A missing keyword is a TypeError;
## an amount that is negative or needs more than 8 bytes is an OverflowError; decimals outside
## 0..255 a ValueError.
## @fallible
static func transfer_checked(opts: Dictionary) -> Variant:
	for key: String in ["source", "mint", "destination", "owner", "amount", "decimals"]:
		if not opts.has(key):
			return ScfgErr.make("TypeError", "transfer_checked() missing required keyword-only argument: '%s'" % key)
	var accounts: Array = [
		account_meta(opts["source"], false, true),
		account_meta(opts["mint"], false, false),
		account_meta(opts["destination"], false, true),
		account_meta(opts["owner"], true, false),
	]
	var extra: Variant = opts.get("extra", [])
	if not extra is Array:
		return ScfgErr.make("TypeError", "'%s' object is not iterable" % ScfgPy.py_type(extra))
	for k: Variant in extra:
		accounts.append(account_meta(k, false, false))
	var amount_le: Variant = _uint_le(opts["amount"], 8)
	if amount_le is ScfgErr:
		return amount_le
	var decimals: Variant = opts["decimals"]
	if decimals is bool:
		decimals = 1 if decimals else 0
	if not decimals is int:
		return ScfgErr.make("TypeError", "'%s' object cannot be interpreted as an integer" % ScfgPy.py_type(decimals))
	if decimals < 0 or decimals > 255:
		return ScfgErr.make("ValueError", "bytes must be in range(0, 256)")
	var data := ScfgBytes.concat([PackedByteArray([12]), amount_le, PackedByteArray([decimals])])
	return instruction(ScfgSolanaPda.TOKEN_PROGRAM_ID, accounts, data)


## A TypeError unless `instructions` is an Array of Instruction Dictionaries (every field
## present, accounts an Array of AccountMeta Dictionaries, data a PackedByteArray); else null.
## @fallible
static func _check_instructions(instructions: Variant) -> Variant:
	if not instructions is Array:
		return ScfgErr.make("TypeError", "'%s' object is not iterable" % ScfgPy.py_type(instructions))
	for ix: Variant in instructions:
		if not (ix is Dictionary and ix.has("program_id") and ix.has("accounts") and ix.has("data")):
			return ScfgErr.make("TypeError", "not an Instruction: %s" % ScfgPy.py_repr(ix))
		if not ix["accounts"] is Array:
			return ScfgErr.make("TypeError", "'%s' object is not iterable" % ScfgPy.py_type(ix["accounts"]))
		if not ix["data"] is PackedByteArray:
			return ScfgErr.make("TypeError", "instruction data must be bytes, not '%s'" % ScfgPy.py_type(ix["data"]))
		for meta: Variant in ix["accounts"]:
			if not (meta is Dictionary and meta.has("pubkey") and meta.has("is_signer") and meta.has("is_writable")):
				return ScfgErr.make("TypeError", "not an AccountMeta: %s" % ScfgPy.py_repr(meta))
	return null


## Record `key` (base58) with the OR of the flags it has been used with.
## @fallible
static func _add(flags: Dictionary, key: Variant, signer: Variant, writable: Variant) -> Variant:
	var raw: Variant = ScfgSolanaBase58.decode_fixed(key, 32)
	if raw is ScfgErr:
		return raw
	var id := ScfgBytes.bytes_to_hex(raw)
	var f: Array = flags.get(id, [false, false])
	f[0] = f[0] or ScfgPy.py_truthy(signer)
	f[1] = f[1] or ScfgPy.py_truthy(writable)
	flags[id] = f
	return null


## The keys (lowercase hex) with exactly these flags, sorted.
static func _group(flags: Dictionary, signer: bool, writable: bool) -> Array:
	var out := []
	for id: String in flags:
		var f: Array = flags[id]
		if f[0] == signer and f[1] == writable:
			out.append(id)
	out.sort()
	return out


## `value.to_bytes(size, "little")` for an int or ScfgBigInt: negative or too large is an
## OverflowError; a bool is the int 0 or 1; anything else has no `to_bytes` (AttributeError).
## @fallible
static func _uint_le(value: Variant, size: int) -> Variant:
	if value is ScfgBigInt:
		var big: ScfgBigInt = value
		return big.to_bytes_le(size)
	var v: int
	match typeof(value):
		TYPE_INT:
			v = value
		TYPE_BOOL:
			v = 1 if value else 0
		_:
			return ScfgErr.make("AttributeError", "'%s' object has no attribute 'to_bytes'" % ScfgPy.py_type(value))
	if v < 0:
		return ScfgErr.make("OverflowError", "can't convert negative int to unsigned")
	if size < 8 and v >> (8 * size) != 0:
		return ScfgErr.make("OverflowError", "int too big to convert")
	var out := PackedByteArray()
	out.resize(size)
	out.fill(0)
	for i in mini(size, 8):
		out[i] = (v >> (8 * i)) & 0xff
	return out
