## Value objects shared with the developer's script. Mirrors scfg_core/models.py.
##
## Each is a Dictionary with Python's dataclass fields in Python's order (GDScript dictionaries
## keep insertion order). The `make_*` functions are the dataclass constructors: a missing
## required field or an unknown one is a TypeError, as Python's generated __init__ raises.
##
## - Item: a paid unlock declared with scfg.add_item(): item_id, code, price (ScfgDec), token,
##   title, min_price (ScfgDec).
## - PaymentResult: what pay(), callbacks and on_result receive. status: "paid" |
##   "already_owned" | "cancelled" | "expired" | "underpaid" | "failed" | "pending".
## - RestoreResult: what restore() returns. status: "restored" | "nothing_found" | "cancelled"
##   | "failed".
class_name ScfgModels
extends RefCounted

const ITEM_FIELDS: Array[String] = ["item_id", "code", "price", "token", "title", "min_price"]
const PAYMENT_RESULT_FIELDS: Array[String] = ["status", "kind", "item", "amount", "token", "chain", "tx_hash",
	"payer", "session_id", "error"]
const RESTORE_RESULT_FIELDS: Array[String] = ["status", "restored", "error"]


## Python `Item(**fields)`.
## @fallible
static func make_item(fields: Dictionary) -> Variant:
	return dataclass("Item", ITEM_FIELDS, {}, fields)


## Python `PaymentResult(**partial)`: status and kind are required, every other field
## defaults to None.
## @fallible
static func make_payment_result(partial: Dictionary) -> Variant:
	return dataclass("PaymentResult", PAYMENT_RESULT_FIELDS, {"item": null, "amount": null, "token": null,
		"chain": null, "tx_hash": null, "payer": null, "session_id": null, "error": null}, partial)


## Python `RestoreResult(**partial)`: status is required; restored defaults to a new empty
## list, error to None.
## @fallible
static func make_restore_result(partial: Dictionary) -> Variant:
	return dataclass("RestoreResult", RESTORE_RESULT_FIELDS, {"restored": [], "error": null}, partial)


## A dataclass constructor: `fields` in order, `defaults` for the optional ones (deep-copied, so
## a list default is fresh per call, as `field(default_factory=list)`), values from `given`.
## @fallible
static func dataclass(name: String, fields: Array[String], defaults: Dictionary, given: Dictionary) -> Variant:
	for key: Variant in given:
		if not (key is String or key is StringName) or not str(key) in fields:
			return ScfgErr.make("TypeError", "%s.__init__() got an unexpected keyword argument %s" % [
				name, ScfgPy.py_repr(key)])
	var out := {}
	var missing := PackedStringArray()
	for f: String in fields:
		if given.has(f):
			out[f] = given[f]
		elif defaults.has(f):
			out[f] = defaults[f].duplicate(true) if (defaults[f] is Array or defaults[f] is Dictionary) else defaults[f]
		else:
			missing.append("'%s'" % f)
	if not missing.is_empty():
		return ScfgErr.make("TypeError", "%s.__init__() missing %d required positional argument%s: %s" % [
			name, missing.size(), "" if missing.size() == 1 else "s", " and ".join(missing)])
	return out
