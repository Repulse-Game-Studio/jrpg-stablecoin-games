## The browser checkout as an overlay inside the game's own web page (a Godot web export).
##
## On the web the game already runs in a browser, and wallet extensions inject into its page, so
## the checkout page (app.js, evm.js, solana.js) is shown over the game instead of being served on
## localhost. ScfgOverlayCheckout has ScfgCheckoutServer's interface (start, stop, url, running,
## reopen, pump), so ScfgCheckoutController drives both alike. The page's messages are read by
## pump(), which the controller calls every frame; they are validated like the local server's
## requests (ScfgPageMessages), and a tx hash stays a hint the watcher verifies on-chain.
## Mirrors scfg_core/overlay.py.
##
## `ScfgOverlayCheckout.new(opts)` takes the Python constructor's keyword arguments as keys:
##   - ui: the page (ScfgWebPage): open(key, session_info, logo_url), show(key), close(key),
##     take(key) -> Array, set_status(key, status), reply(key, id, status, body);
##   - key (String), session_info (Dictionary);
##   - on_tx(tx_hash, sender), get_status() -> Dictionary;
##   - logo_url (a data: URL, optional), build_tx(account) -> ScfgJob (optional, Solana);
##   - clock: a Callable returning seconds (default: a monotonic clock).
## on_tx, get_status and build_tx are the checkout controller's, bound to a RefCounted controller
## that may be released while this still holds them: each is checked with is_valid() before it is
## called (a freed get_status leaves the status as it was; a freed build_tx answers 503).
## Every Callable is synchronous; start(), stop(), reopen() and pump() never suspend. Once stop()
## ran (also from inside one of those callbacks) none is called again, not for the rest of that
## batch of messages either, and a job handed back by a build_tx that stopped the overlay is
## cancelled at once.
class_name ScfgOverlayCheckout
extends RefCounted

const URL := "overlay:checkout"  # what url is: there is no page to open

var ui: Variant
var key: String
var session_info: Dictionary
var on_tx: Variant
var get_status: Variant
var logo_url: Variant
var build_tx: Variant
var clock: Callable
## What the controller reads after start(): there is no page to open.
var url: String = URL
var running: bool:
	get:
		return _open

var _builds := {}  # page request id -> [ScfgJob, deadline]
var _status: Variant = null
var _open := false


func _init(opts: Dictionary) -> void:
	ui = opts["ui"]
	key = opts["key"]
	session_info = opts["session_info"]
	on_tx = opts.get("on_tx")
	get_status = opts.get("get_status")
	logo_url = opts.get("logo_url")
	build_tx = opts.get("build_tx")
	clock = opts.get("clock", _ticks)


static func _ticks() -> float:
	return Time.get_ticks_usec() / 1000000.0


## Shows the overlay; null (it cannot fail).
## @fallible
func start() -> Variant:
	ui.open(key, session_info, logo_url)
	_open = true
	_push_status()
	return null


func reopen() -> void:
	if _open:
		ui.show(key)


func stop() -> void:
	if not _open:
		return
	_open = false
	for entry: Array in _builds.values():
		(entry[0] as ScfgJob).cancel()
	_builds.clear()
	ui.close(key)


func pump() -> void:
	if not _open:
		return
	for message: Variant in ui.take(key):
		_handle(message)
		if not _open:
			return  # a callback stopped the overlay: the rest of the batch is moot
	_finish_builds()
	_push_status()


func _handle(message: Variant) -> void:
	if not message is Dictionary:
		return
	var kind: Variant = (message as Dictionary).get("type")
	if kind == "tx":
		var parsed: Variant = ScfgPageMessages.parse_tx_message((message as Dictionary).get("body"))
		if parsed is ScfgErr:
			return  # the page only posts what a wallet returned: nothing to answer
		if on_tx is Callable and (on_tx as Callable).is_valid():
			(on_tx as Callable).call(parsed[0], parsed[1])
		return
	var rid: Variant = (message as Dictionary).get("id")
	if kind != "build" or typeof(rid) != TYPE_INT:
		return
	if not build_tx is Callable:
		ui.reply(key, rid, 404, {"error": "not_found"})
		return
	var account: Variant = ScfgPageMessages.parse_build_message((message as Dictionary).get("body"))
	if account is ScfgErr:
		ui.reply(key, rid, 400, {"error": "bad account"})
		return
	if not (build_tx as Callable).is_valid():
		ui.reply(key, rid, 503, {"error": "unavailable"})
		return
	var job: Variant = (build_tx as Callable).call(account)
	if not _open:
		# build_tx stopped the overlay (its controller closed): nobody answers this build, so it
		# must not run
		if job is ScfgJob:
			(job as ScfgJob).cancel()
		return
	if not job is ScfgJob:
		ui.reply(key, rid, 503, {"error": "unavailable"})
		return
	if _builds.has(rid):
		(_builds[rid][0] as ScfgJob).cancel()  # the page reused an id: the older build is moot
	_builds[rid] = [job, clock.call() + ScfgPageMessages.BUILD_TIMEOUT_S]


func _finish_builds() -> void:
	for rid: Variant in _builds.keys():
		var entry: Array = _builds[rid]
		var job: ScfgJob = entry[0]
		if job.done():
			_builds.erase(rid)
			var failure: Variant = job.exception()  # err-ok: the job's failure (or its cancel), answered as a 503
			var value: Variant = job.result() if failure == null else null  # err-ok: exception() is null, so this is the value
			if value is Dictionary:
				ui.reply(key, rid, 200, value)
			else:
				ui.reply(key, rid, 503, {"error": "unavailable"})
		elif clock.call() >= entry[1]:
			_builds.erase(rid)
			job.cancel()  # don't build it later for a page that stopped waiting
			ui.reply(key, rid, 503, {"error": "unavailable"})


func _push_status() -> void:
	if not get_status is Callable or not (get_status as Callable).is_valid():
		return
	var status: Variant = (get_status as Callable).call()
	if not status is Dictionary:
		return
	if typeof(_status) != TYPE_DICTIONARY or status != _status:
		_status = (status as Dictionary).duplicate(true)
		ui.set_status(key, status)
