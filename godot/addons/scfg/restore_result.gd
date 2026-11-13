## What `await Scfg.restore()` and a restore() callback give the game: the core's RestoreResult
## (ScfgModels) as a typed object.
##
## `status` is "restored", "nothing_found", "cancelled" or "failed". `restored` lists the receipts
## that were added (Dictionaries: chain, tx_hash, kind, item, amount, token, ...). `error` is null,
## the core's error code for a lookup that failed ("tx_not_found", ...) or a ConfigError code for
## a misused restore().
class_name ScfgRestoreResult
extends RefCounted

var status: String = ""
var restored: Array = []
var error: Variant = null


## The result for a core RestoreResult Dictionary (ScfgModels.make_restore_result).
static func from_core(d: Dictionary) -> ScfgRestoreResult:
	var r := ScfgRestoreResult.new()
	r.status = str(d.get("status", ""))
	var list: Variant = d.get("restored")
	r.restored = (list as Array).duplicate(true) if list is Array else []
	r.error = d.get("error")
	return r


func to_dict() -> Dictionary:
	return {"status": status, "restored": restored, "error": error}


func _to_string() -> String:
	return "ScfgRestoreResult(%s)" % JSON.stringify(to_dict())
