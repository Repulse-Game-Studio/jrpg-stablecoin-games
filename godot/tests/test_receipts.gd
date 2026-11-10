extends RefCounted

const T := preload("res://tests/t.gd")

const USDC := "0xaf88d065e77c8cC2239327C5EDb3A432268e5831"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"


func _d(s: String) -> ScfgDec:
	var v: Variant = ScfgDec.parse(s)
	return v if v is ScfgDec else null


func _items() -> Dictionary:
	return {
		"chapter2": ScfgModels.make_item({"item_id": "chapter2", "code": 37, "price": _d("4.99"), "token": "USDC",
			"title": "Chapter 2", "min_price": _d("4.99")}),
		"ost": ScfgModels.make_item({"item_id": "ost", "code": 2, "price": _d("2.00"), "token": "USDT",
			"title": "OST", "min_price": _d("1.50")}),
	}


## A TransferLog (scfg_core.evm), as ScfgEvm.parse_transfer_log builds it.
func _transfer(value: int = 4993742, tx: String = "0x" + "ab".repeat(32), log_index: int = 3,
		block: int = 777) -> Dictionary:
	return {"token": USDC, "sender": PAYER, "recipient": DEV, "value": ScfgBigInt.from_int(value),
		"tx_hash": tx, "log_index": log_index, "block_number": block, "block_hash": "0x" + "11".repeat(32)}


func _receipt(kind: Variant = "unlock", item: Variant = "chapter2", value: int = 4993742, log_index: int = 3) -> Variant:
	return ScfgReceipts.make_receipt({"chain": "arbitrum", "transfer": _transfer(value, "0x" + "ab".repeat(32), log_index),
		"kind": kind, "item": item, "token": "USDC", "source": "payment"})


func _owned(store: Dictionary, items: Dictionary) -> Array:
	var got: Variant = ScfgReceipts.compute_ownership(store, items)
	if not got is Array:
		return ["<%s>" % str(got)]
	return [got[0], str(got[1])]


# tests/unit/test_receipts.py::test_receipt_key
func test_receipt_key() -> bool:
	T.eq(ScfgReceipts.receipt_key("base", "0xABC", 4), "base:0xabc:4", "EVM hashes are lowercased")
	T.eq(ScfgReceipts.receipt_key("solana", "AbC", 0), "solana:AbC:0", "Solana signatures keep their case")
	T.eq(ScfgReceipts.receipt_key("base", "0XAB", 1), "base:0XAB:1", "only a lowercase 0x prefix lowers")
	T.is_err(ScfgReceipts.receipt_key("base", 5, 1), "AttributeError", "Python: 5.startswith")
	return T.done()


# tests/unit/test_receipts.py::test_make_receipt_shape
func test_make_receipt_shape() -> bool:
	var r: Variant = _receipt()
	T.ok(r is Dictionary, "a receipt")
	if r is Dictionary:
		T.eq((r as Dictionary).keys(), ["chain", "tx_hash", "log_index", "block", "kind", "item", "amount", "token",
			"payer", "recipient", "source"], "the frozen field order")
		T.eq(r, {"chain": "arbitrum", "tx_hash": "0x" + "ab".repeat(32), "log_index": 3, "block": 777,
			"kind": "unlock", "item": "chapter2", "amount": "4.993742", "token": "USDC",
			"payer": PAYER, "recipient": DEV, "source": "payment"}, "the receipt")
		T.json_eq(JSON.parse_string(JSON.stringify(r)), r, "JSON round trip")
	var sol: Variant = ScfgReceipts.make_receipt({"chain": "solana", "transfer": ScfgTransfers.make_transfer({
		"token": "mint", "sender": null, "recipient": "dev", "value": 5000000, "tx_hash": "Sig",
		"log_index": 2, "block_number": 9}), "kind": "tip", "item": null, "token": "USDC", "source": "restore"})
	T.eq(sol, {"chain": "solana", "tx_hash": "Sig", "log_index": 2, "block": 9, "kind": "tip", "item": null,
		"amount": "5.000000", "token": "USDC", "payer": null, "recipient": "dev", "source": "restore"},
		"a Transfer with an int value")
	return T.done()


# tests/unit/test_receipts.py::test_make_receipt_validates
func test_make_receipt_validates() -> bool:
	T.is_err(ScfgReceipts.make_receipt({"chain": "base", "transfer": _transfer(), "kind": "gift", "item": null,
		"token": "USDC", "source": "payment"}), "ValueError", "bad kind")
	T.is_err(ScfgReceipts.make_receipt({"chain": "base", "transfer": _transfer(), "kind": "tip", "item": null,
		"token": "USDC", "source": "magic"}), "ValueError", "bad source")
	T.is_err(ScfgReceipts.make_receipt({"chain": "base", "transfer": _transfer(), "kind": "tip", "item": null,
		"token": "USDC"}), "TypeError", "a missing keyword argument")
	var partial := _transfer()
	partial.erase("value")
	T.is_err(ScfgReceipts.make_receipt({"chain": "base", "transfer": partial, "kind": "tip", "item": null,
		"token": "USDC", "source": "payment"}), "AttributeError", "a transfer without value")
	return T.done()


# tests/unit/test_receipts.py::test_add_receipt_dedupes_on_key
func test_add_receipt_dedupes_on_key() -> bool:
	var store := {}
	T.eq(ScfgReceipts.add_receipt(store, _receipt()), true, "added")
	T.eq(ScfgReceipts.add_receipt(store, _receipt()), false, "the same key again")
	T.eq(store.keys(), ["arbitrum:0x" + "ab".repeat(32) + ":3"], "one key")
	ScfgReceipts.remove_receipt(store, store.keys()[0])
	ScfgReceipts.remove_receipt(store, "missing")  # idempotent
	T.eq(store, {}, "empty again")
	T.is_err(ScfgReceipts.add_receipt(store, {"chain": "x"}), "KeyError", "a receipt without tx_hash")
	return T.done()


# tests/unit/test_receipts.py::test_ownership_unlock_and_tips
func test_ownership_unlock_and_tips() -> bool:
	var store := {}
	ScfgReceipts.add_receipt(store, _receipt())
	ScfgReceipts.add_receipt(store, _receipt("tip", null, 3000005, 4))
	ScfgReceipts.add_receipt(store, _receipt("tip", "tip_jar", 1000000, 5))
	T.eq(_owned(store, _items()), [["chapter2"], "4.000005"], "owned and tipped")
	return T.done()


# tests/unit/test_receipts.py::test_ownership_requires_min_price
func test_ownership_requires_min_price() -> bool:
	var store := {}
	ScfgReceipts.add_receipt(store, _receipt("unlock", "ost", 1490000))
	T.eq(_owned(store, _items())[0], [], "below min_price")
	ScfgReceipts.add_receipt(store, _receipt("unlock", "ost", 1500000, 9))
	T.eq(_owned(store, _items())[0], ["ost"], "at min_price")
	return T.done()


# tests/unit/test_receipts.py::test_ownership_ignores_unknown_items
func test_ownership_ignores_unknown_items() -> bool:
	var store := {}
	ScfgReceipts.add_receipt(store, _receipt("unlock", "deleted_dlc"))
	T.eq(_owned(store, _items()), [[], "0"], "nothing owned, Decimal(0) tipped")
	return T.done()


# tests/unit/test_receipts.py::test_ownership_skips_malformed_entries
func test_ownership_skips_malformed_entries() -> bool:
	for junk: Variant in [{}, {"kind": "unlock"}, {"kind": "unlock", "item": "chapter2", "amount": "lots"},
			{"kind": "unlock", "item": "chapter2", "amount": null}, {"kind": "tip", "amount": "NaN"},
			{"kind": "weird", "amount": "1"}, null, "text", [], 5, {"kind": ["tip"], "amount": "1"},
			{"kind": "tip", "amount": {}}, {"kind": "tip", "amount": "-Infinity"}]:
		var store := {"junk": junk}
		ScfgReceipts.add_receipt(store, _receipt())
		T.eq(_owned(store, _items()), [["chapter2"], "0"], "skips %s" % ScfgPy.py_repr(junk))
	return T.done()


## What Python does not skip: Decimal([...]) is a ValueError and an unhashable item a TypeError
## (neither is caught by compute_ownership); a bool or number amount is Decimal(True) / exact.
func test_ownership_mirrors_python_on_odd_values() -> bool:
	T.is_err(ScfgReceipts.compute_ownership({"a": {"kind": "tip", "amount": []}}, _items()), "ValueError",
		"Decimal([]) is not caught")
	T.is_err(ScfgReceipts.compute_ownership({"a": {"kind": "unlock", "item": ["x"], "amount": "5"}}, _items()),
		"TypeError", "items.get([...]): unhashable")
	T.eq(_owned({"a": {"kind": "tip", "amount": true}, "b": {"kind": "tip", "amount": 4.0},
		"c": {"kind": "tip", "amount": 0.5}, "d": {"kind": "tip", "amount": [0, [2, 5], -1]}}, _items()),
		[[], "8.0"], "True + 4 + 0.5 + Decimal((0, (2, 5), -1))")
	# Python: compute_ownership raises decimal.Overflow (uncaught) for these stored tips
	T.is_err(ScfgReceipts.compute_ownership({"a": {"kind": "tip", "amount": "1E+1000000"}}, _items()), "Overflow",
		"one tip above Emax")
	T.is_err(ScfgReceipts.compute_ownership({"a": {"kind": "tip", "amount": "9E+999999"},
		"b": {"kind": "tip", "amount": "9E+999999"}}, _items()), "Overflow", "two tips whose sum passes Emax")
	T.eq(_owned({"a": {"kind": "tip", "amount": "9E+999999"}}, _items()), [[], "9.000000000000000000000000000E+999999"],
		"one 9E+999999 tip is still a value")
	T.eq(_owned({"a": {"kind": "tip", "amount": "1.5"}, "b": {"kind": "tip", "amount": "0"}}, _items()),
		[[], "1.5"], "a Decimal sum keeps the widest exponent")
	T.eq(_owned({"b": {"kind": "unlock", "item": "ost", "amount": "2"}, "a": {"kind": "unlock", "item": "chapter2",
		"amount": "5"}, "c": {"kind": "unlock", "item": "ost", "amount": "9"}}, _items())[0], ["chapter2", "ost"],
		"owned ids are a set, returned sorted")
	return T.done()
