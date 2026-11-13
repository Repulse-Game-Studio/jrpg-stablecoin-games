## The worker's default timer source: SceneTree.create_timer. Any object with these two methods
## can replace it (tests use a manual one):
##
##     schedule(callable: Callable, seconds: float) -> handle
##     cancel(handle) -> void
class_name ScfgTreeScheduler
extends RefCounted

var _tree: SceneTree


func _init(tree: SceneTree = null) -> void:
	_tree = tree if tree != null else (Engine.get_main_loop() as SceneTree)


## Calls `callable` after `seconds` of real time (the time scale and a paused tree are ignored).
## The handle is passed to cancel().
func schedule(callable: Callable, seconds: float) -> Variant:
	var handle := {"live": true}
	if _tree == null:
		push_error("ScfgTreeScheduler: no SceneTree to schedule on")
		handle["live"] = false
		return handle
	_tree.create_timer(maxf(seconds, 0.0), true, false, true).timeout.connect(func() -> void:
		if handle["live"]:
			handle["live"] = false
			# a freed target (a worker gone before its timer fired) must not log an engine error
			if callable.is_valid():
				callable.call())
	return handle


func cancel(handle: Variant) -> void:
	if handle is Dictionary:
		handle["live"] = false
