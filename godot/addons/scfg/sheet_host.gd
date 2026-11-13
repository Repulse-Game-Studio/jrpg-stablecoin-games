## The interface between the payment and restore flows (`Scfg.pay()`, `Scfg.restore()`) and the
## screens that show them. `Scfg.sheet_host` holds one; replace it to show the flows your own way.
##
## A host shows a screen for a controller and hands back its Node. That Node must have
## `signal closed(how: String)` and emit it once when the player is done with the screen:
##   - payment sheet: how = "cancel" (the player gave up), "keep_playing" (close the sheet but keep
##     the payment and its browser page alive; the final result arrives later) or "finish" (close
##     after a final state: paid, underpaid or expired). These are Ren'Py's `_close_checkout` hows.
##   - restore screen: any how (the restore screen only closes).
## Never emit `closed` from inside open_payment() / open_restore(): the flow listens once they
## have returned.
##
## Lifetime of the returned Node (the flow holds it weakly):
##   - freed without emitting `closed`: counts as "cancel", noticed on the next frame, and not
##     handed to close().
##   - taken out of the tree, reparented, or never added to it: still open, so a host may add it
##     later (the default host adds it deferred).
##   - the game quits while it is open: pay() / restore() resolve with what they have so far and
##     nothing is delivered (Ren'Py's `_shutdown`); close(node) is called only for a Node still in
##     the tree.
##
## The autoload polls the controller every frame, so the screen only reads its fields and calls its
## methods, never `poll()`. Buttons that leave the game call `Scfg.open_browser(ctl)`,
## `Scfg.open_wallet_app(ctl)`, `Scfg.open_explorer(url)`, `Scfg.copy(text)` and
## `Scfg.paste_into(ctl)`. The flows close the controller (stopping its browser page) before they
## call `close(node)`.
##
## This base class is the interface and shows nothing: open_payment() and open_restore() return
## null, and pay() / restore() then resolve "failed" with error "sheet.unavailable" (reported with
## push_error). The autoload's default is ScfgDefaultSheetHost (ui/default_sheet_host.gd), which
## shows the addon's own sheet and restore screen.
class_name ScfgSheetHost
extends RefCounted


## Show the payment sheet for `controller`; returns its Node (with `signal closed(how)`), or null
## when no sheet can be shown.
func open_payment(_controller: ScfgCheckoutController) -> Node:
	return null


## Show the restore screen for `controller`; returns its Node (with `signal closed(how)`), or null
## when no screen can be shown.
func open_restore(_controller: ScfgRestoreController) -> Node:
	return null


## Remove a screen opened by open_payment() / open_restore(), after its controller was closed.
func close(node: Node) -> void:
	if is_instance_valid(node) and not node.is_queued_for_deletion():
		node.queue_free()


## Show a short, already translated message to the player. Payment and restore announcements
## come only when `Scfg.notify` is true; the web "Couldn't copy" notice comes regardless, since it
## answers the player's own Copy press.
func notify(_text: String) -> void:
	pass
