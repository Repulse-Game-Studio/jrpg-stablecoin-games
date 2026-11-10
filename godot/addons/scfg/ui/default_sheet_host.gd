## The sheet host the `Scfg` autoload uses by default: it shows the addon's own payment sheet
## (ui/sheet.tscn, ScfgSheet) and restore screen (ui/restore.tscn, ScfgRestoreScreen), each on its
## own CanvasLayer at LAYER (128), above the game. See sheet_host.gd for the contract.
##
## `Scfg.configure({"sheet_scene": PackedScene})` replaces the payment sheet: the scene is
## instanced instead of sheet.tscn. Its root must have `func bind(controller, scfg)` and
## `signal closed(how)`; bind() is called once, before the root enters the tree, with the
## ScfgCheckoutController and the `Scfg` autoload. A root without them is not shown (reported,
## and pay() resolves "failed" with "sheet.unavailable"). The restore screen is not replaced.
##
## A screen is added to the tree deferred (pay() may be called while the tree root is busy, from
## a scene's _ready()). Notifications are short toasts (ScfgToast).
class_name ScfgDefaultSheetHost
extends ScfgSheetHost

const LAYER := 128
const SHEET_SCENE := "res://addons/scfg/ui/sheet.tscn"
const RESTORE_SCENE := "res://addons/scfg/ui/restore.tscn"

var _scfg: WeakRef = weakref(null)  # the autoload
var _open: Array[WeakRef] = []  # screens shown and not freed yet


## A CanvasLayer that frees itself once its screen is gone (freed, or moved elsewhere). It runs
## while the game is paused, as the screens and the autoload do.
class _Layer:
	extends CanvasLayer

	func _init() -> void:
		process_mode = Node.PROCESS_MODE_ALWAYS

	func _process(_delta: float) -> void:
		if get_child_count() == 0 and not is_queued_for_deletion():
			queue_free()


func _init(scfg: Object = null) -> void:
	_scfg = weakref(scfg)


func open_payment(controller: ScfgCheckoutController) -> Node:
	var scfg: Object = _scfg.get_ref()
	var custom: Variant = scfg.get("_sheet_scene") if scfg != null else null
	return _show(custom if custom is PackedScene else load(SHEET_SCENE), controller, custom is PackedScene)


func open_restore(controller: ScfgRestoreController) -> Node:
	return _show(load(RESTORE_SCENE), controller, false)


func close(node: Node) -> void:
	if not is_instance_valid(node) or node.is_queued_for_deletion():
		return
	var layer := node.get_parent()
	if layer is _Layer:
		layer.queue_free()
	else:
		node.queue_free()


func notify(text: String) -> void:
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null:
		return
	var layer := _Layer.new()
	layer.layer = LAYER + 1
	var toast := ScfgToast.new()
	toast.setup(text, _scfg.get_ref())
	layer.add_child(toast)
	tree.root.add_child.call_deferred(layer)


## The screens this host shows that still exist, oldest first (the e2e driver and tests find the
## sheet here).
func screens() -> Array[Node]:
	var out: Array[Node] = []
	var alive: Array[WeakRef] = []
	for ref: WeakRef in _open:
		var node: Variant = ref.get_ref()
		if node != null and not (node as Node).is_queued_for_deletion():
			out.append(node)
			alive.append(ref)
	_open = alive
	return out


func _show(scene: Variant, controller: Object, custom: bool) -> Node:
	var scfg: Object = _scfg.get_ref()
	var tree := Engine.get_main_loop() as SceneTree
	if not scene is PackedScene or tree == null:
		return null
	var node: Node = (scene as PackedScene).instantiate()
	if node == null:
		return null
	if not (node.has_method("bind") and node.has_signal("closed")):
		node.free()
		if scfg != null and scfg.has_method("_report"):
			scfg.call("_report", "the sheet_scene's root needs func bind(controller, scfg) and signal closed(how)")
		return null
	node.call("bind", controller, scfg)
	var layer := _Layer.new()
	layer.layer = LAYER
	layer.add_child(node)
	tree.root.add_child.call_deferred(layer)
	_open.append(weakref(node))
	if not custom and node is Control:
		(node as Control).set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	return node
