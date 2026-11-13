## A short notification ("Payment confirmed: ...") at the top right of the screen, shown by
## ScfgDefaultSheetHost.notify() when `Scfg.notify` is on (Ren'Py's renpy.notify). It fades out
## after SECONDS and frees itself. The text is already translated.
class_name ScfgToast
extends MarginContainer

const SECONDS := 3.0


func _init() -> void:
	auto_translate_mode = Node.AUTO_TRANSLATE_MODE_DISABLED
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	process_mode = Node.PROCESS_MODE_ALWAYS


## `scfg`: the autoload, for the theme's accent and the developer's Theme (may be null).
func setup(text: String, scfg: Object) -> void:
	var tree := Engine.get_main_loop() as SceneTree
	var size_now := tree.root.get_visible_rect().size if tree != null else Vector2(1280, 720)
	var k := ScfgTheme.scale_for(size_now)
	theme = ScfgTheme.for_screen(scfg, k)
	set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	grow_horizontal = Control.GROW_DIRECTION_BEGIN
	for side: String in ["margin_top", "margin_right"]:
		add_theme_constant_override(side, ScfgTheme.px(24, k))
	var panel := PanelContainer.new()
	panel.theme_type_variation = "ScfgToast"
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var l := Label.new()
	l.name = "Text"
	l.text = text
	l.theme_type_variation = "ScfgToastText"
	panel.add_child(l)
	add_child(panel)


func _ready() -> void:
	var fade := create_tween()
	fade.tween_interval(SECONDS)
	fade.tween_property(self, "modulate:a", 0.0, 0.3)
	fade.tween_callback(queue_free)
