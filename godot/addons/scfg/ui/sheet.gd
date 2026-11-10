## The payment sheet: the Godot twin of the `scfg_payment` screen in
## plugin/game/scfg/scfg_screens.rpy, shown by ScfgDefaultSheetHost for `await Scfg.pay(...)`.
##
## One step at a time, as the controller's `step` says: amount (tips) -> network -> method ->
## browser | qr | manual -> waiting -> done. Every step has the header (the back arrow while the
## controller `can_go_back`, the step dots, the logo, the title and the price), a scrolling body,
## the pinned forward button when the step has one, and the footer (the status message, the
## status line and Cancel / Continue playing / Continue). Debug builds also show the developer
## lines (DEV MODE and the controller's dev_warnings()).
##
## The sheet only reads the ScfgCheckoutController's fields and calls its methods; the autoload
## polls it every frame. Buttons that leave the game go through the autoload (open_browser,
## open_wallet_app, open_explorer, copy, paste_into). `closed(how)` is "cancel", "keep_playing" or
## "finish", Ren'Py's _close_checkout hows.
##
## Keyboard and gamepad: each step focuses its primary control (the selected option, the forward
## button, the first Copy, Continue); `ui_cancel` goes back, or closes as Cancel on the first step.
class_name ScfgSheet
extends ScfgScreenBase

## The controller this sheet shows.
var controller: ScfgCheckoutController
## The step the body shows now (the controller's step as of the last rebuild).
var shown_step := ""
var _synced := {}  # field key -> the controller's value last copied into the field


## Called by the sheet host before the sheet enters the tree.
func bind(c: ScfgCheckoutController, s: Variant) -> void:
	controller = c
	scfg = s


func _ready_to_show() -> bool:
	return controller != null


## The step dots shown ([index, total]), or [] when there are none.
func dots() -> Array:
	var ctl := controller
	if ctl.state == "closed" or ctl.step == "done":
		return []
	var d := ctl.step_dots()
	return d if d[1] > 1 else []


# --- what is shown ---------------------------------------------------------------------------


func _signature() -> Variant:
	var ctl := controller
	var tx: Variant = ctl.result.get("tx_hash") if ctl.result is Dictionary else null
	return [ctl.step, ctl.state, ctl.error, ctl.can_go_back, ctl.hint_tx, ctl.confirmations, ctl.required,
		ctl.method, ctl.chain_key, ctl.price_text, ctl.session_id, ctl.browser_failed, ctl.chain_options(),
		ctl.methods(), tx, ctl.message(), k]


func _moved(before: Variant, after: Variant) -> bool:
	return before == null or before[0] != after[0]


func _primary() -> String:
	var ctl := controller
	if ctl.state in ["closed", "paid", "underpaid", "expired"]:
		return "continue"
	match ctl.step:
		"amount":
			for preset: ScfgDec in ctl.tip_presets():
				if ctl.amount.cmp(preset) == 0:
					return "preset_" + str(preset)
			return "custom_amount"
		"network":
			var options := ctl.chain_options()
			if ctl.chain_key != null:
				return "network_" + str(ctl.chain_key)
			return "network_" + str(options[0][0]) if not options.is_empty() else "cancel"
		"method":
			var methods := ctl.methods()
			if ctl.method != null and methods.has(ctl.method):
				return "method_" + str(ctl.method)
			return "method_" + str(methods[0]) if not methods.is_empty() else "cancel"
		"browser", "qr":
			return "forward"
		"manual":
			return "copy_amount"
		"waiting":
			for key: String in ["explorer", "reopen", "keep_playing"]:
				if control(key) != null:
					return key
	return "cancel"


func _sync() -> void:
	for key: String in ["tx_input", "custom_amount"]:
		var f := control(key) as LineEdit
		var value: String = controller.get(key)
		if f != null and _synced.get(key) != value:
			f.text = value
			f.caret_column = value.length()
			_synced[key] = value


func _on_ui_cancel() -> void:
	var ctl := controller
	if ctl.can_go_back:
		_back()
	elif not ctl.tx_seen and not (ctl.step in ["waiting", "done"]) and ctl.step_dots()[0] == 0:
		close_with("cancel")


# --- the build -------------------------------------------------------------------------------


func _build() -> void:
	shown_step = controller.step
	_synced.clear()
	_build_header()
	var step := vbox(12)
	step.size_flags_vertical = Control.SIZE_EXPAND_FILL
	var scroll := ScrollContainer.new()
	scroll.name = "Scroll"
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.follow_focus = true
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	var body := vbox(10)
	body.name = "Body"
	body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(body)
	step.add_child(scroll)
	_layout.add_child(step)
	var forward := _build_step(body)
	if forward.is_empty():
		var none := Button.new()  # the key exists on every step; hidden when the step has none
		none.visible = false
		step.add_child(reg(none, "forward"))
	else:
		step.add_child(button("forward", forward[0], "ScfgAccent", forward[1]))
	_build_footer()


func _build_header() -> void:
	var ctl := controller
	var header := vbox(10)
	_layout.add_child(header)
	var nav := hbox(6)
	nav.custom_minimum_size.y = px(36)
	header.add_child(nav)
	var back := Button.new()
	back.theme_type_variation = "ScfgBack"
	back.custom_minimum_size = Vector2(px(36), px(36))
	back.focus_mode = Control.FOCUS_ALL
	back.tooltip_text = tr("Back")
	back.visible = ctl.can_go_back
	back.pressed.connect(_back)
	back.add_child(_arrow())
	nav.add_child(reg(back, "back"))
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	nav.add_child(spacer)
	var d := dots()
	if not d.is_empty():
		var row := hbox(6)
		row.name = "Dots"
		for i: int in d[1]:
			row.add_child(dot(color("accent") if i <= d[0] else color("line")))
		nav.add_child(row)
	var mark := logo()
	if mark != null:
		header.add_child(mark)
	if ctl.state == "closed":
		return
	if OS.is_debug_build():
		var testnet := " (testnet)" if ScfgPy.py_truthy(ctl.config["testnet"]) else ""
		header.add_child(label("DEV MODE: paying to %s%s" % [_short(str(ctl.recipient)), testnet], "ScfgDev"))
		for w: Variant in ctl.dev_warnings():
			header.add_child(label(str(w), "ScfgDev"))
	header.add_child(reg(label(tr(str(ctl.spec["title"])), "ScfgTitle"), "title"))
	var price := hbox(8)
	price.add_child(reg(label(ctl.price_text, "ScfgAmount", false), "price"))
	var token := label(str(ctl.spec["token"]), "ScfgAmountToken", false)
	token.size_flags_vertical = Control.SIZE_SHRINK_END
	price.add_child(token)
	header.add_child(price)
	var note := _exact_note()
	if note != "":
		header.add_child(label(note, "ScfgNote"))


## The back arrow, drawn (no font needs the glyph).
func _arrow() -> Control:
	var a := Control.new()
	a.mouse_filter = Control.MOUSE_FILTER_IGNORE
	a.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var ink := color("ink")
	var width := maxf(1.5, 2.0 * k)
	a.draw.connect(func() -> void:
		var c := a.size / 2.0
		var r := minf(a.size.x, a.size.y) * 0.22
		a.draw_line(c + Vector2(-r, 0), c + Vector2(r, 0), ink, width, true)
		a.draw_polyline(PackedVector2Array([c + Vector2(-r * 0.1, -r * 0.9), c + Vector2(-r, 0), c + Vector2(-r * 0.1, r * 0.9)]),
			ink, width, true))
	return a


## Builds the step's body; returns [label, action] of its forward button, or [].
func _build_step(body: VBoxContainer) -> Array:
	var ctl := controller
	if ctl.state == "closed":
		body.add_child(label(tr("This payment screen is no longer active. Any payment you made is still being checked in the background.")))
		return []
	match ctl.step:
		"amount":
			_step_amount(body)
			return [tr("Next"), _next]
		"network":
			_step_network(body)
		"method":
			_step_method(body)
		"browser":
			_step_browser(body)
			return [tr("Open wallet") if _web() else tr("Open checkout in browser"), _open_browser]
		"qr":
			_step_qr(body)
			return [tr("I've sent it"), _mark_sent]
		"manual":
			_step_manual(body)
			return [tr("I've sent it"), _mark_sent]
		"waiting":
			_step_waiting(body)
		_:
			_step_done(body)
	return []


func _step_amount(body: VBoxContainer) -> void:
	var ctl := controller
	body.add_child(label(tr("Choose an amount"), "ScfgLabel"))
	var presets := ctl.tip_presets()
	var grid := option_grid("presets", mini(presets.size(), 4))
	for preset: ScfgDec in presets:
		var text := "%s %s" % [_plain_amount(str(preset)), ctl.spec["token"]]
		grid.add_child(option("preset_" + str(preset), text, "", ctl.amount.cmp(preset) == 0,
			_set_tip.bind(str(preset)), "presets"))
	body.add_child(grid)
	body.add_child(label(tr("Or enter your own"), "ScfgLabel"))
	var row := hbox(8)
	row.add_child(field("custom_amount", ctl.custom_amount, 8, "0123456789.", _amount_typed, _apply_custom, "amount"))
	var set_button := button("set_amount", tr("Set"), "ScfgSmall", _apply_custom, "amount")
	set_button.size_flags_horizontal = Control.SIZE_FILL
	set_button.custom_minimum_size.x = px(90)
	row.add_child(set_button)
	body.add_child(row)
	_synced["custom_amount"] = ctl.custom_amount


func _step_network(body: VBoxContainer) -> void:
	var ctl := controller
	body.add_child(label(tr("Which network holds your %s?") % ctl.spec["token"], "ScfgLabel"))
	var options := ctl.chain_options()
	var grid := option_grid("networks", mini(options.size(), 2))
	for i in options.size():
		var o: Array = options[i]
		grid.add_child(option("network_" + str(o[0]), str(o[1]), _fee_label(o[2]), ctl.chain_key == o[0],
			_choose_network.bind(o[0]), "networks:%d" % (i / 2)))
	body.add_child(grid)


func _step_method(body: VBoxContainer) -> void:
	var ctl := controller
	body.add_child(label(tr("How do you want to pay?"), "ScfgLabel"))
	var list := vbox(8)
	for m: String in ctl.methods():
		list.add_child(option("method_" + m, _method_label(m), _method_hint(m), ctl.method == m, _choose_method.bind(m)))
	body.add_child(list)


func _step_browser(body: VBoxContainer) -> void:
	var ctl := controller
	body.add_child(label(tr("Browser wallet"), "ScfgLabel"))
	var solana: bool = ctl.family == "solana"
	if _web():
		body.add_child(label(tr("Opens your wallet right here, over the game: Phantom, Solflare, Backpack and other wallet extensions.") if solana
			else tr("Opens your wallet right here, over the game: MetaMask, Rabby, Coinbase Wallet and other wallet extensions.")))
	elif solana:
		body.add_child(label(tr("Opens a checkout page in your browser for Phantom, Solflare, Backpack and other wallet extensions.")))
	else:
		body.add_child(label(tr("Opens a checkout page in your browser for MetaMask, Rabby, Coinbase Wallet and other wallet extensions.")))
	body.add_child(label(tr("Approve %s on %s in your wallet.") % [_amount_line(false), _chain_display()], "ScfgMuted"))


func _step_qr(body: VBoxContainer) -> void:
	var ctl := controller
	body.add_child(label(tr("Phone wallet"), "ScfgLabel"))
	var row := hbox(20)
	var uri: Variant = ctl.payment_uri()
	if uri is ScfgErr:
		_report(uri)
	elif uri is String:
		var frame := PanelContainer.new()
		frame.theme_type_variation = "ScfgQrFrame"
		frame.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		var qr := ScfgQrView.new()
		var made: Variant = qr.show_text(uri, px(176))
		if made is ScfgErr:
			_report(made)
		frame.add_child(reg(qr, "qr"))
		row.add_child(frame)
	var steps := vbox(8)
	steps.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	steps.alignment = BoxContainer.ALIGNMENT_CENTER
	steps.add_child(label(tr("1. Open your wallet app"), "ScfgQrStep"))
	steps.add_child(label(tr("2. Scan the code"), "ScfgQrStep"))
	steps.add_child(label(tr("3. Approve %s") % _amount_line(false), "ScfgQrStep"))
	row.add_child(steps)
	body.add_child(row)
	if scfg != null and scfg.is_mobile() and ScfgPy.py_truthy(ctl.config["methods"]["qr"]["wallet_button"]):
		body.add_child(button("wallet_app", tr("Open wallet app"), "", _open_wallet_app))
	if ctl.family == "solana":
		body.add_child(label(tr("If your wallet changes the amount, the game still finds the payment."), "ScfgMuted"))
	else:
		body.add_child(label(tr("If your wallet changes the amount, use Manual transfer and paste the transaction hash."), "ScfgMuted"))


func _step_manual(body: VBoxContainer) -> void:
	var ctl := controller
	var solana: bool = ctl.family == "solana"
	if solana:
		body.add_child(label(tr("Send exactly this amount on Solana (SPL) only. Withdrawing on another network may lose your funds."), "ScfgWarning"))
	else:
		body.add_child(label(tr("Send exactly this amount on %s only. Sending on another network may lose your funds.") % _chain_display(), "ScfgWarning"))
	var rows := [
		["amount", tr("Amount"), _amount_line(true)],
		["recipient", tr("To address"), str(ctl.recipient)],
		["token", tr("Token mint") if solana else tr("Token contract"), str(ctl.token_address)],
	]
	for r: Array in rows:
		var group := vbox(0)
		group.add_child(label(r[1], "ScfgLabel"))
		var line := hbox(8)
		var value := mono(r[2])
		value.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		line.add_child(value)
		var copy := button("copy_" + str(r[0]), tr("Copy"), "ScfgSmall", _copy.bind((r[2] as String).split(" ")[0]))
		copy.size_flags_horizontal = Control.SIZE_SHRINK_END
		copy.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		copy.custom_minimum_size.x = px(72)
		line.add_child(copy)
		group.add_child(line)
		body.add_child(group)
	if ScfgPy.py_truthy(ctl.config["methods"]["manual"]["allow_tx_paste"]):
		body.add_child(label(tr("Sent from an exchange? Paste the %s:") % _tx_word(), "ScfgLabel"))
		var row := hbox(8)
		row.add_child(field("tx_input", ctl.tx_input, 90, TX_CHARS, _tx_typed, _submit_hash, "tx"))
		if _web():
			var paste := button("paste", tr("Paste"), "ScfgSmall", _paste, "tx")
			paste.size_flags_horizontal = Control.SIZE_FILL
			paste.custom_minimum_size.x = px(90)
			row.add_child(paste)
		var check := button("check", tr("Check"), "ScfgSmall", _submit_hash, "tx")
		check.size_flags_horizontal = Control.SIZE_FILL
		check.custom_minimum_size.x = px(90)
		row.add_child(check)
		body.add_child(row)
		_synced["tx_input"] = ctl.tx_input


func _step_waiting(body: VBoxContainer) -> void:
	var ctl := controller
	body.add_child(label(_progress_text(), "ScfgHeading"))
	if ctl.state == "confirming":
		var bar := ProgressBar.new()
		bar.show_percentage = false
		bar.max_value = maxi(1, ctl.required)
		bar.value = ctl.confirmations
		bar.custom_minimum_size.y = px(8)
		body.add_child(reg(bar, "progress"))
	if ScfgPy.py_truthy(ctl.hint_tx):
		body.add_child(label(tr("Your %s:") % _tx_word(), "ScfgLabel"))
		body.add_child(mono(str(ctl.hint_tx)))
		var url: Variant = ctl.explorer_url()
		if url is ScfgErr:
			_report(url)
		elif url is String:
			body.add_child(_small(button("explorer", tr("View on explorer"), "ScfgSmall", _open_explorer.bind(url))))
	elif ctl.method == "browser":
		if _web():
			body.add_child(label(tr("Finish the payment in the wallet window."), "ScfgMuted"))
			body.add_child(_small(button("reopen", tr("Reopen wallet window"), "ScfgSmall", _open_browser)))
		else:
			body.add_child(label(tr("Finish the payment on the checkout page in your browser."), "ScfgMuted"))
			body.add_child(_small(button("reopen", tr("Reopen checkout page"), "ScfgSmall", _open_browser)))
	var gap := Control.new()
	gap.custom_minimum_size.y = px(4)
	body.add_child(gap)
	body.add_child(label(tr("You can keep playing: the game keeps checking in the background and finishes the purchase by itself."), "ScfgMuted"))


func _step_done(body: VBoxContainer) -> void:
	var ctl := controller
	var title := label(_done_title(), "ScfgHeading")
	title.add_theme_color_override("font_color", _status_color())
	body.add_child(title)
	var tx: Variant = ctl.result.get("tx_hash") if ctl.result is Dictionary else null
	if ctl.state == "paid" and ScfgPy.py_truthy(tx):
		body.add_child(label(tr("Keep this %s to restore your purchase on another device:") % _tx_word(), "ScfgMuted"))
		body.add_child(mono(str(tx)))
		var row := hbox(8)
		var copy_text := tr("Copy signature") if ctl.family == "solana" else tr("Copy hash")
		row.add_child(_small(button("copy_tx", copy_text, "ScfgSmall", _copy.bind(str(tx)), "done")))
		var url: Variant = ctl.explorer_url()
		if url is ScfgErr:
			_report(url)
		row.add_child(_small(button("explorer", tr("View on explorer"), "ScfgSmall", _open_explorer.bind(url), "done")))
		body.add_child(row)
	elif ctl.state == "underpaid":
		body.add_child(label(tr("The amount received was less than the price. Please contact the developer."), "ScfgMuted"))
	elif ctl.state == "expired":
		body.add_child(label(tr("If you already paid, the game will keep checking for 24 hours and unlock it automatically."), "ScfgMuted"))


func _build_footer() -> void:
	var ctl := controller
	var footer := vbox(12)
	var msg: Variant = ctl.message()
	if ScfgPy.py_truthy(msg):
		footer.add_child(reg(label(tr(str(msg)), "ScfgError"), "message"))
	footer.add_child(rule())
	var status := _status_text() if ctl.state != "closed" else ""
	if status != "":
		footer.add_child(reg(status_row(status, _status_color()), "status"))
	var actions := hbox(10)
	if ctl.state in ["closed", "paid", "underpaid", "expired"]:
		actions.add_child(button("continue", tr("Continue"), "ScfgPrimary", close_with.bind("finish"), "footer"))
	elif ScfgPy.py_truthy(ctl.session):
		actions.add_child(button("cancel", tr("Cancel"), "", close_with.bind("cancel"), "footer"))
		actions.add_child(button("keep_playing", tr("Continue playing"), "ScfgPrimary", close_with.bind("keep_playing"), "footer"))
	else:
		actions.add_child(button("cancel", tr("Cancel"), "", close_with.bind("cancel"), "footer"))
	footer.add_child(actions)
	_layout.add_child(footer)


func _small(b: Button) -> Button:
	b.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	return b


# --- texts (Ren'Py's helpers of the same names) ----------------------------------------------


func _web() -> bool:
	return scfg != null and scfg.is_web()


func _chain_display() -> String:
	var c: Variant = controller.chain
	return str(c["name"]) if c is Dictionary else ""


func _tx_word() -> String:
	return tr("signature") if controller.family == "solana" else tr("transaction hash")


func _method_label(m: String) -> String:
	match m:
		"browser":
			return tr("Browser wallet")
		"qr":
			return tr("Phone wallet")
	return tr("Manual transfer")


func _method_hint(m: String) -> String:
	var solana: bool = controller.family == "solana"
	match m:
		"browser":
			if solana:
				return tr("Phantom, Solflare or another wallet in this browser") if _web() else tr("Phantom, Solflare or another wallet extension")
			return tr("MetaMask, Rabby or another wallet in this browser") if _web() else tr("MetaMask, Rabby or another wallet extension")
		"qr":
			return tr("Scan a code with your phone wallet")
	return tr("Send from an exchange or any wallet")


func _fee_label(fee: Variant) -> String:
	match str(fee):
		"low":
			return tr("low fees")
		"lowest":
			return tr("lowest fees")
		"high":
			return tr("high fees")
	return str(fee) if fee != null else ""


func _status_text() -> String:
	var ctl := controller
	if ctl.step in ["amount", "waiting", "done"] or ctl.state == "choose":
		return ""
	if ctl.state == "waiting" and not ScfgPy.py_truthy(ctl.hint_tx):
		return ""
	return _progress_text()


func _progress_text() -> String:
	var ctl := controller
	var chain := _chain_display()
	if ctl.state == "starting":
		return tr("Preparing the payment...")
	if ctl.state == "waiting":
		if ScfgPy.py_truthy(ctl.hint_tx):
			return tr("Transaction received. Waiting for it to be included on %s...") % chain
		return tr("Waiting for your payment on %s...") % chain
	if ctl.state == "confirming":
		return tr("Confirming on %s... %d/%d") % [chain, ctl.confirmations, ctl.required]
	return ""


func _done_title() -> String:
	if controller.state == "paid":
		return tr("Payment confirmed. Thank you!")
	if controller.state == "underpaid":
		return tr("Payment too small")
	return tr("The payment window has closed")


func _status_color() -> Color:
	match controller.state:
		"paid":
			return color("ok")
		"underpaid":
			return color("error")
		"expired":
			return color("warn")
	return color("accent")


func _amount_line(exact: bool) -> String:
	var ctl := controller
	return "%s %s" % [str(ctl.pay_amount_text) if exact else ctl.price_text, ctl.spec["token"]]


func _exact_note() -> String:
	var ctl := controller
	if not ScfgPy.py_truthy(ctl.session) or ctl.step in ["manual", "done"] or ctl.state == "closed":
		return ""
	return tr("Your wallet will ask for %s. The last digits identify your payment and add less than 1 cent.") % _amount_line(true)


## 1.00 -> "1", 2.50 -> "2.50".
static func _plain_amount(text: String) -> String:
	return text.substr(0, text.length() - 3) if text.ends_with(".00") else text


static func _short(address: String) -> String:
	return address.substr(0, 6) + "..." + address.substr(address.length() - 4)


# --- actions ---------------------------------------------------------------------------------


func _back() -> void:
	controller.back()


func _next() -> void:
	var r: Variant = controller.next_step()
	if r is ScfgErr:
		_report(r)


func _set_tip(value: String) -> void:
	var r: Variant = controller.set_tip_amount(value)
	if r is ScfgErr:
		_report(r)


func _apply_custom() -> void:
	var r: Variant = controller.apply_custom_amount()
	if r is ScfgErr:
		_report(r)


func _amount_typed(text: String) -> void:
	controller.custom_amount = text
	_synced["custom_amount"] = text


func _choose_network(key: Variant) -> void:
	var r: Variant = controller.choose_network(key)
	if r is ScfgErr:
		_report(r)


func _choose_method(m: String) -> void:
	controller.choose_method(m)


func _mark_sent() -> void:
	controller.mark_sent()


func _tx_typed(text: String) -> void:
	controller.tx_input = text
	_synced["tx_input"] = text


func _submit_hash() -> void:
	var r: Variant = controller.submit_hash()
	if r is ScfgErr:
		_report(r)


func _open_browser() -> void:
	if scfg != null:
		scfg.open_browser(controller)


func _open_wallet_app() -> void:
	if scfg != null:
		scfg.open_wallet_app(controller)


func _open_explorer(url: Variant) -> void:
	if scfg != null:
		scfg.open_explorer(url)


func _copy(text: String) -> void:
	if scfg != null:
		scfg.copy(text)


func _paste() -> void:
	if scfg != null:
		scfg.paste_into(controller)
