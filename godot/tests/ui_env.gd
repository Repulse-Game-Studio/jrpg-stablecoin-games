## Shared set-up for the screen tests (test_sheet.gd, test_restore_screen.gd): an instance of the
## autoload script under the tree root with the addon's own sheet host (ScfgDefaultSheetHost, the
## default), FakeChains behind the Engine, a manual worker scheduler (a worker pass runs only when
## the test awaits `pass_()`) and a fake platform (no browser, no real clipboard). Not a test file:
## the runner only loads test_*.gd.
##
## Input is driven as a keyboard or gamepad player would: `act()` sends an action press and release
## through Input.parse_input_event, which Godot dispatches on the next frame. An action event is
## what a gamepad sends as far as a LineEdit is concerned (it ignores it); `key()` sends a real
## InputEventKey, which a focused LineEdit sees first, as a keyboard player's arrows.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")
const FLOWS := preload("res://tests/test_scfg_flows.gd")
const SCFG := preload("res://addons/scfg/scfg.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const TX := "0xabababababababababababababababababababababababababababababababab"
const DIR := "user://scfg-tests/ui"

static var USDC_ARB: String = ScfgChains.MAINNET["arbitrum"]["tokens"]["USDC"]["address"]


static func tree() -> SceneTree:
	return Engine.get_main_loop() as SceneTree


static func frames(n: int = 1) -> void:
	for i in n:
		await tree().process_frame


## {node, chains, clock, platform}: a configured autoload instance with chapter2 declared, its
## Worker started and its first pass run. `config` adds configure() keys, `setup` _setup keys.
static func make(name: String, config: Dictionary = {}, setup: Dictionary = {}) -> Dictionary:
	if not tree().root.is_inside_tree():
		await tree().process_frame
	var chains := {}
	var factory := func(chain: Dictionary) -> Variant:
		if not chains.has(chain["key"]):
			chains[chain["key"]] = F.FakeChain.new(1000)
		chains[chain["key"]].chain_obj = chain
		return chains[chain["key"]]
	var clock := F.FakeClock.new(1000.0)
	var platform := FLOWS.FakePlatform.new()
	var dir := DIR.path_join(name)
	FLOWS._remove_tree(dir)
	DirAccess.make_dir_recursive_absolute(dir)
	var opts := {"store_path": dir.path_join("state.json"), "clock": clock.fn, "scheduler": FLOWS.ManualTimer.new(),
		"transport": F.FakeTransport.new({}), "engine_opts": {"rpc_factory": factory, "clock": clock.fn},
		"quiet": true, "platform": platform}
	opts.merge(setup, true)
	var node: Node = SCFG.new()
	node._setup(opts)
	tree().root.add_child(node)
	var cfg := {"wallet": DEV}
	cfg.merge(config, true)
	T.eq(node.configure(cfg), null, "configure")
	T.eq(node.add_item("chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"}), null, "add_item")
	node._pump()
	await (node._worker as ScfgWorker).run_once()
	node._pump()
	return {"node": node, "chains": chains, "clock": clock, "platform": platform}


## Frees the autoload instance (which releases any screen still open) and what it showed.
static func drop(env: Dictionary) -> void:
	var s: Node = env["node"]
	var host: Variant = s.sheet_host
	if host is ScfgDefaultSheetHost:
		for screen: Node in (host as ScfgDefaultSheetHost).screens():
			if is_instance_valid(screen) and screen.get_parent() != null:
				screen.get_parent().queue_free()
	s.queue_free()
	await frames(2)


## One worker pass, then one pump frame.
static func pass_(s: Node) -> void:
	await (s._worker as ScfgWorker).run_once()
	s._pump()


## Starts `Scfg.pay(kind, opts)` without waiting for it: the result lands in box["result"].
static func pay(s: Node, kind: Variant, opts: Dictionary = {}) -> Dictionary:
	var box := {}
	var run := func() -> void:
		box["result"] = await s.pay(kind, opts)
	run.call()
	return box


## Starts `Scfg.restore(opts)` without waiting for it: the result lands in box["result"].
static func restore(s: Node, opts: Dictionary = {}) -> Dictionary:
	var box := {}
	var run := func() -> void:
		box["result"] = await s.restore(opts)
	run.call()
	return box


## The newest screen the default host shows, or null.
static func screen(s: Node) -> Node:
	var host: Variant = s.sheet_host
	if not host is ScfgDefaultSheetHost:
		return null
	var open: Array = (host as ScfgDefaultSheetHost).screens()
	return open[-1] if not open.is_empty() else null


static func focused() -> Control:
	return tree().root.gui_get_focus_owner()


## Presses and releases an input action as a keyboard or gamepad player does, then lets the
## screens react (the event is dispatched next frame, the screen rebuilds the frame after).
static func act(action: String) -> void:
	for pressed: bool in [true, false]:
		var ev := InputEventAction.new()
		ev.action = action
		ev.pressed = pressed
		Input.parse_input_event(ev)
		await frames(1)
	await frames(1)


## Presses and releases the real key `code` (an InputEventKey, as a keyboard player sends), then
## lets the screens react.
static func key(code: Key) -> void:
	for pressed: bool in [true, false]:
		var ev := InputEventKey.new()
		ev.keycode = code
		ev.physical_keycode = code
		ev.pressed = pressed
		Input.parse_input_event(ev)
		await frames(1)
	await frames(1)


## Types `text` into the focused control, one key event per character.
static func type_text(text: String) -> void:
	for i in text.length():
		var ev := InputEventKey.new()
		ev.pressed = true
		ev.unicode = text.unicode_at(i)
		ev.keycode = OS.find_keycode_from_string(text[i].to_upper())
		Input.parse_input_event(ev)
		var up := ev.duplicate() as InputEventKey
		up.pressed = false
		Input.parse_input_event(up)
	await frames(3)


## Moves the focus with ui_down (then ui_up) until the screen's control `key` has it; true when
## it got there. Only the arrow actions are used, as a gamepad player would.
static func focus_to(screen_node: Node, key: String) -> bool:
	var target: Control = screen_node.control(key)
	if target == null:
		return false
	for action: String in ["ui_down", "ui_up"]:
		for i in 25:
			if focused() == target:
				return true
			var before := focused()
			await act(action)
			if focused() == before:
				break  # the end of the focus chain in this direction
	return focused() == target


## Binds the letter keys W and S to ui_up and ui_down, as a game with WASD menus does; returns
## the events added, for unbind_letters().
static func bind_letters() -> Array[InputEvent]:
	var added: Array[InputEvent] = []
	for pair: Array in [["ui_up", KEY_W], ["ui_down", KEY_S]]:
		var ev := InputEventKey.new()
		ev.keycode = pair[1]
		InputMap.action_add_event(pair[0], ev)
		added.append(ev)
	return added


## Takes the bind_letters() events out of the InputMap again.
static func unbind_letters(added: Array[InputEvent]) -> void:
	for i in added.size():
		InputMap.action_erase_event(["ui_up", "ui_down"][i], added[i])


## focus_to() with the real arrow keys (KEY_DOWN, then KEY_UP), as a keyboard player moves.
static func key_to(screen_node: Node, key_name: String) -> bool:
	var target: Control = screen_node.control(key_name)
	if target == null:
		return false
	for code: Key in [KEY_DOWN, KEY_UP]:
		for i in 25:
			if focused() == target:
				return true
			var before := focused()
			await key(code)
			if focused() == before:
				break  # the end of the focus chain, or a control that kept the key
	return focused() == target


## Pays the session on the fake chain in a new block and lets it confirm (Arbitrum: 20).
static func pay_on_chain(env: Dictionary, ctl: ScfgCheckoutController) -> void:
	var chain: F.FakeChain = env["chains"]["arbitrum"]
	var block := chain.head + 1
	chain.logs.append(F.transfer_log(USDC_ARB, PAYER, DEV, ctl.session["units"], {"block": block, "tx_hash": TX}))
	chain.head = block
	await pass_(env["node"])
	chain.head = block + 19
	await pass_(env["node"])


## Every text a player can see in `node`'s subtree: visible Labels, Buttons and LineEdit
## placeholders.
static func visible_texts(node: Node) -> Array[String]:
	var out: Array[String] = []
	_collect(node, out)
	return out


static func _collect(node: Node, out: Array[String]) -> void:
	if node is CanvasItem and not (node as CanvasItem).is_visible_in_tree():
		return
	if node is Label and (node as Label).text != "":
		out.append((node as Label).text)
	elif node is Button and (node as Button).text != "":
		out.append((node as Button).text)
	elif node is LineEdit and (node as LineEdit).placeholder_text != "":
		out.append((node as LineEdit).placeholder_text)
	for child in node.get_children():
		_collect(child, out)
