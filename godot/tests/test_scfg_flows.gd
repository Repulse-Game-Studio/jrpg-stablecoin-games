## The Scfg autoload's payment and restore flows (addons/scfg/scfg.gd): `await Scfg.pay()`,
## `await Scfg.restore()`, the sheet host they show the screens through, background "keep
## playing" payments, callbacks versus `payment_resolved`, notifications, wallet-app links and
## the clipboard. Behaviour authority: plugin/game/scfg/scfg_api.rpy (_open_checkout,
## _close_checkout, _deliver, _open_restore, _close_restore, _deliver_restore, _open_*, Copy,
## _take_paste).
##
## Each test builds its own instance of the autoload script, as test_scfg_api.gd does, with a fake
## sheet host (a sheet is a Node the test closes by emitting `closed(how)`), a fake platform (no
## browser is opened, no real clipboard is touched), FakeChains behind the Engine and a manual
## worker scheduler: a worker pass runs only when the test awaits `run_once()`.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")
const SCFG := preload("res://addons/scfg/scfg.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const TX := "0xabababababababababababababababababababababababababababababababab"
const DIR := "user://scfg-tests/flows"

static var USDC_ARB: String = ScfgChains.MAINNET["arbitrum"]["tokens"]["USDC"]["address"]


class ManualTimer:
	extends RefCounted
	var pending: Variant = null

	func schedule(cb: Callable, seconds: float) -> Variant:
		pending = {"cb": cb, "seconds": seconds}
		return pending

	func cancel(handle: Variant) -> void:
		if is_same(pending, handle):
			pending = null


## A pass of the Worker landing between a drain and a later snapshot read: right after a drain
## that found nothing, the paid session leaves the snapshot and its outcome is queued.
class RacingWorker:
	extends ScfgWorker
	var armed := true
	var sid: Variant
	var paid: Variant

	func drain_outcomes() -> Array:
		var out := super.drain_outcomes()
		if armed and out.is_empty():
			armed = false
			var snap: Dictionary = (snapshot as Dictionary).duplicate()
			var pending: Dictionary = (snap["pending"] as Dictionary).duplicate()
			pending.erase(sid)
			snap["pending"] = pending
			snapshot = snap
			_outcomes.append(paid)
		return out


## A sheet: the test closes it by emitting `closed`.
class FakeSheet:
	extends Node
	signal closed(how: String)
	var controller: Variant


## The sheet host the flows show their screens through, recording what they ask for.
class FakeHost:
	extends ScfgSheetHost
	var sheets: Array = []  # every FakeSheet opened, newest last
	var kinds: Array[String] = []  # "payment" / "restore", per sheet
	var closes := 0
	var notes: Array[String] = []
	var journal: Array = []  # shared with a RecordingServer: the order things happened in

	func open_payment(controller: ScfgCheckoutController) -> Node:
		return _open("payment", controller)

	func open_restore(controller: ScfgRestoreController) -> Node:
		return _open("restore", controller)

	func _open(kind: String, controller: Variant) -> Node:
		var sheet := FakeSheet.new()
		sheet.controller = controller
		(Engine.get_main_loop() as SceneTree).root.add_child(sheet)
		sheets.append(sheet)
		kinds.append(kind)
		return sheet

	func close(node: Node) -> void:
		closes += 1
		journal.append("sheet closed")
		super(node)

	func notify(text: String) -> void:
		notes.append(text)


## The platform the autoload opens URLs and uses the clipboard through.
class FakePlatform:
	extends RefCounted
	var web := false
	var mobile := false
	var opened: Array[String] = []
	var clipboard := ""
	var persistent := true

	func is_web() -> bool:
		return web

	func is_userfs_persistent() -> bool:
		return persistent

	func is_mobile() -> bool:
		return mobile

	func shell_open(url: String) -> void:
		opened.append(url)

	func clipboard_set(text: String) -> void:
		clipboard = text

	func clipboard_get() -> String:
		return clipboard


## The real desktop page server, noting when it is stopped and whether the controller's callbacks
## it holds were still valid then (the controller still alive).
class RecordingServer:
	extends ScfgCheckoutServer
	var journal: Array = []

	func stop() -> void:
		if running:
			journal.append(["page stopped", (on_tx as Callable).is_valid(), (get_status as Callable).is_valid()])
		super()


static func _remove_tree(dir: String) -> void:
	if not DirAccess.dir_exists_absolute(dir):
		return
	for f: String in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(f))
	for d: String in DirAccess.get_directories_at(dir):
		_remove_tree(dir.path_join(d))
	DirAccess.remove_absolute(dir)


func _path(name: String) -> String:
	var dir := DIR.path_join(name)
	_remove_tree(dir)
	DirAccess.make_dir_recursive_absolute(dir)
	return dir.path_join("state.json")


## {node, chains, clock, host, platform}: a configured autoload instance under the tree root
## with chapter2 declared, its Worker started and its first pass run. `setup` adds _setup keys.
func _make(name: String, setup: Dictionary = {}) -> Dictionary:
	var tree := Engine.get_main_loop() as SceneTree
	if not tree.root.is_inside_tree():
		await tree.process_frame
	var chains := {}
	var factory := func(chain: Dictionary) -> Variant:
		if not chains.has(chain["key"]):
			chains[chain["key"]] = F.FakeChain.new(1000)
		chains[chain["key"]].chain_obj = chain
		return chains[chain["key"]]
	var clock := F.FakeClock.new(1000.0)
	var platform := FakePlatform.new()
	var opts := {"store_path": _path(name), "clock": clock.fn, "scheduler": ManualTimer.new(),
		"transport": F.FakeTransport.new({}), "engine_opts": {"rpc_factory": factory, "clock": clock.fn},
		"quiet": true, "platform": platform}
	opts.merge(setup, true)
	var node: Node = SCFG.new()
	node._setup(opts)
	tree.root.add_child(node)
	var host := FakeHost.new()
	node.sheet_host = host
	T.eq(node.configure({"wallet": DEV}), null, "configure")
	T.eq(node.add_item("chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"}), null, "add_item")
	node._pump()
	await (node._worker as ScfgWorker).run_once()
	node._pump()
	return {"node": node, "chains": chains, "clock": clock, "host": host, "platform": platform}


func _drop(env: Dictionary) -> void:
	var s: Node = env["node"]
	for sheet: Variant in (env["host"] as FakeHost).sheets:
		if is_instance_valid(sheet):
			(sheet as Node).queue_free()
	s.queue_free()
	await (Engine.get_main_loop() as SceneTree).process_frame


## One worker pass, then one pump frame.
func _pass(s: Node) -> void:
	await (s._worker as ScfgWorker).run_once()
	s._pump()


## Starts `Scfg.pay(kind, opts)` without waiting for it: the result lands in box["result"].
func _pay(s: Node, kind: Variant, opts: Dictionary = {}) -> Dictionary:
	var box := {}
	var run := func() -> void:
		box["result"] = await s.pay(kind, opts)
	run.call()
	return box


## Starts `Scfg.restore(opts)` without waiting for it: the result lands in box["result"].
func _restore(s: Node, opts: Dictionary = {}) -> Dictionary:
	var box := {}
	var run := func() -> void:
		box["result"] = await s.restore(opts)
	run.call()
	return box


## The open sheet's controller, with a session started on Arbitrum.
func _session(s: Node, host: FakeHost) -> ScfgCheckoutController:
	var ctl: ScfgCheckoutController = (host.sheets[-1] as FakeSheet).controller
	T.eq(ctl.choose_network("arbitrum"), null, "choose a network")
	await _pass(s)
	T.ok(ScfgPy.py_truthy(ctl.session_id), "a session started")
	return ctl


## Pays the session on the fake chain, in a new block (the session's scan has already passed the
## head), and lets it confirm (Arbitrum: 20 confirmations).
func _pay_on_chain(env: Dictionary, ctl: ScfgCheckoutController) -> void:
	var chain: F.FakeChain = env["chains"]["arbitrum"]
	var block := chain.head + 1
	chain.logs.append(F.transfer_log(USDC_ARB, PAYER, DEV, ctl.session["units"], {"block": block, "tx_hash": TX}))
	chain.head = block
	await _pass(env["node"])
	chain.head = block + 19
	await _pass(env["node"])


## The status code of one HTTP request to 127.0.0.1:`port`, pumping the autoload (which pumps the
## page) meanwhile; 0 when there is no answer.
func _http(s: Node, port: int, request: String) -> int:
	var c := StreamPeerTCP.new()
	if c.connect_to_host("127.0.0.1", port) != OK:
		return 0
	var sent := false
	var buf := PackedByteArray()
	for i in 1000:
		c.poll()
		var status := c.get_status()
		if status == StreamPeerTCP.STATUS_CONNECTED:
			if not sent:
				c.put_data(request.to_utf8_buffer())
				sent = true
			var n := c.get_available_bytes()
			if n > 0:
				buf.append_array(c.get_partial_data(n)[1])
		elif status != StreamPeerTCP.STATUS_CONNECTING:
			break
		var text := buf.get_string_from_ascii()
		if text.contains("\r\n\r\n"):
			c.disconnect_from_host()
			return int(text.split(" ")[1])
		s._pump()
		OS.delay_msec(1)
	c.disconnect_from_host()
	return 0


# --- pay -------------------------------------------------------------------------------------


func test_pay_already_owned_short_circuits() -> bool:
	var env := await _make("owned")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	await _pass(s)
	var w: ScfgWorker = s._worker
	var job := w.submit(func(e: ScfgEngine) -> Variant:
		return ScfgReceipts.add_receipt(e.receipts, {"chain": "arbitrum", "tx_hash": TX, "log_index": 3,
			"block": 990, "kind": "unlock", "item": "chapter2", "amount": "4.99", "token": "USDC",
			"payer": PAYER.to_lower(), "recipient": DEV.to_lower(), "source": "payment"}))
	await _pass(s)
	T.eq(job.result(), true, "a receipt for chapter2")
	var called: Array = []
	var r: ScfgPaymentResult = await s.pay("unlock", {"item": "chapter2",
		"callback": func(got: ScfgPaymentResult) -> void: called.append(got)})
	T.eq([r.status, r.kind, r.item, r.token, r.session_id], ["already_owned", "unlock", "chapter2", "USDC", null],
		"already_owned, at once")
	T.eq(host.sheets.size(), 0, "no sheet was shown")
	T.eq(s._checkouts.size(), 0, "no checkout")
	T.ok(called.size() == 1 and is_same(called[0], r), "the callback gets it too (as Ren'Py's ScfgPay)")
	await _drop(env)
	return T.done()


func test_pay_cancel_returns_cancelled_and_calls_callback() -> bool:
	var env := await _make("cancel")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var called: Array = []
	var resolved: Array = []
	s.payment_resolved.connect(func(r: ScfgPaymentResult) -> void: resolved.append(r))
	var box := _pay(s, "unlock", {"item": "chapter2", "callback": func(r: ScfgPaymentResult) -> void: called.append(r)})
	T.eq(host.kinds, ["payment"], "the payment sheet is shown")
	T.ok(not box.has("result"), "pay() waits for the sheet")
	var ctl := await _session(s, host)
	var sid: Variant = ctl.session_id
	T.ok(s._callbacks.has(sid), "the callback is registered for the session")
	(host.sheets[-1] as FakeSheet).closed.emit("cancel")
	T.ok(box.has("result"), "pay() resolves when the sheet closes")
	var r: ScfgPaymentResult = box.get("result")
	T.eq([r.status, r.kind, r.item, r.session_id], ["cancelled", "unlock", "chapter2", sid], "cancelled")
	T.ok(called.size() == 1 and is_same(called[0], r), "the callback got the same result")
	T.eq(resolved.size(), 0, "not payment_resolved: the callback had it")
	T.eq([host.closes, s._checkouts.size(), s._background.size()], [1, 0, 0], "the sheet is closed and the checkout gone")
	await _pass(s)
	T.eq(s.pending()[sid]["ui_status"], "cancelled", "the session is marked cancelled")
	# without a callback, the cancel reaches payment_resolved (Ren'Py's on_result)
	box = _pay(s, "unlock", {"item": "chapter2"})
	(host.sheets[-1] as FakeSheet).closed.emit("cancel")
	r = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "cancelled before any session")
	T.ok(resolved.size() == 1 and is_same(resolved[0], r), "and emitted")
	await _drop(env)
	return T.done()


func test_keep_playing_delivers_later_result_via_signal() -> bool:
	var env := await _make("keep_playing")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	s.notify = true
	var resolved: Array = []
	s.payment_resolved.connect(func(r: ScfgPaymentResult) -> void: resolved.append(r))
	var box := _pay(s, "unlock", {"item": "chapter2"})
	var ctl := await _session(s, host)
	var sid: Variant = ctl.session_id
	(host.sheets[-1] as FakeSheet).closed.emit("keep_playing")
	var r: ScfgPaymentResult = box.get("result")
	T.eq([r.status, r.session_id] if r != null else null, ["pending", sid], "pending, while the payment confirms")
	T.eq([s._checkouts.size(), s._background.size()], [0, 1], "the controller moved to the background")
	T.eq(resolved.size(), 0, "nothing resolved yet")
	await _pass(s)
	T.eq(s.pending()[sid]["ui_status"], "pending", "the session is marked pending")
	await _pay_on_chain(env, ctl)
	T.eq(resolved.size(), 1, "the final result reaches payment_resolved")
	if resolved.size() == 1:
		var paid: ScfgPaymentResult = resolved[0]
		T.eq([paid.status, paid.item, paid.session_id, paid.tx_hash], ["paid", "chapter2", sid, TX], "paid")
	T.eq(host.notes, ["Payment confirmed: Chapter 2. Thank you!"], "a notification (notify is on)")
	T.eq(s._background.size(), 0, "a final background controller is closed and dropped")
	T.eq(s.owns("chapter2"), true, "owned")
	await _drop(env)
	return T.done()


func test_paid_tip_notifies_without_a_title_and_respects_notify_off() -> bool:
	var env := await _make("notify")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var called: Array = []
	_pay(s, "tip", {"amount": "3.00", "token": "USDC", "callback": func(r: ScfgPaymentResult) -> void: called.append(r)})
	var ctl: ScfgCheckoutController = (host.sheets[-1] as FakeSheet).controller
	T.eq(ctl.next_step(), true, "past the amount step")
	ctl = await _session(s, host)
	await _pay_on_chain(env, ctl)
	T.eq(called.map(func(r: ScfgPaymentResult) -> String: return r.status), ["paid"], "the callback gets the paid result")
	T.eq(host.notes, [], "no notification: notify is off by default")
	s.notify = true
	s._deliver(ScfgModels.make_payment_result({"status": "paid", "kind": "tip", "session_id": "ps_x"}))
	T.eq(host.notes, ["Payment confirmed. Thank you for your support!"], "a tip's notification")
	s._deliver(ScfgModels.make_payment_result({"status": "expired", "kind": "tip", "session_id": "ps_y"}))
	T.eq(host.notes.size(), 1, "only paid results notify")
	await _drop(env)
	return T.done()


func test_closing_the_sheet_releases_the_page_first() -> bool:
	var journal: Array = []
	var servers: Array = []
	var env := {}
	var factory := func(kw: Dictionary) -> Variant:
		var srv := RecordingServer.new(env["node"]._server_opts(kw))
		srv.journal = journal
		servers.append(srv)
		return srv
	env.merge(await _make("release", {"server_factory": factory}))
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var platform: FakePlatform = env["platform"]
	host.journal = journal
	_pay(s, "unlock", {"item": "chapter2"})
	var ctl := await _session(s, host)
	s.open_browser(ctl)
	T.eq(servers.size(), 1, "the browser checkout started the desktop page")
	var srv: RecordingServer = servers[0] if servers.size() == 1 else null
	if srv == null:
		await _drop(env)
		return T.done()
	T.eq(platform.opened, [srv.url], "and opened the browser on it")
	T.eq(srv.build_tx, null, "an EVM page gets no build_tx (no /build route, as in Python)")
	var base := "/c/%s/" % srv.secret
	var host_line := "Host: 127.0.0.1:%d\r\n" % srv.port
	T.eq(_http(s, srv.port, "GET %s HTTP/1.1\r\n%s\r\n" % [base, host_line]), 200, "the page is served")
	T.eq(_http(s, srv.port, "POST %sbuild HTTP/1.1\r\n%sContent-Length: 2\r\n\r\n{}" % [base, host_line]), 404,
		"/build is not a route on EVM")
	var weak: WeakRef = weakref(ctl)
	ctl = null
	(host.sheets[-1] as FakeSheet).closed.emit("cancel")
	T.eq(journal, [["page stopped", true, true], "sheet closed"],
		"the page is stopped while the controller still exists, and only then is the sheet closed")
	T.eq(srv.running, false, "not running")
	await (Engine.get_main_loop() as SceneTree).process_frame  # the sheet node is freed
	T.eq(weak.get_ref(), null, "the controller is released")
	T.eq([(srv.on_tx as Callable).is_valid(), (srv.get_status as Callable).is_valid()], [false, false],
		"the callbacks the page held are now invalid")
	for i in 3:
		s._pump()  # nothing left to pump the page or call into the controller
	# keep playing: the page keeps answering until the session is final or the browser timeout
	journal.clear()
	_pay(s, "unlock", {"item": "chapter2"})
	ctl = await _session(s, host)
	s.open_browser(ctl)
	srv = servers[-1]
	weak = weakref(ctl)
	ctl = null
	(host.sheets[-1] as FakeSheet).closed.emit("keep_playing")
	T.eq(journal, ["sheet closed"], "keep playing leaves the page running")
	T.eq(_http(s, srv.port, "GET /c/%s/status HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n" % [srv.secret, srv.port]), 200,
		"and answering")
	var clock: F.FakeClock = env["clock"]
	clock.advance(float(s._config["methods"]["browser"]["timeout_min"]) * 60.0 + 1.0)
	s._pump()
	T.eq(journal, ["sheet closed", ["page stopped", true, true]], "past the browser timeout the page is stopped first")
	T.eq(s._background.size(), 0, "and the controller dropped")
	await (Engine.get_main_loop() as SceneTree).process_frame
	T.eq(weak.get_ref(), null, "and released")
	await _drop(env)
	return T.done()


func test_no_sheet_ui_fails_clearly() -> bool:
	var env := await _make("no_ui")
	var s: Node = env["node"]
	s.sheet_host = ScfgSheetHost.new()  # the bare interface: it shows nothing
	var r: ScfgPaymentResult = await s.pay("unlock", {"item": "chapter2"})
	T.eq([r.status, r.error], ["failed", "sheet.unavailable"], "a failed result")
	T.eq(s._checkouts.size(), 0, "no checkout left open")
	T.ok(s._reported.size() == 1 and (s._reported[0] as String).begins_with("scfg: "), "reported (%s)" % [s._reported])
	var rr: ScfgRestoreResult = await s.restore()
	T.eq([rr.status, rr.error], ["failed", "sheet.unavailable"], "restore too")
	T.eq(s._restores.size(), 0, "no restore left open")
	await _drop(env)
	return T.done()


func test_misused_pay_resolves_failed_with_code() -> bool:
	var tree := Engine.get_main_loop() as SceneTree
	var bare: Node = SCFG.new()
	bare._setup({"store_path": _path("misuse_bare"), "quiet": true, "platform": FakePlatform.new()})
	tree.root.add_child(bare)
	var r: ScfgPaymentResult = await bare.pay("unlock", {"item": "chapter2"})
	T.eq([r.status, r.error, r.kind, r.item], ["failed", "configure.not_called", "unlock", "chapter2"], "before configure")
	var rr: ScfgRestoreResult = await bare.restore()
	T.eq([rr.status, rr.error], ["failed", "configure.not_called"], "restore before configure")
	T.eq(bare._reported.size(), 2, "both reported")
	bare.queue_free()
	var env := await _make("misuse")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var called: Array = []
	var cb := func(got: ScfgPaymentResult) -> void: called.append(got.error)
	var cases := [
		["unlock", {"item": "nope"}, "item.unknown"],
		["unlock", {}, "item.unknown"],
		["gift", {"item": "chapter2"}, "payment.bad_kind"],
		["tip", {"amount": "3.00"}, "tip.needs_amount_and_token"],
		["tip", {"amount": "0.10", "token": "USDC"}, "tip.below_minimum"],
		["unlock", {"item": "chapter2", "token": "DAI"}, "token.not_enabled"],
		["unlock", {"item": "chapter2", "colour": "red"}, "TypeError"],
		["unlock", {"item": "chapter2", "callback": "not callable"}, "TypeError"],
	]
	for c: Array in cases:
		var opts: Dictionary = (c[1] as Dictionary).duplicate()
		if not opts.has("callback"):
			opts["callback"] = cb
		var got: ScfgPaymentResult = await s.pay(c[0], opts)
		T.eq([got.status, got.error], ["failed", c[2]], "%s %s" % [c[0], c[1]])
	T.eq(called, ["item.unknown", "item.unknown", "payment.bad_kind", "tip.needs_amount_and_token",
		"tip.below_minimum", "token.not_enabled", "TypeError"], "a valid callback gets the failure too")
	T.eq(s._reported.size(), cases.size(), "each reported once (push_error)")
	var bad_restore: ScfgRestoreResult = await s.restore({"colour": "red"})
	T.eq([bad_restore.status, bad_restore.error], ["failed", "TypeError"], "restore() with an unknown option")
	T.eq([host.sheets.size(), s._checkouts.size(), s._restores.size()], [0, 0, 0], "no sheet was shown")
	await _drop(env)
	return T.done()


func test_interval_is_3s_while_a_sheet_is_open() -> bool:
	var env := await _make("interval")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var w: ScfgWorker = s._worker
	T.eq(w.interval.call(), 15.0, "15 s with nothing open")
	_pay(s, "unlock", {"item": "chapter2"})
	T.eq(w.interval.call(), 3.0, "3 s while the payment sheet is open")
	await _session(s, host)
	(host.sheets[-1] as FakeSheet).closed.emit("keep_playing")
	T.eq(w.interval.call(), 15.0, "15 s again for a background payment")
	_restore(s)
	T.eq(w.interval.call(), 3.0, "3 s while the restore screen is open")
	(host.sheets[-1] as FakeSheet).closed.emit("close")
	T.eq(w.interval.call(), 15.0, "15 s once it is closed")
	await _drop(env)
	return T.done()


func test_a_sheet_freed_without_closing_counts_as_cancel() -> bool:
	var env := await _make("freed_sheet")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var box := _pay(s, "unlock", {"item": "chapter2"})
	(host.sheets[-1] as FakeSheet).free()  # e.g. the game freed it with its scene
	await (Engine.get_main_loop() as SceneTree).process_frame  # noticed within a frame
	var r: ScfgPaymentResult = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "pay() resolves cancelled")
	T.eq(s._checkouts.size(), 0, "the checkout is closed")
	T.eq(host.closes, 0, "a freed sheet is not closed again")
	# queue_free() too
	box = _pay(s, "unlock", {"item": "chapter2"})
	(host.sheets[-1] as FakeSheet).queue_free()
	await (Engine.get_main_loop() as SceneTree).process_frame
	await (Engine.get_main_loop() as SceneTree).process_frame
	r = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "a queue_free()d sheet counts as cancel too")
	T.eq([s._checkouts.size(), host.closes], [0, 0], "closed, and the sheet not closed again")
	await _drop(env)
	return T.done()


## The sheet host for screens the game keeps out of the tree.
class DetachedHost:
	extends FakeHost

	func _open(kind: String, controller: Variant) -> Node:
		var sheet := FakeSheet.new()
		sheet.controller = controller
		sheets.append(sheet)
		kinds.append(kind)
		return sheet


func test_a_sheet_never_in_the_tree_or_reparented() -> bool:
	var env := await _make("detached_sheet")
	var s: Node = env["node"]
	var tree := Engine.get_main_loop() as SceneTree
	var detached := DetachedHost.new()
	s.sheet_host = detached
	var box := _pay(s, "unlock", {"item": "chapter2"})
	for i in 3:
		await tree.process_frame
	T.ok(not box.has("result"), "a sheet outside the tree keeps pay() waiting")
	T.eq(s._checkouts.size(), 1, "and its checkout open")
	(detached.sheets[-1] as FakeSheet).free()
	await tree.process_frame
	var r: ScfgPaymentResult = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "freed without ever entering the tree: cancelled")
	T.eq([s._checkouts.size(), detached.closes], [0, 0], "closed, and the freed sheet not closed again")
	# reparenting is not closing
	var host: FakeHost = env["host"]
	s.sheet_host = host
	box = _pay(s, "unlock", {"item": "chapter2"})
	var sheet: FakeSheet = host.sheets[-1]
	var other := Node.new()
	tree.root.add_child(other)
	sheet.reparent(other)
	for i in 3:
		await tree.process_frame
	T.ok(not box.has("result"), "a reparented sheet keeps pay() waiting")
	T.eq(s._checkouts.size(), 1, "and its checkout open")
	other.remove_child(sheet)
	for i in 3:
		await tree.process_frame
	T.ok(not box.has("result"), "so does a sheet taken out of the tree")
	other.add_child(sheet)
	sheet.closed.emit("cancel")
	r = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "its closed(how) still ends pay()")
	T.eq(host.closes, 1, "and it is closed")
	other.queue_free()
	await _drop(env)
	return T.done()


# --- quitting (Ren'Py's _shutdown) -----------------------------------------------------------


func test_quitting_with_the_sheet_open_closes_without_delivery() -> bool:
	var journal: Array = []
	var env := {}
	var factory := func(kw: Dictionary) -> Variant:
		var srv := RecordingServer.new(env["node"]._server_opts(kw))
		srv.journal = journal
		return srv
	env.merge(await _make("quit", {"server_factory": factory}))
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	s.notify = true
	var called: Array = []
	var resolved: Array = []
	s.payment_resolved.connect(func(r: ScfgPaymentResult) -> void: resolved.append(r))
	var cb := func(r: ScfgPaymentResult) -> void: called.append(r)
	# one payment kept playing in the background, one with its sheet open, and the restore screen
	_pay(s, "unlock", {"item": "chapter2", "callback": cb})
	var background := await _session(s, host)
	s.open_browser(background)
	(host.sheets[-1] as FakeSheet).closed.emit("keep_playing")
	var box := _pay(s, "unlock", {"item": "chapter2", "callback": cb})
	var ctl := await _session(s, host)
	var sid: Variant = ctl.session_id
	s.open_browser(ctl)
	var rbox := _restore(s, {"callback": func(r: ScfgRestoreResult) -> void: called.append(r)})
	T.eq(called.size() + resolved.size(), 0, "nothing delivered yet")
	journal.clear()
	(Engine.get_main_loop() as SceneTree).root.remove_child(s)  # the game quits
	var r: ScfgPaymentResult = box.get("result")
	T.eq([r.status, r.session_id] if r != null else null, ["cancelled", sid], "pay() is released, cancelled")
	var rr: ScfgRestoreResult = rbox.get("result")
	T.eq(rr.status if rr != null else null, "cancelled", "restore() is released, cancelled")
	T.eq([called.size(), resolved.size(), host.notes.size()], [0, 0, 0], "nothing is delivered or announced")
	T.eq(journal, [["page stopped", true, true], ["page stopped", true, true]],
		"both pages are stopped while their controllers exist")
	T.eq([s._checkouts.size(), s._background.size(), s._restores.size()], [0, 0, 0], "every controller is closed")
	T.ok(s.pending().has(sid) and s.pending()[sid].get("ui_status") != "cancelled",
		"the session is not cancelled: it is still watched on the next launch")
	await _drop(env)
	return T.done()


func test_awaiters_are_released_when_the_autoload_is_freed() -> bool:
	var env := await _make("freed_autoload")
	var s: Node = env["node"]
	var tree := Engine.get_main_loop() as SceneTree
	tree.root.remove_child(s)
	s.free()
	# an autoload instance that never enters the tree (the scheduler stands in for the tree)
	var lone: Node = SCFG.new()
	lone._setup({"store_path": _path("freed_autoload_lone"), "quiet": true, "platform": FakePlatform.new(),
		"scheduler": ManualTimer.new(), "transport": F.FakeTransport.new({}),
		"engine_opts": {"rpc_factory": func(_c: Dictionary) -> Variant: return F.FakeChain.new(1000)}})
	var host := FakeHost.new()
	lone.sheet_host = host
	T.eq(lone.configure({"wallet": DEV}), null, "configure")
	T.eq(lone.add_item("chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"}), null, "add_item")
	var box := _pay(lone, "unlock", {"item": "chapter2"})
	var rbox := _restore(lone)
	T.ok(not box.has("result") and not rbox.has("result"), "both wait")
	lone.free()
	var r: ScfgPaymentResult = box.get("result")
	var rr: ScfgRestoreResult = rbox.get("result")
	T.eq([r.status if r != null else null, rr.status if rr != null else null], ["cancelled", "cancelled"],
		"freeing the autoload releases pay() and restore() (cancelled), so no coroutine is left behind")
	for sheet: Variant in host.sheets:
		if is_instance_valid(sheet):
			(sheet as Node).queue_free()
	await tree.process_frame
	return T.done()


func test_pay_before_configure_reports_the_require_config_error() -> bool:
	var bare: Node = SCFG.new()
	bare._setup({"store_path": _path("require_config"), "quiet": true, "platform": FakePlatform.new()})
	(Engine.get_main_loop() as SceneTree).root.add_child(bare)
	T.eq(bare.owns("chapter2"), false, "owns() before configure")
	var r: ScfgPaymentResult = await bare.pay("unlock", {"item": "chapter2"})
	var rr: ScfgRestoreResult = await bare.restore()
	T.eq([r.error, rr.error], ["configure.not_called", "configure.not_called"], "both fail with the code")
	T.eq(bare._reported, ["scfg: call Scfg.configure(...) first", "scfg: call Scfg.configure(...) first",
		"scfg: call Scfg.configure(...) first"], "each reported once, with _require_config's error")
	bare.queue_free()
	await (Engine.get_main_loop() as SceneTree).process_frame
	return T.done()


# --- restore ---------------------------------------------------------------------------------


func test_restore_flow_restores_and_notifies() -> bool:
	var env := await _make("restore")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	s.notify = true
	var chain := F.FakeChain.new(1000)
	chain.receipts[TX] = {"status": "0x1", "logs": [F.transfer_log(USDC_ARB, PAYER, DEV, 4_993_742, {"block": 10, "tx_hash": TX})]}
	(env["chains"] as Dictionary)["arbitrum"] = chain
	var called: Array = []
	var box := _restore(s, {"callback": func(r: ScfgRestoreResult) -> void: called.append(r)})
	T.eq(host.kinds, ["restore"], "the restore screen is shown")
	var ctl: ScfgRestoreController = (host.sheets[-1] as FakeSheet).controller
	T.eq(ctl.check(TX), true, "a transaction hash")
	await _pass(s)  # the lookup; one candidate, classified: saved at once
	await _pass(s)  # the save
	T.eq(ctl.state, "done", "restored")
	T.eq(called.size(), 1, "the callback gets the result when it is done")
	if called.size() == 1:
		var got: ScfgRestoreResult = called[0]
		T.eq([got.status, got.restored.size()], ["restored", 1], "one receipt restored")
	T.eq(host.notes, ["Purchases restored."], "a notification")
	T.eq(s.owns("chapter2"), true, "owned")
	T.ok(not box.has("result"), "restore() waits for the screen to close")
	(host.sheets[-1] as FakeSheet).closed.emit("close")
	var r: ScfgRestoreResult = box.get("result")
	T.eq([r.status, r.restored.size()] if r != null else null, ["restored", 1], "restore() returns the result")
	T.eq([s._restores.size(), host.closes, called.size()], [0, 1, 1], "the screen is closed; the callback was called once")
	# closing before anything is done: cancelled, and the callback is dropped
	box = _restore(s, {"callback": func(got: ScfgRestoreResult) -> void: called.append(got)})
	(host.sheets[-1] as FakeSheet).closed.emit("close")
	r = box.get("result")
	T.eq(r.status if r != null else null, "cancelled", "closed at once")
	T.eq([called.size(), s._restore_callbacks.size()], [1, 0], "no callback for a cancelled restore")
	await _drop(env)
	return T.done()


# --- concurrent flows ------------------------------------------------------------------------


## Two pay() calls at once (an unlock, then a tip while its sheet is still open): two sheets, two
## sessions, and each call resolves from its own sheet, with its own callback, in any order.
func test_two_pay_calls_at_once_resolve_independently() -> bool:
	var env := await _make("two_pays")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var calls := {"unlock": [], "tip": []}
	var unlock := _pay(s, "unlock", {"item": "chapter2", "callback": func(r: ScfgPaymentResult) -> void: calls["unlock"].append(r)})
	var first := await _session(s, host)
	var tip := _pay(s, "tip", {"amount": "3.00", "token": "USDC", "callback": func(r: ScfgPaymentResult) -> void: calls["tip"].append(r)})
	var second := await _session(s, host)
	T.eq(host.kinds, ["payment", "payment"], "a sheet each")
	T.eq(s._checkouts.size(), 2, "two checkouts")
	T.ok(first.session_id != second.session_id, "two sessions (%s, %s)" % [first.session_id, second.session_id])
	await _pay_on_chain(env, second)
	T.eq(second.state, "paid", "the tip is paid")
	T.ok(first.state != "paid", "the unlock is not (%s)" % first.state)
	(host.sheets[1] as FakeSheet).closed.emit("close")
	var r: ScfgPaymentResult = tip.get("result")
	T.eq([r.status, r.kind, r.session_id] if r != null else null, ["paid", "tip", second.session_id], "the tip resolves paid")
	T.ok(not unlock.has("result"), "the unlock still waits for its sheet")
	T.eq(s._checkouts.keys(), [first.cid], "only its checkout is left")
	(host.sheets[0] as FakeSheet).closed.emit("cancel")
	r = unlock.get("result")
	T.eq([r.status, r.kind, r.session_id] if r != null else null, ["cancelled", "unlock", first.session_id], "the unlock resolves cancelled")
	T.eq([calls["unlock"].size(), calls["tip"].size()], [1, 1], "each callback once, with its own result")
	T.eq(s.owns("chapter2"), false, "the tip did not unlock chapter2")
	await _drop(env)
	return T.done()


## restore() while a pay() sheet is open: the restore screen opens over it, restores and closes,
## and the payment carries on and resolves from its own sheet.
func test_restore_during_pay() -> bool:
	var env := await _make("restore_during_pay")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var paying := _pay(s, "tip", {"amount": "3.00", "token": "USDC"})
	var ctl := await _session(s, host)
	var chain: F.FakeChain = env["chains"]["arbitrum"]
	var old_tx := "0x" + "cd".repeat(32)
	chain.receipts[old_tx] = {"status": "0x1", "logs": [F.transfer_log(USDC_ARB, PAYER, DEV, 4_993_742, {"block": 10, "tx_hash": old_tx})]}
	var restoring := _restore(s)
	T.eq(host.kinds, ["payment", "restore"], "the restore screen opens while the sheet is open")
	var rctl: ScfgRestoreController = (host.sheets[1] as FakeSheet).controller
	T.eq(rctl.check(old_tx), true, "a transaction hash")
	await _pass(s)  # the lookup
	await _pass(s)  # the save
	T.eq(rctl.state, "done", "restored")
	(host.sheets[1] as FakeSheet).closed.emit("close")
	var rr: ScfgRestoreResult = restoring.get("result")
	T.eq([rr.status, rr.restored.size()] if rr != null else null, ["restored", 1], "restore() resolves restored")
	T.eq(s.owns("chapter2"), true, "chapter2 is owned")
	T.ok(not paying.has("result"), "pay() still waits for its sheet")
	T.eq([s._checkouts.size(), s._restores.size()], [1, 0], "the checkout is still open")
	await _pay_on_chain(env, ctl)
	T.eq(ctl.state, "paid", "the tip confirms meanwhile")
	(host.sheets[0] as FakeSheet).closed.emit("close")
	var r: ScfgPaymentResult = paying.get("result")
	T.eq([r.status, r.kind, r.session_id] if r != null else null, ["paid", "tip", ctl.session_id], "pay() resolves paid")
	await _drop(env)
	return T.done()


# --- actions for the sheet -------------------------------------------------------------------


func test_mobile_offers_wallet_app_link() -> bool:
	var env := await _make("mobile")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var platform: FakePlatform = env["platform"]
	platform.mobile = true
	_pay(s, "unlock", {"item": "chapter2"})
	var ctl: ScfgCheckoutController = (host.sheets[-1] as FakeSheet).controller
	T.eq(ctl.mobile, true, "a mobile checkout")
	s.open_wallet_app(ctl)
	T.eq(platform.opened, [], "nothing to open before a session")
	ctl = await _session(s, host)
	T.ok(not ctl.methods().has("browser"), "no browser checkout on a phone (%s)" % [ctl.methods()])
	var uri: Variant = ctl.payment_uri()
	T.ok(uri is String and (uri as String).begins_with("ethereum:"), "an EIP-681 link (%s)" % [uri])
	s.open_wallet_app(ctl)
	T.eq(platform.opened, [uri], "Open wallet app opens it")
	s.open_explorer("https://arbiscan.io/tx/" + TX)
	s.open_explorer(null)
	s.open_explorer("")
	T.eq(platform.opened.size(), 2, "open_explorer opens a URL, and nothing for none")
	await _drop(env)
	return T.done()


func test_copy_and_paste_use_the_clipboard() -> bool:
	var env := await _make("clipboard")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var platform: FakePlatform = env["platform"]
	s.copy(TX)
	T.eq(platform.clipboard, TX, "copy")
	_pay(s, "unlock", {"item": "chapter2"})
	var ctl := await _session(s, host)
	ctl.error = "invalid_hash"
	platform.clipboard = "  0x12ab-CD!ef  and more words\n"
	s.paste_into(ctl)
	T.eq([ctl.tx_input, ctl.error], ["0x12abCDef", null], "the first word, hash characters only (Ren'Py's _clean_paste)")
	platform.clipboard = "x".repeat(100)
	s.paste_into(ctl)
	T.eq(ctl.tx_input.length(), 90, "at most 90 characters")
	platform.clipboard = "   "
	s.paste_into(ctl)
	T.eq([ctl.tx_input.length(), ctl.error], [90, null], "an empty clipboard pastes nothing")
	_restore(s)
	var rc: ScfgRestoreController = (host.sheets[-1] as FakeSheet).controller
	platform.clipboard = TX
	s.paste_into(rc)
	T.eq(rc.tx_input, TX, "into the restore screen's field")
	rc.tx_input = ""
	rc.state = "checking"
	s.paste_into(rc)
	T.eq(rc.tx_input, "", "dropped once the player pressed Check")
	await _drop(env)
	return T.done()


## Ren'Py's _clean_paste: `(text or "").split()[0]`, then only TX_CHARS, then `[:90]` (90 hash
## characters, not the first 90 characters filtered), and it stops at 90.
func test_clean_paste_matches_renpy_and_stops_at_90() -> bool:
	var cases := [
		["  0x12ab-CD!ef  and more words\n", "0x12abCDef"],
		["x".repeat(100), "x".repeat(90)],
		["-".repeat(50) + "a".repeat(100), "a".repeat(90)],
		["a-".repeat(100), "a".repeat(90)],
		["\u3000abc def", "abc"],
		["ab\u00a0cd", "ab"],
		["\n\t ", ""],
		["", ""],
		[null, ""],
	]
	for c: Array in cases:
		T.eq(SCFG._clean_paste(c[0]), c[1], "clean_paste(%s)" % JSON.stringify(c[0]).left(40))
	var huge := "a".repeat(100_000)
	var started := Time.get_ticks_usec()
	var cleaned: String = SCFG._clean_paste(huge)
	var took_ms := (Time.get_ticks_usec() - started) / 1000.0
	T.eq(cleaned.length(), 90, "a 100 kB word: 90 characters")
	T.ok(took_ms < 100.0, "and it stops there instead of filtering the whole word (%.1f ms)" % took_ms)
	return T.done()


# --- the pay() callback map (Ren'Py's _state.callbacks; tests/unit/test_renpy_callbacks.py) --------


func test_a_delivered_paid_result_drops_the_callback() -> bool:
	var env := await _make("callback_paid")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var called: Array = []
	_pay(s, "unlock", {"item": "chapter2", "callback": func(r: ScfgPaymentResult) -> void: called.append(r.status)})
	var ctl := await _session(s, host)
	var sid: Variant = ctl.session_id
	(host.sheets[-1] as FakeSheet).closed.emit("keep_playing")
	await _pass(s)
	T.ok(s._callbacks.has(sid), "registered while the payment is pending")
	await _pay_on_chain(env, ctl)
	T.eq(called, ["paid"], "the callback got the final result")
	T.eq(s._callbacks.size(), 0, "and is dropped: the session is over")
	await _pass(s)
	T.eq(s._callbacks.size(), 0, "not registered again")
	await _drop(env)
	return T.done()


func test_an_expired_session_keeps_its_callback_until_it_is_paid() -> bool:
	var env := await _make("callback_expired")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var called: Array = []
	_pay(s, "unlock", {"item": "chapter2", "callback": func(r: ScfgPaymentResult) -> void: called.append(r.status)})
	var ctl := await _session(s, host)
	var sid: Variant = ctl.session_id
	(host.sheets[-1] as FakeSheet).closed.emit("keep_playing")
	await _pass(s)
	(env["clock"] as F.FakeClock).advance(31 * 60)
	await _pass(s)
	T.eq(called, ["expired"], "the window closed")
	T.eq(s._background.size(), 0, "its controller is closed")
	T.ok(s._callbacks.has(sid), "the scan goes on: a later payment still reaches the callback")
	await _pay_on_chain(env, ctl)
	T.eq(called, ["expired", "paid"], "the payment found later")
	T.eq(s._callbacks.size(), 0, "dropped once paid")
	await _drop(env)
	return T.done()


## A sheet left open on an expired session: once the session leaves the Engine at its scan horizon
## the callback is dropped, and the sheet's controller, still polled by every pump, does not
## register it again (no snapshot would ever list the session to prune it).
func test_an_open_sheet_past_the_scan_horizon_does_not_register_its_callback_again() -> bool:
	var env := await _make("callback_horizon")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	_pay(s, "unlock", {"item": "chapter2", "callback": func(_r: ScfgPaymentResult) -> void: pass})
	var ctl := await _session(s, host)
	var sid: Variant = ctl.session_id
	await _pass(s)
	(env["clock"] as F.FakeClock).advance(31 * 60)
	await _pass(s)
	T.eq(ctl.state, "expired", "the open sheet shows the expired session")
	T.ok(s._callbacks.has(sid), "the scan goes on")
	(env["clock"] as F.FakeClock).advance(24 * 3600)
	await _pass(s)
	await _pass(s)
	T.ok(not ((s._worker as ScfgWorker).snapshot["pending"] as Dictionary).has(sid), "the session left the Engine")
	T.eq(s._callbacks.size(), 0, "its callback is dropped")
	await _pass(s)
	await _pass(s)
	T.eq(s._callbacks.size(), 0, "not registered again")
	T.eq(s._checkouts.size(), 1, "the sheet is still open")
	(host.sheets[-1] as FakeSheet).closed.emit("cancel")
	await _drop(env)
	return T.done()


func test_a_discarded_session_drops_its_callback() -> bool:
	var env := await _make("callback_discarded")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	_pay(s, "unlock", {"item": "chapter2", "callback": func(_r: ScfgPaymentResult) -> void: pass})
	var ctl := await _session(s, host)
	var first: Variant = ctl.session_id
	await _pass(s)
	T.ok(s._callbacks.has(first), "registered")
	T.eq(ctl.choose_network("base"), null, "the player switched networks: the unpaid session is discarded")
	await _pass(s)
	await _pass(s)
	var second: Variant = ctl.session_id
	T.ok(ScfgPy.py_truthy(second) and second != first, "a new session")
	T.eq(s._callbacks.keys(), [second], "only the live session keeps a callback")
	(host.sheets[-1] as FakeSheet).closed.emit("cancel")
	await _drop(env)
	return T.done()


## The state guard of _register_callback: no snapshot ever lists the session (so _prune_callbacks
## cannot drop it), and the paid controller is still polled in the pump that delivered the result.
func test_a_session_paid_before_any_snapshot_listed_it_releases_its_callback() -> bool:
	var env := await _make("callback_unlisted")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var worker: ScfgWorker = s._worker
	var frozen: Variant = worker.snapshot
	var called: Array = []
	_pay(s, "unlock", {"item": "chapter2", "callback": func(r: ScfgPaymentResult) -> void: called.append(r.status)})
	var ctl: ScfgCheckoutController = (host.sheets[-1] as FakeSheet).controller
	T.eq(ctl.choose_network("arbitrum"), null, "choose a network")
	await worker.run_once()
	worker.snapshot = frozen  # no snapshot lists the session
	s._pump()
	var sid: Variant = ctl.session_id
	T.ok(ScfgPy.py_truthy(sid) and s._callbacks.has(sid), "registered")
	(host.sheets[-1] as FakeSheet).closed.emit("keep_playing")
	var chain: F.FakeChain = env["chains"]["arbitrum"]
	var block := chain.head + 1
	chain.logs.append(F.transfer_log(USDC_ARB, PAYER, DEV, ctl.session["units"], {"block": block, "tx_hash": TX}))
	chain.head = block
	await worker.run_once()
	worker.snapshot = frozen
	s._pump()
	chain.head = block + 19
	await worker.run_once()
	worker.snapshot = frozen
	s._pump()
	T.eq(called, ["paid"], "the callback got the paid result")
	T.eq(s._callbacks.size(), 0, "released although no snapshot listed it")
	await worker.run_once()
	worker.snapshot = frozen
	s._pump()
	T.eq(s._callbacks.size(), 0, "not registered again")
	await _drop(env)
	return T.done()


func test_the_snapshot_is_read_before_the_outcomes_are_drained() -> bool:
	var env := await _make("callback_race")
	var s: Node = env["node"]
	var host: FakeHost = env["host"]
	var called: Array = []
	_pay(s, "unlock", {"item": "chapter2", "callback": func(r: ScfgPaymentResult) -> void: called.append(r.status)})
	var ctl := await _session(s, host)
	var sid: Variant = ctl.session_id
	(host.sheets[-1] as FakeSheet).closed.emit("keep_playing")
	await _pass(s)
	await _pass(s)  # listed by a snapshot: the callback could be pruned
	var old: ScfgWorker = s._worker
	var racing := RacingWorker.new(s._engine, {"scheduler": ManualTimer.new()})
	racing.snapshot = old.snapshot
	racing.sid = sid
	racing.paid = ScfgWatcher.step_outcome(sid, "paid", {"result": ScfgModels.make_payment_result(
		{"status": "paid", "kind": "unlock", "item": "chapter2", "session_id": sid})})
	s._worker = racing
	s._pump()  # drains nothing, then the pass lands
	s._pump()  # drains the paid outcome
	T.eq(called, ["paid"], "the paid result reached the callback")
	T.eq(s._callbacks.size(), 0, "released")
	s._worker = old
	await _drop(env)
	return T.done()
