## Test-only autoload for tests/godot_showcase: plays the showcase's buttons that need no chain
## (export, clear, restore and a tip, both closed with ui_cancel), then prints SHOWCASE-OK and
## quits. The test copies it into a temporary copy of the showcase; it is never part of it.
extends Node

## [frame, step]: each step runs once, on its frame.
const SCRIPT := [
	[30, "export_and_clear"],
	[40, "open_restore"],
	[100, "cancel"],
	[140, "open_tip"],
	[200, "cancel"],
	[260, "finish"],
]
const MAX_FRAMES := 600  # a step that never comes is a failure, not a hang

var _frame := 0


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS  # the showcase pauses the tree while a sheet is open


func _process(_delta: float) -> void:
	_frame += 1
	var main := get_tree().current_scene
	for entry: Array in SCRIPT:
		if entry[0] == _frame:
			_step(main, entry[1])
	if _frame > MAX_FRAMES:
		print("SHOWCASE-TIMEOUT")
		get_tree().quit(1)


func _step(main: Node, step: String) -> void:
	match step:
		"export_and_clear":
			main._export()
			main._clear()
		"open_restore":
			main._restore()
		"open_tip":
			main._tip("1.00")
		"cancel":
			var ev := InputEventAction.new()
			ev.action = "ui_cancel"
			ev.pressed = true
			Input.parse_input_event(ev)
		"finish":
			var log_text: String = (main.get_node("%Log") as RichTextLabel).get_parsed_text()
			print("SHOWCASE-LOG-BEGIN\n%s\nSHOWCASE-LOG-END" % log_text)
			print("SHOWCASE-OK")
			get_tree().quit(0)
