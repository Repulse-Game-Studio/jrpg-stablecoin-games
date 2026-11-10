## Stablecoin for Games (SCFG): USDC/USDT tips and unlock purchases for Godot, paid straight to
## the developer's wallet. No server, no accounts. This script is the `Scfg` autoload, which the
## editor plugin (plugin.gd) registers. It is the Godot twin of plugin/game/scfg/scfg_api.rpy:
## when this file and the plan are silent, the Ren'Py glue decides.
##
##     func _ready() -> void:
##         Scfg.add_chain("myl2", {...})              # optional: your own EVM chain, before configure
##         Scfg.configure({"wallet": "0x...", "testnet": true})
##         Scfg.add_item("chapter2", {"code": 1, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
##
##     if Scfg.owns("chapter2"):
##         ...
##
##     var result := await Scfg.pay("unlock", {"item": "chapter2"})   # shows the payment sheet
##     var restored := await Scfg.restore()                           # shows the restore screen
##
## DEVELOPER ERRORS. Ren'Py raises ConfigError. GDScript has no exceptions, so a misused call
## reports `scfg: <message>` with push_error and returns the ScfgErr (configure, add_chain,
## add_item); a query (owns, ...) reports the same way and returns its neutral value. Nothing
## here crashes the game. `_warn` advice (a token the wallet cannot receive yet, a corrupt state
## file) is printed in debug builds only.
##
## STATE. Receipts and pending payments live in `user://scfg/state.json` (ScfgStore), global to
## the install and never inside a save slot. It is read on first use. The Engine and the Worker
## start on the first frame after configure() (`_ensure_started`, from `_process`), with this
## node as the HTTP host of their transport and its tree as the Worker's timer source.
##
## THE PUMP (`_process`), in Ren'Py's `_periodic` order: drain the Worker's outcomes, give each
## to every open and background checkout controller (`on_outcome`) and deliver its result; then
## `poll()` every controller and register its callback; close background controllers whose
## session is final or older than the browser timeout; poll the restore controllers and deliver a
## finished one; warn once about unready routes; then mirror a new snapshot to disk.
##
## FLOWS (Ren'Py's _open_checkout / _close_checkout / _open_restore / _close_restore). pay() and
## restore() build a controller (ScfgCheckoutController / ScfgRestoreController, the core's), show
## it through `sheet_host` (ScfgSheetHost) and wait for the screen's `closed(how)`. The pump polls
## the open controllers every frame, so a screen only reads their fields and calls their methods.
## Closing a payment sheet closes its controller first (cancel / finish stop its browser page),
## and only then is the screen removed; "keep_playing" moves the controller to `_background`,
## where its page keeps answering the browser until the session is final or the browser timeout
## passes. A misused pay() / restore() reports with push_error and resolves "failed" with the
## ConfigError code (the kind, such as "TypeError", for an error without one).
##
## PERSISTENCE (`_sync`, Ren'Py's `_sync_persistent`): new or changed receipts, a session added or
## removed, and a session's `state` or `hint_tx` changing are written at once (a web tab, or a
## crash, can end the game any moment); other pending changes such as scan progress (`cursor`)
## at most every PENDING_SAVE_INTERVAL seconds, and once more when the autoload leaves the tree
## (the game quits). A save that failed is tried again every PENDING_SAVE_INTERVAL seconds.
## `receipts_changed` is emitted whenever the receipts change. A state file that exists but
## cannot be read (a lock, a permission, an I/O error; not a corrupt one, which is moved aside)
## starts no payments this run and is never saved over (see store.gd); the next launch retries.
extends Node

## A final result of a payment that has no pay() callback (Ren'Py's `on_result`).
signal payment_resolved(result: ScfgPaymentResult)
## The stored receipts changed (a payment confirmed, a restore, clear_local_data()).
signal receipts_changed

const STATE_PATH := "user://scfg/state.json"
const EXPORT_PATH := "user://scfg-receipts.json"
const PENDING_SAVE_INTERVAL := 10.0
## After a Copy on the web: how long the page is asked whether the browser refused it.
const COPY_WATCH_S := 5.0
const _SETUP_KEYS: Array[String] = ["store_path", "clock", "transport", "engine_opts", "scheduler", "quiet",
	"platform", "server_factory", "js"]
const _PAY_OPTIONS: Array[String] = ["item", "amount", "token", "callback"]
const _RESTORE_OPTIONS: Array[String] = ["callback"]
## The checkout page's files, served to the browser from here (copies of plugin/game/scfg/web/).
const WEB_DIR := "res://addons/scfg/web/"
## Characters that can appear in a transaction hash (hex) or a Solana signature (base58).
const TX_CHARS := "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

## Show a short notification (through `sheet_host`) when a payment confirms or purchases are
## restored.
var notify: bool = false
## How the payment sheet and the restore screen are shown (see sheet_host.gd). The default,
## ScfgDefaultSheetHost, shows the addon's own screens (addons/scfg/ui/); replace it to show the
## flows your own way.
var sheet_host: ScfgSheetHost

var _config: Variant = null  # the core Config Dictionary, once configure() succeeded
var _custom_chains: Array = []  # add_chain(), consumed by configure()
var _logo_res: Variant = null  # the logo as a res:// path
var _engine: Variant = null
var _worker: ScfgWorker = null
var _start_error: Variant = null  # the Engine could not be built from this config and state
var _checkouts := {}  # cid -> controller with its sheet open
var _background := {}  # cid -> "keep playing" controller (its browser page still answers)
var _restores := {}  # rid -> restore controller with its screen open
var _callbacks := {}  # session id -> the pay() callback
var _restore_callbacks := {}  # rid -> the restore() callback
var _next_id := 0
var _last_snapshot: Variant = null
var _last_save := -INF
var _warned_unready: Variant = null
var _persisted: Variant = null  # {"pending", "receipts"}: what was last mirrored (null: not read yet)
var _dirty := false  # _persisted has changes that are not on disk
## How many times `receipts_changed` was emitted: a screen compares it each frame instead of
## copying the receipts (ui/restore.gd).
var _receipts_rev := 0
var _store_error: Variant = null  # the state file exists but could not be read: never saved over
var _wipe: Variant = null  # clear_local_data() in progress: {"job", "done", "before"}
var _theme: Variant = null  # configure's "theme" (merged into the screens' built-in Theme)
var _sheet_scene: Variant = null  # configure's "sheet_scene" (replaces the payment sheet)
var _logo_texture: Variant = null  # the logo as a Texture2D, loaded on first use
var _waits: Array = []  # the screens pay() / restore() are waiting on (_ScreenWait)
var _shutting_down := false  # the autoload left the tree: flows close without delivering

# Test hooks (see _setup). Not part of the API.
var _store_path := STATE_PATH
var _clock: Callable = _monotonic
var _transport: Variant = null
var _engine_opts := {}
var _scheduler: Variant = null
var _quiet := false
var _platform: Variant = _Platform.new()
var _server_factory_override: Variant = null
var _paste_target: Variant = null  # web: the cid or rid waiting for the page's clipboard answer
var _js_override: Variant = null  # web: the page's JavaScript (default ScfgWebPage.BrowserJs)
var _page: ScfgWebPage = null  # web: the page's bridge, built on first use (_web_page())
var _copy_watch_until := -INF  # web: until when to ask the page whether a Copy failed (_clock time)
## Errors the Engine or Worker reported after this node left the tree (a pass still in flight
## at quit), counted and dropped: there is nobody left to tell.
static var errors_after_exit := 0
## In quiet mode only (tests): every developer error and worker error reported, newest last
## ("scfg: ..." lines). Empty otherwise, so a long session does not accumulate them.
var _reported: Array[String] = []
## In quiet mode only (tests): every developer warning, newest last.
var _warnings: Array[String] = []


func _init() -> void:
	sheet_host = ScfgDefaultSheetHost.new(self)
	process_mode = Node.PROCESS_MODE_ALWAYS  # payments keep being watched while the game is paused


## TEST HOOK, not part of the API: replaces defaults before first use. `opts` keys:
##   store_path (String, default STATE_PATH); clock (Callable -> seconds, for the save interval;
##   default a monotonic clock); transport (the Engine's transport; default
##   ScfgHttpTransport.new(self)); engine_opts (extra ScfgEngine options such as rpc_factory and
##   clock); scheduler (the Worker's timer source; default this node's tree); quiet (true: record
##   reports in `_reported` / `_warnings` instead of push_error / push_warning); platform (an
##   object like _Platform: is_web, is_mobile, shell_open, clipboard_set, clipboard_get);
##   server_factory (replaces the checkout controllers' server_factory; `_server_opts(kw)` gives
##   the desktop page's options); js (an object with `eval(code) -> Variant` standing in for the
##   browser page's JavaScript; only used where `platform.is_web()`).
func _setup(opts: Dictionary) -> void:
	for k: Variant in opts:
		if not (k is String and k in _SETUP_KEYS):
			push_error("scfg: _setup: unknown option %s" % [k])
	_store_path = opts.get("store_path", _store_path)
	if opts.has("clock"):
		_clock = opts["clock"]
	_transport = opts.get("transport", _transport)
	_engine_opts = opts.get("engine_opts", _engine_opts)
	_scheduler = opts.get("scheduler", _scheduler)
	_quiet = opts.get("quiet", _quiet)
	_platform = opts.get("platform", _platform)
	_server_factory_override = opts.get("server_factory", _server_factory_override)
	_js_override = opts.get("js", _js_override)
	_persisted = null


static func _monotonic() -> float:
	return Time.get_ticks_usec() / 1000000.0


# --- reports ---------------------------------------------------------------------------------


## A developer error (Ren'Py's ConfigError reaching the developer): push_error, always.
func _report(message: String) -> void:
	var line := "scfg: " + message
	if _quiet:
		_reported.append(line)
	else:
		push_error(line)


## The ScfgErr `err` as a developer error; returns it.
## @fallible
func _misuse(err: ScfgErr) -> Variant:
	_report(err.message)
	return err


## A ConfigError built here (not by the core), reported; returns it.
## @fallible
func _config_misuse(code: String, message: String) -> Variant:
	return _misuse(ScfgErr.make("ConfigError", message, code))


## Setup advice for the developer (Ren'Py's `_warn`): debug builds only.
func _warn(message: String) -> void:
	if _quiet:
		_warnings.append("scfg: " + message)
	elif OS.is_debug_build():
		push_warning("scfg: " + message)


## An error for the developer from the running plugin (Ren'Py's `_report_error`). A web export
## prints it in debug builds only (its console is the player's browser).
func _report_error(heading: String, err: Variant = null) -> void:
	var line := "scfg: %s%s" % [heading, "" if err == null else " " + str(err)]
	if _quiet:
		_reported.append(line)
	elif not (OS.has_feature("web") and not OS.is_debug_build()):
		push_error(line)


func _log_error(err: Variant) -> void:
	_report_error("worker error:", err)


# --- configuration ---------------------------------------------------------------------------


## Add an EVM chain that isn't built in (Optimism, Avalanche, your own L2). Call it before
## configure(); the key can then be used in configure()'s `chains`, `rpc_urls` and
## `confirmations`. `opts`: name, chain_id, rpc (Array of https urls), tokens ({"USDC": "0x..."},
## 6-decimal tokens only), confirmations, explorer_tx (an https url with `{}` where the hash goes),
## and optionally native_symbol ("ETH"), fee_hint ("low"), testnet (false) and max_log_range
## (1000). Returns null, or the ScfgErr it reported.
## @fallible
func add_chain(key: Variant, opts: Dictionary = {}) -> Variant:
	if _config != null:
		return _config_misuse("configure.after_add_chain", "call Scfg.add_chain() before Scfg.configure()")
	var chain: Variant = ScfgConfig.build_chain(key, opts)
	if chain is ScfgErr:
		return _misuse(chain)
	_custom_chains.append(chain)
	return null


## Set the payout wallet and options. `opts` (Ren'Py's keyword arguments): wallet (an EVM 0x
## address) and/or solana_wallet, testnet, chains, tokens, methods, rpc_urls, confirmations,
## previous_wallets, logo and accent. `logo` is an image in the project: "res://images/logo.png"
## (or "images/logo.png"), .png, .jpg or .webp. `accent` is "#rrggbb". Two keys are Godot's own:
## `theme` (a Theme merged over the screens' built-in look; see ui/theme.gd) and `sheet_scene` (a
## PackedScene that replaces the payment sheet; see godot/README.md). Call it before the first
## frame that needs payments; it can be called again until the plugin has started (the frame
## after a successful configure()). Returns null, or the ScfgErr it reported.
## @fallible
func configure(opts: Dictionary = {}) -> Variant:
	if _worker != null or _start_error != null:
		return _config_misuse("configure.after_start", "Scfg.configure() can't change the configuration once payments have started")
	if opts.has("custom_chains"):
		return _misuse(ScfgErr.make("TypeError", "configure() got an unexpected keyword argument 'custom_chains' (use Scfg.add_chain())"))
	var o := opts.duplicate()
	# the glue's own keys, which the core does not know
	var theme: Variant = o.get("theme")
	var scene: Variant = o.get("sheet_scene")
	o.erase("theme")
	o.erase("sheet_scene")
	if theme != null and not theme is Theme:
		return _misuse(ScfgErr.make("TypeError", "configure(): theme must be a Theme, not %s" % _type_name(theme)))
	if scene != null and not scene is PackedScene:
		return _misuse(ScfgErr.make("TypeError", "configure(): sheet_scene must be a PackedScene, not %s" % _type_name(scene)))
	var logo: Variant = o.get("logo")
	if logo is String and (logo as String).begins_with("res://"):
		o["logo"] = (logo as String).substr("res://".length())  # the core rejects ":"
	o["custom_chains"] = _custom_chains
	var cfg: Variant = ScfgConfig.build_config(o)
	if cfg is ScfgErr:
		return _misuse(cfg)
	var res_logo: Variant = null
	if cfg["logo"] != null:
		res_logo = "res://" + str(cfg["logo"])
		if not (FileAccess.file_exists(res_logo) or ResourceLoader.exists(res_logo)):
			return _config_misuse("logo.file_missing", "logo file not found in the project: %s" % ScfgPy.py_repr(res_logo))
	_config = cfg
	_logo_res = res_logo
	_logo_texture = null
	_theme = theme
	_sheet_scene = scene
	return null


static func _type_name(v: Variant) -> String:
	return (v as Object).get_class() if v is Object else type_string(typeof(v))


## Declare a paid unlock. `opts`: code (1-99, never change it after release), price ("4.99"),
## token ("USDC" or "USDT"), title, and optionally min_price. Returns null, or the ScfgErr it
## reported.
## @fallible
func add_item(item_id: Variant, opts: Dictionary = {}) -> Variant:
	var cfg: Variant = _require_config()
	if cfg is ScfgErr:
		return cfg
	var item: Variant = ScfgConfig.add_item(cfg, item_id, opts)
	if item is ScfgErr:
		return _misuse(item)
	return null


## The Config, or the `configure.not_called` ConfigError: reported here unless `report` is false
## (pay() and restore() report it with their failed result).
## @fallible
func _require_config(report: bool = true) -> Variant:
	if _config == null:
		var err := ScfgErr.make("ConfigError", "call Scfg.configure(...) first", "configure.not_called")
		if report:
			return _misuse(err)
		return err
	return _config


## The configured logo as a res:// path, or null.
func _logo_path() -> Variant:
	return _logo_res


## [bytes, content type] of the configured logo, or null (no logo, or it cannot be read). The
## file itself when it is in the export (or the game runs from the project); otherwise the
## imported texture, re-encoded as PNG.
func _logo_file() -> Variant:
	if _logo_res == null:
		return null
	var path: String = _logo_res
	if FileAccess.file_exists(path):
		var kind: Variant = ScfgConfig.logo_content_type(path)
		if kind is ScfgErr:
			return null
		return [FileAccess.get_file_as_bytes(path), kind]
	if ResourceLoader.exists(path):
		var tex := load(path) as Texture2D
		var image := tex.get_image() if tex != null else null
		if image != null:
			return [image.save_png_to_buffer(), "image/png"]
	return null


# --- for the screens (ui/sheet.gd, ui/restore.gd, a sheet_scene) ----------------------------


## The accent color ("#rrggbb"): configure's `accent`, or the default.
func accent() -> String:
	return str(_config["accent"]) if _config != null else ScfgConfig.DEFAULT_ACCENT


## The configured logo as a texture, or null (none configured, or it cannot be read).
func logo_texture() -> Texture2D:
	if _logo_texture == null and _logo_res != null:
		if ResourceLoader.exists(_logo_res):
			_logo_texture = load(_logo_res) as Texture2D
		if _logo_texture == null:
			var file: Variant = _logo_file()
			if file != null:
				var image := Image.new()
				var bytes: PackedByteArray = file[0]
				var loaded := ERR_FILE_UNRECOGNIZED
				match str(file[1]):
					"image/png":
						loaded = image.load_png_from_buffer(bytes)
					"image/jpeg":
						loaded = image.load_jpg_from_buffer(bytes)
					"image/webp":
						loaded = image.load_webp_from_buffer(bytes)
				if loaded == OK:
					_logo_texture = ImageTexture.create_from_image(image)
	return _logo_texture


## An item's title (Ren'Py's `_item_title`): the declared title, else the id itself.
func item_title(item_id: Variant) -> String:
	var items: Dictionary = _config["items"] if _config != null else {}
	var item: Variant = items.get(str(item_id)) if item_id is String or item_id is StringName else null
	return str(item["title"]) if item is Dictionary else str(item_id)


## A chain's display name (Ren'Py's `_chain_name`), else its key.
func chain_name(chain_key: Variant) -> String:
	if _config == null:
		return str(chain_key)
	var chain: Variant = ScfgConfig.registry(_config).get(chain_key)
	return str(chain["name"]) if chain is Dictionary else str(chain_key)


## True in a web export (the screens word the browser wallet and Paste for it).
func is_web() -> bool:
	return _platform.is_web()


## True on a phone or tablet (Ren'Py's renpy.mobile).
func is_mobile() -> bool:
	return _platform.is_mobile()


# --- platform support ------------------------------------------------------------------------


## True where payments can run: every platform Godot exports to. Kept for parity with Ren'Py.
func supported() -> bool:
	return true


# --- worker lifecycle ------------------------------------------------------------------------


func _interval() -> float:
	return 3.0 if not (_checkouts.is_empty() and _restores.is_empty()) else 15.0


## The running Worker, started on first use; null when not configured or when it cannot start
## (reported once).
func _ensure_started() -> ScfgWorker:
	if _worker != null or _start_error != null or _config == null:
		return _worker
	if _platform.is_web() and not _platform.is_userfs_persistent():
		_warn("user:// is not persistent in this browser (IndexedDB is unavailable: private browsing, blocked site data or a restricted iframe). Players' receipts are then lost when the tab closes. See \"Web exports\" in the addon's README.")
	var stored := _stored()
	if _store_error is ScfgErr:
		# Not corrupt, only unreadable for now (a lock, a permission, an I/O error): starting
		# with empty stores would save them over the player's receipts. The next launch retries.
		_start_error = _store_error
		_report_error("cannot read the stored receipts:", _store_error)
		return null
	var opts := {"transport": _transport if _transport != null else ScfgHttpTransport.new(self),
		"pending": stored["pending"], "receipts": stored["receipts"], "on_error": _log_error}
	opts.merge(_engine_opts, true)
	var engine: Variant = ScfgEngine.create(_config, opts)
	if engine is ScfgErr:
		_start_error = engine
		_report_error("cannot start the payment engine:", engine)
		return null
	var wopts := {"interval": _interval, "on_error": _log_error}
	if _scheduler != null:
		wopts["scheduler"] = _scheduler
	elif is_inside_tree():
		wopts["tree"] = get_tree()
	var worker := ScfgWorker.new(engine, wopts)
	if worker.snapshot is ScfgErr:
		_start_error = worker.snapshot
		_report_error("cannot read the stored receipts:", worker.snapshot)
		return null
	_engine = engine
	worker.start()
	_worker = worker
	return worker


## Ren'Py's quit callback (`_shutdown`): close the pages, stop the Worker, and flush what is not on
## disk. The autoload leaves the tree only when the game quits, so this ends the plugin for good.
## A pay() or restore() still waiting on its screen is released first and resolves "cancelled"
## (or the restore's finished result) without delivering anything: no callback, no
## `payment_resolved`, no notification, and the session is not marked cancelled, so the Engine
## keeps watching it on the next launch.
##
## A Worker pass already in flight ends here: the Callables the Engine and Worker hold that are
## bound to this node (on_error, interval) are replaced with detached ones (the pass may finish
## after the node is freed, and calling a freed node's method would be a script error), then this
## node's own HTTP transport is closed, so a request in flight ends with HostGone (which the
## Engine contains and reports to the detached handler) and the pass finishes before the tree is
## gone. At a real quit there is no later frame: a pass left waiting would never resume and its
## coroutines would be reported as leaked at exit.
func _exit_tree() -> void:
	_release_waits()
	for c: Variant in _checkouts.values() + _background.values():
		c.close()
	_checkouts.clear()
	_background.clear()
	_restores.clear()
	if _worker != null:
		_worker.stop()
		_worker.on_error = _detached_error()
		_worker.interval = _detached_interval()
		(_engine as ScfgEngine).on_error = _detached_error()
		var transport: Variant = (_engine as ScfgEngine).transport
		if transport is ScfgHttpTransport and (transport as ScfgHttpTransport).host == self:
			(transport as ScfgHttpTransport).close()
		var snap: Variant = _worker.snapshot
		if snap is Dictionary and _wipe == null:
			_sync(snap)
	if _dirty:
		_save()


func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE:
		_release_waits()  # an instance freed outside the tree (no _exit_tree)


## Shutdown: every flow still waiting on a screen resumes now and closes without delivering.
func _release_waits() -> void:
	_shutting_down = true
	for wait: Variant in _waits.duplicate():
		(wait as _ScreenWait).release()
	_waits.clear()


## An on_error that is not bound to any node (made in a static function): counts and drops.
static func _detached_error() -> Callable:
	return func(_err: Variant) -> void:
		errors_after_exit += 1


static func _detached_interval() -> Callable:
	return func() -> float: return 15.0


# --- the pump --------------------------------------------------------------------------------


func _process(_delta: float) -> void:
	_pump()


## One frame of the pump (see the header).
func _pump() -> void:
	if _config == null:
		return
	var worker := _ensure_started()
	if worker == null:
		return
	_pump_page()
	var controllers: Array = _checkouts.values() + _background.values()
	for outcome: Variant in worker.drain_outcomes():
		for c: Variant in controllers:
			c.on_outcome(outcome)
		if outcome.get("result") != null:
			_deliver(outcome["result"])
	for c: Variant in controllers:
		c.poll()
		_register_callback(c)
	var timeout: float = float(_config["methods"]["browser"]["timeout_min"]) * 60.0
	var now: float = (_engine as ScfgEngine).clock.call()
	for cid: Variant in _background.keys():
		var c: Variant = _background[cid]
		var session: Variant = c.session
		var expired: bool = not session is Dictionary or now - float(session["created_at"]) > timeout
		if expired or c.state in ["paid", "underpaid", "expired", "choose"]:
			c.close()
			_background.erase(cid)
	for r: Variant in _restores.values():
		var before: String = r.state
		r.poll()
		if r.state != before and r.state == "done":
			_deliver_restore(r)
	var snap: Variant = worker.snapshot
	if not snap is Dictionary:
		return
	var unready: Array = snap["unready"]
	if not unready.is_empty() and unready != _warned_unready:
		_warned_unready = unready
		for route: Array in unready:
			_warn("your wallet can't receive %s on %s yet (no token account). Receive any amount of %s there once; until then players aren't offered it."
				% [route[1], route[0], route[1]])
	# clear_local_data(): the snapshots made before the Engine was wiped are skipped
	if _wipe != null and ((_wipe["job"] as ScfgJob).cancelled() or (_wipe["done"] and not is_same(snap, _wipe["before"]))):
		_wipe = null
	if not is_same(snap, _last_snapshot):
		_last_snapshot = snap
		if _wipe == null:
			_sync(snap)
	# what is not on disk yet (a save that failed, scan progress inside the interval) is written
	# once the interval passed, even when no new snapshot comes
	if _dirty and _clock.call() - _last_save >= PENDING_SAVE_INTERVAL:
		_last_save = _clock.call()
		_save()


func _register_callback(ctl: Variant) -> void:
	var cb: Variant = ctl.get("callback")
	var sid: Variant = ctl.session_id
	if ScfgPy.py_truthy(sid) and cb is Callable and (cb as Callable).is_valid():
		_callbacks[sid] = cb


## A session's result: to its pay() callback, else to `payment_resolved`. A paid one is
## announced when `notify` is on.
func _deliver(result: Variant) -> void:
	if not result is Dictionary:
		return
	var typed := ScfgPaymentResult.from_core(result)
	var cb: Variant = _callbacks.get(typed.session_id)
	if typed.status in ["paid", "underpaid"]:
		_callbacks.erase(typed.session_id)
	if typed.status == "paid" and notify:
		var item: Variant = (_config["items"] as Dictionary).get(typed.item) if typed.item is String else null
		if item is Dictionary:
			_notify(tr("Payment confirmed: %s. Thank you!") % str(item["title"]))
		else:
			_notify(tr("Payment confirmed. Thank you for your support!"))
	_resolve(cb, typed)


## Ren'Py's `_call(cb or on_result, result)`: the callback when it is still valid, else
## `payment_resolved`.
func _resolve(cb: Variant, typed: ScfgPaymentResult) -> void:
	if cb is Callable and (cb as Callable).is_valid():
		(cb as Callable).call(typed)
	else:
		payment_resolved.emit(typed)


## The callback only, when there is a valid one (Ren'Py's ScfgPay: `_call(callback, result)`).
static func _call(cb: Variant, result: Variant) -> void:
	if cb is Callable and (cb as Callable).is_valid():
		(cb as Callable).call(result)


## A finished restore: to its restore() callback (once), announced when `notify` is on.
func _deliver_restore(controller: ScfgRestoreController) -> void:
	var cb: Variant = _restore_callbacks.get(controller.get("rid"))
	_restore_callbacks.erase(controller.get("rid"))
	var typed := ScfgRestoreResult.from_core(controller.result())
	if not typed.restored.is_empty() and notify:
		_notify(tr("Purchases restored."))
	_call(cb, typed)


func _notify(text: String) -> void:
	if sheet_host != null:
		sheet_host.notify(text)


# --- payment and restore flows ---------------------------------------------------------------


## The controller behind a payment sheet (Ren'Py's _ScreenCheckout): the core's controller plus
## its id and the pay() callback, which the pump registers once the session exists.
class _Checkout:
	extends ScfgCheckoutController
	var cid := ""
	var callback: Variant = null


## The controller behind a restore screen, with its id.
class _Restore:
	extends ScfgRestoreController
	var rid := ""


## Where the game runs and how it reaches the world outside: the default platform. Tests replace
## it (`_setup`'s platform).
class _Platform:
	extends RefCounted

	func is_web() -> bool:
		return OS.has_feature("web")

	## Ren'Py's renpy.mobile: a phone or tablet, a native export or a browser on one.
	func is_mobile() -> bool:
		return OS.has_feature("mobile") or OS.has_feature("web_android") or OS.has_feature("web_ios")

	func shell_open(url: String) -> void:
		OS.shell_open(url)

	func clipboard_set(text: String) -> void:
		DisplayServer.clipboard_set(text)

	func clipboard_get() -> String:
		return DisplayServer.clipboard_get()

	## Is `user://` kept after the game closes? Only a web export can say no (no IndexedDB).
	func is_userfs_persistent() -> bool:
		return OS.is_userfs_persistent()


## Waits for a screen's `closed(how)`. A screen freed (free() or queue_free()) without emitting it
## counts as "cancel", noticed on the next frame (`gone`: it is being removed, so it is not closed
## again). Leaving the tree is not closing: a screen that is reparented, or kept out of the tree, or
## never added to it, is still waited on. `release()` ends the wait with "shutdown" (the autoload
## is going away). The node is held weakly, and the frame check is a signal connection, which
## Godot drops if this wait is freed.
class _ScreenWait:
	extends RefCounted
	signal done
	var how: Variant = null
	var gone := false
	var _node: WeakRef
	var _tree: SceneTree

	func _init(node: Node) -> void:
		_node = weakref(node)
		node.connect("closed", _on_closed)
		_tree = Engine.get_main_loop() as SceneTree
		if _tree != null:
			_tree.process_frame.connect(_check)

	func _on_closed(h: Variant = "cancel") -> void:
		_end(str(h), false)

	func _check() -> void:
		var node: Variant = _node.get_ref()
		if node == null or (node as Node).is_queued_for_deletion():
			_end("cancel", true)

	## The autoload is going away. A screen still in the tree is closed; one that is not (the
	## tree is being torn down) is left to it.
	func release() -> void:
		var node: Variant = _node.get_ref()
		_end("shutdown", node == null or (node as Node).is_queued_for_deletion() or not (node as Node).is_inside_tree())

	func _end(h: String, g: bool) -> void:
		if how != null:
			return
		how = h
		gone = g
		if _tree != null and _tree.process_frame.is_connected(_check):
			_tree.process_frame.disconnect(_check)
		done.emit()


## Show the payment sheet and wait until the player closes it; returns the outcome as an
## ScfgPaymentResult. A coroutine: `var result := await Scfg.pay("unlock", {"item": "chapter2"})`.
##
## `kind` is "unlock" (`item` required; `amount` and `token` default to the item's) or "tip"
## (`amount`, the suggested amount the player may change, and `token` required). Amounts are
## Strings such as "4.99", never floats. `opts`: item, amount, token, callback.
##
## The result's status: "already_owned" (no sheet is shown), "cancelled", "pending" (the player
## chose to keep playing while the payment confirms), "paid", "underpaid", "expired", or "failed"
## (a misused call, reported with push_error; `error` is the ConfigError code).
##
## `callback` (a Callable taking the ScfgPaymentResult): it gets this call's own result when that
## is already_owned, failed or cancelled, and every later result of the payment, such as the paid
## result of a payment the player kept playing through. Without a callback a cancel and the later
## results reach `payment_resolved` (Ren'Py's `on_result`), also when this call returns the same
## result: a payment that confirms while the sheet is open is emitted then, and returned when the
## player closes the sheet. A handler of both must be idempotent (or pass a callback).
func pay(kind: Variant, opts: Dictionary = {}) -> ScfgPaymentResult:
	var spec: Variant = _payment_spec(kind, opts)
	if spec is ScfgErr:
		return _failed_payment(kind, opts, spec)
	var cb: Variant = opts.get("callback")
	if spec["kind"] == "unlock":
		var owned: Variant = _ownership()
		if owned != null and str(spec["item"]) in (owned[0] as Array):
			var short := _payment_result("already_owned", spec["kind"], spec["item"], spec["token"])
			_call(cb, short)
			return short
	var worker := _ensure_started()
	if worker == null:
		var down: Variant = _start_failure()
		if down is ScfgErr:
			return _failed_payment(kind, opts, down)
	var ctl := _Checkout.new({"config": _config, "spec": spec, "submit": worker.submit,
		"mobile": _platform.is_mobile(), "server_factory": _server_factory, "unready": _unready,
		"game_name": ProjectSettings.get_setting("application/config/name", ""),
		"browser_wallet": _browser_wallet()})
	ctl.cid = _new_id("c")
	ctl.callback = cb
	if ctl.init_error is ScfgErr:
		ctl.close()
		return _failed_payment(kind, opts, ctl.init_error)
	_checkouts[ctl.cid] = ctl
	var node: Node = sheet_host.open_payment(ctl) if sheet_host != null else null
	if not _is_screen(node):
		_checkouts.erase(ctl.cid)
		ctl.cancel()  # discards a session start already submitted, as a cancel does
		if node != null:
			sheet_host.close(node)
		var none: Variant = _no_screen("open_payment", node)
		if none is ScfgErr:
			return _failed_payment(kind, opts, none)
	var closed: _ScreenWait = await _wait_closed(node)
	return _close_checkout(ctl, node, closed)


## The PaymentSpec for pay(), or the ScfgErr of a misused call (not reported here).
## @fallible
func _payment_spec(kind: Variant, opts: Dictionary) -> Variant:
	var bad: Variant = _options_error("pay", opts, _PAY_OPTIONS)
	if bad is ScfgErr:
		return bad
	var cfg: Variant = _require_config(false)
	if cfg is ScfgErr:
		return cfg
	return ScfgPayments.prepare_payment(cfg, kind,
		{"item": opts.get("item"), "amount": opts.get("amount"), "token": opts.get("token")})


## A TypeError for an option `fn` does not take, or a callback that is not a Callable; else null.
## @fallible
static func _options_error(fn: String, opts: Dictionary, known: Array[String]) -> Variant:
	for k: Variant in opts:
		if not (k is String and k in known):
			return ScfgErr.make("TypeError", "%s() got an unexpected option %s" % [fn, ScfgPy.py_repr(k)])
	var cb: Variant = opts.get("callback")
	if cb != null and not cb is Callable:
		return ScfgErr.make("TypeError", "%s(): callback must be a Callable, not %s" % [fn, type_string(typeof(cb))])
	return null


## Ren'Py's _close_checkout: close the controller (cancel / keep playing / finish) and only then
## the sheet. A cancel goes to the callback or `payment_resolved` too. At shutdown ("shutdown")
## the controller is only closed, and nothing is delivered.
func _close_checkout(ctl: _Checkout, node: Variant, closed: _ScreenWait) -> ScfgPaymentResult:  # node may be freed
	_checkouts.erase(ctl.cid)
	var result: Variant
	var how: String = closed.how
	if how == "shutdown":
		# Ren'Py's _shutdown: close the page and deliver nothing; the session stays watched
		result = ctl.finish()
		if not closed.gone:
			sheet_host.close(node)
		return ScfgPaymentResult.from_core(result)
	_register_callback(ctl)
	if how == "cancel":
		result = ctl.cancel()
	elif how == "keep_playing" and ScfgPy.py_truthy(ctl.session_id):
		result = ctl.keep_playing()
		_background[ctl.cid] = ctl  # keeps the browser page answering until final
	else:
		result = ctl.finish()
	if not closed.gone:
		sheet_host.close(node)
	var typed := ScfgPaymentResult.from_core(result) if result is Dictionary \
		else _payment_result("cancelled", ctl.spec["kind"], ctl.spec["item"], ctl.spec["token"])
	if how == "cancel":
		_resolve(ctl.callback, typed)
	return typed


## Show the restore screen (the player pastes a transaction hash) and wait until it is closed;
## returns an ScfgRestoreResult: "restored", "nothing_found", "cancelled" (closed before a
## restore finished), or "failed" (a misused call, reported with push_error; `error` is the
## ConfigError code). A coroutine. `opts`: callback, a Callable that gets the result once a
## restore finishes, while the screen is still open.
func restore(opts: Dictionary = {}) -> ScfgRestoreResult:
	var bad: Variant = _restore_misuse(opts)
	if bad is ScfgErr:
		return _failed_restore(opts, bad)
	var worker := _ensure_started()
	if worker == null:
		var down: Variant = _start_failure()
		if down is ScfgErr:
			return _failed_restore(opts, down)
	var ctl := _Restore.new({"submit": worker.submit})
	ctl.rid = _new_id("r")
	_restores[ctl.rid] = ctl
	if opts.get("callback") != null:
		_restore_callbacks[ctl.rid] = opts["callback"]
	var node: Node = sheet_host.open_restore(ctl) if sheet_host != null else null
	if not _is_screen(node):
		_restores.erase(ctl.rid)
		_restore_callbacks.erase(ctl.rid)
		if node != null:
			sheet_host.close(node)
		var none: Variant = _no_screen("open_restore", node)
		if none is ScfgErr:
			return _failed_restore(opts, none)
	var closed: _ScreenWait = await _wait_closed(node)
	_restores.erase(ctl.rid)
	_restore_callbacks.erase(ctl.rid)
	if not closed.gone:
		sheet_host.close(node)
	return ScfgRestoreResult.from_core(ctl.result())


## The ScfgErr of a misused restore() (not reported here), or null.
## @fallible
func _restore_misuse(opts: Dictionary) -> Variant:
	var bad: Variant = _options_error("restore", opts, _RESTORE_OPTIONS)
	if bad is ScfgErr:
		return bad
	var cfg: Variant = _require_config(false)
	if cfg is ScfgErr:
		return cfg
	return null


## Resolves when the screen `node` is closed (see _ScreenWait), or at once with "shutdown" once
## the autoload is going away. A coroutine.
func _wait_closed(node: Node) -> _ScreenWait:
	var wait := _ScreenWait.new(node)
	if _shutting_down:
		wait.release()
	if wait.how == null:
		_waits.append(wait)
		await wait.done
		_waits.erase(wait)
	return wait


## True for a screen a host may hand back: a Node with `signal closed(how)`.
static func _is_screen(node: Variant) -> bool:
	return node is Node and is_instance_valid(node) and (node as Node).has_signal("closed")


## @fallible
func _no_screen(method: String, node: Variant) -> Variant:
	var why := "returned null" if node == null else "returned a Node without signal closed(how)"
	return ScfgErr.make("ConfigError", "no screen to show: Scfg.sheet_host.%s() %s" % [method, why],
		"sheet.unavailable")


## Why the Engine could not start (already reported once), as an error for a result.
## @fallible
func _start_failure() -> Variant:
	if _start_error is ScfgErr:
		return _start_error
	return ScfgErr.make("RuntimeError", "payments could not start")


## A misused pay(): reported, given to a valid callback, and returned as a "failed" result.
func _failed_payment(kind: Variant, opts: Dictionary, err: ScfgErr) -> ScfgPaymentResult:
	if err != _start_error:
		_report(err.message)
	var item: Variant = opts.get("item")
	var token: Variant = opts.get("token")
	var r := _payment_result("failed", kind if kind is String else null, item if item is String else null,
		token if token is String else null)
	r.error = _err_code(err)
	_call(opts.get("callback"), r)
	return r


func _failed_restore(opts: Dictionary, err: ScfgErr) -> ScfgRestoreResult:
	if err != _start_error:
		_report(err.message)
	var r := ScfgRestoreResult.new()
	r.status = "failed"
	r.error = _err_code(err)
	_call(opts.get("callback"), r)
	return r


## A ConfigError's code, else the error's kind ("TypeError").
static func _err_code(err: ScfgErr) -> String:
	return err.code if err.code is String and err.code != "" else err.kind


static func _payment_result(status: String, kind: Variant, item: Variant, token: Variant) -> ScfgPaymentResult:
	var r := ScfgPaymentResult.new()
	r.status = status
	r.kind = kind
	r.item = item
	r.token = token
	return r


func _new_id(prefix: String) -> String:
	_next_id += 1
	return "%s%d" % [prefix, _next_id]


## The checkout controllers' `unready`: the routes the Worker's snapshot marks unusable.
func _unready() -> Array:
	if _worker != null and _worker.snapshot is Dictionary:
		return _worker.snapshot["unready"]
	return []


## The checkout controllers' `browser_wallet` (web only: does the page have a wallet for a chain).
func _browser_wallet() -> Variant:
	return _web_page().has_wallet if _platform.is_web() else null


## The page's JavaScript side (web exports only), loaded on first use (Ren'Py's _web_page()).
func _web_page() -> ScfgWebPage:
	if _page == null:
		var js: Variant = _js_override if _js_override != null else ScfgWebPage.BrowserJs.new()
		_page = ScfgWebPage.new(js, _page_asset_loader(), _warn)
		_page.install()
	return _page


## The logo as a data: URL for the page (Ren'Py's _logo_data_url), or null without a logo.
func _logo_data_url() -> Variant:
	var logo: Variant = _logo_file()
	if logo == null:
		return null
	return "data:%s;base64,%s" % [logo[1], Marshalls.raw_to_base64(logo[0])]


## The checkout controllers' `server_factory`: the page a browser wallet pays on. Desktop: a
## localhost ScfgCheckoutServer; web: the in-page overlay.
## @fallible
func _server_factory(kw: Dictionary) -> Variant:
	if _server_factory_override is Callable:
		return (_server_factory_override as Callable).call(kw)
	if _platform.is_web():
		return ScfgOverlayCheckout.new({"ui": _web_page(), "key": _new_id("o"), "session_info": kw["session_info"],
			"on_tx": kw["on_tx"], "get_status": kw["get_status"], "logo_url": _logo_data_url(),
			"build_tx": kw.get("build_tx"), "clock": _clock})
	return ScfgCheckoutServer.new(_server_opts(kw))


## The desktop page's options (Ren'Py's _server_factory). `build_tx` stays null on EVM, so the page
## has no /build route there, as in Python. The logo is read here, once, on the game's thread.
func _server_opts(kw: Dictionary) -> Dictionary:
	var logo: Variant = _logo_file()
	return {"load_asset": _page_asset_loader(), "session_info": kw["session_info"], "on_tx": kw["on_tx"],
		"get_status": kw["get_status"], "load_logo": _constant(logo) if logo != null else null,
		"build_tx": kw.get("build_tx")}


## load_asset for the page: the bundled file's bytes, or null. Bound to no node, so a page that
## outlives this autoload still answers.
static func _page_asset_loader() -> Callable:
	return func(name: String) -> Variant:
		var path := WEB_DIR + name
		return FileAccess.get_file_as_bytes(path) if FileAccess.file_exists(path) else null


static func _constant(value: Variant) -> Callable:
	return func() -> Variant: return value


## The sheet's "Open checkout in browser" (Ren'Py's _open_browser): start the controller's page
## and open the browser on it. On web the overlay is already showing.
func open_browser(ctl: ScfgCheckoutController) -> void:
	if _screen_id(ctl) == "":
		return
	var url: Variant = ctl.open_browser()
	if ScfgPy.py_truthy(url) and not _platform.is_web():
		_open_url(str(url))


## The sheet's "Open wallet app" (mobile): open the payment request link (EIP-681 or Solana Pay)
## in the wallet app that handles it.
func open_wallet_app(ctl: ScfgCheckoutController) -> void:
	if _screen_id(ctl) == "":
		return
	var uri: Variant = ctl.payment_uri()
	if uri is ScfgErr:
		_report_error("cannot build the payment link:", uri)
		return
	if ScfgPy.py_truthy(uri):
		_open_url(str(uri))


## The sheet's "View on explorer": open `url` (nothing for null or "").
func open_explorer(url: Variant) -> void:
	if url is String and url != "":
		_open_url(url)


func _open_url(url: String) -> void:
	if _platform.is_web():
		_web_page().open_url(url)  # window.open with noopener (the engine's shell_open omits noopener)
	else:
		_platform.shell_open(url)


## The sheet's Copy buttons: `text` to the clipboard.
func copy(text: String) -> void:
	if _platform.is_web():
		_copy_watch_until = float(_clock.call()) + COPY_WATCH_S
		_web_page().copy(text)  # the browser may refuse: _pump_page() tells the player
	else:
		_platform.clipboard_set(text)


## The Paste button of a payment sheet's or restore screen's hash field: the clipboard's first
## word, hash characters only (Ren'Py's _clean_paste), replaces the field. An empty clipboard
## pastes nothing; a restore screen that moved past its input drops the paste.
func paste_into(ctl: Object) -> void:
	var id := _screen_id(ctl)
	if id == "":
		return
	if _platform.is_web():
		_paste_target = id
		_request_page_paste()
		return
	_apply_paste(id, _platform.clipboard_get(), false)


## Ask the page (navigator.clipboard, asynchronous); `_pump_page()` hands its answer to
## `_apply_paste(_paste_target, text, true)`.
func _request_page_paste() -> void:
	_web_page().request_paste()


## Web exports, once per frame (the first part of Ren'Py's WEB `_periodic`): the browser's answer
## to a Paste button, and whether a Copy was refused.
func _pump_page() -> void:
	if _page == null:
		return
	if _paste_target != null:
		var text: Variant = _page.take_paste()
		if text != null:
			_apply_paste(str(_paste_target), text, true)
	if float(_clock.call()) < _copy_watch_until and _page.take_copy_failed():
		_copy_watch_until = -INF
		_notify(tr("Couldn't copy: your browser blocked the clipboard."))


## Ren'Py's _take_paste: `text` into the hash field of the screen `id`. `from_page`: the browser's
## answer, where an empty one means it refused (error "paste_blocked").
func _apply_paste(id: String, text: Variant, from_page: bool) -> void:
	if _paste_target == id:
		_paste_target = null
	var cleaned := _clean_paste(text)
	var ctl: Variant = _checkouts.get(id)
	if ctl == null:
		ctl = _restores.get(id)
		if ctl != null and ctl.state != "input":
			return  # the player moved on (Check was pressed): drop the paste
	if ctl == null:
		return
	if cleaned != "":
		ctl.tx_input = cleaned
		ctl.error = null
	elif from_page:
		ctl.error = "paste_blocked"


## What a pasted clipboard becomes in a hash field, like typing it there: the first word, only
## hash characters, at most 90 of them (Ren'Py's `"".join(c for c in words[0] if c in TX_CHARS)[:90]`;
## the scan stops at the 90th kept character, so a huge clipboard costs nothing).
static func _clean_paste(text: Variant) -> String:
	var s := str(text) if text != null else ""
	var start := 0
	while start < s.length() and ScfgPy.is_py_space(s.unicode_at(start)):
		start += 1
	var out := ""
	var i := start
	while i < s.length() and out.length() < 90 and not ScfgPy.is_py_space(s.unicode_at(i)):
		if TX_CHARS.contains(s[i]):
			out += s[i]
		i += 1
	return out


## The id of an open payment sheet's or restore screen's controller, or "" for any other object.
func _screen_id(ctl: Variant) -> String:
	if ctl is _Checkout and is_same(_checkouts.get((ctl as _Checkout).cid), ctl):
		return (ctl as _Checkout).cid
	if ctl is _Restore and is_same(_restores.get((ctl as _Restore).rid), ctl):
		return (ctl as _Restore).rid
	return ""


# --- persistence -----------------------------------------------------------------------------


## The mirrored state, read from disk on first use.
func _stored() -> Dictionary:
	if _persisted == null:
		var got := ScfgStore.load(_store_path)
		if got["warning"] != null:
			_warn(got["warning"])
		_store_error = got.get("error")
		_persisted = {"pending": got["pending"], "receipts": got["receipts"]}
	return _persisted


## What must reach disk at once: which sessions exist, their state and tx hint.
static func _pending_key(pending: Dictionary) -> Dictionary:
	var key := {}
	for sid: Variant in pending:
		var r: Variant = pending[sid]
		key[sid] = [r.get("state"), r.get("hint_tx")] if r is Dictionary else r
	return key


## Mirror a snapshot to disk (see the header).
func _sync(snap: Dictionary) -> void:
	var stored := _stored()
	var receipts_changed_now: bool = stored["receipts"] != snap["receipts"]
	var pending_changed: bool = stored["pending"] != snap["pending"]
	if not (receipts_changed_now or pending_changed):
		return
	var sessions_changed := _pending_key(stored["pending"]) != _pending_key(snap["pending"])
	_persisted = {"pending": (snap["pending"] as Dictionary).duplicate(true),
		"receipts": (snap["receipts"] as Dictionary).duplicate(true)}
	_dirty = true
	var now: float = _clock.call()
	if receipts_changed_now or sessions_changed or now - _last_save >= PENDING_SAVE_INTERVAL:
		_last_save = now
		_save()
	if receipts_changed_now:
		_emit_receipts_changed()


## Emits `receipts_changed`, and counts it in `_receipts_rev`.
func _emit_receipts_changed() -> void:
	_receipts_rev += 1
	receipts_changed.emit()


func _save() -> void:
	var stored := _stored()
	if _store_error != null:
		return  # the file on disk could not be read: what is in memory must not replace it
	var err: Variant = ScfgStore.save(_store_path, stored["pending"], stored["receipts"])
	if err is ScfgErr:
		_report_error("cannot save the payment state:", err)
		return
	_dirty = false


# --- queries ---------------------------------------------------------------------------------


## The receipts and pending sessions readers see: the Worker's snapshot, or the stored state
## before the Worker starts and while clear_local_data() is in progress.
func _source() -> Dictionary:
	if _worker != null and _wipe == null and _worker.snapshot is Dictionary:
		return _worker.snapshot
	return _stored()


## [owned item ids, tip total (ScfgDec)], or null when the stored receipts cannot be read
## (reported).
func _ownership() -> Variant:
	if _worker != null and _wipe == null and _worker.snapshot is Dictionary:
		return [_worker.snapshot["owned"], _worker.snapshot["tip_total"]]
	var got: Variant = ScfgReceipts.compute_ownership(_stored()["receipts"], _config["items"])
	if got is ScfgErr:
		_report_error("cannot read the stored receipts:", got)
		return null
	return got


## True if a verified receipt unlocks `item_id`. Instant and offline. An unknown item is a
## developer error (reported; false).
func owns(item_id: Variant) -> bool:
	var cfg: Variant = _require_config()
	if cfg is ScfgErr:
		return false
	if not ((item_id is String or item_id is StringName) and (cfg["items"] as Dictionary).has(str(item_id))):
		_config_misuse("item.unknown", "unknown item %s; declare it with Scfg.add_item()" % ScfgPy.py_repr(item_id))  # err-ok: reported; owns() answers false
		return false
	var o: Variant = _ownership()
	return o != null and str(item_id) in (o[0] as Array)


## True once the player has tipped any amount (Ren'Py: `tip_total() > 0`).
func has_tipped() -> bool:
	var cfg: Variant = _require_config()
	if cfg is ScfgErr:
		return false
	var o: Variant = _ownership()
	var tips: ScfgDec = o[1] if o != null else null
	return tips != null and not tips.is_zero() and not tips.neg


## The sum of the player's tips as a decimal String ("0", "2.50"), Python's str(Decimal).
func tip_total() -> String:
	var cfg: Variant = _require_config()
	if cfg is ScfgErr:
		return "0"
	var o: Variant = _ownership()
	return "0" if o == null else str(o[1])


## The stored receipts, {receipt key: receipt}, as a deep copy. A receipt has chain, tx_hash,
## log_index, block, kind, item, amount, token, payer, recipient and source.
func receipts() -> Dictionary:
	return (_source()["receipts"] as Dictionary).duplicate(true)


## The payments still being watched for, {session id: record}, as a deep copy.
func pending() -> Dictionary:
	return (_source()["pending"] as Dictionary).duplicate(true)


## Testing helper: forget every receipt and pending payment on THIS device, to see your paywall
## again after a test purchase. Real purchases are not lost: the payments are still on the
## blockchain and Restore brings them back.
func clear_local_data() -> void:
	var had_receipts := not (_source()["receipts"] as Dictionary).is_empty()
	if _worker != null:
		var wipe := {"job": null, "done": false, "before": null}
		var worker := _worker
		wipe["job"] = worker.submit(func(engine: Variant) -> Variant:
			engine.receipts.clear()
			engine.pending.clear()
			wipe["before"] = worker.snapshot  # the last snapshot made before the wipe
			wipe["done"] = true
			return null)
		_wipe = wipe
	_persisted = {"pending": {}, "receipts": {}}
	_last_save = _clock.call()
	_save()
	if had_receipts:
		_emit_receipts_changed()


## Support helper: write the receipts as readable JSON (a list, sorted keys, indented). Returns
## the file's absolute path, or a RuntimeError ScfgErr (reported with push_error) when it cannot
## be written.
## @fallible
func export_receipts(path: String = EXPORT_PATH) -> Variant:
	var dir := path.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		return _misuse(ScfgErr.make("RuntimeError", "cannot write %s: %s" % [path, error_string(FileAccess.get_open_error())]))
	f.store_string(JSON.stringify(receipts().values(), "  ", true))
	f.flush()
	var written := f.get_error()
	f.close()
	if written != OK:
		return _misuse(ScfgErr.make("RuntimeError", "cannot write %s: %s" % [path, error_string(written)]))
	return ProjectSettings.globalize_path(path)
