## The "Margin" look of the payment sheet and restore screen, the twin of the scfg_* styles in
## plugin/game/scfg/scfg_screens.rpy: a light sheet, ink text, outlined buttons and options with
## rounded corners (StyleBoxFlat), and the developer's accent color for the selected option, the
## pinned forward button and the progress. Sizes are designed for a 1280x720 game and scaled by
## `scale_for(viewport size)`, as Ren'Py's `_k()`.
##
## `for_screen(scfg, k)` is the Theme a screen uses: `build()` with the configured accent, merged
## with the Theme given to `Scfg.configure({"theme": ...})` (the developer's definitions win; the
## developer's Theme itself is not changed).
##
## Type variations (set a control's `theme_type_variation` to one of these):
##   Label: ScfgTitle, ScfgHeading, ScfgAmount, ScfgAmountToken, ScfgNote, ScfgLabel, ScfgMuted,
##     ScfgWarning, ScfgDev, ScfgError, ScfgOk, ScfgStatus, ScfgMono, ScfgQrStep, ScfgOptionText,
##     ScfgOptionSub, ScfgOptionTextSelected, ScfgOptionSubSelected, ScfgToastText;
##   Button: ScfgOption, ScfgOptionSelected, ScfgPrimary, ScfgAccent, ScfgSmall, ScfgBack;
##   PanelContainer: ScfgSheetPanel, ScfgQrFrame, ScfgToast; Panel: ScfgRule.
## Colors the screens draw themselves (the step dots, the status dot, the back arrow) are theme
## colors of the type "ScfgSheet": accent, on_accent, ink, line, ok, warn, error. Its constants
## are sheet_width (the docked sheet's width) and dot_size.
class_name ScfgTheme
extends RefCounted

const SHEET := Color("#fbfcfd")
const INK := Color("#17202e")
const INK_HOVER := Color("#2c3a50")
const MUTED := Color("#5b6676")
const FAINT := Color("#a3abb6")
const LINE := Color("#cfd5dd")
const RULE := Color("#e4e8ed")
const HOVER := Color("#eef1f4")
const DIM := Color("#0a0c1866")
const OK := Color("#17804f")
const WARN := Color("#9a5b00")
const ERROR := Color("#c0392b")
const WHITE := Color("#ffffff")


## Ren'Py's `_k()`: the screens are designed for 1280x720 and grow with the game.
static func scale_for(size: Vector2) -> float:
	if size.x <= 0.0 or size.y <= 0.0:
		return 1.0
	return minf(size.y / 720.0, size.x / 960.0)


## Ren'Py's `_sheet_width()`: 520 design pixels docked on the right, or the whole width on a
## portrait or narrow screen.
static func sheet_width(size: Vector2, k: float) -> float:
	var w := float(px(520, k))
	return size.x if (size.x < size.y or w * 1.6 > size.x) else w


static func px(n: float, k: float) -> int:
	return int(round(n * k))


## The Theme for a screen of the autoload `scfg`: the built-in look with its accent, merged with
## the developer's Theme (configure's "theme"), if any.
static func for_screen(scfg: Object, k: float) -> Theme:
	var accent: String = ScfgConfig.DEFAULT_ACCENT
	var custom: Variant = null
	if scfg != null:
		accent = scfg.accent()
		custom = scfg.get("_theme")
	var t := build(accent, k)
	if custom is Theme:
		t.merge_with(custom)
	return t


## The built-in Margin theme with `accent` (#rrggbb), scaled by `k`.
static func build(accent_hex: String, k: float) -> Theme:
	var t := Theme.new()
	var accent := Color(accent_hex)
	var on_accent := Color(ScfgConfig.text_on(accent_hex))
	var bold := FontVariation.new()
	bold.base_font = ThemeDB.fallback_font
	bold.variation_embolden = 0.8
	var radius := px(10, k)
	var line_w := maxi(1, px(1.5, k))
	t.default_font_size = px(18, k)

	# colors the screens draw with
	for c: Array in [["accent", accent], ["on_accent", on_accent], ["ink", INK], ["line", LINE], ["ok", OK],
			["warn", WARN], ["error", ERROR], ["muted", MUTED], ["dim", DIM]]:
		t.set_color(c[0], "ScfgSheet", c[1])
	t.set_constant("sheet_width", "ScfgSheet", px(520, k))
	t.set_constant("dot_size", "ScfgSheet", maxi(4, px(10, k)))

	# text
	t.set_color("font_color", "Label", INK)
	t.set_font_size("font_size", "Label", px(18, k))
	for v: Array in [
			["ScfgTitle", 26, INK, true], ["ScfgHeading", 21, INK, true], ["ScfgAmount", 40, INK, true],
			["ScfgAmountToken", 22, MUTED, false], ["ScfgNote", 14, MUTED, false], ["ScfgLabel", 15, MUTED, false],
			["ScfgMuted", 16, MUTED, false], ["ScfgWarning", 16, WARN, false], ["ScfgDev", 14, WARN, false],
			["ScfgError", 16, ERROR, false], ["ScfgOk", 18, OK, false], ["ScfgStatus", 16, INK, false],
			["ScfgMono", 14, INK, false], ["ScfgQrStep", 16, INK, false], ["ScfgOptionText", 15, INK, false],
			["ScfgOptionSub", 12, MUTED, false], ["ScfgOptionTextSelected", 15, on_accent, false],
			["ScfgOptionSubSelected", 12, on_accent, false], ["ScfgToastText", 16, WHITE, false]]:
		t.set_type_variation(v[0], "Label")
		t.set_font_size("font_size", v[0], px(v[1], k))
		t.set_color("font_color", v[0], v[2])
		if v[3]:
			t.set_font("font", v[0], bold)

	# buttons
	var focus := _box(Color(0, 0, 0, 0), radius, accent, maxi(2, px(2, k)), 0, 0)
	focus.draw_center = false
	focus.set_expand_margin_all(maxf(2.0, px(2, k)))
	_button(t, "Button", px(17, k), bold, focus, {
		"normal": _box(Color(0, 0, 0, 0), radius, LINE, line_w, px(16, k), px(11, k)),
		"hover": _box(HOVER, radius, HOVER, line_w, px(16, k), px(11, k)),
		"pressed": _box(HOVER, radius, LINE, line_w, px(16, k), px(11, k)),
		"disabled": _box(Color(0, 0, 0, 0), radius, RULE, line_w, px(16, k), px(11, k)),
	}, INK, FAINT)
	_button(t, "ScfgSmall", px(15, k), bold, focus, {
		"normal": _box(Color(0, 0, 0, 0), radius, LINE, line_w, px(10, k), px(6, k)),
		"hover": _box(HOVER, radius, HOVER, line_w, px(10, k), px(6, k)),
		"pressed": _box(HOVER, radius, LINE, line_w, px(10, k), px(6, k)),
		"disabled": _box(Color(0, 0, 0, 0), radius, RULE, line_w, px(10, k), px(6, k)),
	}, INK, FAINT)
	_button(t, "ScfgPrimary", px(17, k), bold, focus, {
		"normal": _box(INK, radius, INK, 0, px(16, k), px(11, k)),
		"hover": _box(INK_HOVER, radius, INK_HOVER, 0, px(16, k), px(11, k)),
		"pressed": _box(INK_HOVER, radius, INK_HOVER, 0, px(16, k), px(11, k)),
		"disabled": _box(LINE, radius, LINE, 0, px(16, k), px(11, k)),
	}, WHITE, WHITE)
	_button(t, "ScfgAccent", px(17, k), bold, focus, {
		"normal": _box(accent, radius, accent, 0, px(16, k), px(11, k)),
		"hover": _box(accent.lightened(0.08), radius, accent, 0, px(16, k), px(11, k)),
		"pressed": _box(accent.darkened(0.08), radius, accent, 0, px(16, k), px(11, k)),
		"disabled": _box(LINE, radius, LINE, 0, px(16, k), px(11, k)),
	}, on_accent, WHITE)
	_button(t, "ScfgOption", px(15, k), null, focus, {
		"normal": _box(Color(0, 0, 0, 0), radius, LINE, line_w, px(6, k), px(7, k)),
		"hover": _box(HOVER, radius, HOVER, line_w, px(6, k), px(7, k)),
		"pressed": _box(HOVER, radius, LINE, line_w, px(6, k), px(7, k)),
		"disabled": _box(Color(0, 0, 0, 0), radius, RULE, line_w, px(6, k), px(7, k)),
	}, INK, FAINT)
	_button(t, "ScfgOptionSelected", px(15, k), null, focus, {
		"normal": _box(accent, radius, accent, line_w, px(6, k), px(7, k)),
		"hover": _box(accent, radius, accent, line_w, px(6, k), px(7, k)),
		"pressed": _box(accent, radius, accent, line_w, px(6, k), px(7, k)),
		"disabled": _box(accent, radius, accent, line_w, px(6, k), px(7, k)),
	}, on_accent, on_accent)
	_button(t, "ScfgBack", px(20, k), null, focus, {
		"normal": _box(Color(0, 0, 0, 0), radius, LINE, line_w, 0, 0),
		"hover": _box(HOVER, radius, HOVER, line_w, 0, 0),
		"pressed": _box(HOVER, radius, LINE, line_w, 0, 0),
		"disabled": _box(Color(0, 0, 0, 0), radius, RULE, line_w, 0, 0),
	}, INK, FAINT)

	# fields
	t.set_stylebox("normal", "LineEdit", _box(Color(0, 0, 0, 0), radius, LINE, line_w, px(12, k), px(9, k)))
	t.set_stylebox("focus", "LineEdit", focus)
	t.set_stylebox("read_only", "LineEdit", _box(Color(0, 0, 0, 0), radius, RULE, line_w, px(12, k), px(9, k)))
	t.set_font_size("font_size", "LineEdit", px(17, k))
	for name: String in ["font_color", "caret_color"]:
		t.set_color(name, "LineEdit", INK)
	t.set_color("font_placeholder_color", "LineEdit", FAINT)
	t.set_color("selection_color", "LineEdit", Color(accent, 0.3))

	# panels, progress, scroll bar
	t.set_type_variation("ScfgSheetPanel", "PanelContainer")
	t.set_stylebox("panel", "ScfgSheetPanel", _box(SHEET, 0, SHEET, 0, px(44, k), 0, px(32, k), px(28, k)))
	t.set_type_variation("ScfgQrFrame", "PanelContainer")
	t.set_stylebox("panel", "ScfgQrFrame", _box(WHITE, radius, LINE, line_w, px(8, k), px(8, k)))
	t.set_type_variation("ScfgToast", "PanelContainer")
	t.set_stylebox("panel", "ScfgToast", _box(INK, radius, INK, 0, px(18, k), px(12, k)))
	t.set_type_variation("ScfgRule", "Panel")
	t.set_stylebox("panel", "ScfgRule", _box(RULE, 0, RULE, 0, 0, 0))
	t.set_stylebox("background", "ProgressBar", _box(RULE, px(4, k), RULE, 0, 0, 0))
	t.set_stylebox("fill", "ProgressBar", _box(accent, px(4, k), accent, 0, 0, 0))
	var scroll := StyleBoxEmpty.new()
	scroll.content_margin_left = px(3, k)
	scroll.content_margin_right = px(3, k)
	t.set_stylebox("scroll", "VScrollBar", scroll)
	t.set_stylebox("grabber", "VScrollBar", _box(LINE, px(3, k), LINE, 0, 0, 0))
	t.set_stylebox("grabber_highlight", "VScrollBar", _box(MUTED, px(3, k), MUTED, 0, 0, 0))
	t.set_stylebox("grabber_pressed", "VScrollBar", _box(MUTED, px(3, k), MUTED, 0, 0, 0))
	return t


static func _button(t: Theme, type: String, size: int, font: Variant, focus: StyleBox, boxes: Dictionary,
		color: Color, disabled: Color) -> void:
	if type != "Button":
		t.set_type_variation(type, "Button")
	for state: String in boxes:
		t.set_stylebox(state, type, boxes[state])
	t.set_stylebox("focus", type, focus)
	t.set_font_size("font_size", type, size)
	if font is Font:
		t.set_font("font", type, font)
	for name: String in ["font_color", "font_hover_color", "font_pressed_color", "font_focus_color",
			"font_hover_pressed_color"]:
		t.set_color(name, type, color)
	t.set_color("font_disabled_color", type, disabled)


## A rounded box: fill, corner radius, border color and width, and content margins (x, y; or
## left/right x with separate top and bottom).
static func _box(fill: Color, radius: int, border: Color, border_w: int, mx: int, my: int,
		top: int = -1, bottom: int = -1) -> StyleBoxFlat:
	var b := StyleBoxFlat.new()
	b.bg_color = fill
	b.set_corner_radius_all(radius)
	b.border_color = border
	b.set_border_width_all(border_w)
	b.anti_aliasing = true
	b.content_margin_left = mx
	b.content_margin_right = mx
	b.content_margin_top = my if top < 0 else top
	b.content_margin_bottom = my if bottom < 0 else bottom
	return b
