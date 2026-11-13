## The addon's payment sheet (addons/scfg/ui/sheet.gd, shown by ScfgDefaultSheetHost): the steps
## it shows follow the checkout controller, the back arrow, keyboard / gamepad focus, the QR code,
## replacing the sheet (`sheet_scene`) or its theme, and that every static text it shows is
## translatable (in addons/scfg/translations/scfg.pot). Behaviour authority:
## plugin/game/scfg/scfg_screens.rpy (scfg_payment and its step screens).
##
## The screens run in the real main loop here: the sheet rebuilds itself in _process when the
## controller changes, and input goes through Input.parse_input_event (see ui_env.gd).
extends RefCounted

const T := preload("res://tests/t.gd")
const UI := preload("res://tests/ui_env.gd")
const POT := "res://addons/scfg/translations/scfg.pot"


## Opens an unlock payment and returns [box, sheet, controller] once the sheet is up.
func _open_unlock(s: Node) -> Array:
	var box := UI.pay(s, "unlock", {"item": "chapter2"})
	await UI.frames(2)
	var sheet: Node = UI.screen(s)
	var ctl: ScfgCheckoutController = sheet.controller if sheet != null else null
	return [box, sheet, ctl]


## Picks Arbitrum (the session starts on the next worker pass), then `method`.
func _to_method_step(s: Node, sheet: Node, method: String) -> void:
	(sheet.control("network_arbitrum") as Button).pressed.emit()
	await UI.pass_(s)
	await UI.frames(2)
	T.eq(sheet.shown_step, "method", "the method step once the session started")
	var b: Button = sheet.control("method_" + method)
	T.ok(b != null, "the %s method is offered" % method)
	if b != null:
		b.pressed.emit()
	await UI.frames(2)


func test_steps_follow_the_controller() -> bool:
	var env := await UI.make("steps")
	var s: Node = env["node"]
	var box := UI.pay(s, "tip", {"amount": "3.00", "token": "USDC"})
	await UI.frames(2)
	var sheet: Node = UI.screen(s)
	T.ok(sheet is ScfgSheet, "the default host shows the addon's sheet (%s)" % [sheet])
	if not sheet is ScfgSheet:
		await UI.drop(env)
		return T.done()
	var layer := sheet.get_parent()
	T.ok(layer is CanvasLayer and (layer as CanvasLayer).layer == 128, "under a CanvasLayer at layer 128")
	var ctl: ScfgCheckoutController = sheet.controller
	T.ok(is_same(ctl, s._checkouts.values()[0]), "bound to the open checkout")
	# amount
	T.eq([ctl.step, sheet.shown_step], ["amount", "amount"], "a tip starts on the amount step")
	T.eq((sheet.control("forward") as Button).text, "Next", "pinned forward button: Next")
	T.eq((sheet.control("back") as Control).visible, false, "no back arrow on the first step")
	T.eq(sheet.dots(), ctl.step_dots(), "the step dots")
	T.ok(sheet.control("preset_3.00") != null and sheet.control("custom_amount") is LineEdit, "presets and an amount field")
	(sheet.control("preset_5.00") as Button).pressed.emit()
	await UI.frames(2)
	T.eq([ctl.price_text, (sheet.control("price") as Label).text], ["5.00", "5.00"], "a preset changes the price in the header")
	(sheet.control("forward") as Button).pressed.emit()
	await UI.frames(2)
	# network
	T.eq([ctl.step, sheet.shown_step], ["network", "network"], "Next: the network step")
	T.eq((sheet.control("back") as Control).visible, true, "the back arrow")
	T.ok(not sheet.control("forward").visible, "no forward button on the network step")
	for option: Array in ctl.chain_options():
		T.ok(sheet.control("network_" + str(option[0])) is Button, "an option for %s" % option[0])
	await _to_method_step(s, sheet, "manual")
	# manual
	T.eq([ctl.step, sheet.shown_step], ["manual", "manual"], "the manual transfer step")
	var texts := UI.visible_texts(sheet)
	T.ok(texts.has(ctl.recipient), "shows the address (%s)" % [texts])
	T.ok(texts.has("%s %s" % [ctl.pay_amount_text, "USDC"]), "and the exact amount")
	T.ok(texts.has(ctl.token_address), "and the token contract")
	T.eq((sheet.control("forward") as Button).text, "I've sent it", "forward: I've sent it")
	T.eq(sheet.dots(), ctl.step_dots(), "the step dots follow")
	(sheet.control("cancel") as Button).pressed.emit()
	await UI.frames(1)
	var r: ScfgPaymentResult = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "Cancel closes the sheet")
	await UI.frames(2)
	T.ok(not is_instance_valid(sheet) and not is_instance_valid(layer), "the sheet and its layer are freed")
	await UI.drop(env)
	return T.done()


func test_back_arrow_hidden_once_tx_seen() -> bool:
	var env := await UI.make("back")
	var s: Node = env["node"]
	var opened := await _open_unlock(s)
	var sheet: Node = opened[1]
	var ctl: ScfgCheckoutController = opened[2]
	await _to_method_step(s, sheet, "manual")
	T.eq((sheet.control("back") as Control).visible, true, "back arrow on the manual step")
	(sheet.control("back") as Button).pressed.emit()
	await UI.frames(2)
	T.eq([ctl.step, sheet.shown_step], ["method", "method"], "the back arrow goes back")
	(sheet.control("method_manual") as Button).pressed.emit()
	await UI.frames(2)
	await UI.act("ui_cancel")
	T.eq(sheet.shown_step, "method", "ui_cancel is back too")
	(sheet.control("method_manual") as Button).pressed.emit()
	await UI.frames(2)
	var field: LineEdit = sheet.control("tx_input")
	T.ok(field != null, "a field for the transaction hash")
	field.text = UI.TX
	field.text_changed.emit(UI.TX)
	(sheet.control("check") as Button).pressed.emit()
	await UI.frames(2)
	T.eq([ctl.hint_tx, ctl.tx_seen], [UI.TX, true], "the hash is reported")
	T.eq(sheet.shown_step, "waiting", "the waiting step")
	T.eq((sheet.control("back") as Control).visible, false, "no back arrow once a transaction is seen")
	T.ok(UI.visible_texts(sheet).has(UI.TX), "the hash is shown")
	await UI.act("ui_cancel")
	T.eq([ctl.step, is_instance_valid(sheet) and sheet.is_inside_tree()], ["waiting", true],
		"ui_cancel neither goes back nor closes now")
	await UI.drop(env)
	return T.done()


func test_keyboard_only_manual_payment() -> bool:
	var env := await UI.make("keyboard")
	var s: Node = env["node"]
	var opened := await _open_unlock(s)
	var box: Dictionary = opened[0]
	var sheet: Node = opened[1]
	var ctl: ScfgCheckoutController = opened[2]
	T.eq(sheet.shown_step, "network", "the network step")
	var first: Control = UI.focused()
	T.ok(first != null and sheet.is_ancestor_of(first), "the sheet has the focus (%s)" % [first])
	T.ok(await UI.focus_to(sheet, "network_arbitrum"), "ui_down reaches Arbitrum")
	await UI.act("ui_accept")
	await UI.pass_(s)
	await UI.frames(2)
	T.eq([ctl.chain_key, sheet.shown_step], ["arbitrum", "method"], "ui_accept chose it")
	T.ok(sheet.is_ancestor_of(UI.focused()), "the method step has the focus")
	T.ok(await UI.focus_to(sheet, "method_manual"), "ui_down reaches Manual transfer")
	await UI.act("ui_accept")
	T.eq(sheet.shown_step, "manual", "the manual step")
	T.ok(UI.focused() == sheet.control("copy_amount"), "focus on its first control, Copy (%s)" % [UI.focused()])
	await UI.act("ui_accept")
	var platform: Variant = env["platform"]
	T.eq(platform.clipboard, ctl.pay_amount_text, "ui_accept on Copy copies the amount")
	T.ok(await UI.focus_to(sheet, "forward"), "ui_down reaches I've sent it")
	await UI.act("ui_accept")
	T.eq(sheet.shown_step, "waiting", "the waiting step")
	await UI.pay_on_chain(env, ctl)
	await UI.frames(2)
	T.eq([ctl.state, sheet.shown_step], ["paid", "done"], "paid: the done step")
	T.ok(UI.focused() == sheet.control("continue"), "focus on Continue (%s)" % [UI.focused()])
	await UI.act("ui_accept")
	var r: ScfgPaymentResult = box.get("result")
	T.eq([r.status, r.tx_hash] if r != null else null, ["paid", UI.TX], "pay() resolves paid")
	T.eq(s.owns("chapter2"), true, "owned")
	# ui_cancel on the first step closes the sheet
	box = UI.pay(s, "tip", {"amount": "3.00", "token": "USDC"})
	await UI.frames(2)
	await UI.act("ui_cancel")
	r = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "ui_cancel on the first step closes as Cancel")
	await UI.drop(env)
	return T.done()


## The real arrow keys (InputEventKey, which a focused LineEdit sees first, unlike the gamepad's
## action events) move through the tip-amount field and the manual step's hash field, and a field
## reached by the arrows takes typing.
func test_real_arrow_keys_move_through_the_fields() -> bool:
	var env := await UI.make("keys")
	var s: Node = env["node"]
	var box := UI.pay(s, "tip", {"amount": "3.00", "token": "USDC"})
	await UI.frames(2)
	var sheet: Node = UI.screen(s)
	var ctl: ScfgCheckoutController = sheet.controller
	T.eq(sheet.shown_step, "amount", "the amount step")
	T.ok(await UI.key_to(sheet, "custom_amount"), "KEY_DOWN reaches the amount field (%s)" % [UI.focused()])
	await UI.type_text("7")
	T.eq(ctl.custom_amount, "7", "the field reached by the arrows takes typing")
	T.ok(await UI.key_to(sheet, "forward"), "KEY_DOWN leaves the field for Next (%s)" % [UI.focused()])
	T.ok(await UI.key_to(sheet, "custom_amount"), "KEY_UP comes back into it (%s)" % [UI.focused()])
	await UI.type_text("5")
	T.eq(ctl.custom_amount, "75", "and it takes typing again")
	T.ok(await UI.key_to(sheet, "preset_3.00"), "KEY_UP leaves the field for the presets (%s)" % [UI.focused()])
	(sheet.control("preset_3.00") as Button).pressed.emit()
	await UI.frames(2)
	(sheet.control("forward") as Button).pressed.emit()
	await UI.frames(2)
	await _to_method_step(s, sheet, "manual")
	T.eq(sheet.shown_step, "manual", "the manual step")
	T.ok(await UI.key_to(sheet, "tx_input"), "KEY_DOWN reaches the hash field (%s)" % [UI.focused()])
	await UI.type_text("ab")
	T.eq(ctl.tx_input, "ab", "the field reached by the arrows takes typing")
	T.ok(await UI.key_to(sheet, "check"), "KEY_DOWN leaves the field for Check (%s)" % [UI.focused()])
	T.ok(await UI.key_to(sheet, "forward"), "and goes on to I've sent it (%s)" % [UI.focused()])
	T.ok(await UI.key_to(sheet, "tx_input"), "KEY_UP comes back into the field (%s)" % [UI.focused()])
	await UI.type_text("cd")
	T.eq(ctl.tx_input, "abcd", "and it takes typing again")
	T.ok(await UI.key_to(sheet, "copy_token"), "KEY_UP leaves it for Copy (%s)" % [UI.focused()])
	(sheet.control("cancel") as Button).pressed.emit()
	await UI.frames(2)
	var r: ScfgPaymentResult = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "closed")
	await UI.drop(env)
	return T.done()


## A game may bind letter keys to ui_up / ui_down (WASD menus). Those letters are hash and
## signature characters: typed into the manual step's hash field they stay text, and the focus
## stays put.
func test_letters_bound_to_ui_up_and_down_are_typed_into_the_manual_hash_field() -> bool:
	var added := UI.bind_letters()
	var env := await UI.make("wasd")
	var s: Node = env["node"]
	var opened := await _open_unlock(s)
	var sheet: Node = opened[1]
	var ctl: ScfgCheckoutController = opened[2]
	await _to_method_step(s, sheet, "manual")
	var field: LineEdit = sheet.control("tx_input")
	field.grab_focus()
	await UI.frames(1)
	await UI.type_text("aswd")
	T.eq(ctl.tx_input, "aswd", "W and S are typed, not taken as ui_up / ui_down")
	T.ok(UI.focused() == sheet.control("tx_input"), "and the focus stays in the field (%s)" % [UI.focused()])
	T.ok(await UI.key_to(sheet, "check"), "the real KEY_DOWN still leaves it (%s)" % [UI.focused()])
	(sheet.control("cancel") as Button).pressed.emit()
	await UI.frames(2)
	await UI.drop(env)
	UI.unbind_letters(added)
	return T.done()


func test_focus_neighbours_stay_in_the_sheet() -> bool:
	var env := await UI.make("focus")
	var s: Node = env["node"]
	var game := Button.new()  # a game button behind the sheet
	game.text = "game"
	UI.tree().root.add_child(game)
	game.grab_focus()
	var opened := await _open_unlock(s)
	var sheet: Node = opened[1]
	T.ok(sheet.is_ancestor_of(UI.focused()), "the sheet took the focus")
	for action: String in ["ui_up", "ui_up", "ui_left", "ui_right", "ui_down", "ui_down", "ui_down", "ui_down", "ui_down"]:
		await UI.act(action)
		T.ok(sheet.is_ancestor_of(UI.focused()), "%s keeps the focus in the sheet (%s)" % [action, UI.focused()])
	for c: Control in sheet.focus_order():
		for side: StringName in [&"focus_neighbor_top", &"focus_neighbor_bottom", &"focus_next", &"focus_previous"]:
			var path: NodePath = c.get(side)
			var target: Node = c.get_node_or_null(path) if not path.is_empty() else null
			T.ok(target != null and (target == c or sheet.is_ancestor_of(target)),
				"%s.%s is set inside the sheet" % [c.name, side])
	(sheet.control("cancel") as Button).pressed.emit()
	await UI.frames(2)
	T.ok(UI.focused() == game, "the focus goes back to the game once the sheet closes")
	game.queue_free()
	await UI.drop(env)
	return T.done()


func test_qr_matches_core_matrix() -> bool:
	var env := await UI.make("qr")
	var s: Node = env["node"]
	var opened := await _open_unlock(s)
	var sheet: Node = opened[1]
	var ctl: ScfgCheckoutController = opened[2]
	await _to_method_step(s, sheet, "qr")
	T.eq(sheet.shown_step, "qr", "the phone wallet step")
	var qr: Variant = sheet.control("qr")
	T.ok(qr is ScfgQrView, "a QR view (%s)" % [qr])
	if not qr is ScfgQrView:
		await UI.drop(env)
		return T.done()
	var view: ScfgQrView = qr
	var uri: Variant = ctl.payment_uri()
	var matrix: Array = ScfgPaylinks.qr_matrix(uri)
	var n := matrix.size()
	var scale := view.module_scale()
	var side := (n + 4) * scale
	T.ok(scale >= 1, "an integer scale")
	T.eq(scale, maxi(1, view.target_size / (n + 4)), "the largest integer scale that fits")
	var img := view.render_image()
	T.eq([img.get_width(), img.get_height()], [side, side], "modules plus a quiet zone of 2 on each side")
	T.eq(view.custom_minimum_size, Vector2(side, side), "the view is that size")
	var wrong := 0
	for y in n + 4:
		for x in n + 4:
			var dark: bool = x >= 2 and y >= 2 and x < n + 2 and y < n + 2 and matrix[y - 2][x - 2]
			for p: Vector2i in [Vector2i(x * scale, y * scale), Vector2i(x * scale + scale - 1, y * scale + scale - 1)]:
				var c := img.get_pixelv(p)
				if c != (Color.BLACK if dark else Color.WHITE):
					wrong += 1
	T.eq(wrong, 0, "every module's pixels match the core's matrix, black on white")
	T.ok(UI.visible_texts(sheet).has("1. Open your wallet app"), "with the steps beside it")
	await UI.drop(env)
	return T.done()


## A replacement sheet scene: its root records bind(controller, scfg).
static func _replacement_scene(with_bind: bool) -> PackedScene:
	var gd := GDScript.new()
	gd.source_code = "extends Control\nsignal closed(how: String)\nvar bound: Array = []\n"
	if with_bind:
		gd.source_code += "func bind(controller: Variant, scfg: Variant) -> void:\n\tbound = [controller, scfg, is_inside_tree()]\n"
	gd.reload()
	var root := Control.new()
	root.set_script(gd)
	var packed := PackedScene.new()
	packed.pack(root)
	root.free()
	return packed


func test_replacement_sheet_scene_is_bound() -> bool:
	var env := await UI.make("replace", {"sheet_scene": _replacement_scene(true)})
	var s: Node = env["node"]
	var box := UI.pay(s, "unlock", {"item": "chapter2"})
	await UI.frames(2)
	var screen: Node = UI.screen(s)
	T.ok(screen != null and not screen is ScfgSheet, "the replacement is shown instead of the sheet")
	if screen == null:
		await UI.drop(env)
		return T.done()
	var bound: Array = screen.get("bound")
	T.ok(bound.size() == 3 and is_same(bound[0], s._checkouts.values()[0]) and is_same(bound[1], s),
		"bind(controller, scfg) got the checkout controller and the autoload")
	T.eq(bound[2] if bound.size() == 3 else null, false, "bind() runs before the scene enters the tree")
	var layer := screen.get_parent()
	T.ok(layer is CanvasLayer and (layer as CanvasLayer).layer == 128, "on the CanvasLayer at 128")
	screen.emit_signal("closed", "cancel")
	var r: ScfgPaymentResult = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "its closed(how) ends pay()")
	await UI.frames(2)
	T.ok(not is_instance_valid(layer), "and it is removed")
	await UI.drop(env)
	# a scene whose root has no bind(): no screen, reported
	env = await UI.make("replace_bad", {"sheet_scene": _replacement_scene(false)})
	s = env["node"]
	r = await s.pay("unlock", {"item": "chapter2"})
	T.eq([r.status, r.error], ["failed", "sheet.unavailable"], "a root without bind() is not shown")
	T.ok(s._reported.any(func(line: String) -> bool: return line.contains("bind(controller, scfg)")),
		"and the reason is reported (%s)" % [s._reported])
	await UI.drop(env)
	# configure() type checks of the glue-only keys
	s = UI.SCFG.new()
	s._setup({"store_path": UI.DIR.path_join("types.json"), "quiet": true})
	UI.tree().root.add_child(s)
	T.is_err(s.configure({"wallet": UI.DEV, "theme": "dark"}), "TypeError", "theme must be a Theme")
	T.is_err(s.configure({"wallet": UI.DEV, "sheet_scene": Theme.new()}), "TypeError", "sheet_scene must be a PackedScene")
	T.eq(s._reported.size(), 2, "both reported")
	T.eq(s._config, null, "and not configured")
	T.eq(s.configure({"wallet": UI.DEV, "theme": null, "sheet_scene": null}), null, "null is no override")
	s.queue_free()
	await UI.frames(1)
	return T.done()


func test_theme_override_merges_into_the_margin_theme() -> bool:
	var custom := Theme.new()
	custom.set_color("font_color", "ScfgTitle", Color.RED)
	var env := await UI.make("theme", {"theme": custom, "accent": "#ffcc00"})
	var s: Node = env["node"]
	var opened := await _open_unlock(s)
	var sheet: Node = opened[1]
	var title: Label = sheet.control("title")
	T.eq(title.get_theme_color("font_color"), Color.RED, "the developer's color for the title")
	var forward_box := (sheet.theme as Theme).get_stylebox("normal", "ScfgAccent") as StyleBoxFlat
	T.ok(forward_box != null and forward_box.bg_color == Color("#ffcc00"), "the accent from configure")
	T.eq((sheet.theme as Theme).get_color("font_color", "ScfgAccent"), Color(ScfgConfig.text_on("#ffcc00")),
		"text on the accent from ScfgConfig.text_on")
	T.ok(forward_box != null and forward_box.corner_radius_top_left > 0, "rounded corners")
	T.eq(custom.get_color("font_color", "ScfgTitle"), Color.RED, "the developer's Theme is not changed")
	T.ok(not custom.has_stylebox("normal", "ScfgAccent"), "nor filled in")
	await UI.drop(env)
	return T.done()


# --- translations ----------------------------------------------------------------------------


## The msgids of scfg.pot.
static func _pot_ids() -> Array[String]:
	var ids: Array[String] = []
	var text := FileAccess.get_file_as_string(POT)
	for line: String in text.split("\n"):
		if line.begins_with("msgid \"") and line != "msgid \"\"":
			ids.append(line.substr(7, line.length() - 8).c_unescape())
	return ids


## Texts that are data rather than the sheet's own words: amounts, addresses, chain names, the
## developer's titles, and the debug-only developer lines.
static func _dynamic(ctl: ScfgCheckoutController) -> Array:
	var out := [ctl.price_text, ctl.spec["token"], ctl.spec["title"], ctl.recipient, ctl.token_address, UI.TX]
	if ctl.session != null:
		out.append("%s %s" % [ctl.pay_amount_text, ctl.spec["token"]])
	for option: Array in ctl.chain_options():
		out.append(option[1])
	for preset: ScfgDec in ctl.tip_presets():
		out.append("%s %s" % [str(preset).trim_suffix(".00"), ctl.spec["token"]])
	return out


## Fails for every visible text that did not come through tr() (the pseudo locale marks those as
## «text»), other than data. A tr() string missing from scfg.pot is not marked, so it fails too.
static func _check_texts(screen: Node, dynamic: Array, where: String) -> void:
	for text: String in UI.visible_texts(screen):
		var marked := text.begins_with("«") and text.ends_with("»")
		var data := dynamic.has(text) or text.begins_with("DEV MODE: ")
		T.ok(marked or data, "%s: %s is translatable (in scfg.pot)" % [where, JSON.stringify(text)])


func test_strings_are_translatable() -> bool:
	var ids := _pot_ids()
	T.ok(ids.has("I've sent it") and ids.has("Restore purchases") and ids.has(ScfgCheckoutController.MESSAGES["invalid_hash"]),
		"scfg.pot has the sheet's strings and the core MESSAGES (%d ids)" % ids.size())
	var pseudo := Translation.new()
	pseudo.locale = "eo"
	for id: String in ids:
		pseudo.add_message(id, "«%s»" % id)
	var locale := TranslationServer.get_locale()
	TranslationServer.add_translation(pseudo)
	TranslationServer.set_locale("eo")
	await _visit_every_step()
	TranslationServer.set_locale(locale)
	TranslationServer.remove_translation(pseudo)
	return T.done()


func _visit_every_step() -> void:
	var env := await UI.make("strings")
	var s: Node = env["node"]
	var box := UI.pay(s, "tip", {"amount": "3.00", "token": "USDC"})
	await UI.frames(2)
	var sheet: Node = UI.screen(s)
	var ctl: ScfgCheckoutController = sheet.controller
	_check_texts(sheet, _dynamic(ctl), "amount")
	ctl.error = "invalid_amount"
	await UI.frames(2)
	T.ok(UI.visible_texts(sheet).has("«%s»" % ScfgCheckoutController.MESSAGES["invalid_amount"]),
		"a core message is translated at display time")
	_check_texts(sheet, _dynamic(ctl), "amount with an error")
	(sheet.control("forward") as Button).pressed.emit()
	await UI.frames(2)
	_check_texts(sheet, _dynamic(ctl), "network")
	(sheet.control("network_arbitrum") as Button).pressed.emit()
	await UI.pass_(s)
	await UI.frames(2)
	_check_texts(sheet, _dynamic(ctl), "method")
	for method: String in ["browser", "qr", "manual"]:
		(sheet.control("method_" + method) as Button).pressed.emit()
		await UI.frames(2)
		T.eq(sheet.shown_step, method, "the %s step" % method)
		_check_texts(sheet, _dynamic(ctl), method)
		(sheet.control("back") as Button).pressed.emit()
		await UI.frames(2)
	(sheet.control("method_manual") as Button).pressed.emit()
	await UI.frames(2)
	(sheet.control("forward") as Button).pressed.emit()
	await UI.frames(2)
	_check_texts(sheet, _dynamic(ctl), "waiting")
	ctl.hint_tx = UI.TX  # as a reported hash shows it (no lookup: the payment below confirms by scan)
	await UI.frames(2)
	_check_texts(sheet, _dynamic(ctl), "waiting with a hash")
	ctl.hint_tx = null
	await UI.pay_on_chain(env, ctl)
	await UI.frames(2)
	T.eq(sheet.shown_step, "done", "the done step")
	_check_texts(sheet, _dynamic(ctl), "done")
	(sheet.control("continue") as Button).pressed.emit()
	await UI.frames(1)
	T.ok(box.has("result"), "closed")
	# the restore screen
	var rbox := UI.restore(s)
	await UI.frames(2)
	var screen: Node = UI.screen(s)
	var dynamic := [UI.TX]
	for r: Dictionary in s.receipts().values():
		dynamic.append(r["tx_hash"])
	_check_texts(screen, dynamic, "restore")
	(screen.controller as ScfgRestoreController).error = "tx_not_found"
	await UI.frames(2)
	T.ok(UI.visible_texts(screen).has("«%s»" % ScfgRestoreController.MESSAGES["tx_not_found"]),
		"a restore message is translated at display time")
	_check_texts(screen, dynamic, "restore with an error")
	(screen.control("close") as Button).pressed.emit()
	await UI.frames(1)
	T.ok(rbox.has("result"), "closed")
	await UI.drop(env)


## pay() and restore() at once: the restore screen opens over the payment sheet and takes the
## focus. A rebuild of the sheet below (its session starts) leaves the focus on the screen on top;
## when that closes, the focus goes back to the sheet, although the control it had was rebuilt.
func test_a_rebuild_below_never_takes_the_focus_from_a_screen_on_top() -> bool:
	var env := await UI.make("stacked")
	var s: Node = env["node"]
	var opened := await _open_unlock(s)
	var sheet: Node = opened[1]
	var ctl: ScfgCheckoutController = opened[2]
	T.ok(sheet is ScfgSheet and sheet.is_ancestor_of(UI.focused()), "the sheet has the focus (%s)" % [UI.focused()])
	var restoring := UI.restore(s)
	await UI.frames(2)
	var top: Node = UI.screen(s)
	T.ok(top is ScfgRestoreScreen and top != sheet, "the restore screen opens over the sheet (%s)" % [top])
	if not top is ScfgRestoreScreen or not sheet is ScfgSheet:
		await UI.drop(env)
		return T.done()
	T.ok(UI.focused() == top.control("tx_input"), "the screen on top has the focus (%s)" % [UI.focused()])
	T.eq(ctl.choose_network("arbitrum"), null, "the payment moves on below")
	await UI.pass_(s)
	await UI.frames(3)
	T.eq(sheet.shown_step, "method", "the sheet rebuilt for the method step")
	T.ok(UI.focused() == top.control("tx_input"), "the focus stayed on the screen on top (%s)" % [UI.focused()])
	await UI.type_text("ab")
	T.eq((top.controller as ScfgRestoreController).tx_input, "ab", "and typing still goes there")
	await UI.act("ui_cancel")
	var r: ScfgRestoreResult = restoring.get("result")
	T.eq(r.status if r != null else null, "cancelled", "ui_cancel closes the screen on top only")
	T.eq(s._checkouts.size(), 1, "the payment is still open")
	await UI.frames(2)
	T.ok(UI.focused() != null and sheet.is_ancestor_of(UI.focused()), "the focus is back on the sheet (%s)" % [UI.focused()])
	await UI.drop(env)
	return T.done()


## A screen below that goes away while another is open over it (the game closes the payment, a
## host frees the sheet) must leave the focus on the screen on top: giving it back to the game's
## control would let the keyboard press the game's buttons behind an open screen. The screen on
## top then gives the focus back to the game when it closes itself.
func test_a_screen_closing_below_leaves_the_focus_on_the_screen_on_top() -> bool:
	var game_button := Button.new()
	game_button.text = "game"
	UI.tree().root.add_child(game_button)
	game_button.grab_focus()
	var env := await UI.make("stacked_close")
	var s: Node = env["node"]
	var opened := await _open_unlock(s)
	var box: Dictionary = opened[0]
	var sheet: Node = opened[1]
	var restoring := UI.restore(s)
	await UI.frames(2)
	var top: Node = UI.screen(s)
	T.ok(top is ScfgRestoreScreen and sheet is ScfgSheet, "the restore screen is open over the sheet (%s)" % [top])
	if not top is ScfgRestoreScreen or not sheet is ScfgSheet:
		game_button.queue_free()
		await UI.drop(env)
		return T.done()
	sheet.close_with("cancel")
	await UI.frames(3)
	var r: ScfgPaymentResult = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "the payment below was closed")
	T.ok(not is_instance_valid(sheet) or not sheet.is_inside_tree(), "and its sheet is gone")
	T.ok(UI.focused() == top.control("tx_input"), "the focus stayed on the screen on top (%s)" % [UI.focused()])
	await UI.type_text("ab")
	T.eq((top.controller as ScfgRestoreController).tx_input, "ab", "and typing still goes there")
	await UI.act("ui_cancel")
	var rr: ScfgRestoreResult = restoring.get("result")
	T.eq(rr.status if rr != null else null, "cancelled", "the screen on top closes")
	await UI.frames(2)
	T.ok(UI.focused() == game_button, "then the game's control has the focus again (%s)" % [UI.focused()])
	game_button.queue_free()
	await UI.drop(env)
	return T.done()


## A rebuild focuses a control before the new build is laid out, so the body's follow_focus has
## nothing to scroll to yet. When the body no longer fits (here: the error line of a bad hash
## takes room from it, in a low window), the focused field must still be brought into view.
func test_the_focused_field_is_scrolled_into_view_after_a_rebuild() -> bool:
	var root := UI.tree().root
	var size_before := root.size
	root.size = Vector2i(1280, 560)
	var env := await UI.make("reveal")
	var s: Node = env["node"]
	var opened := await _open_unlock(s)
	var sheet: Node = opened[1]
	var ctl: ScfgCheckoutController = opened[2]
	await _to_method_step(s, sheet, "manual")
	(sheet.control("tx_input") as Control).grab_focus()
	await UI.frames(2)
	await UI.type_text("0x12")
	await UI.key(KEY_ENTER)
	await UI.frames(3)
	T.eq(ctl.error, "invalid_hash", "Enter checked it: not a hash")
	var field := sheet.control("tx_input") as Control
	var scroll := sheet.find_child("Scroll", true, false) as ScrollContainer
	T.ok(field != null and UI.focused() == field, "the rebuilt field has the focus (%s)" % [UI.focused()])
	T.ok(scroll != null, "the body scrolls")
	if field != null and scroll != null:
		var body := scroll.get_child(0) as Control
		T.ok(body.size.y > scroll.size.y, "the body does not fit (%s > %s)" % [body.size.y, scroll.size.y])
		var view := scroll.get_global_rect().grow(1.0)
		T.ok(view.encloses(field.get_global_rect()),
			"the focused field is inside the visible part of the body (%s in %s)" % [field.get_global_rect(), view])
	(sheet.control("cancel") as Button).pressed.emit()
	await UI.frames(2)
	await UI.drop(env)
	root.size = size_before
	await UI.frames(1)
	return T.done()
