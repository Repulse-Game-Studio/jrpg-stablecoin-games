## The demo's end-to-end driver (tools/run_godot_e2e.py on desktop, tools/run_godot_web_e2e.py in a
## web export): it plays the demo the way a player does, over stdin or a page hook. It finds the
## buttons on the real payment sheet and in the game, clicks them with mouse events pushed into the
## viewport (or presses input actions), and reports what the screen shows. It never calls the
## checkout controller.
##
## Loaded only in e2e runs, as an extra autoload: on desktop through a temporary, git-ignored
## `override.cfg`; on the web through a temporary copy of the demo whose project.godot lists it (an
## export ignores override.cfg). Without the demo's e2e config (`SCFG_E2E_CONFIG`, or
## `window.scfgE2EConfig` on the web) it does nothing, so a stray override.cfg is harmless.
##
## Protocol (as tests/integration/gd_driver.py): one JSON command per stdin line, one reply per
## command, in order, printed as a line that starts with REPLY_PREFIX (Godot prints its own banner
## on stdout): {"ok": <result>} or {"error": "<why>"}. Commands:
##   snapshot              what the player sees: for the open screens (newest last, with their
##                         kind: "sheet" or "restore") and the game scene, the visible texts in
##                         tree order, the visible buttons ({name, text, disabled, rect}) and the
##                         focused button's name; plus the URLs the game opened, the texts it
##                         copied, whether the tree is paused and the viewport size
##   click {name, in,      a mouse click (motion, press, release) at the centre of the visible,
##          press?}        enabled button named `name` (its node name: the sheet's control keys,
##                         such as method_manual or copy_amount) in the newest screen
##                         (in = "screen") or in the game scene (in = "game"). A button below the
##                         fold of the sheet's scrolling body is first scrolled into view with the
##                         mouse wheel, as a player would. Replies {at, wheel_notches}. With
##                         press = false it only scrolls and replies where to click: the web
##                         runner clicks there with the browser's own mouse (a real click is a
##                         user gesture, which the browser's clipboard needs).
##   action {action}       press and release an input action (ui_accept, ui_down, ...)
##   owns {item}           Scfg.owns(item)
##   user_dir              OS.get_user_data_dir() (where user://scfg/state.json lives)
##   state                 user://scfg/state.json as the game last wrote it (null if absent)
##   quit                  reply, then quit the game the normal way (the autoload saves its state)
## The end of stdin quits too.
##
## Web: the page calls `window.scfgE2E(<command JSON text>)` (a JavaScriptBridge callback, made
## only when `window.scfgE2EConfig` is set) and each reply's JSON text is appended to the array
## `window.scfgE2EReplies`, in order.
##
## Desktop test seams (set up before the demo's _ready configures the addon; the web needs none:
## its runner points the chain at Anvil through the config's own rpc_urls):
## - SCFG_E2E_CHAIN (JSON {network: "mainnet"|"testnet", key, rpc: [url], confirmations?,
##   tokens?: {name: address}}): the built-in chain entry points at the local chain and its mock
##   token, as the Ren'Py e2e game and godot/tests/integration/engine_driver.gd do.
## - The autoload's platform (`Scfg._setup({"platform": ...})`, its existing test hook): the
##   browser (shell_open) and the clipboard are recorded instead of reaching the desktop.
## - Headless only: the root window gets the project's 1280x720 (the headless display server's
##   window is 64x64).
##
## Stdin is read on a thread, since OS.read_string_from_stdin blocks and the game must keep running
## its frames. The thread ends after "quit" or at the end of stdin and is joined at exit; a game
## that quits on its own prints QUITTING_LINE first, so the runner closes stdin and ends the read.
extends Node

const REPLY_PREFIX := "SCFG-REPLY "
## Printed when the game quits while the reader still waits on stdin (tools/run_godot_e2e.py's
## QUITTING): the runner closes stdin so the reader ends and can be joined.
const QUITTING_LINE := "SCFG-QUITTING"
const CONFIG_ENV := "SCFG_E2E_CONFIG"
const CHAIN_ENV := "SCFG_E2E_CHAIN"
## Godot 4.4+ stdin read size (bytes, including fgets' terminator): the longest command line + 1.
const STDIN_BUFFER := 1 << 16
## How far a click scrolls the sheet's body to bring its button into view.
const MAX_WHEEL_NOTCHES := 40


## The desktop as the e2e sees it: the browser and the clipboard are recorded.
class _Platform:
	extends RefCounted
	var opened: Array[String] = []
	var copied: Array[String] = []

	func is_web() -> bool:
		return false

	func is_mobile() -> bool:
		return false

	func shell_open(url: String) -> void:
		opened.append(url)

	func clipboard_set(text: String) -> void:
		copied.append(text)

	func clipboard_get() -> String:
		return copied.back() if not copied.is_empty() else ""

	func is_userfs_persistent() -> bool:
		return true


var _platform := _Platform.new()
var _thread: Thread = null
var _mutex := Mutex.new()
var _lines: Array[String] = []
var _eof := false
var _reader_done := false  # the reader read "quit" or the end of stdin: it ends without more input
var _quitting := false
var _busy := false  # a command is running (it may await frames)
var _web := OS.has_feature("web")
## Web: window.scfgE2E. Held here, since the page only keeps a handle to the Godot callable.
var _js_hook: JavaScriptObject = null


func _ready() -> void:
	if _web:
		_ready_web()
		return
	if OS.get_environment(CONFIG_ENV) == "":
		queue_free()
		return
	process_mode = Node.PROCESS_MODE_ALWAYS  # the demo pauses the tree while the sheet is open
	var bad := _apply_chain(OS.get_environment(CHAIN_ENV))
	if bad != "":
		push_error("e2e driver: %s: %s" % [CHAIN_ENV, bad])
		get_tree().quit(1)
		return
	var scfg := get_node_or_null("/root/Scfg")
	if scfg == null:
		push_error("e2e driver: the Scfg autoload is missing")
		get_tree().quit(1)
		return
	scfg.call("_setup", {"platform": _platform})
	if DisplayServer.get_name() == "headless":
		# The headless display server has a 64x64 window: give the game the size it was made for,
		# so the sheet is laid out (and clicked) as a player at that size sees it.
		get_tree().root.size = Vector2i(ProjectSettings.get_setting("display/window/size/viewport_width"),
			ProjectSettings.get_setting("display/window/size/viewport_height"))
	_thread = Thread.new()
	_thread.start(_read_stdin)


## Web: commands come from the page (window.scfgE2E), only in a run the demo's web e2e config
## drives. The real browser, clipboard and IndexedDB are used.
func _ready_web() -> void:
	# (a string answer: JavaScriptBridge.eval gives a JS boolean back as an int in 4.3)
	var config: Variant = JavaScriptBridge.eval("typeof window.scfgE2EConfig === 'string' && window.scfgE2EConfig !== '' ? 'set' : ''", true)
	if config != "set":
		queue_free()
		return
	process_mode = Node.PROCESS_MODE_ALWAYS
	JavaScriptBridge.eval("window.scfgE2EReplies = [];", true)
	_js_hook = JavaScriptBridge.create_callback(_on_page_command)
	JavaScriptBridge.get_interface("window").set("scfgE2E", _js_hook)


## window.scfgE2E(line): queues the command; _process runs it on a later frame.
func _on_page_command(args: Array) -> void:
	_mutex.lock()
	_lines.append(str(args[0]) if not args.is_empty() else "")
	_mutex.unlock()


func _exit_tree() -> void:
	if _thread == null or not _thread.is_started():
		return
	_mutex.lock()
	var done := _reader_done
	_mutex.unlock()
	if not done:
		# the game quits on its own while the reader waits on stdin: ask the runner to close it
		print(QUITTING_LINE)
	_thread.wait_to_finish()


func _process(_delta: float) -> void:
	if _busy or _quitting:
		return
	_mutex.lock()
	var line: Variant = _lines.pop_front() if not _lines.is_empty() else null
	var eof := _eof and _lines.is_empty()
	_mutex.unlock()
	if line != null:
		_busy = true
		await _serve(line)
		_busy = false
	elif eof:
		_quitting = true
		get_tree().quit(0)


## Runs one command line and prints its reply (a command may take a few frames: a click that
## first scrolls its button into view).
func _serve(line: String) -> void:
	var cmd: Variant = JSON.parse_string(line)
	var reply: Dictionary
	if not (cmd is Dictionary and cmd.get("op") is String):
		reply = {"error": "not a command object: %s" % line}
	else:
		reply = await _run(cmd)
	if _web:
		JavaScriptBridge.eval("window.scfgE2EReplies.push(%s);" % JSON.stringify(JSON.stringify(reply)), true)
	else:
		print(REPLY_PREFIX + JSON.stringify(reply))
	if _quitting:
		get_tree().quit(0)


## The reply to one command.
func _run(cmd: Dictionary) -> Dictionary:
	match str(cmd["op"]):
		"snapshot":
			return {"ok": _snapshot()}
		"click":
			return await _click(str(cmd.get("name", "")), str(cmd.get("in", "screen")), cmd.get("press", true) != false)
		"action":
			return _action(str(cmd.get("action", "")))
		"owns":
			return {"ok": Scfg.owns(cmd.get("item"))}
		"user_dir":
			return {"ok": OS.get_user_data_dir()}
		"state":
			var path := "user://scfg/state.json"
			return {"ok": JSON.parse_string(FileAccess.get_file_as_string(path)) if FileAccess.file_exists(path) else null}
		"quit":
			_quitting = true
			return {"ok": null}
	return {"error": "unknown op %s" % JSON.stringify(cmd["op"])}


# --- reading the screen ----------------------------------------------------------------------


func _screens() -> Array:
	var host: Variant = Scfg.sheet_host
	if host is Object and (host as Object).has_method("screens"):
		return (host as Object).call("screens")
	return []


func _snapshot() -> Dictionary:
	var screens := []
	for s: Variant in _screens():
		if s is Control and (s as Control).is_inside_tree():
			var view := _view(s)
			view["kind"] = "sheet" if s is ScfgSheet else ("restore" if s is ScfgRestoreScreen else "other")
			screens.append(view)
	var scene := get_tree().current_scene
	var size := get_viewport().get_visible_rect().size
	return {"screens": screens, "game": _view(scene) if scene != null else {},
		"opened": _platform.opened.duplicate(), "copied": _platform.copied.duplicate(),
		"paused": get_tree().paused, "viewport": [size.x, size.y]}


## What `root` shows: {texts, buttons: [{name, text, disabled, rect: [x, y, w, h]}], focus}.
func _view(root: Node) -> Dictionary:
	var texts: Array[String] = []
	var buttons := []
	_collect(root, texts, buttons)
	var focus := get_viewport().gui_get_focus_owner()
	return {"texts": texts, "buttons": buttons,
		"focus": str(focus.name) if focus != null and root.is_ancestor_of(focus) else null}


func _collect(node: Node, texts: Array[String], buttons: Array) -> void:
	if node is CanvasItem and not (node as CanvasItem).visible:
		return
	if node is Label and (node as Label).text != "":
		texts.append((node as Label).text)
	elif node is LineEdit and (node as LineEdit).text != "":
		texts.append((node as LineEdit).text)
	elif node is Button:
		var b := node as Button
		var label := b.text
		if label == "":  # an option button: its text is in the labels inside it
			var inner: Array[String] = []
			_collect_labels(b, inner)
			label = " / ".join(inner)
		if label != "":
			texts.append(label)
		var r := b.get_global_rect()
		buttons.append({"name": str(b.name), "text": label, "disabled": b.disabled,
			"rect": [r.position.x, r.position.y, r.size.x, r.size.y]})
		return
	for child in node.get_children():
		_collect(child, texts, buttons)


func _collect_labels(node: Node, out: Array[String]) -> void:
	for child in node.get_children():
		if child is CanvasItem and not (child as CanvasItem).visible:
			continue
		if child is Label and (child as Label).text != "":
			out.append((child as Label).text)
		_collect_labels(child, out)


# --- input -----------------------------------------------------------------------------------


func _click(name: String, where: String, press: bool = true) -> Dictionary:
	var root: Node = null
	if where == "game":
		root = get_tree().current_scene
	else:
		var screens := _screens()
		root = screens.back() if not screens.is_empty() else null
	if root == null:
		return {"error": "no %s to click in" % where}
	var found := root.find_child(name, true, false)
	if not found is BaseButton:
		return {"error": "no button %s in the %s" % [name, where]}
	var b := found as BaseButton
	if not b.is_visible_in_tree():
		return {"error": "button %s is hidden" % name}
	if b.disabled:
		return {"error": "button %s is disabled" % name}
	# A button below the fold of the sheet's scrolling body: scroll the mouse wheel over it.
	var scroll := _scroll_parent(b)
	var notches := 0
	while scroll != null and not scroll.get_global_rect().has_point(_centre(b)):
		if notches == MAX_WHEEL_NOTCHES or not is_instance_valid(b):
			return {"error": "button %s stays out of view after %d wheel notches" % [name, notches]}
		var down := _centre(b).y > scroll.get_global_rect().get_center().y
		_mouse(MOUSE_BUTTON_WHEEL_DOWN if down else MOUSE_BUTTON_WHEEL_UP, scroll.get_global_rect().get_center())
		notches += 1
		await get_tree().process_frame  # the container moves its content at the end of the frame
	var at := _centre(b)
	if not get_viewport().get_visible_rect().has_point(at):
		return {"error": "button %s is off screen at %s" % [name, at]}
	if press:
		_mouse(MOUSE_BUTTON_LEFT, at)
	return {"ok": {"at": [at.x, at.y], "wheel_notches": notches}}


## The middle of `c` in viewport coordinates (the screens' CanvasLayer has no offset or scale).
static func _centre(c: Control) -> Vector2:
	return c.get_global_rect().get_center()


## The mouse moves to `at`, then `button` goes down and up there.
func _mouse(button: MouseButton, at: Vector2) -> void:
	var move := InputEventMouseMotion.new()
	move.position = at
	move.global_position = at
	get_viewport().push_input(move, true)
	for pressed: bool in [true, false]:
		var ev := InputEventMouseButton.new()
		ev.button_index = button
		ev.pressed = pressed
		ev.factor = 1.0
		ev.position = at
		ev.global_position = at
		get_viewport().push_input(ev, true)


func _scroll_parent(c: Control) -> ScrollContainer:
	var n := c.get_parent()
	while n != null:
		if n is ScrollContainer:
			return n
		n = n.get_parent()
	return null


func _action(action: String) -> Dictionary:
	if not InputMap.has_action(action):
		return {"error": "unknown input action %s" % action}
	for pressed: bool in [true, false]:
		var ev := InputEventAction.new()
		ev.action = action
		ev.pressed = pressed
		Input.parse_input_event(ev)
	return {"ok": null}


# --- set-up ----------------------------------------------------------------------------------


## Points ScfgChains.MAINNET/TESTNET[key] at the local chain (the JSON in `text`); "" when done
## (or when `text` is empty), else why not.
func _apply_chain(text: String) -> String:
	if text == "":
		return ""
	var o: Variant = JSON.parse_string(text)
	if not (o is Dictionary and o.get("rpc") is Array):
		return "needs a JSON object with an rpc list"
	o = ScfgPy.from_json(o)
	var registry: Dictionary = ScfgChains.TESTNET if o.get("network") == "testnet" else ScfgChains.MAINNET
	var key: Variant = o.get("key")
	if not registry.has(key):
		return "unknown chain %s" % JSON.stringify(key)
	var next: Dictionary = (registry[key] as Dictionary).duplicate()
	next["rpc"] = (o["rpc"] as Array).duplicate()
	if o.get("confirmations") != null:
		next["confirmations"] = o["confirmations"]
	if o.get("tokens") != null:
		if not o["tokens"] is Dictionary:
			return "tokens must be an object"
		var tokens := {}
		for token_name: Variant in o["tokens"]:
			var token: Variant = ScfgChains.make_token(o["tokens"][token_name])
			if token is ScfgErr:
				return str(token)
			tokens[token_name] = token
		next["tokens"] = tokens
	registry[key] = next
	return ""


## The stdin reader (a thread): queues each command line; stops after "quit" or at the end.
func _read_stdin() -> void:
	while true:
		var line := _read_line()
		var cmd: Variant = JSON.parse_string(line) if line != "" else null
		var last: bool = line == "" or (cmd is Dictionary and cmd.get("op") == "quit")
		_mutex.lock()
		if line == "":
			_eof = true
		else:
			_lines.append(line)
		_reader_done = last
		_mutex.unlock()
		if last:
			return


## One command line without its newline, or "" at the end of stdin (engine_driver.gd's reader).
func _read_line() -> String:
	if Engine.get_version_info()["hex"] >= 0x040400:
		# (a dynamic call: 4.3's read_string_from_stdin takes no argument)
		return str(OS.call("read_string_from_stdin", STDIN_BUFFER)).strip_edges()
	var buf := ""
	while not buf.ends_with("\n"):
		var piece := OS.read_string_from_stdin()
		if piece == "":
			break
		buf += piece
	return buf.strip_edges()
