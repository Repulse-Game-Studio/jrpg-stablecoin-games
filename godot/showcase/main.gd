## Showcase for the Stablecoin for Games (SCFG) addon: one screen that uses the whole public API.
##
## Left column: a small shop (three unlocks, one with pay-what-you-want), a tip jar and the player
## tools (restore, export, clear). Right column: what the addon knows (owned items, tip total,
## pending payments, stored receipts) and a log of every result and signal as it happens.
##
## Read it top to bottom: 1. configure, 2. build the shop, 3. pay, 4. listen, 5. show the state.
## Testnets only: no real money moves.
extends Control

# --- 1. Configuration ----------------------------------------------------------------------------

## The wallet that receives the payments. This placeholder is the public example address from the
## EIP-55 spec: it is not yours, so a real payment to it would be lost. Replace it with your own.
const WALLET := "0x52908400098527886E0F7030069857D2E4169EE7"

## The accent color of the payment sheet, as "#rrggbb".
const ACCENT := "#7c5cff"

## The paid items, passed to Scfg.add_item(). `code` (1-99) identifies the item in the payment
## amount: never change it after release. pay() may charge another amount than the price: anything
## above it (pay what you want), or down to an optional `min_price` (a sale).
const ITEMS := {
	"chapter2": {"code": 1, "price": "1.00", "token": "USDC", "title": "Chapter 2: The Fall"},
	"soundtrack": {"code": 2, "price": "0.50", "token": "USDC", "title": "Original soundtrack"},
	"supporter": {"code": 3, "price": "2.00", "token": "USDC", "title": "Supporter pack"},
}

## The supporter pack also offers a bigger, pay-what-you-want amount.
const SUPPORTER_GENEROUS_AMOUNT := "5.00"

## The tip jar's suggested amounts (the player can change the amount in the sheet).
const TIP_AMOUNTS: Array[String] = ["0.50", "1.00", "3.00"]
const TIP_TOKEN := "USDC"

@onready var _items_box: VBoxContainer = %Items
@onready var _tips_box: HBoxContainer = %Tips
@onready var _restore_button: Button = %Restore
@onready var _export_button: Button = %Export
@onready var _clear_button: Button = %Clear
@onready var _summary: Label = %Summary
@onready var _receipts_view: RichTextLabel = %Receipts
@onready var _log_view: RichTextLabel = %Log

## item id -> the Label showing whether it is owned.
var _item_state_labels: Dictionary = {}
## item id -> the Buttons that buy it, hidden once the item is owned.
var _item_buttons: Dictionary = {}


func _ready() -> void:
	if not _configure():
		_log("[color=#ff6b6b]The addon refused the configuration; see the Output panel.[/color]")
		return
	_build_shop()
	_connect_signals()
	_refresh()
	_log("Ready. Build: %s. Pick something on the left." % _build_kind())
	_focus_first_button()


## Configures the addon and declares the items. configure() and add_item() return null on success,
## or the ScfgErr they already reported with push_error (GDScript has no exceptions).
func _configure() -> bool:
	var err: Variant = Scfg.configure({
		"wallet": WALLET,
		"testnet": true,  # Sepolia, Arbitrum Sepolia, Amoy, Base Sepolia, Arc Testnet
		"accent": ACCENT,
		# Optional, among others: "solana_wallet", "chains", "tokens", "methods", "logo", "theme".
	})
	if err != null:
		return false
	for id: String in ITEMS:
		if Scfg.add_item(id, ITEMS[id]) != null:
			return false
	Scfg.notify = true  # show a "Payment confirmed" toast for payments that confirm in the background
	return true


# --- 2. The shop -------------------------------------------------------------------------------


func _build_shop() -> void:
	for id: String in ITEMS:
		_add_item_row(id)
	for amount in TIP_AMOUNTS:
		_tips_box.add_child(_button("%s %s" % [amount, TIP_TOKEN], _tip.bind(amount)))
	_restore_button.pressed.connect(_restore)
	_export_button.pressed.connect(_export)
	_clear_button.pressed.connect(_clear)


## One row per item: its title, whether it is owned, and the button(s) that buy it.
func _add_item_row(id: String) -> void:
	var item: Dictionary = ITEMS[id]
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	var title := Label.new()
	title.text = item["title"]
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(title)
	var state := Label.new()
	row.add_child(state)
	_item_state_labels[id] = state
	var buttons: Array[Button] = [
		_button("Unlock: %s %s" % [item["price"], item["token"]], _unlock.bind(id, "")),
	]
	if id == "supporter":
		buttons.append(_button("Pay %s %s" % [SUPPORTER_GENEROUS_AMOUNT, item["token"]],
				_unlock.bind(id, SUPPORTER_GENEROUS_AMOUNT)))
	for b in buttons:
		row.add_child(b)
	_item_buttons[id] = buttons
	_items_box.add_child(row)


func _button(text: String, on_pressed: Callable) -> Button:
	var b := Button.new()
	b.text = text
	b.pressed.connect(on_pressed)
	return b


# --- 3. Paying, restoring, exporting -----------------------------------------------------------


## Opens the payment sheet for an item. `amount` overrides the price ("" keeps it).
func _unlock(id: String, amount: String) -> void:
	var opts := {"item": id}
	if amount != "":
		opts["amount"] = amount
	_log("pay(\"unlock\", %s)" % opts)
	var result: ScfgPaymentResult = await _while_paused(func() -> ScfgPaymentResult:
		return await Scfg.pay("unlock", opts))
	if _quitting():
		return
	_log_payment("unlock result", result)
	_after_flow()


func _tip(amount: String) -> void:
	var opts := {"amount": amount, "token": TIP_TOKEN}
	_log("pay(\"tip\", %s)" % opts)
	var result: ScfgPaymentResult = await _while_paused(func() -> ScfgPaymentResult:
		return await Scfg.pay("tip", opts))
	if _quitting():
		return
	_log_payment("tip result", result)
	_after_flow()


## Shows the restore screen, where the player pastes the hash of a payment made elsewhere.
func _restore() -> void:
	_log("restore()")
	var result: ScfgRestoreResult = await _while_paused(func() -> ScfgRestoreResult:
		return await Scfg.restore())
	if _quitting():
		return
	var detail := "%d receipt(s) added" % result.restored.size() if result.status == "restored" else ""
	_log("restore result: [b]%s[/b] %s%s" % [result.status, detail, _error_suffix(result.error)])
	_after_flow()


func _export() -> void:
	var path: Variant = Scfg.export_receipts()
	if path is String:
		_log("export_receipts() wrote %s" % path)
	else:
		_log("export_receipts() failed: %s" % path)


func _clear() -> void:
	Scfg.clear_local_data()
	_log("clear_local_data(): receipts forgotten on this device. Restore brings them back.")
	_refresh()  # receipts_changed only comes when there were receipts; the pending payments go too


## Runs a payment or restore flow with the game paused. The sheet takes focus but does not stop
## game nodes that read input; the addon's autoload and screens keep running while paused.
func _while_paused(flow: Callable) -> Variant:
	var tree := get_tree()
	var was_paused := tree.paused
	tree.paused = true
	var result: Variant = await flow.call()
	tree.paused = was_paused
	return result


## True when the game quit while a sheet was open: the addon then releases the awaited pay() or
## restore() at once, and this scene is already leaving the tree.
func _quitting() -> bool:
	return not is_inside_tree()


func _after_flow() -> void:
	_refresh()
	_focus_first_button()


# --- 4. Listening -------------------------------------------------------------------------------


func _connect_signals() -> void:
	# A final result with no pay() callback to go to: a payment that confirms in the background or
	# after a restart, and the results of a pay() awaited above (so log, don't count).
	Scfg.payment_resolved.connect(func(result: ScfgPaymentResult) -> void:
		_log_payment("signal payment_resolved", result)
		_refresh())
	# The stored receipts changed: a payment confirmed, a restore, clear_local_data().
	Scfg.receipts_changed.connect(func() -> void:
		_log("signal receipts_changed")
		_refresh())


# --- 5. Showing the state ----------------------------------------------------------------------


func _refresh() -> void:
	for id: String in ITEMS:
		var owned := Scfg.owns(id)
		(_item_state_labels[id] as Label).text = "owned" if owned else ""
		for b: Button in _item_buttons[id]:
			b.visible = not owned
	var owned_titles: Array[String] = []
	for id: String in ITEMS:
		if Scfg.owns(id):
			owned_titles.append(Scfg.item_title(id))
	_summary.text = "\n".join([
		"Owned: %s" % (", ".join(owned_titles) if not owned_titles.is_empty() else "nothing yet"),
		"Tips: %s %s%s" % [Scfg.tip_total(), TIP_TOKEN, " (thank you!)" if Scfg.has_tipped() else ""],
		"Payments still being watched: %d" % Scfg.pending().size(),
	])
	_receipts_view.text = _receipts_text()


func _receipts_text() -> String:
	var receipts := Scfg.receipts()
	if receipts.is_empty():
		return "None yet. Pay for something, or restore a payment."
	var lines: Array[String] = []
	for key: String in receipts:
		var r: Dictionary = receipts[key]
		var what: String = Scfg.item_title(r["item"]) if r["kind"] == "unlock" else "Tip"
		lines.append("%s: %s %s on %s, tx %s" % [
			what, r["amount"], r["token"], Scfg.chain_name(r["chain"]), _short(str(r["tx_hash"]))])
	return "\n".join(lines)


func _log_payment(source: String, result: ScfgPaymentResult) -> void:
	var paid := ""
	if result.amount != null:
		paid = " %s %s on %s" % [result.amount, result.token, Scfg.chain_name(result.chain)]
	_log("%s: [b]%s[/b]%s%s" % [source, result.status, paid, _error_suffix(result.error)])


func _log(text: String) -> void:
	_log_view.append_text("[color=#5c6370]%s[/color]  %s\n" % [Time.get_time_string_from_system(), text])


func _error_suffix(error: Variant) -> String:
	return "" if error == null else " (error: %s)" % error


func _short(hash: String) -> String:
	return hash if hash.length() <= 14 else "%s...%s" % [hash.left(8), hash.right(6)]


func _build_kind() -> String:
	if Scfg.is_web():
		return "web export (the checkout opens inside this page)"
	if Scfg.is_mobile():
		return "mobile (best effort, unverified)"
	return "desktop (the checkout opens in your browser)"


## Keyboard and gamepad players keep a focused button after every flow.
func _focus_first_button() -> void:
	for row in _items_box.get_children():
		for child in row.get_children():
			if child is Button and child.visible:
				child.grab_focus()
				return
	if _tips_box.get_child_count() > 0:
		(_tips_box.get_child(0) as Button).grab_focus()
