## Receipts: verified on-chain transfers to the developer, stored in `persistent`. Mirrors
## scfg_core/receipts.py.
##
## There is deliberately no "owned" flag anywhere. Ownership is always recomputed from
## receipts, and receipts are re-checked against the chain at every launch.
##
## A receipt is the frozen JSON record (conformance/schema/state.json), a Dictionary in this
## field order: chain, tx_hash, log_index, block, kind, item, amount, token, payer, recipient,
## source. A store is a Dictionary {receipt_key: receipt}.
class_name ScfgReceipts
extends RefCounted


## "<chain>:<tx>:<log_index>". EVM hashes are hex (case-insensitive) and are lowercased when
## they start with "0x"; Solana signatures are base58 (case matters). A non-string tx_hash is
## Python's AttributeError (`.startswith`).
## @fallible
static func receipt_key(chain: Variant, tx_hash: Variant, log_index: Variant) -> Variant:
	if not (tx_hash is String or tx_hash is StringName):
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'startswith'" % ScfgPy.py_type(tx_hash))
	var tx := str(tx_hash)
	if tx.begins_with("0x"):
		tx = tx.to_lower()
	return "%s:%s:%s" % [ScfgPy.py_str(chain), tx, ScfgPy.py_str(log_index)]


## Python `make_receipt(*, chain, transfer, kind, item, token, source)`; the keyword arguments
## travel in `opts` (a missing one is a TypeError). `transfer` is a Transfer or TransferLog
## Dictionary (a missing field is Python's AttributeError). A kind other than "unlock"/"tip" or a
## source other than "payment"/"restore" is a ValueError.
## @fallible
static func make_receipt(opts: Dictionary) -> Variant:
	for key: String in ["chain", "transfer", "kind", "item", "token", "source"]:
		if not opts.has(key):
			return ScfgErr.make("TypeError", "make_receipt() missing 1 required keyword-only argument: '%s'" % key)
	var kind: Variant = opts["kind"]
	if not (typeof(kind) == TYPE_STRING and kind in ["unlock", "tip"]):
		return ScfgErr.make("ValueError", "invalid kind %s" % ScfgPy.py_repr(kind))
	var source: Variant = opts["source"]
	if not (typeof(source) == TYPE_STRING and source in ["payment", "restore"]):
		return ScfgErr.make("ValueError", "invalid source %s" % ScfgPy.py_repr(source))
	var transfer: Variant = opts["transfer"]
	var t := {}
	for f: String in ["tx_hash", "log_index", "block_number", "value", "sender", "recipient"]:
		if not ScfgPy.has(transfer, f):
			return ScfgErr.make("AttributeError", "'%s' object has no attribute '%s'" % [ScfgPy.py_type(transfer), f])
		t[f] = transfer[f]
	var amount: Variant = ScfgAmounts.units_to_decimal(t["value"])
	if amount is ScfgErr:
		return amount
	return {
		"chain": opts["chain"], "tx_hash": t["tx_hash"], "log_index": t["log_index"],
		"block": t["block_number"], "kind": kind, "item": opts["item"],
		"amount": str(amount), "token": opts["token"],
		"payer": t["sender"], "recipient": t["recipient"], "source": source,
	}


## Store the receipt unless its key is already there. Returns whether it was added. A receipt
## without chain, tx_hash or log_index is a KeyError.
## @fallible
static func add_receipt(store: Dictionary, receipt: Variant) -> Variant:
	var parts := []
	for f: String in ["chain", "tx_hash", "log_index"]:
		var v: Variant = ScfgPy.py_get(receipt, f)
		if v is ScfgErr:
			return v
		parts.append(v)
	var key: Variant = receipt_key(parts[0], parts[1], parts[2])
	if key is ScfgErr:
		return key
	if store.has(key):
		return false
	store[key] = receipt
	return true


## Python `store.pop(key, None)`.
static func remove_receipt(store: Dictionary, key: Variant) -> void:
	store.erase(key)


## [owned item ids, total tipped]: `owned` is Python's set as an Array, sorted; the tip total is
## an ScfgDec (a Python Decimal sum: it keeps the widest exponent of the amounts, and no tips is
## `Decimal(0)`). `items` maps item_id to an Item Dictionary (ScfgModels.make_item).
##
## Malformed entries are skipped, never fatal, as Python's
## `except (KeyError, TypeError, ValueError, AttributeError)` skips them: a record that is not a
## dict, a missing kind or amount, an amount that is not a number within the stored-amount range
## (ScfgAmounts.stored_amount, so the tip total stays finite) and an unhashable item (a list or
## dict). An Item whose min_price is not an ScfgDec is a TypeError, as Python's comparison
## raises outside that `try`.
## @fallible
static func compute_ownership(store: Dictionary, items: Dictionary) -> Variant:
	var owned := {}
	var tips: ScfgDec = ScfgDec.from_int(0)  # err-ok: an int is always accepted
	for r: Variant in store.values():
		if not r is Dictionary or not (r as Dictionary).has("kind") or not (r as Dictionary).has("amount"):
			continue  # Python: TypeError (not subscriptable) or KeyError
		var kind: Variant = r["kind"]
		var amount: Variant = ScfgAmounts.stored_amount(r["amount"])
		if amount is ScfgErr:
			var e: ScfgErr = amount
			if e.is_a("TypeError") or e.is_a("ValueError"):
				continue  # not a number within the stored-amount range: skipped
			return e
		if typeof(kind) == TYPE_STRING and kind == "unlock":
			var key: Variant = (r as Dictionary).get("item")
			if key is Array or key is Dictionary:
				continue  # Python: items.get([...]) is a TypeError: skipped
			var item: Variant = items.get(key) if (key is String or key is StringName) else null
			if item is Dictionary:
				var min_price: Variant = (item as Dictionary).get("min_price")
				if not min_price is ScfgDec:
					return ScfgErr.make("TypeError", "an Item's min_price must be a Decimal, not '%s'" % ScfgPy.py_type(min_price))
				if (amount as ScfgDec).cmp(min_price) >= 0:
					owned[item["item_id"]] = true
		elif typeof(kind) == TYPE_STRING and kind == "tip":
			var sum: Variant = tips.add_checked(amount)
			if sum is ScfgErr:
				return sum  # cannot happen: stored amounts are bounded, so the sum stays below Emax
			tips = sum
	var ids: Array = owned.keys()
	ids.sort()
	return [ids, tips]
