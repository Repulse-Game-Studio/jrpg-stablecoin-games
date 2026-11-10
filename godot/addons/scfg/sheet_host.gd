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
##   - freed (free() or queue_free()) without emitting `closed`: counts as "cancel", noticed on the
##     next frame. It is not handed to close() again.
##   - taken out of the tree, reparented, or never added to it: still open. Leaving the tree is not
##     closing, so a host may add the Node later (the default host adds it deferred) or move it.
##   - the autoload leaves the tree (the game quits) or is freed while the Node is open: the flow
##     resumes at once; pay() resolves "cancelled" (or the final result the controller already
##     has) and restore() its result so far ("cancelled" unless a restore finished), and nothing
##     is delivered (no callback, no `payment_resolved`, no notification), as Ren'Py's
##     `_shutdown`. The controller's page is stopped, and close(node) is called only for a Node
##     still in the tree.
##
## The screen only reads the controller's fields and calls its methods (ScfgCheckoutController,
## ScfgRestoreController); the autoload polls the controller every frame, so the screen never calls
## `poll()` itself. The screen's buttons that leave the game call the autoload's actions:
## `Scfg.open_browser(ctl)`, `Scfg.open_wallet_app(ctl)`, `Scfg.open_explorer(url)`,
## `Scfg.copy(text)` and `Scfg.paste_into(ctl)`.
##
## The flows close the controller (stopping its browser page) BEFORE they call `close(node)`, so a
## screen never outlives the controller's page callbacks by accident.
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


## Show a short message to the player. Already translated. The payment and restore
## announcements ("Payment confirmed: ...", "Purchases restored.") come only when `Scfg.notify` is
## true; in a web export the "Couldn't copy: your browser blocked the clipboard." notice comes
## whatever `Scfg.notify` is, since it answers the player's own Copy press.
func notify(_text: String) -> void:
	pass
