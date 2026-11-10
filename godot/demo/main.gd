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
	_configure()
	Scfg.payment_resolved.connect(_on_payment_resolved)
	Scfg.receipts_changed.connect(_refresh)
	_unlock.pressed.connect(_on_unlock)
	_tip.pressed.connect(_on_tip)
	_restore.pressed.connect(_on_restore)
	_refresh()
	_unlock.grab_focus()


func _configure() -> void:
	var hook := _e2e_config()
	if hook.get("failed", false):
		get_tree().quit(1)  # a test run must never silently fall back to the built-in testnet config
		return
	var override: Dictionary = hook.get("config", {})
	if override.is_empty():
		Scfg.configure({
			# TODO: replace with YOUR wallet address. This placeholder is the public EIP-55 example
			# address from the EIP-55 spec: it is not yours, so a real payment to it is lost.
			"wallet": "0x52908400098527886E0F7030069857D2E4169EE7",
			"testnet": true,  # Sepolia / Arbitrum Sepolia / Amoy / Base Sepolia: no real money
		})
		Scfg.add_item(ITEM, {"code": 1, "price": "1.00", "token": "USDC", "title": "Chapter 2: The Fall"})
		return
	Scfg.configure(override["configure"])
	var items: Dictionary = override["items"]
	for id: String in items:
		Scfg.add_item(id, items[id])


## The test-only override, {"configure": {...}, "items": {id: {...}}}. On desktop it is the JSON file
## named by SCFG_E2E_CONFIG; on web it is the JSON string in window.scfgE2EConfig. Returns {} when
## the hook is not set (every real run), {"config": {...}} when valid, {"failed": true} (after
## push_error) when it is set but unusable.
func _e2e_config() -> Dictionary:
	var text := ""
	var source := CONFIG_ENV
	if OS.has_feature("web"):
		source = "window.scfgE2EConfig"
		var raw: Variant = JavaScriptBridge.eval("typeof window.scfgE2EConfig === 'string' ? window.scfgE2EConfig : ''")
		text = str(raw) if raw != null else ""
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
	if text == "":
		return {}
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
	_restore_focus()
	if result.status == "paid":
		_status.text = "Thank you for the %s %s tip!" % [result.amount, result.token]
	else:
		_status.text = "Tip: %s" % result.status


func _on_restore() -> void:
	var result: ScfgRestoreResult = await _while_paused(func() -> ScfgRestoreResult: return await Scfg.restore())
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
	var was_paused := get_tree().paused
	get_tree().paused = true
	var result: Variant = await flow.call()
	get_tree().paused = was_paused
	return result
