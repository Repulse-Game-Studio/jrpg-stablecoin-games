## The addon's restore screen (addons/scfg/ui/restore.gd, shown by ScfgDefaultSheetHost): a
## keyboard-only restore that needs the player's choice, and closing it. Behaviour authority:
## plugin/game/scfg/scfg_screens.rpy (scfg_restore).
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")
const UI := preload("res://tests/ui_env.gd")


## A transfer of 5.00 USDC to the developer: a whole-cent amount carries no item code, so the
## player says what it paid for (chapter2, whose price it covers, or a tip).
static func _round_payment(env: Dictionary) -> void:
	var chain := F.FakeChain.new(1000)
	chain.receipts[UI.TX] = {"status": "0x1", "logs": [F.transfer_log(UI.USDC_ARB, UI.PAYER, UI.DEV, 5_000_000,
		{"block": 10, "tx_hash": UI.TX})]}
	(env["chains"] as Dictionary)["arbitrum"] = chain


func test_restore_choose_and_confirm() -> bool:
	var env := await UI.make("restore")
	var s: Node = env["node"]
	_round_payment(env)
	var called: Array = []
	var box := UI.restore(s, {"callback": func(r: ScfgRestoreResult) -> void: called.append(r)})
	await UI.frames(2)
	var screen: Node = UI.screen(s)
	T.ok(screen is ScfgRestoreScreen, "the default host shows the addon's restore screen (%s)" % [screen])
	if not screen is ScfgRestoreScreen:
		await UI.drop(env)
		return T.done()
	var layer := screen.get_parent()
	T.ok(layer is CanvasLayer and (layer as CanvasLayer).layer == 128, "under a CanvasLayer at layer 128")
	var ctl: ScfgRestoreController = screen.controller
	T.ok(UI.visible_texts(screen).has("Restore purchases"), "the title")
	T.ok(UI.focused() == screen.control("tx_input"), "the hash field has the focus (%s)" % [UI.focused()])
	await UI.type_text(UI.TX)
	T.eq(ctl.tx_input, UI.TX, "typed into the field")
	T.ok(await UI.focus_to(screen, "check"), "ui_down reaches Check")
	await UI.act("ui_accept")
	T.eq(ctl.state, "checking", "Check looks it up")
	T.ok(UI.visible_texts(screen).has("Checking the blockchain..."), "and says so")
	await UI.pass_(s)
	await UI.frames(2)
	T.eq(ctl.state, "choose", "one candidate to choose for")
	T.ok(UI.visible_texts(screen).has("Payment of 5.000000 USDC on Arbitrum One. What was it for?")
		or UI.visible_texts(screen).any(func(t: String) -> bool: return t.begins_with("Payment of 5.000000 USDC on ")),
		"the candidate is described (%s)" % [UI.visible_texts(screen)])
	var restore_button: Button = screen.control("restore")
	T.ok(restore_button != null and restore_button.disabled, "Restore waits for a choice")
	T.ok(screen.control("choice_0_tip") is Button, "Keep as a tip is offered")
	T.ok(await UI.focus_to(screen, "choice_0_chapter2"), "ui_down reaches Chapter 2")
	await UI.act("ui_accept")
	T.eq(ctl.choices, {0: "chapter2"}, "chosen, keyed by the int candidate index")
	restore_button = screen.control("restore")
	T.ok(restore_button != null and not restore_button.disabled, "Restore can be pressed now")
	T.ok(await UI.focus_to(screen, "restore"), "ui_down reaches Restore")
	await UI.act("ui_accept")
	T.eq(ctl.state, "saving", "Restore saves")
	await UI.pass_(s)
	await UI.frames(2)
	T.eq(ctl.state, "done", "restored")
	T.eq(called.size(), 1, "the callback got the result")
	T.eq(s.owns("chapter2"), true, "owned")
	var texts := UI.visible_texts(screen)
	T.ok(texts.has("Restored:") and texts.has("Purchases on this device"), "the restored and owned lists (%s)" % [texts])
	T.ok(screen.control("copy_0") is Button, "with a Copy hash button per receipt")
	T.ok(await UI.focus_to(screen, "close"), "ui_down reaches Close")
	await UI.act("ui_accept")
	var r: ScfgRestoreResult = box.get("result")
	T.eq([r.status, r.restored.size()] if r != null else null, ["restored", 1], "restore() resolves restored")
	await UI.frames(2)
	T.ok(not is_instance_valid(layer), "the screen and its layer are freed")
	await UI.drop(env)
	return T.done()


func test_restore_screen_shows_errors_and_closes_on_ui_cancel() -> bool:
	var env := await UI.make("restore_cancel")
	var s: Node = env["node"]
	var box := UI.restore(s)
	await UI.frames(2)
	var screen: Node = UI.screen(s)
	var ctl: ScfgRestoreController = screen.controller
	await UI.type_text("nothex")
	T.ok(await UI.focus_to(screen, "check"), "ui_down reaches Check")
	await UI.act("ui_accept")
	T.eq([ctl.state, ctl.error], ["input", "invalid_hash"], "not a hash")
	T.ok(UI.visible_texts(screen).has(ScfgRestoreController.MESSAGES["invalid_hash"]), "the message is shown")
	T.ok(screen.control("paste") == null, "no Paste button off the web (the field takes Ctrl+V)")
	await UI.act("ui_cancel")
	var r: ScfgRestoreResult = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "ui_cancel closes the restore screen")
	await UI.drop(env)
	return T.done()


## The real arrow keys (InputEventKey, which a focused LineEdit sees first, unlike the gamepad's
## action events) move out of the hash field and back into it, and the field takes typing again.
func test_real_arrow_keys_leave_and_reenter_the_hash_field() -> bool:
	var env := await UI.make("restore_keys")
	var s: Node = env["node"]
	var box := UI.restore(s)
	await UI.frames(2)
	var screen: Node = UI.screen(s)
	var ctl: ScfgRestoreController = screen.controller
	T.ok(UI.focused() == screen.control("tx_input"), "the hash field has the focus (%s)" % [UI.focused()])
	await UI.type_text("ab")
	T.eq(ctl.tx_input, "ab", "typed into the field")
	T.ok(await UI.key_to(screen, "check"), "KEY_DOWN leaves the field for Check (%s)" % [UI.focused()])
	T.ok(await UI.key_to(screen, "close"), "and goes on to Close (%s)" % [UI.focused()])
	T.ok(await UI.key_to(screen, "tx_input"), "KEY_UP comes back into the field (%s)" % [UI.focused()])
	await UI.type_text("cd")
	T.eq(ctl.tx_input, "abcd", "the field reached by the arrows takes typing")
	T.ok(await UI.key_to(screen, "check"), "and KEY_DOWN leaves it again (%s)" % [UI.focused()])
	await UI.act("ui_cancel")
	var r: ScfgRestoreResult = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "closed")
	await UI.drop(env)
	return T.done()


## A rebuild while the hash field has the focus (here: Enter on a bad hash shows the error) builds
## a new field with the same text. Its caret must be at the end of that text (LineEdit.text = ...
## leaves it at 0), or what the player types next would land in front of it.
func test_typing_goes_on_at_the_end_of_the_field_after_a_rebuild() -> bool:
	var env := await UI.make("restore_caret")
	var s: Node = env["node"]
	var box := UI.restore(s)
	await UI.frames(2)
	var screen: Node = UI.screen(s)
	var ctl: ScfgRestoreController = screen.controller
	await UI.type_text("0x12")
	var before: Control = screen.control("tx_input")
	await UI.key(KEY_ENTER)
	T.eq([ctl.state, ctl.error], ["input", "invalid_hash"], "Enter checked it: not a hash")
	var field := screen.control("tx_input") as LineEdit
	T.ok(field != null and field != before, "the screen rebuilt the field")
	T.ok(UI.focused() == field, "the new field keeps the focus (%s)" % [UI.focused()])
	T.eq(field.caret_column if field != null else -1, 4, "its caret is at the end of the text")
	await UI.type_text("34")
	T.eq(ctl.tx_input, "0x1234", "typing goes on at the end")
	await UI.act("ui_cancel")
	var r: ScfgRestoreResult = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "closed")
	await UI.drop(env)
	return T.done()


## Stands in for the autoload in _signature(): counts receipts() calls.
class CountingScfg:
	extends RefCounted
	var calls := 0
	var _receipts_rev := 0

	func receipts() -> Dictionary:
		calls += 1
		return {}


## The screen checks each frame whether it must rebuild: that check must not copy every receipt
## (Scfg.receipts() is a deep copy). It compares the autoload's receipts_changed count instead,
## and a receipt that arrives while the screen is open still shows up.
func test_the_owned_list_follows_receipts_without_copying_them_each_frame() -> bool:
	var env := await UI.make("restore_rev")
	var s: Node = env["node"]
	var box := UI.restore(s)
	await UI.frames(2)
	var screen: Node = UI.screen(s)
	T.ok(screen.control("copy_0") == null, "no purchases on this device yet")
	var counting := CountingScfg.new()
	screen.scfg = counting
	var sig: Variant = screen._signature()
	T.eq(counting.calls, 0, "the per-frame check does not call receipts()")
	counting._receipts_rev += 1
	T.ok(screen._signature() != sig, "a receipts_changed makes the screen rebuild")
	screen.scfg = s
	var receipt := {"chain": "arbitrum", "tx_hash": UI.TX, "log_index": 3, "block": 990, "kind": "unlock",
		"item": "chapter2", "amount": "4.99", "token": "USDC", "payer": UI.PAYER.to_lower(),
		"recipient": UI.DEV.to_lower(), "source": "payment"}
	(s._worker as ScfgWorker).submit(func(e: ScfgEngine) -> Variant: return ScfgReceipts.add_receipt(e.receipts, receipt))
	await UI.pass_(s)
	await UI.frames(2)
	T.ok(screen.control("copy_0") is Button, "a receipt that arrives while the screen is open is listed")
	await UI.act("ui_cancel")
	T.eq((box.get("result") as ScfgRestoreResult).status if box.get("result") != null else null, "cancelled", "closed")
	await UI.drop(env)
	return T.done()


## A screen's CanvasLayer frees itself once the screen is gone, also while the game is paused
## (games pause behind the screens; the layer runs PROCESS_MODE_ALWAYS like them).
func test_an_empty_layer_is_freed_while_the_game_is_paused() -> bool:
	var env := await UI.make("restore_paused")
	var s: Node = env["node"]
	UI.tree().paused = true
	var box := UI.restore(s)
	await UI.frames(2)
	var screen: Node = UI.screen(s)
	var layer: Node = screen.get_parent() if screen != null else null
	T.ok(layer is CanvasLayer, "the screen is up while paused")
	screen.queue_free()  # freed without closing: the restore counts as cancelled
	await UI.frames(4)
	T.eq((box.get("result") as ScfgRestoreResult).status if box.get("result") != null else null, "cancelled",
		"the freed screen counts as closed")
	T.ok(not is_instance_valid(layer), "its empty layer is freed too, though the game is paused")
	UI.tree().paused = false
	await UI.drop(env)
	return T.done()


## A game may bind letter keys to ui_up / ui_down (WASD menus). Those letters are hash and
## signature characters: typed into the hash field they stay text, and the focus stays put (only
## the arrow keys themselves leave a field).
func test_letters_bound_to_ui_up_and_down_are_typed_into_the_hash_field() -> bool:
	var added := UI.bind_letters()
	var env := await UI.make("restore_wasd")
	var s: Node = env["node"]
	var box := UI.restore(s)
	await UI.frames(2)
	var screen: Node = UI.screen(s)
	var ctl: ScfgRestoreController = screen.controller
	var field: Control = screen.control("tx_input")
	T.ok(UI.focused() == field, "the hash field has the focus (%s)" % [UI.focused()])
	await UI.type_text("aswd")
	T.eq(ctl.tx_input, "aswd", "W and S are typed, not taken as ui_up / ui_down")
	T.ok(UI.focused() == field, "and the focus stays in the field (%s)" % [UI.focused()])
	T.ok(await UI.key_to(screen, "check"), "the real KEY_DOWN still leaves it (%s)" % [UI.focused()])
	await UI.act("ui_cancel")
	T.eq((box.get("result") as ScfgRestoreResult).status if box.get("result") != null else null, "cancelled", "closed")
	await UI.drop(env)
	UI.unbind_letters(added)
	return T.done()
