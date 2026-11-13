## Tier 1 harness: replays conformance/vectors/tier1/*.json against the GDScript core.
##
## FUNCS maps the Python function name of a case (`fn`) to an adapter Callable(input: Dictionary)
## -> Variant that turns the JSON-friendly `in` into the core's arguments, calls the core, and
## turns the result back into a JSON-friendly value (bytes as lowercase hex, big ints as decimal
## strings), as conformance/python/scfg_conformance/tier1.py does for Python. An adapter returns
## its output, NONE for Python's None, or the core's ScfgErr. A bare null is never an output: a
## script error aborts the adapter with null, so null fails the case.
##
## Errors are bucketed exactly as tier1.py's `_compute` does:
## - a ConfigError is `{"error": <its code>}`;
## - any other ValueError or TypeError (by Python's class tree: ScfgErr.is_a) is
##   `{"error": "invalid"}`;
## - any other ScfgErr (KeyError, IndexError, AttributeError, OverflowError, ...) fails the case:
##   Python's `except (ValueError, TypeError)` lets those propagate, so the generator never
##   recorded one, and a port producing one has diverged from the reference.
##
## A topic is listed in IMPLEMENTED once all its adapters are in FUNCS; test_tier1.gd skips
## (never passes) every other topic.
extends RefCounted

const T := preload("res://tests/t.gd")

## Python's None as an adapter output (a bare null means the adapter aborted).
const NONE := &"__scfg_tier1_none__"

## Topics (vector file names without `.json`) whose every `fn` has an adapter in FUNCS.
const IMPLEMENTED: Array[String] = [
	"keccak", "ed25519", "evm_address", "base58", "pda", "solana_tx",
	"amounts", "paylinks", "qr", "page_messages", "receipts", "tx_ids",
	"chains", "config", "custom_chains", "items", "payments", "rpc_errors",
]

## {Python fn name: Callable(input: Dictionary) -> Variant}.
static var FUNCS: Dictionary = _register()


static func _register() -> Dictionary:
	return {
		# keccak
		"keccak256": _keccak256,
		# ed25519
		"is_on_curve": _is_on_curve,
		# evm_address
		"to_checksum_address": _to_checksum_address,
		"is_checksum_address": _is_checksum_address,
		"normalize_address": _normalize_address,
		"address_topic": _address_topic,
		"parse_transfer_log": _parse_transfer_log,
		# base58
		"b58encode": _b58encode,
		"b58decode": _b58decode,
		"decode_fixed": _decode_fixed,
		"normalize_pubkey": _normalize_pubkey,
		"normalize_signature": _normalize_signature,
		# pda
		"create_program_address": _create_program_address,
		"find_program_address": _find_program_address,
		"associated_token_address": _associated_token_address,
		# solana_tx
		"compact_u16": _compact_u16,
		"compile_message": _compile_message,
		"unsigned_transaction": _unsigned_transaction,
		# amounts
		"parse_amount": _parse_amount,
		"encode": _encode,
		"decode": _decode,
		"units_to_decimal": _units_to_decimal,
		"decimal_to_units": _decimal_to_units,
		"stored_amount": _stored_amount,
		"format_units": _format_units,
		# paylinks
		"eip681_transfer_uri": _eip681_transfer_uri,
		"solana_pay_uri": _solana_pay_uri,
		# qr
		"qr_matrix": _qr_matrix,
		"qr_rects": _qr_rects,
		# page_messages
		"parse_tx_message": _parse_tx_message,
		"parse_build_message": _parse_build_message,
		# receipts
		"receipt_key": _receipt_key,
		"make_receipt": _make_receipt,
		"add_receipt": _add_receipt,
		"remove_receipt": _remove_receipt,
		"compute_ownership": _compute_ownership,
		# tx_ids (normalize_signature is registered with base58)
		"normalize_tx_hash": _normalize_tx_hash,
		"normalize_any_tx": _normalize_any_tx,
		# chains, config, custom_chains, items, payments
		"get_chains": _get_chains,
		"build_chain": _build_chain,
		"build_config": _build_config,
		"add_item": _add_item,
		"prepare_payment": _prepare_payment,
		# rpc_errors
		"suggested_range": _suggested_range,
		"named_version": _named_version,
	}


# --- adapters ---------------------------------------------------------------------------------


## keccak256(bytes.fromhex(i["data_hex"])).hex()
static func _keccak256(i: Dictionary) -> Variant:
	var data: Variant = _hex_field(i, "data_hex")
	if data is ScfgErr:
		return data
	return ScfgBytes.bytes_to_hex(ScfgKeccak.keccak256(data))


## is_on_curve(bytes.fromhex(i["point_hex"]))
static func _is_on_curve(i: Dictionary) -> Variant:
	var point: Variant = _hex_field(i, "point_hex")
	if point is ScfgErr:
		return point
	return ScfgEd25519.is_on_curve(point)


## evm.to_checksum_address(i["address"])
static func _to_checksum_address(i: Dictionary) -> Variant:
	var address: Variant = ScfgPy.py_get(i, "address")
	if address is ScfgErr:
		return address
	return ScfgEvm.to_checksum_address(address)


## evm.is_checksum_address(i["address"])
static func _is_checksum_address(i: Dictionary) -> Variant:
	var address: Variant = ScfgPy.py_get(i, "address")
	if address is ScfgErr:
		return address
	return ScfgEvm.is_checksum_address(address)


## evm.normalize_address(i["address"])
static func _normalize_address(i: Dictionary) -> Variant:
	var address: Variant = ScfgPy.py_get(i, "address")
	if address is ScfgErr:
		return address
	return ScfgEvm.normalize_address(address)


## evm.address_topic(i["address"])
static func _address_topic(i: Dictionary) -> Variant:
	var address: Variant = ScfgPy.py_get(i, "address")
	if address is ScfgErr:
		return address
	return ScfgEvm.address_topic(address)


## The TransferLog with `value` as a decimal string, or None.
static func _parse_transfer_log(i: Dictionary) -> Variant:
	var log: Variant = ScfgPy.py_get(i, "log")
	if log is ScfgErr:
		return log
	var t: Variant = ScfgEvm.parse_transfer_log(log)
	if t is ScfgErr:
		return t
	if t == null:
		return NONE
	var out: Dictionary = (t as Dictionary).duplicate()
	out["value"] = (out["value"] as ScfgBigInt).to_decimal()
	return out


## base58.b58encode(bytes.fromhex(i["data_hex"]))
static func _b58encode(i: Dictionary) -> Variant:
	var data: Variant = _hex_field(i, "data_hex")
	if data is ScfgErr:
		return data
	return ScfgSolanaBase58.b58encode(data)


## base58.b58decode(i["text"]).hex()
static func _b58decode(i: Dictionary) -> Variant:
	var text: Variant = ScfgPy.py_get(i, "text")
	if text is ScfgErr:
		return text
	return _hex_of(ScfgSolanaBase58.b58decode(text))


## base58.decode_fixed(i["text"], i["length"]).hex()
static func _decode_fixed(i: Dictionary) -> Variant:
	var text: Variant = ScfgPy.py_get(i, "text")
	if text is ScfgErr:
		return text
	var length: Variant = ScfgPy.py_get(i, "length")
	if length is ScfgErr:
		return length
	return _hex_of(ScfgSolanaBase58.decode_fixed(text, length))


## base58.normalize_pubkey(i["text"])
static func _normalize_pubkey(i: Dictionary) -> Variant:
	var text: Variant = ScfgPy.py_get(i, "text")
	if text is ScfgErr:
		return text
	return ScfgSolanaBase58.normalize_pubkey(text)


## base58.normalize_signature(i["text"])
static func _normalize_signature(i: Dictionary) -> Variant:
	var text: Variant = ScfgPy.py_get(i, "text")
	if text is ScfgErr:
		return text
	return ScfgSolanaBase58.normalize_signature(text)


## `[bytes.fromhex(s) for s in i["seeds_hex"]]`: a list of hex strings. (Python iterates whatever
## it is given: a string yields its characters, a dict its keys; any other value is not iterable.)
static func _seeds(i: Dictionary) -> Variant:
	var seeds_hex: Variant = ScfgPy.py_get(i, "seeds_hex")
	if seeds_hex is ScfgErr:
		return seeds_hex
	var items: Array
	if seeds_hex is Array:
		items = seeds_hex
	elif seeds_hex is String:
		for ch in (seeds_hex as String):
			items.append(ch)
	elif seeds_hex is Dictionary:
		items = (seeds_hex as Dictionary).keys()
	else:
		return ScfgErr.make("TypeError", "'%s' object is not iterable" % ScfgPy.py_type(seeds_hex))
	var out: Array = []
	for item: Variant in items:
		var raw: Variant = bytes_fromhex(item)
		if raw is ScfgErr:
			return raw
		out.append(raw)
	return out


## pda.create_program_address(_seeds(i), base58.decode_fixed(i["program_id"], 32)), base58 or None
static func _create_program_address(i: Dictionary) -> Variant:
	var seeds: Variant = _seeds(i)
	if seeds is ScfgErr:
		return seeds
	var program_id: Variant = _program_id(i)
	if program_id is ScfgErr:
		return program_id
	var out: Variant = ScfgSolanaPda.create_program_address(seeds, program_id)
	if out is ScfgErr:
		return out
	if out == null:
		return NONE
	return ScfgSolanaBase58.b58encode(out)


## {"address": base58, "bump": bump} of pda.find_program_address(_seeds(i), decode_fixed(program_id))
static func _find_program_address(i: Dictionary) -> Variant:
	var seeds: Variant = _seeds(i)
	if seeds is ScfgErr:
		return seeds
	var program_id: Variant = _program_id(i)
	if program_id is ScfgErr:
		return program_id
	var found: Variant = ScfgSolanaPda.find_program_address(seeds, program_id)
	if found is ScfgErr:
		return found
	return {"address": ScfgSolanaBase58.b58encode(found[0]), "bump": found[1]}


## pda.associated_token_address(i["owner"], i["mint"])
static func _associated_token_address(i: Dictionary) -> Variant:
	var owner: Variant = ScfgPy.py_get(i, "owner")
	if owner is ScfgErr:
		return owner
	var mint: Variant = ScfgPy.py_get(i, "mint")
	if mint is ScfgErr:
		return mint
	return ScfgSolanaPda.associated_token_address(owner, mint)


## tx.compact_u16(i["n"]).hex(). `n` is read as Python's json.load reads it (ScfgPy.from_json):
## an integral number is an int, a fractional one stays a float, which the core rejects as Python
## does. An integral number of 2^53 or more stays a float too (no vector has one); Python holds it
## as an int, which is out of range.
static func _compact_u16(i: Dictionary) -> Variant:
	var n: Variant = ScfgPy.py_get(ScfgPy.from_json(i), "n")
	if n is ScfgErr:
		return n
	if n is float and not is_inf(n) and n == floorf(n):
		return ScfgErr.make("ValueError", "compact-u16 out of range: %s" % ScfgPy.py_repr(n))
	return _hex_of(ScfgSolanaTx.compact_u16(n))


## tx.compile_message(payer, [Instruction(...)], recent_blockhash).hex(); the instructions are
## built first (hex parsed), as in the Python adapter.
static func _compile_message(i: Dictionary) -> Variant:
	var raw_ixs: Variant = ScfgPy.py_get(i, "instructions")
	if raw_ixs is ScfgErr:
		return raw_ixs
	if not raw_ixs is Array:
		return ScfgErr.make("TypeError", "'%s' object is not iterable" % ScfgPy.py_type(raw_ixs))
	var ixs: Array = []
	for x: Variant in raw_ixs:
		var program_id: Variant = ScfgPy.py_get(x, "program_id")
		if program_id is ScfgErr:
			return program_id
		var raw_metas: Variant = ScfgPy.py_get(x, "accounts")
		if raw_metas is ScfgErr:
			return raw_metas
		if not raw_metas is Array:
			return ScfgErr.make("TypeError", "'%s' object is not iterable" % ScfgPy.py_type(raw_metas))
		var metas: Array = []
		for m: Variant in raw_metas:
			var pubkey: Variant = ScfgPy.py_get(m, "pubkey")
			if pubkey is ScfgErr:
				return pubkey
			var is_signer: Variant = ScfgPy.py_get(m, "is_signer")
			if is_signer is ScfgErr:
				return is_signer
			var is_writable: Variant = ScfgPy.py_get(m, "is_writable")
			if is_writable is ScfgErr:
				return is_writable
			metas.append({"pubkey": pubkey, "is_signer": is_signer, "is_writable": is_writable})
		var data: Variant = _hex_field(x, "data_hex")
		if data is ScfgErr:
			return data
		ixs.append({"program_id": program_id, "accounts": metas, "data": data})
	var payer: Variant = ScfgPy.py_get(i, "payer")
	if payer is ScfgErr:
		return payer
	var blockhash: Variant = ScfgPy.py_get(i, "recent_blockhash")
	if blockhash is ScfgErr:
		return blockhash
	return _hex_of(ScfgSolanaTx.compile_message(payer, ixs, blockhash))


## tx.unsigned_transaction(bytes.fromhex(i["message_hex"])).hex()
static func _unsigned_transaction(i: Dictionary) -> Variant:
	var message: Variant = _hex_field(i, "message_hex")
	if message is ScfgErr:
		return message
	return _hex_of(ScfgSolanaTx.unsigned_transaction(message))


# amounts


## str(amounts.parse_amount(value)); `type` "float" means the float 4.99 and "bool"
## bool(value), otherwise the JSON value itself.
static func _parse_amount(i: Dictionary) -> Variant:
	var j: Dictionary = ScfgPy.from_json(i)
	var kind: Variant = ScfgPy.py_get(j, "type")
	if kind is ScfgErr:
		return kind
	var value: Variant
	if typeof(kind) == TYPE_STRING and kind == "float":
		value = 4.99
	else:
		value = ScfgPy.py_get(j, "value")
		if value is ScfgErr:
			return value
		if typeof(kind) == TYPE_STRING and kind == "bool":
			value = ScfgPy.py_truthy(value)
	return _str_of(ScfgAmounts.parse_amount(value))


## str(amounts.encode(i["price"], i["code"], i["nonce"]))
static func _encode(i: Dictionary) -> Variant:
	var args: Variant = _args(ScfgPy.from_json(i), ["price", "code", "nonce"])
	if args is ScfgErr:
		return args
	return _str_of(ScfgAmounts.encode(args[0], args[1], args[2]))


## {"price": str, "code", "nonce"} of amounts.decode(int(i["units"]))
static func _decode(i: Dictionary) -> Variant:
	var units: Variant = _int_field(i, "units")
	if units is ScfgErr:
		return units
	var got: Variant = ScfgAmounts.decode(units)
	if got is ScfgErr:
		return got
	return {"price": str(got[0]), "code": got[1], "nonce": got[2]}


## str(amounts.units_to_decimal(int(i["units"])))
static func _units_to_decimal(i: Dictionary) -> Variant:
	var units: Variant = _int_field(i, "units")
	if units is ScfgErr:
		return units
	return _str_of(ScfgAmounts.units_to_decimal(units))


## str(amounts.decimal_to_units(Decimal(i["amount"])))
static func _decimal_to_units(i: Dictionary) -> Variant:
	var amount: Variant = ScfgPy.py_get(ScfgPy.from_json(i), "amount")
	if amount is ScfgErr:
		return amount
	var d: Variant = ScfgDec.from_value(amount)
	if d is ScfgErr:
		return d
	return _str_of(ScfgAmounts.decimal_to_units(d))


## str(amounts.stored_amount(value)): the JSON value itself, a {"float": repr} a float.
static func _stored_amount(i: Dictionary) -> Variant:
	var raw: Variant = ScfgPy.py_get(ScfgPy.from_json(i), "value")
	if raw is ScfgErr:
		return raw
	var value: Variant = _value(raw)
	if value is ScfgErr:
		return value
	return _str_of(ScfgAmounts.stored_amount(value))


## amounts.format_units(int(i["units"]))
static func _format_units(i: Dictionary) -> Variant:
	var units: Variant = _int_field(i, "units")
	if units is ScfgErr:
		return units
	return ScfgAmounts.format_units(units)


# payment links and QR


## paylinks.eip681_transfer_uri(token_address=..., chain_id=..., recipient=..., units=int(i["units"]))
static func _eip681_transfer_uri(i: Dictionary) -> Variant:
	var args: Variant = _args(ScfgPy.from_json(i), ["token_address", "chain_id", "recipient"])
	if args is ScfgErr:
		return args
	var units: Variant = _int_field(i, "units")
	if units is ScfgErr:
		return units
	return ScfgPaylinks.eip681_transfer_uri({"token_address": args[0], "chain_id": args[1], "recipient": args[2],
		"units": units})


## solana_pay_uri(recipient=..., units=int(i["units"]), mint=..., reference=..., label=..., message=...)
static func _solana_pay_uri(i: Dictionary) -> Variant:
	var args: Variant = _args(ScfgPy.from_json(i), ["recipient", "mint", "reference", "label", "message"])
	if args is ScfgErr:
		return args
	var units: Variant = _int_field(i, "units")
	if units is ScfgErr:
		return units
	return ScfgSolanaPaylinks.solana_pay_uri({"recipient": args[0], "units": units, "mint": args[1],
		"reference": args[2], "label": args[3], "message": args[4]})


## ["0101...", ...]: one string per row of paylinks.qr_matrix(i["text"]), "1" = dark
static func _qr_matrix(i: Dictionary) -> Variant:
	var text: Variant = ScfgPy.py_get(i, "text")
	if text is ScfgErr:
		return text
	var m: Variant = ScfgPaylinks.qr_matrix(text)
	if m is ScfgErr:
		return m
	var rows := []
	for row: Array in m:
		var line := PackedStringArray()
		for v: bool in row:
			line.append("1" if v else "0")
		rows.append("".join(line))
	return rows


## {"size", "rects": [[x, y, w], ...]} of paylinks.qr_rects(i["text"], i["border"])
static func _qr_rects(i: Dictionary) -> Variant:
	var args: Variant = _args(ScfgPy.from_json(i), ["text", "border"])
	if args is ScfgErr:
		return args
	if not args[1] is int:
		return ScfgErr.make("TypeError", "border must be an int, not '%s'" % ScfgPy.py_type(args[1]))
	var got: Variant = ScfgPaylinks.qr_rects(args[0], args[1])
	if got is ScfgErr:
		return got
	return {"size": got[0], "rects": got[1]}


# checkout page messages


## [tx_hash, sender] of page_messages.parse_tx_message(i["data"])
static func _parse_tx_message(i: Dictionary) -> Variant:
	var data: Variant = ScfgPy.py_get(ScfgPy.from_json(i), "data")
	if data is ScfgErr:
		return data
	return ScfgPageMessages.parse_tx_message(data)


## page_messages.parse_build_message(i["data"])
static func _parse_build_message(i: Dictionary) -> Variant:
	var data: Variant = ScfgPy.py_get(ScfgPy.from_json(i), "data")
	if data is ScfgErr:
		return data
	return ScfgPageMessages.parse_build_message(data)


# receipts


## receipts.receipt_key(i["chain"], i["tx_hash"], i["log_index"])
static func _receipt_key(i: Dictionary) -> Variant:
	var args: Variant = _args(ScfgPy.from_json(i), ["chain", "tx_hash", "log_index"])
	if args is ScfgErr:
		return args
	return ScfgReceipts.receipt_key(args[0], args[1], args[2])


## The transfer of a make_receipt case: its fields but "class", value = int(value), built as a
## TransferLog (block_hash required) or a Transfer (no block_hash), as the Python adapter does.
static func _transfer(t: Variant) -> Variant:
	var cls: Variant = ScfgPy.py_get(t, "class")
	if cls is ScfgErr:
		return cls
	var fields: Dictionary = (t as Dictionary).duplicate()
	fields.erase("class")
	var value: Variant = ScfgPy.py_int(ScfgPy.py_get(fields, "value"))  # err-ok: checked on the next line
	if value is ScfgErr:
		return value
	fields["value"] = value
	if typeof(cls) == TYPE_STRING and cls == "TransferLog":
		return ScfgModels.dataclass("TransferLog", ["token", "sender", "recipient", "value", "tx_hash", "log_index",
			"block_number", "block_hash"], {}, fields)
	if typeof(cls) == TYPE_STRING and cls == "Transfer":
		return ScfgTransfers.make_transfer(fields)
	return ScfgErr.make("ValueError", "unknown transfer class %s" % ScfgPy.py_repr(cls))


## receipts.make_receipt(chain=..., transfer=_transfer(i["transfer"]), kind=..., item=..., token=..., source=...)
static func _make_receipt(i: Dictionary) -> Variant:
	var j: Dictionary = ScfgPy.from_json(i)
	var args: Variant = _args(j, ["chain", "transfer", "kind", "item", "token", "source"])
	if args is ScfgErr:
		return args
	var transfer: Variant = _transfer(args[1])
	if transfer is ScfgErr:
		return transfer
	return ScfgReceipts.make_receipt({"chain": args[0], "transfer": transfer, "kind": args[2], "item": args[3],
		"token": args[4], "source": args[5]})


## {"added", "store"} after receipts.add_receipt(dict(i["store"]), i["receipt"])
static func _add_receipt(i: Dictionary) -> Variant:
	var args: Variant = _args(ScfgPy.from_json(i), ["store", "receipt"])
	if args is ScfgErr:
		return args
	if not args[0] is Dictionary:
		return ScfgErr.make("TypeError", "the store must be a dict")
	var store: Dictionary = (args[0] as Dictionary).duplicate()
	var added: Variant = ScfgReceipts.add_receipt(store, args[1])
	if added is ScfgErr:
		return added
	return {"added": added, "store": store}


## The store after receipts.remove_receipt(dict(i["store"]), i["key"])
static func _remove_receipt(i: Dictionary) -> Variant:
	var args: Variant = _args(ScfgPy.from_json(i), ["store", "key"])
	if args is ScfgErr:
		return args
	if not args[0] is Dictionary:
		return ScfgErr.make("TypeError", "the store must be a dict")
	var store: Dictionary = (args[0] as Dictionary).duplicate()
	ScfgReceipts.remove_receipt(store, args[1])
	return store


## {"owned": sorted ids, "tip_total": str} of receipts.compute_ownership(i["store"], items),
## items keyed by item_id with Decimal prices.
static func _compute_ownership(i: Dictionary) -> Variant:
	var args: Variant = _args(ScfgPy.from_json(i), ["store", "items"])
	if args is ScfgErr:
		return args
	if not (args[0] is Dictionary and args[1] is Array):
		return ScfgErr.make("TypeError", "store must be a dict and items a list")
	var items := {}
	for x: Variant in args[1]:
		var f: Variant = _args(x, ["item_id", "code", "price", "token", "title", "min_price"])
		if f is ScfgErr:
			return f
		var price: Variant = ScfgDec.from_value(f[2])
		if price is ScfgErr:
			return price
		var min_price: Variant = ScfgDec.from_value(f[5])
		if min_price is ScfgErr:
			return min_price
		var item: Variant = ScfgModels.make_item({"item_id": f[0], "code": f[1], "price": price, "token": f[3],
			"title": f[4], "min_price": min_price})
		if item is ScfgErr:
			return item
		items[f[0]] = item
	var got: Variant = ScfgReceipts.compute_ownership(args[0], items)
	if got is ScfgErr:
		return got
	return {"owned": got[0], "tip_total": str(got[1])}


# transaction ids


## sessions.normalize_tx_hash(i["text"])
static func _normalize_tx_hash(i: Dictionary) -> Variant:
	var text: Variant = ScfgPy.py_get(ScfgPy.from_json(i), "text")
	if text is ScfgErr:
		return text
	return ScfgSessions.normalize_tx_hash(text)


## backends.normalize_any_tx(i["text"])
static func _normalize_any_tx(i: Dictionary) -> Variant:
	var text: Variant = ScfgPy.py_get(ScfgPy.from_json(i), "text")
	if text is ScfgErr:
		return text
	return ScfgBackends.normalize_any_tx(text)


# rpc_errors: numbers read out of a node's error message


## rpc.suggested_range(RpcError(-32005, i["message"]), i["tried"])
static func _suggested_range(i: Dictionary) -> Variant:
	var v: Variant = ScfgPy.from_json(i)
	return ScfgRpcClient.suggested_range(ScfgRpcClient.rpc_error(-32005, v["message"]), v["tried"])


## SolanaRpc(None) with tx_version = i["tx_version"], then _named_version(RpcError(code, message))
static func _named_version(i: Dictionary) -> Variant:
	var v: Variant = ScfgPy.from_json(i)
	var rpc := ScfgSolanaRpc.new(null)
	rpc.tx_version = v["tx_version"]
	var version: Variant = rpc._named_version(ScfgRpcClient.rpc_error(v["code"], v["message"]))
	return NONE if version == null else version


# chains, configuration and payments (conformance/python/scfg_conformance/tier1_config.py)
#
# A Config is always given as the keyword arguments of build_config; a custom chain inside it is
# the keyword arguments of build_chain (a non-dict entry is passed through untouched, which is
# how chain.not_from_add_chain is reached). A float that JSON cannot carry exactly is written
# `{"float": "4.99"}` and rebuilt as a float (GDScript has Python's int/float distinction, so
# even `{"float": "5.0"}` stays a float). Outputs are `to_jsonable` of the core's values.


## `{"float": "4.99"}` -> float("4.99"); everything else unchanged. A string Python's float()
## refuses is a ValueError.
## @fallible
static func _value(v: Variant) -> Variant:
	if v is Dictionary and (v as Dictionary).size() == 1 and (v as Dictionary).has("float"):
		return _py_float(v["float"])
	return v


## Python `float(text)`: surrounding whitespace, a sign, digits of any script with single
## underscores between digits, a fraction and an exponent, or inf/infinity/nan in any case.
## Anything else is a ValueError, a non-str a TypeError.
## @fallible
static func _py_float(text: Variant) -> Variant:
	if text is bool or text is int or text is float:
		return float(text)
	if text is ScfgBigInt:
		return float((text as ScfgBigInt).to_decimal())
	if not (text is String or text is StringName):
		return ScfgErr.make("TypeError", "float() argument must be a string or a real number, not '%s'" % ScfgPy.py_type(text))
	var t := ScfgPy.py_strip(str(text))
	var ascii := ""
	for i in t.length():
		var c := t.unicode_at(i)
		var d := ScfgPy.decimal_digit(c)
		ascii += String.chr(0x30 + d) if d >= 0 and c > 0x7f else String.chr(c)
	var special := RegEx.create_from_string("^([+-]?)(?:(inf|infinity)|(nan))$").search(ascii.to_lower())
	if special != null:
		if special.get_string(3) != "":
			return NAN
		return -INF if special.get_string(1) == "-" else INF
	var number := "^[+-]?(?:\\d+(?:_\\d+)*(?:\\.(?:\\d+(?:_\\d+)*)?)?|\\.\\d+(?:_\\d+)*)(?:[eE][+-]?\\d+(?:_\\d+)*)?$"
	if RegEx.create_from_string(number).search(ascii) == null:
		return ScfgErr.make("ValueError", "could not convert string to float: %s" % ScfgPy.py_repr(str(text)))
	return ascii.replace("_", "").to_float()


## Python `dict(x)` for a JSON value: a Dictionary is copied, an empty list or str is `{}`, a list of
## pairs is those pairs; anything else is an error.
## @fallible
static func _dict_of(x: Variant) -> Variant:
	if x is Dictionary:
		return (x as Dictionary).duplicate()
	if x is Array:
		var out := {}
		for n in (x as Array).size():
			var pair: Variant = x[n]
			if not (pair is Array or pair is String):
				return ScfgErr.make("TypeError", "cannot convert dictionary update sequence element #%d to a sequence" % n)
			var length: int = pair.length() if pair is String else pair.size()
			if length != 2:
				return ScfgErr.make("ValueError", "dictionary update sequence element #%d has length %d; 2 is required" % [n, length])
			if pair[0] is Array or pair[0] is Dictionary:
				return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(pair[0]))
			out[pair[0]] = pair[1]
		return out
	if x is String:
		if (x as String).is_empty():
			return {}
		return ScfgErr.make("ValueError", "dictionary update sequence element #0 has length 1; 2 is required")
	return ScfgErr.make("TypeError", "'%s' object is not iterable" % ScfgPy.py_type(x))


## `build_chain(**entry)` for a dict, anything else untouched. A missing `key` is a TypeError.
## @fallible
static func _chain_or_raw(entry: Variant) -> Variant:
	if not entry is Dictionary:
		return entry
	if not (entry as Dictionary).has("key"):
		return ScfgErr.make("TypeError", "build_chain() missing 1 required positional argument: 'key'")
	var opts: Dictionary = (entry as Dictionary).duplicate()
	var key: Variant = opts["key"]
	opts.erase("key")
	return ScfgConfig.build_chain(key, opts)


## `build_config(**kwargs)` with the custom chains built first.
## @fallible
static func _config(kwargs: Variant) -> Variant:
	var copied: Variant = _dict_of(kwargs)
	if copied is ScfgErr:
		return copied
	var kw: Dictionary = copied
	if kw.has("custom_chains"):
		var entries: Variant = ScfgConfig._iterate(kw["custom_chains"])
		if entries is ScfgErr:
			return entries
		var built := []
		for entry: Variant in entries:
			var chain: Variant = _chain_or_raw(entry)
			if chain is ScfgErr:
				return chain
			built.append(chain)
		kw["custom_chains"] = built
	return ScfgConfig.build_config(kw)


## `{k: _value(v) for k, v in d.items()}`
## @fallible
static func _item_kwargs(d: Variant) -> Variant:
	if not d is Dictionary:
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'items'" % ScfgPy.py_type(d))
	var out := {}
	for k: Variant in d:
		var v: Variant = _value(d[k])
		if v is ScfgErr:
			return v
		out[k] = v
	return out


## `add_item(config, kw.pop("item_id"), **kw)`.
## @fallible
static func _add_one(config: Dictionary, item: Variant) -> Variant:
	var kw: Variant = _item_kwargs(item)
	if kw is ScfgErr:
		return kw
	if not (kw as Dictionary).has("item_id"):
		return ScfgErr.make("KeyError", "'item_id'")
	var item_id: Variant = kw["item_id"]
	(kw as Dictionary).erase("item_id")
	return ScfgConfig.add_item(config, item_id, kw)


## `_config(config_kw)` with each of `items` added in order.
## @fallible
static func _with_items(config_kw: Variant, items: Variant) -> Variant:
	var config: Variant = _config(config_kw)
	if config is ScfgErr:
		return config
	var each: Variant = ScfgConfig._iterate(items)
	if each is ScfgErr:
		return each
	for item: Variant in each:
		var added: Variant = _add_one(config, item)
		if added is ScfgErr:
			return added
	return config


## to_jsonable(get_chains(i["testnet"]))
static func _get_chains(i: Dictionary) -> Variant:
	var testnet: Variant = ScfgPy.py_get(i, "testnet")
	if testnet is ScfgErr:
		return testnet
	return to_jsonable(ScfgChains.get_chains(testnet))


## to_jsonable(build_chain(**i))
static func _build_chain(i: Dictionary) -> Variant:
	var chain: Variant = _chain_or_raw(ScfgPy.from_json(i))
	if chain is ScfgErr:
		return chain
	return to_jsonable(chain)


## to_jsonable(_config(i))
static func _build_config(i: Dictionary) -> Variant:
	var config: Variant = _config(ScfgPy.from_json(i))
	if config is ScfgErr:
		return config
	return to_jsonable(config)


## to_jsonable(add_item(_with_items(i["config"], i.get("existing", [])), **i["item"]))
static func _add_item(i: Dictionary) -> Variant:
	var j: Variant = ScfgPy.from_json(i)
	var config_kw: Variant = ScfgPy.py_get(j, "config")
	if config_kw is ScfgErr:
		return config_kw
	var config: Variant = _with_items(config_kw, j.get("existing", []))
	if config is ScfgErr:
		return config
	var item: Variant = ScfgPy.py_get(j, "item")
	if item is ScfgErr:
		return item
	var added: Variant = _add_one(config, item)
	if added is ScfgErr:
		return added
	return to_jsonable(added)


## to_jsonable(prepare_payment(_with_items(i["config"], i.get("items", [])), i["kind"], **fields))
static func _prepare_payment(i: Dictionary) -> Variant:
	var j: Variant = ScfgPy.from_json(i)
	var config_kw: Variant = ScfgPy.py_get(j, "config")
	if config_kw is ScfgErr:
		return config_kw
	var config: Variant = _with_items(config_kw, j.get("items", []))
	if config is ScfgErr:
		return config
	var kind: Variant = ScfgPy.py_get(j, "kind")
	if kind is ScfgErr:
		return kind
	var fields := {}
	for k: String in ["item", "amount", "token", "title"]:
		if (j as Dictionary).has(k):
			var v: Variant = _value(j[k])
			if v is ScfgErr:
				return v
			fields[k] = v
	var spec: Variant = ScfgPayments.prepare_payment(config, kind, fields)
	if spec is ScfgErr:
		return spec
	return to_jsonable(spec)


## canonical.to_jsonable: a core value as plain JSON types, enforcing the vector rules. An
## ScfgDec is its `str()`; an integral float is an int; any int of 2^53 or more, a non-integral
## float, a non-string key and bytes are errors (ValueError, ValueError, TypeError, TypeError).
## @fallible
static func to_jsonable(obj: Variant, path: String = "") -> Variant:
	match typeof(obj):
		TYPE_NIL, TYPE_BOOL, TYPE_STRING, TYPE_STRING_NAME:
			return obj
		TYPE_INT:
			return _check_safe_int(obj, path)
		TYPE_FLOAT:
			var f: float = obj
			if is_nan(f) or is_inf(f) or f != floorf(f):
				return ScfgErr.make("ValueError", "non-integral float %s at %s" % [ScfgPy.py_repr(f), path if path != "" else "<root>"])
			if absf(f) >= 9007199254740992.0:
				return ScfgErr.make("ValueError", "float %s at %s is not safe in JSON" % [ScfgPy.py_repr(f), path if path != "" else "<root>"])
			return int(f)
		TYPE_PACKED_BYTE_ARRAY:
			return ScfgErr.make("TypeError", "bytes at %s: hex-encode into a *_hex field" % (path if path != "" else "<root>"))
		TYPE_OBJECT:
			if obj is ScfgDec:
				return str(obj)
			if obj is ScfgBigInt:
				return ScfgErr.make("ValueError", "integer %s at %s is not safe in JSON (|n| >= 2**53); write it as a string" % [
					(obj as ScfgBigInt).to_decimal(), path if path != "" else "<root>"])
		TYPE_DICTIONARY:
			var out := {}
			for key: Variant in obj:
				if not (key is String or key is StringName):
					return ScfgErr.make("TypeError", "non-string dict key %s at %s" % [ScfgPy.py_repr(key), path if path != "" else "<root>"])
				var walked: Variant = to_jsonable(obj[key], "%s.%s" % [path, key] if path != "" else str(key))
				if walked is ScfgErr:
					return walked
				out[str(key)] = walked
			return out
		TYPE_ARRAY:
			var out := []
			for n in (obj as Array).size():
				var walked: Variant = to_jsonable(obj[n], "%s[%d]" % [path, n])
				if walked is ScfgErr:
					return walked
				out.append(walked)
			return out
	return ScfgErr.make("TypeError", "unsupported type %s at %s" % [ScfgPy.py_type(obj), path if path != "" else "<root>"])


## @fallible
static func _check_safe_int(n: int, path: String) -> Variant:
	if n >= 9007199254740992 or n <= -9007199254740992:
		return ScfgErr.make("ValueError", "integer %d at %s is not safe in JSON (|n| >= 2**53); write it as a string" % [
			n, path if path != "" else "<root>"])
	return n


## base58.decode_fixed(i["program_id"], 32)
static func _program_id(i: Dictionary) -> Variant:
	var text: Variant = ScfgPy.py_get(i, "program_id")
	if text is ScfgErr:
		return text
	return ScfgSolanaBase58.decode_fixed(text, 32)


## [i[k] for k in keys]: a KeyError for a missing key, a TypeError when `i` is not a dict.
static func _args(i: Variant, keys: Array) -> Variant:
	var out := []
	for k: String in keys:
		var v: Variant = ScfgPy.py_get(i, k)
		if v is ScfgErr:
			return v
		out.append(v)
	return out


## Python `int(i[key])` (the decimal strings of the units fields).
static func _int_field(i: Dictionary, key: String) -> Variant:
	var v: Variant = ScfgPy.py_get(ScfgPy.from_json(i), key)
	if v is ScfgErr:
		return v
	return ScfgPy.py_int(v)


## `str()` of a core result (an int, ScfgBigInt or ScfgDec), passing an ScfgErr through.
static func _str_of(v: Variant) -> Variant:
	if v is ScfgErr:
		return v
	if v is ScfgBigInt:
		return (v as ScfgBigInt).to_decimal()
	return str(v)


## `.hex()` of a core result, passing an ScfgErr through.
static func _hex_of(v: Variant) -> Variant:
	if v is ScfgErr:
		return v
	return ScfgBytes.bytes_to_hex(v)


## `bytes.fromhex(i[key])`: KeyError for a missing field, TypeError for a non-string, ValueError
## for bad hex.
static func _hex_field(i: Dictionary, key: String) -> Variant:
	var v: Variant = ScfgPy.py_get(i, key)
	if v is ScfgErr:
		return v
	return bytes_fromhex(v)


## Python's `bytes.fromhex(s)` (3.12): ASCII whitespace is skipped between bytes but never inside
## a pair; no `0x` prefix; anything else (non-ASCII included) is a ValueError, a non-string a
## TypeError. Not ScfgBytes.hex_to_bytes, which takes `0x` and rejects whitespace: the harness
## must parse exactly as the Python adapters do.
## @fallible
static func bytes_fromhex(s: Variant) -> Variant:
	if not (s is String or s is StringName):
		return ScfgErr.make("TypeError", "fromhex() argument must be str, not %s" % ScfgPy.py_type(s))
	var text := str(s)
	var out := PackedByteArray()
	var i := 0
	while i < text.length():
		var c := text.unicode_at(i)
		if c == 0x20 or (c >= 0x09 and c <= 0x0d):
			i += 1
			continue
		var hi := _hex_digit(c)
		var lo := _hex_digit(text.unicode_at(i + 1)) if i + 1 < text.length() else -1
		if hi < 0 or lo < 0:
			return ScfgErr.make("ValueError", "non-hexadecimal number found in fromhex() arg at position %d" % (
				i if hi < 0 else i + 1))
		out.append(hi * 16 + lo)
		i += 2
	return out


static func _hex_digit(c: int) -> int:
	if c >= 0x30 and c <= 0x39:
		return c - 0x30
	if c >= 0x61 and c <= 0x66:
		return c - 0x61 + 10
	if c >= 0x41 and c <= 0x46:
		return c - 0x41 + 10
	return -1


# --- runner -----------------------------------------------------------------------------------


## "" when the case passes, otherwise why it failed (naming the first difference).
static func run_case(c: Variant) -> String:
	if not (c is Dictionary and typeof(c.get("fn")) == TYPE_STRING):
		return "malformed case %s" % ScfgPy.py_repr(c)
	var fn: String = c["fn"]
	var label := "%s(%s)" % [fn, _json(c.get("in"))]
	if c.has("out") == c.has("error"):
		return "%s: the case needs exactly one of out and error" % label
	if not c.get("in") is Dictionary:
		return "%s: `in` is not an object" % label
	if not FUNCS.has(fn):
		return "%s: no GDScript adapter for %s" % [label, fn]
	var got: Variant = _compute(FUNCS[fn], c["in"])
	if got is String:
		return "%s: %s" % [label, got]
	var want := {"out": c["out"]} if c.has("out") else {"error": c["error"]}
	var diff := T.first_diff(got, want)
	if diff == "":
		return ""
	return "%s: %s (got %s, vector says %s)" % [label, diff, _json(got), _json(want)]


## {"out": v} or {"error": code}, or a String saying why the adapter itself failed.
static func _compute(f: Callable, input: Dictionary) -> Variant:
	var r: Variant = f.call(input.duplicate(true))
	if r is ScfgErr:
		var e: ScfgErr = r
		if e.is_a("ConfigError"):
			return {"error": e.code}
		if e.is_a("ValueError") or e.is_a("TypeError"):
			return {"error": "invalid"}
		return "raised %s, which Python's Tier 1 runner does not bucket" % str(e)
	if typeof(r) == TYPE_NIL:
		return "the adapter did not return (aborted by a script error?); use Tier1.NONE for None"
	if typeof(r) == TYPE_STRING_NAME and r == NONE:
		return {"out": null}
	return {"out": r}


static func _json(v: Variant) -> String:
	var s := JSON.stringify(v, "", true)
	return s if s.length() <= 200 else s.substr(0, 197) + "..."
