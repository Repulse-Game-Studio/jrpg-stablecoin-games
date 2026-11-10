## State behind the restore screen, independent of any engine. Mirrors scfg_core/restore_flow.py
## (and rpgmaker/src/core/restore_flow.ts).
##
##   input -> checking -> (choose ->) saving -> done
##
## `ScfgRestoreController.new({"submit": ...})`: `submit(fn: Callable) -> ScfgJob` is the
## ScfgWorker's. The screen shows the fields (`state`, `tx_input`, `error`, `candidates`, `choices`,
## `added`), reads `message()` and `can_confirm`, and calls `check`, `choose`, `confirm` from its
## buttons and `poll()` every frame. Python's `message()` is a method and `can_confirm` a property,
## as here. `candidates` are the Dictionaries ScfgRestore.lookup returns; `choices` maps a candidate
## index (an int) to an item id or "tip" (a screen that holds string keys must convert them).
##
## A job that failed or was cancelled sends the screen back to `input` with the RestoreError's
## code as `error` ("failed" for any other error, as Python's non-RestoreError exceptions). A job
## that succeeded with the wrong kind of value (not a list of candidates, not a list of added
## receipts) is "failed" too, where Python would have raised.
class_name ScfgRestoreController
extends RefCounted

const MESSAGES := {
	"invalid_hash": "That doesn't look like a transaction hash or signature.",
	"tx_not_found": "Transaction not found. Check the hash, or try again later.",
	"tx_reverted": "That transaction failed on-chain; nothing was paid.",
	"tx_not_a_payment": "This transaction isn't a payment to this game.",
	"network_unavailable": "Can't reach the network. Try again in a moment.",
	"paste_blocked": "Your browser blocked pasting. Type the hash instead.",
	"failed": "Something went wrong. Please try again.",
}

var submit: Callable
var state: String = "input"
var tx_input: String = ""
var error: Variant = null
var candidates: Array = []
var choices: Dictionary = {}
var added: Array = []
var _future: Variant = null
var _phase: Variant = null


func _init(opts: Dictionary = {}) -> void:
	submit = opts.get("submit", Callable())


## The text for `error`, or null (no error, or a code with no text).
func message() -> Variant:
	return MESSAGES.get(error) if ScfgPy.py_truthy(error) else null


## Indexes of the candidates that wait for the player's choice.
func pending_choices() -> Array:
	var out := []
	for i in candidates.size():
		var c: Dictionary = candidates[i]
		if c["kind"] == null and not c["already_restored"]:
			out.append(i)
	return out


## Look the text (default: `tx_input`) up. False, with `error` = "invalid_hash", when it is not a
## transaction id of any family.
func check(text: Variant = null) -> bool:
	var tx: Variant = ScfgBackends.normalize_any_tx(tx_input if text == null else text)
	if tx is ScfgErr:
		error = "invalid_hash" if (tx as ScfgErr).is_a("ValueError") or (tx as ScfgErr).is_a("TypeError") else "failed"
		return false
	error = null
	state = "checking"
	_phase = "lookup"
	_future = submit.call(func(e: Variant) -> Variant: return await e.restore_lookup(tx))  # err-ok: an ScfgErr fails the job; poll() reads it
	return true


func choose(index: int, choice: String) -> void:
	choices[index] = choice


## Every candidate that needs the player's choice has one.
var can_confirm: bool:
	get:
		for i: int in pending_choices():
			if not choices.has(i):
				return false
		return true


func confirm() -> void:
	if (state != "checking" and state != "choose") or not can_confirm:
		return
	var cands := []
	var index_map := {}
	for i in candidates.size():
		if not candidates[i]["already_restored"]:
			index_map[i] = cands.size()
			cands.append(candidates[i])
	var picked := {}
	for i: Variant in choices:
		if index_map.has(i):
			picked[index_map[i]] = choices[i]
	state = "saving"
	_phase = "apply"
	_future = submit.call(func(e: Variant) -> Variant: return await e.restore_apply(cands, picked))  # err-ok: an ScfgErr fails the job; poll() reads it


## Pick up the finished lookup or save (call often from the UI).
func poll() -> void:
	var fut: Variant = _future
	if fut == null or not fut.done():
		return
	_future = null
	var exc: Variant = fut.exception()  # err-ok: the job's failure, handled below
	var value: Variant = fut.result() if exc == null else null  # err-ok: exception() is null, so this is the value
	if exc != null or not _is_list_of_dicts(value):
		state = "input"
		error = (exc as ScfgErr).code if exc is ScfgErr and (exc as ScfgErr).is_a("RestoreError") else "failed"
		return
	if _phase == "lookup":
		candidates = value
		var all_restored := true
		for c: Dictionary in candidates:
			all_restored = all_restored and c["already_restored"]
		if all_restored:
			state = "done"
		elif not pending_choices().is_empty():
			state = "choose"
		else:
			confirm()
	else:
		added = value
		state = "done"


## status "restored" / "nothing_found" once done, else "cancelled" (with the last error).
func result() -> Dictionary:
	var made: Variant
	if state == "done":
		made = ScfgModels.make_restore_result({"status": "restored" if not added.is_empty() else "nothing_found",  # err-ok: known fields only
			"restored": added.duplicate()})
	else:
		made = ScfgModels.make_restore_result({"status": "cancelled", "error": error})  # err-ok: known fields only
	return made


static func _is_list_of_dicts(value: Variant) -> bool:
	if not value is Array:
		return false
	for item: Variant in value:
		if not item is Dictionary:
			return false
	return true
