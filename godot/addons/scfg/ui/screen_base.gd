## What the addon's two screens share (ScfgSheet, ui/sheet.gd, and ScfgRestoreScreen,
## ui/restore.gd): a full-screen Control that dims the game and blocks its input, with the light
## sheet docked on the right (the Margin look, ScfgTheme), keyboard and gamepad focus wiring, and
## small builders for labels, buttons, options and fields.
##
## A screen rebuilds its content from `_process` when the subclass's `_signature()` changes. Every
## text goes through tr(); auto-translation is off so addresses and amounts are never translated.
##
## Focus: each build lists its focusable controls in visual order. Up and down (and Tab) move
## along that list, left and right within a row of buttons, and the ends stay put, so the focus
## never leaves the screen for the game behind it. `ui_cancel` calls `_on_ui_cancel()`. The focus
## the game had goes back to it when the screen closes.
##
## Several screens can be open at once, the later one drawn on top. A rebuild never takes the focus
## from a screen on top, and when that one closes the focus returns to the screen below it.
class_name ScfgScreenBase
extends Control

## The player is done with the screen (see sheet_host.gd for the values of `how`).
signal closed(how: String)

const _AUTOLOAD := preload("res://addons/scfg/scfg.gd")
## Characters a hash field takes (hex or base58): the autoload's TX_CHARS.
const TX_CHARS: String = _AUTOLOAD.TX_CHARS
## The most characters a hash field takes: the autoload's TX_MAX_LENGTH.
const TX_MAX_LENGTH: int = _AUTOLOAD.TX_MAX_LENGTH

## The Scfg autoload (the screen calls its actions: open_browser, copy, paste_into, ...).
var scfg: Variant = null
## The design scale (ScfgTheme.scale_for): 1.0 on a 1280x720 game.
var k := 1.0

var _panel: PanelContainer
var _layout: VBoxContainer
var _controls := {}  # key -> Control of the current build
var _order: Array[Control] = []  # focusable controls of the current build, in visual order
var _rows := {}  # Control -> row id: left / right move within a row
var _sig: Variant = null  # what the current build shows
var _ended := false
var _back_focus: WeakRef = weakref(null)
var _back_screen: WeakRef = weakref(null)  # the screen below that had the focus when this one opened
var _back_key := ""  # which of its controls had it


## A small filled circle (the step dots, the status dot).
class _Dot:
	extends Control
	var color := Color.WHITE

	func _init(c: Color, d: float) -> void:
		color = c
		custom_minimum_size = Vector2(d, d)
		size_flags_vertical = Control.SIZE_SHRINK_CENTER
		mouse_filter = Control.MOUSE_FILTER_IGNORE

	func _draw() -> void:
		draw_circle(size / 2.0, minf(size.x, size.y) / 2.0, color)


func _init() -> void:
	auto_translate_mode = Node.AUTO_TRANSLATE_MODE_DISABLED
	mouse_filter = Control.MOUSE_FILTER_STOP
	process_mode = Node.PROCESS_MODE_ALWAYS  # usable while the game is paused
	set_anchors_preset(Control.PRESET_FULL_RECT)


func _ready() -> void:
	var owner_now := get_viewport().gui_get_focus_owner()
	if owner_now != null and not is_ancestor_of(owner_now):
		_back_focus = weakref(owner_now)
		var below := _screen_of(owner_now)
		if below != null:
			_back_screen = weakref(below)
			_back_key = below._focused_key()
	_frame()
	get_viewport().size_changed.connect(_on_resized)
	modulate.a = 0.0
	create_tween().tween_property(self, "modulate:a", 1.0, 0.22)
	_update()


func _exit_tree() -> void:
	# A screen drawn over this one keeps the focus (else the keyboard would reach the game behind
	# it); if it opened over this one, it inherits where this one would give the focus back.
	var top := _screen_of(get_viewport().gui_get_focus_owner())
	if top != null and top != self and top.is_inside_tree() and not top._ended:
		if top._back_screen.get_ref() == self:
			top._back_focus = _back_focus
			top._back_screen = _back_screen
			top._back_key = _back_key
		return
	var back: Variant = _back_focus.get_ref()
	if back is Control and (back as Control).is_inside_tree() and (back as Control).is_visible_in_tree():
		(back as Control).grab_focus()
		return
	# the control is gone (the screen below rebuilt meanwhile): the same key in its new build
	var below: Variant = _back_screen.get_ref()
	if below is ScfgScreenBase and (below as ScfgScreenBase).is_inside_tree() and not (below as ScfgScreenBase)._ended:
		(below as ScfgScreenBase)._focus((below as ScfgScreenBase)._primary(), _back_key)


func _process(_delta: float) -> void:
	_update()


## Rebuild when what the screen shows changed. Subclasses provide `_signature()` and `_build()`.
func _update() -> void:
	if _ended or not is_inside_tree() or not _ready_to_show():
		return
	_sync()
	var sig: Variant = _signature()
	if sig == _sig:
		return
	var keep := _focused_key()
	var moved := _moved(_sig, sig)
	_sig = sig
	_clear(_layout)
	_controls.clear()
	_order.clear()
	_rows.clear()
	_build()
	_wire_focus()
	if not _covered():
		_focus(_primary(), "" if moved else keep)


## Close the screen once: emits `closed(how)`.
func close_with(how: String) -> void:
	if _ended:
		return
	_ended = true
	closed.emit(how)


## The control the current build registered under `key`, or null.
func control(key: String) -> Control:
	var c: Variant = _controls.get(key)
	return c if is_instance_valid(c) else null


## The focusable controls of the current build, in focus order.
func focus_order() -> Array[Control]:
	return _order.filter(func(c: Control) -> bool: return is_instance_valid(c) and c.visible)


# --- for subclasses --------------------------------------------------------------------------


func _ready_to_show() -> bool:
	return true


func _signature() -> Variant:
	return null


func _build() -> void:
	pass


## True when the change from `before` to `after` is a move to another step (the focus goes to the
## new step's primary control instead of staying where it was).
func _moved(_before: Variant, _after: Variant) -> bool:
	return false


func _primary() -> String:
	return ""


## Copies the controller's field values into the text fields when they changed from outside.
func _sync() -> void:
	pass


func _on_ui_cancel() -> void:
	pass


func px(n: float) -> int:
	return ScfgTheme.px(n, k)


func color(name: String) -> Color:
	return get_theme_color(name, "ScfgSheet")


## Reports an error the controller returned (unreachable in practice) for the developer.
func _report(err: Variant) -> void:
	if scfg != null and scfg.has_method("_report_error"):
		scfg._report_error("screen:", err)


## Registers `c` under `key` (its node name too); a focusable one joins the focus order, and
## `row` groups buttons side by side for left / right.
func reg(c: Control, key: String, row: String = "") -> Control:
	c.name = key.validate_node_name()
	_controls[key] = c
	if c.focus_mode != Control.FOCUS_NONE:
		_order.append(c)
		if row != "":
			_rows[c] = row
	return c


func label(text: String, variation: String = "", wrap: bool = true) -> Label:
	var l := Label.new()
	l.text = text
	l.theme_type_variation = variation
	if wrap:
		l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		l.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	return l


## A value that may be long and has no spaces (an address, a hash): wraps anywhere.
func mono(text: String) -> Label:
	var l := label(text, "ScfgMono")
	l.autowrap_mode = TextServer.AUTOWRAP_ARBITRARY
	return l


func button(key: String, text: String, variation: String, on_press: Callable, row: String = "") -> Button:
	var b := Button.new()
	b.text = text
	b.theme_type_variation = variation
	b.focus_mode = Control.FOCUS_ALL
	b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	b.pressed.connect(on_press)
	reg(b, key, row)
	return b


## A segmented option: a label and an optional smaller second line, filled with the accent when
## selected.
func option(key: String, text: String, sub: String, selected: bool, on_press: Callable, row: String = "") -> Button:
	var b := button(key, "", "ScfgOptionSelected" if selected else "ScfgOption", on_press, row)
	var margin := MarginContainer.new()
	margin.mouse_filter = Control.MOUSE_FILTER_IGNORE
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for side: String in ["margin_left", "margin_right"]:
		margin.add_theme_constant_override(side, px(6))
	for side: String in ["margin_top", "margin_bottom"]:
		margin.add_theme_constant_override(side, px(7))
	var box := VBoxContainer.new()
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.alignment = BoxContainer.ALIGNMENT_CENTER
	box.add_theme_constant_override("separation", 0)
	var suffix := "Selected" if selected else ""
	for part: Array in [[text, "ScfgOptionText"], [sub, "ScfgOptionSub"]]:
		if part[0] == "":
			continue
		var l := label(part[0], part[1] + suffix, false)
		l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		l.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		l.mouse_filter = Control.MOUSE_FILTER_IGNORE
		box.add_child(l)
	margin.add_child(box)
	b.add_child(margin)
	var fit := func() -> void:
		if is_instance_valid(b) and is_instance_valid(margin):
			b.custom_minimum_size.y = margin.get_combined_minimum_size().y
	margin.minimum_size_changed.connect(fit)
	fit.call()
	return b


## A grid of options, `columns` wide at most. Give each option its grid row as its focus row.
func option_grid(key: String, columns: int) -> GridContainer:
	var g := GridContainer.new()
	g.name = key.validate_node_name()
	g.columns = maxi(1, columns)
	g.add_theme_constant_override("h_separation", px(8))
	g.add_theme_constant_override("v_separation", px(8))
	return g


## A text field: at most `max_length` characters, only those in `allowed`; `on_text(text)` gets
## the cleaned text, `on_submit()` the Enter key.
func field(key: String, text: String, max_length: int, allowed: String, on_text: Callable, on_submit: Callable,
		row: String = "") -> LineEdit:
	var f := LineEdit.new()
	f.text = text
	f.max_length = max_length
	# `text =` and `max_length =` leave the caret at 0: a field rebuilt while the player types
	# would take the next characters in front of the text
	f.caret_column = f.text.length()
	f.focus_mode = Control.FOCUS_ALL
	f.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	f.text_changed.connect(func(t: String) -> void:
		var clean := ""
		for i in t.length():
			if allowed.contains(t[i]):
				clean += t[i]
		if clean != t and is_instance_valid(f):
			f.text = clean
			f.caret_column = clean.length()
		on_text.call(clean))
	f.text_submitted.connect(func(_t: String) -> void: on_submit.call())
	# Godot 4.4+ gives a LineEdit an edit mode: one the focus reaches by keyboard or gamepad takes
	# no typing until Enter. Start editing on focus, as a mouse click does and as 4.3 always does.
	f.focus_entered.connect(func() -> void:
		if is_instance_valid(f) and f.has_method("edit") and not f.call("is_editing"):
			f.call("edit"))
	reg(f, key, row)
	return f


## A button beside a field or in a row, `width` design pixels wide at least.
func fixed_width(b: Button, width: float) -> Button:
	b.size_flags_horizontal = Control.SIZE_FILL
	b.custom_minimum_size.x = px(width)
	return b


## A vertical scrolling body (named "Body", in a ScrollContainer named "Scroll" added to `parent`)
## that keeps the focused control in view.
func scrolling_body(parent: Container) -> VBoxContainer:
	var scroll := ScrollContainer.new()
	scroll.name = "Scroll"
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.follow_focus = true
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	var body := vbox(10)
	body.name = "Body"
	body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(body)
	parent.add_child(scroll)
	return body


func hbox(separation: float = 8) -> HBoxContainer:
	var h := HBoxContainer.new()
	h.add_theme_constant_override("separation", px(separation))
	return h


func vbox(separation: float = 10) -> VBoxContainer:
	var v := VBoxContainer.new()
	v.add_theme_constant_override("separation", px(separation))
	return v


func rule() -> Panel:
	var p := Panel.new()
	p.theme_type_variation = "ScfgRule"
	p.custom_minimum_size = Vector2(0, maxi(1, px(1.5)))
	p.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return p


func dot(c: Color) -> Control:
	return _Dot.new(c, get_theme_constant("dot_size", "ScfgSheet"))


## The status line: a dot in `c` and the text.
func status_row(text: String, c: Color) -> HBoxContainer:
	var h := hbox(10)
	h.add_child(dot(c))
	h.add_child(label(text, "ScfgStatus"))
	return h


## The developer's logo, fitted into 240x64 design pixels, or null.
func logo() -> Control:
	var tex: Variant = scfg.logo_texture() if scfg != null else null
	if not tex is Texture2D:
		return null
	var t: Texture2D = tex
	var fit := minf(px(240) / maxf(1.0, t.get_width()), px(64) / maxf(1.0, t.get_height()))
	var r := TextureRect.new()
	r.texture = t
	r.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	r.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT
	r.custom_minimum_size = Vector2(t.get_width() * fit, t.get_height() * fit)
	r.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	r.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return reg(r, "logo")


# --- internals -------------------------------------------------------------------------------


func _frame() -> void:
	var size_now := get_viewport_rect().size
	k = ScfgTheme.scale_for(size_now)
	theme = ScfgTheme.for_screen(scfg, k)
	for child in get_children():
		remove_child(child)
		child.queue_free()
	var dim := ColorRect.new()
	dim.name = "Dim"
	dim.color = color("dim")
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(dim)
	_panel = PanelContainer.new()
	_panel.name = "Sheet"
	_panel.theme_type_variation = "ScfgSheetPanel"
	var w := ScfgTheme.sheet_width(size_now, k)
	_panel.anchor_left = 0.0 if w >= size_now.x else 1.0
	_panel.anchor_right = 1.0
	_panel.anchor_top = 0.0
	_panel.anchor_bottom = 1.0
	_panel.offset_left = 0.0 if w >= size_now.x else -w
	_panel.offset_right = 0.0
	_panel.offset_top = 0.0
	_panel.offset_bottom = 0.0
	add_child(_panel)
	_layout = vbox(14)
	_layout.name = "Layout"
	_panel.add_child(_layout)


func _on_resized() -> void:
	if _ended or not is_inside_tree():
		return
	_frame()
	_sig = null
	_update()


static func _clear(container: Node) -> void:
	if container == null:
		return
	for child in container.get_children():
		container.remove_child(child)
		child.queue_free()


## The screen `node` is part of (null: the game's own UI).
static func _screen_of(node: Node) -> ScfgScreenBase:
	while node != null and not node is ScfgScreenBase:
		node = node.get_parent()
	return node as ScfgScreenBase


## True while another screen drawn over this one has the focus.
func _covered() -> bool:
	var owner_now := get_viewport().gui_get_focus_owner()
	var other := _screen_of(owner_now)
	if other == null or other == self or not other.is_inside_tree():
		return false
	var mine := _layer_of(self)
	var theirs := _layer_of(other)
	return theirs > mine or (theirs == mine and other.is_greater_than(self))


## The CanvasLayer index `node` is drawn on (0: the default canvas).
static func _layer_of(node: Node) -> int:
	while node != null and not node is CanvasLayer:
		node = node.get_parent()
	return (node as CanvasLayer).layer if node != null else 0


func _focused_key() -> String:
	var owner_now := get_viewport().gui_get_focus_owner() if is_inside_tree() else null
	for key: String in _controls:
		if _controls[key] == owner_now:
			return key
	return ""


func _wire_focus() -> void:
	var order := focus_order()
	var n := order.size()
	for i in n:
		var c: Control = order[i]
		var prev: Control = order[i - 1] if i > 0 else c
		var next: Control = order[i + 1] if i < n - 1 else c
		c.focus_neighbor_top = c.get_path_to(prev)
		c.focus_neighbor_bottom = c.get_path_to(next)
		c.focus_previous = c.get_path_to(order[(i - 1 + n) % n])
		c.focus_next = c.get_path_to(order[(i + 1) % n])
		var row: Variant = _rows.get(c)
		var left: Control = c
		var right: Control = c
		if row != null:
			if i > 0 and _rows.get(order[i - 1]) == row:
				left = order[i - 1]
			if i < n - 1 and _rows.get(order[i + 1]) == row:
				right = order[i + 1]
		c.focus_neighbor_left = c.get_path_to(left)
		c.focus_neighbor_right = c.get_path_to(right)


func _focus(primary: String, keep: String) -> void:
	var order := focus_order()
	var c: Control = control(keep) if keep != "" else null
	if c == null or not order.has(c):
		c = control(primary)
	if (c == null or not order.has(c)) and not order.is_empty():
		c = order[0]
	if c != null and c.is_inside_tree():
		c.grab_focus()
		# The new build is not laid out yet, so a scrolling body's follow_focus had nothing to
		# scroll to: bring the focused control into view on the next frame.
		var frame := get_tree().process_frame
		if not frame.is_connected(_reveal_focused):
			frame.connect(_reveal_focused, CONNECT_ONE_SHOT)


## Scrolls the body so that this screen's focused control is in view.
func _reveal_focused() -> void:
	if _ended or not is_inside_tree():
		return
	var c := get_viewport().gui_get_focus_owner()
	if c == null or not is_ancestor_of(c):
		return
	var p := c.get_parent()
	while p != null and p != self:
		if p is ScrollContainer:
			(p as ScrollContainer).ensure_control_visible(c)
			return
		p = p.get_parent()


func _input(event: InputEvent) -> void:
	if _ended or not is_visible_in_tree():
		return
	if event.is_action_pressed("ui_cancel", false, true):
		get_viewport().set_input_as_handled()
		_on_ui_cancel()
	elif event is InputEventKey:
		_arrow_out_of_field(event)


## A focused LineEdit keeps the up and down arrow keys (caret to start / end), so a keyboard
## player could never leave it: move the focus instead. Only the arrow keys themselves, since a game
## may bind letters (WASD) to ui_up / ui_down and those are hash characters; modified arrows
## (exact match) stay with the field.
func _arrow_out_of_field(event: InputEventKey) -> void:
	var code := event.keycode if event.keycode != KEY_NONE else event.physical_keycode
	if code != KEY_UP and code != KEY_DOWN:
		return
	var f := get_viewport().gui_get_focus_owner()
	if not f is LineEdit or not _order.has(f):
		return
	var side := &""
	if event.is_action_pressed("ui_down", true, true):
		side = &"focus_neighbor_bottom"
	elif event.is_action_pressed("ui_up", true, true):
		side = &"focus_neighbor_top"
	else:
		return
	get_viewport().set_input_as_handled()
	var path: NodePath = f.get(side)
	var target := f.get_node_or_null(path) as Control if not path.is_empty() else null
	if target != null and target != f and target.is_visible_in_tree():
		target.grab_focus()


## The game behind the screen gets no keys, buttons or shortcuts while it is open (the mouse is
## stopped by the dimmed backdrop).
func _shortcut_input(event: InputEvent) -> void:
	_swallow(event)


func _unhandled_input(event: InputEvent) -> void:
	_swallow(event)


func _swallow(event: InputEvent) -> void:
	if not _ended and is_visible_in_tree() and not event is InputEventMouse:
		get_viewport().set_input_as_handled()
