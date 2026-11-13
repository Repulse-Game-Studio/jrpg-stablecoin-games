## The restore screen: the Godot twin of the `scfg_restore` screen in
## plugin/game/scfg/scfg_screens.rpy, shown by ScfgDefaultSheetHost for `await Scfg.restore()`.
##
## By the ScfgRestoreController's state: input / checking (the hash field and Check), choose (each
## ambiguous payment with its choices, then Restore), saving, done (what was restored); then any
## message and the purchases on this device, each with Copy hash. Close is pinned at the bottom
## and closes with `closed("close")`; so does `ui_cancel`.
class_name ScfgRestoreScreen
extends ScfgScreenBase

## The controller this screen shows.
var controller: ScfgRestoreController
var _synced: Variant = null  # the controller's tx_input last copied into the field


## Called by the sheet host before the screen enters the tree.
func bind(c: ScfgRestoreController, s: Variant) -> void:
	controller = c
	scfg = s


func _ready_to_show() -> bool:
	return controller != null


func _signature() -> Variant:
	var ctl := controller
	# the purchases on this device change only with receipts_changed: its count, not a copy of
	# every receipt each frame
	var owned_rev: Variant = scfg.get("_receipts_rev") if scfg != null else null
	return [ctl.state, ctl.error, ctl.candidates.size(), ctl.choices.duplicate(), ctl.can_confirm, ctl.added.size(),
		owned_rev, k]


func _moved(before: Variant, after: Variant) -> bool:
	return before == null or before[0] != after[0]


func _primary() -> String:
	var ctl := controller
	match ctl.state:
		"input", "checking":
			return "tx_input"
		"choose":
			for i: int in ctl.pending_choices():
				if not ctl.choices.has(i):
					return "choice_%d_%s" % [i, ctl.candidates[i]["choices"][0]]
			return "restore"
	return "close"


func _sync() -> void:
	var f := control("tx_input") as LineEdit
	if f != null and _synced != controller.tx_input:
		f.text = controller.tx_input
		f.caret_column = f.text.length()
		_synced = controller.tx_input


func _on_ui_cancel() -> void:
	close_with("close")


func _build() -> void:
	var ctl := controller
	_synced = null
	var body := scrolling_body(_layout)
	var mark := logo()
	if mark != null:
		body.add_child(mark)
	body.add_child(reg(label(tr("Restore purchases"), "ScfgTitle"), "title"))
	match ctl.state:
		"closed":
			body.add_child(label(tr("This screen is no longer active.")))
		"input", "checking":
			_input_part(body)
		"choose":
			_choose_part(body)
		"saving":
			body.add_child(status_row(tr("Saving..."), color("accent")))
		"done":
			if not ctl.added.is_empty():
				body.add_child(label(tr("Restored:"), "ScfgOk"))
				for r: Dictionary in ctl.added:
					body.add_child(label(_receipt_line(r)))
			else:
				body.add_child(label(tr("Nothing new to restore. These purchases are already on this device.")))
	var msg: Variant = ctl.message()
	if ScfgPy.py_truthy(msg):
		body.add_child(reg(label(tr(str(msg)), "ScfgError"), "message"))
	var owned: Array = scfg.receipts().values() if scfg != null else []
	if not owned.is_empty():
		body.add_child(label(tr("Purchases on this device"), "ScfgLabel"))
		for n in owned.size():
			var r: Dictionary = owned[n]
			var row := hbox(8)
			row.add_child(label(_receipt_line(r)))
			var copy := button("copy_%d" % n, tr("Copy hash"), "ScfgSmall", _copy.bind(str(r.get("tx_hash", ""))))
			copy.size_flags_horizontal = Control.SIZE_SHRINK_END
			copy.size_flags_vertical = Control.SIZE_SHRINK_CENTER
			copy.custom_minimum_size.x = px(110)
			row.add_child(copy)
			body.add_child(row)
	var footer := vbox(12)
	footer.add_child(rule())
	footer.add_child(button("close", tr("Close"), "", close_with.bind("close")))
	_layout.add_child(footer)


func _input_part(body: VBoxContainer) -> void:
	var ctl := controller
	body.add_child(label(tr("Paste the transaction hash or signature of your payment. You can find it in your wallet's activity, or in your exchange's withdrawal history."), "ScfgMuted"))
	var row := hbox(8)
	var f := field("tx_input", ctl.tx_input, TX_MAX_LENGTH, TX_CHARS, _typed, _check, "tx")
	f.editable = ctl.state == "input"
	row.add_child(f)
	_synced = ctl.tx_input
	if scfg != null and scfg.is_web():
		var paste := fixed_width(button("paste", tr("Paste"), "ScfgSmall", _paste, "tx"), 90)
		paste.disabled = ctl.state != "input"
		row.add_child(paste)
	body.add_child(row)
	var check := button("check", tr("Check"), "ScfgAccent", _check)
	check.disabled = ctl.state != "input"
	body.add_child(check)
	if ctl.state == "checking":
		body.add_child(reg(status_row(tr("Checking the blockchain..."), color("accent")), "status"))


func _choose_part(body: VBoxContainer) -> void:
	var ctl := controller
	for i: int in ctl.pending_choices():
		var c: Dictionary = ctl.candidates[i]
		body.add_child(label(_candidate_line(c)))
		var choices: Array = c["choices"]
		var grid := option_grid("choices_%d" % i, mini(choices.size(), 2))
		for j in choices.size():
			var choice: String = str(choices[j])
			grid.add_child(option("choice_%d_%s" % [i, choice], _choice_label(choice), "", ctl.choices.get(i) == choice,
				_choose.bind(i, choice), "choices_%d:%d" % [i, j / 2]))
		body.add_child(grid)
	var restore_button := button("restore", tr("Restore"), "ScfgPrimary", _confirm)
	restore_button.disabled = not ctl.can_confirm
	body.add_child(restore_button)


# --- texts -----------------------------------------------------------------------------------


func _receipt_line(r: Dictionary) -> String:
	var what := tr(scfg.item_title(r.get("item"))) if r.get("kind") == "unlock" else tr("Tip")
	return tr("%s: %s %s on %s") % [what, r.get("amount"), r.get("token"), scfg.chain_name(r.get("chain"))]


func _candidate_line(c: Dictionary) -> String:
	var amount: Variant = ScfgAmounts.format_units(c["transfer"]["value"])
	if amount is ScfgErr:
		_report(amount)
		amount = "?"
	return tr("Payment of %s %s on %s. What was it for?") % [amount, c["token"], scfg.chain_name(c["chain"])]


func _choice_label(choice: String) -> String:
	return tr("Keep as a tip") if choice == ScfgRestore.TIP_CHOICE else tr(scfg.item_title(choice))


# --- actions ---------------------------------------------------------------------------------


func _typed(text: String) -> void:
	controller.tx_input = text
	_synced = text


func _check() -> void:
	if controller.state == "input":
		controller.check()


func _choose(index: int, choice: String) -> void:
	controller.choose(index, choice)


func _confirm() -> void:
	controller.confirm()


func _copy(text: String) -> void:
	if scfg != null:
		scfg.copy(text)


func _paste() -> void:
	if scfg != null:
		scfg.paste_into(controller)
