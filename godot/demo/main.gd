## Demo game for the SCFG addon: a title, a locked Chapter 2, a Tip button and a Restore button.
## It uses only the public API: configure, add_item, pay, owns, restore and payment_resolved.
## Testnet only: no real money moves.
extends Control

const ITEM := "chapter2"
const CONFIG_ENV := "SCFG_E2E_CONFIG"  # test-only hook, see README.md

@onready var _chapter: Label = %Chapter
@onready var _unlock: Button = %Unlock
@onready var _tip: Button = %Tip
@onready var _restore: Button = %Restore
@onready var _status: Label = %Status


func _ready() -> void:
	var hook := _e2e_config()
	if hook.get("failed", false) or not _configure(hook.get("config", {})):
		# A test run must never silently fall back to the built-in testnet config, and a setup
		# the addon refused (it printed why) must not run as if it worked.
		get_tree().quit(1)
		return
	Scfg.payment_resolved.connect(_on_payment_resolved)
	Scfg.receipts_changed.connect(_refresh)
	_unlock.pressed.connect(_on_unlock)
	_tip.pressed.connect(_on_tip)
	_restore.pressed.connect(_on_restore)
	_refresh()
	_unlock.grab_focus()


## Configures the addon: the built-in testnet setup, or the test hook's `override`. False when
## configure() or add_item() refused it (each returns the error it reported with push_error).
func _configure(override: Dictionary) -> bool:
	if override.is_empty():
		var err: Variant = Scfg.configure({
			# TODO: replace with YOUR wallet address. This placeholder is the public EIP-55 example
			# address from the EIP-55 spec: it is not yours, so a real payment to it is lost.
			"wallet": "0x52908400098527886E0F7030069857D2E4169EE7",
			"testnet": true,  # Sepolia / Arbitrum Sepolia / Amoy / Base Sepolia: no real money
		})
		if err == null:
			err = Scfg.add_item(ITEM, {"code": 1, "price": "1.00", "token": "USDC", "title": "Chapter 2: The Fall"})
		return err == null
	if Scfg.configure(override["configure"]) != null:
		return false
	var items: Dictionary = override["items"]
	for id: String in items:
		if Scfg.add_item(id, items[id]) != null:
			return false
	return true


## The test-only override, {"configure": {...}, "items": {id: {...}}}. On desktop it is the JSON file
## named by SCFG_E2E_CONFIG; on web it is the JSON string in window.scfgE2EConfig. Returns {} when
## the hook is not set (every real run), {"config": {...}} when valid, {"failed": true} (after
## push_error) when it is set but unusable (on the web: anything but a non-empty string).
func _e2e_config() -> Dictionary:
	var text := ""
	var source := CONFIG_ENV
	if OS.has_feature("web"):
		source = "window.scfgE2EConfig"
		var kind: Variant = JavaScriptBridge.eval("typeof window.scfgE2EConfig")
		if kind == "undefined":
			return {}
		if kind != "string":
			return _bad_hook(source, "not a string but %s" % kind)
		text = str(JavaScriptBridge.eval("window.scfgE2EConfig"))
		if text == "":
			return _bad_hook(source, "an empty string")
	else:
		var path := OS.get_environment(CONFIG_ENV)
		if path == "":
			return {}
		source = "%s=%s" % [CONFIG_ENV, path]
		if not FileAccess.file_exists(path):
			return _bad_hook(source, "file not found")
		text = FileAccess.get_file_as_string(path)
		if text == "":
			return _bad_hook(source, "file is empty or unreadable")
	var parsed: Variant = JSON.parse_string(text)
	if not parsed is Dictionary:
		return _bad_hook(source, "not a JSON object")
	var d: Dictionary = ScfgPy.from_json(parsed)  # JSON numbers are floats; the core wants ints
	if not d.get("configure") is Dictionary:
		return _bad_hook(source, "\"configure\" must be an object")
	if not d.get("items") is Dictionary:
		return _bad_hook(source, "\"items\" must be an object")
	for id: Variant in d["items"]:
		if not d["items"][id] is Dictionary:
			return _bad_hook(source, "items[%s] must be an object" % str(id))
	return {"config": d}


func _bad_hook(source: String, reason: String) -> Dictionary:
	push_error("e2e config %s: %s" % [source, reason])
	return {"failed": true}


func _refresh() -> void:
	var owned := Scfg.owns(ITEM)
	_chapter.text = "Chapter 2: unlocked. Thank you for supporting the game!" if owned else "Chapter 2: locked"
	_unlock.visible = not owned


func _on_unlock() -> void:
	var result: ScfgPaymentResult = await _while_paused(func() -> ScfgPaymentResult: return await Scfg.pay("unlock", {"item": ITEM}))
	if not is_inside_tree():
		return  # the game quit while the sheet was open: pay() returned at once
	_refresh()
	_restore_focus()
	match result.status:
		"paid", "already_owned":
			_status.text = "Payment confirmed. Thank you!"
		"pending":
			_status.text = "No problem, keep playing. Chapter 2 unlocks by itself when the payment arrives."
		_:
			_status.text = "Maybe later. (Status: %s)" % result.status


func _on_tip() -> void:
	var result: ScfgPaymentResult = await _while_paused(func() -> ScfgPaymentResult: return await Scfg.pay("tip", {"amount": "1.00", "token": "USDC"}))
	if not is_inside_tree():
		return
	_restore_focus()
	if result.status == "paid":
		_status.text = "Thank you for the %s %s tip!" % [result.amount, result.token]
	else:
		_status.text = "Tip: %s" % result.status


func _on_restore() -> void:
	var result: ScfgRestoreResult = await _while_paused(func() -> ScfgRestoreResult: return await Scfg.restore())
	if not is_inside_tree():
		return
	_refresh()
	_restore_focus()
	_status.text = "Restore: %s" % result.status


func _on_payment_resolved(result: ScfgPaymentResult) -> void:
	# Every result without a pay() callback lands here: one that arrives later (a payment kept in
	# the background, or one confirmed after a restart), and also the paid or cancelled result of a
	# pay() still awaited above, before that await returns it. So this handler is idempotent: it
	# only refreshes from owns() and sets a status text, which the awaiting code then replaces.
	if result.status == "paid":
		_refresh()
		_status.text = "Payment confirmed. Thank you!"


## Gives focus back to the first visible button, so keyboard and gamepad players keep it.
func _restore_focus() -> void:
	for b: Button in [_unlock, _tip, _restore]:
		if b.visible:
			b.grab_focus()
			return


## Runs `flow` with the game tree paused: the sheet does not stop game nodes polling Input,
## and the addon's autoload and screens keep running while paused (PROCESS_MODE_ALWAYS).
func _while_paused(flow: Callable) -> Variant:
	var tree := get_tree()  # kept: when the game quits with a sheet open, this node leaves it first
	var was_paused := tree.paused
	tree.paused = true
	var result: Variant = await flow.call()
	tree.paused = was_paused
	return result
