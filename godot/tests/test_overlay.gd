## ScfgOverlayCheckout (addons/scfg/core/overlay.gd): the browser checkout as an overlay inside the
## game's own web page. Twin of tests/unit/test_overlay.py: `ui` is a fake page and the Solana
## transaction builds are ScfgJobs.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const TX := "0xabababababababababababababababababababababababababababababababab"
const SOL := "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3"


class FakeUi:
	extends RefCounted
	var calls: Array = []
	var inbox: Array = []

	func open(key: String, info: Dictionary, logo_url: Variant) -> void:
		calls.append(["open", key, info["title"], logo_url])

	func show(key: String) -> void:
		calls.append(["show", key])

	func close(key: String) -> void:
		calls.append(["close", key])

	func take(_key: String) -> Array:
		var out := inbox
		inbox = []
		return out

	func set_status(_key: String, status: Dictionary) -> void:
		calls.append(["status", status["state"]])

	func reply(_key: String, rid: int, status: int, body: Dictionary) -> void:
		calls.append(["reply", rid, status, body])

	func replies() -> Array:
		return calls.filter(func(c: Array) -> bool: return c[0] == "reply")

	func count(kind: String) -> int:
		return calls.filter(func(c: Array) -> bool: return c[0] == kind).size()


## {overlay, ui, hints, status, clock}
func _make(build_tx: Variant = null, clock: F.FakeClock = null) -> Dictionary:
	var ui := FakeUi.new()
	var hints: Array = []
	var status := {"state": "waiting"}
	var clk := clock if clock != null else F.FakeClock.new(0.0)
	var o := ScfgOverlayCheckout.new({"ui": ui, "key": "o1", "session_info": {"title": "Chapter 2"},
		"on_tx": func(tx: Variant, sender: Variant = null) -> void: hints.append([tx, sender]),
		"get_status": func() -> Dictionary: return status.duplicate(),
		"logo_url": "data:image/png;base64,AA", "build_tx": build_tx, "clock": clk.fn})
	return {"overlay": o, "ui": ui, "hints": hints, "status": status, "clock": clk}


# tests/unit/test_overlay.py::test_start_opens_the_overlay_and_pushes_the_status
func test_start_opens_the_overlay_and_pushes_the_status() -> bool:
	var m := _make()
	var o: ScfgOverlayCheckout = m["overlay"]
	var ui: FakeUi = m["ui"]
	T.eq(o.start(), null, "start succeeds (null)")
	T.eq([o.url, o.running], ["overlay:checkout", true], "its url, and running")
	T.eq(ui.calls, [["open", "o1", "Chapter 2", "data:image/png;base64,AA"], ["status", "waiting"]], "opened, status pushed")
	return T.done()


# tests/unit/test_overlay.py::test_status_is_pushed_only_when_it_changes
func test_status_is_pushed_only_when_it_changes() -> bool:
	var m := _make()
	var o: ScfgOverlayCheckout = m["overlay"]
	var ui: FakeUi = m["ui"]
	o.start()
	o.pump()
	o.pump()
	m["status"]["state"] = "confirming"
	o.pump()
	T.eq(ui.calls.filter(func(c: Array) -> bool: return c[0] == "status"), [["status", "waiting"], ["status", "confirming"]], "only changes")
	return T.done()


# tests/unit/test_overlay.py::test_a_tx_message_becomes_a_hint
func test_a_tx_message_becomes_a_hint() -> bool:
	var m := _make()
	var o: ScfgOverlayCheckout = m["overlay"]
	o.start()
	(m["ui"] as FakeUi).inbox = [{"type": "tx", "body": {"tx_hash": TX}}]
	o.pump()
	T.eq(m["hints"], [[TX, null]], "the hint")
	return T.done()


# tests/unit/test_overlay.py::test_garbled_messages_are_ignored
func test_garbled_messages_are_ignored() -> bool:
	var m := _make(func(_account: Variant) -> Variant: return ScfgJob.new())
	var o: ScfgOverlayCheckout = m["overlay"]
	var ui: FakeUi = m["ui"]
	o.start()
	ui.inbox = ["x", null, {"type": "tx"}, {"type": "tx", "body": "x"}, {"type": "tx", "body": {"tx_hash": "0x12"}},
		{"type": "nope"}, {"type": "build", "id": "7", "body": {"account": SOL}},
		{"type": "build", "id": true, "body": {"account": SOL}}, {"type": "build", "id": 7.5, "body": {"account": SOL}}]
	o.pump()
	T.eq(m["hints"], [], "no hint")
	T.eq(ui.replies(), [], "no reply")
	return T.done()


# tests/unit/test_overlay.py::test_build_replies_when_the_future_is_done
func test_build_replies_when_the_future_is_done() -> bool:
	var job := ScfgJob.new()
	var m := _make(func(_account: Variant) -> Variant: return job)
	var o: ScfgOverlayCheckout = m["overlay"]
	var ui: FakeUi = m["ui"]
	o.start()
	ui.inbox = [{"type": "build", "id": 1, "body": {"account": SOL}}]
	o.pump()
	T.eq(ui.replies(), [], "still building")
	job._succeed({"transaction": "AAA"})
	o.pump()
	T.ok(ui.replies().has(["reply", 1, 200, {"transaction": "AAA"}]), "replied 200 (%s)" % [ui.replies()])
	return T.done()


# tests/unit/test_overlay.py::test_build_failures_and_bad_accounts
func test_build_failures_and_bad_accounts() -> bool:
	var failing := ScfgJob.new()
	failing._fail(ScfgErr.make("RuntimeError", "rpc down"))
	var m := _make(func(_account: Variant) -> Variant: return failing)
	var o: ScfgOverlayCheckout = m["overlay"]
	var ui: FakeUi = m["ui"]
	o.start()
	ui.inbox = [{"type": "build", "id": 1, "body": {"account": SOL}}, {"type": "build", "id": 2, "body": {"account": "0xabc"}}]
	o.pump()
	T.ok(ui.replies().has(["reply", 1, 503, {"error": "unavailable"}]), "a failed build is 503 (%s)" % [ui.replies()])
	T.ok(ui.replies().has(["reply", 2, 400, {"error": "bad account"}]), "a bad account is 400")
	return T.done()


# tests/unit/test_overlay.py::test_build_without_solana_is_not_found
func test_build_without_solana_is_not_found() -> bool:
	var m := _make(null)
	var o: ScfgOverlayCheckout = m["overlay"]
	var ui: FakeUi = m["ui"]
	o.start()
	ui.inbox = [{"type": "build", "id": 3, "body": {"account": SOL}}]
	o.pump()
	T.ok(ui.replies().has(["reply", 3, 404, {"error": "not_found"}]), "not found (%s)" % [ui.replies()])
	return T.done()


# tests/unit/test_overlay.py::test_a_build_that_takes_too_long_is_cancelled
func test_a_build_that_takes_too_long_is_cancelled() -> bool:
	var clock := F.FakeClock.new(0.0)
	var never := ScfgJob.new()
	var m := _make(func(_account: Variant) -> Variant: return never, clock)
	var o: ScfgOverlayCheckout = m["overlay"]
	var ui: FakeUi = m["ui"]
	o.start()
	ui.inbox = [{"type": "build", "id": 1, "body": {"account": SOL}}]
	o.pump()
	clock.advance(16)
	o.pump()
	T.ok(never.cancelled(), "the job is cancelled")
	T.ok(ui.replies().has(["reply", 1, 503, {"error": "unavailable"}]), "and the page is told")
	return T.done()


# tests/unit/test_overlay.py::test_reopen_shows_the_same_overlay
func test_reopen_shows_the_same_overlay() -> bool:
	var m := _make()
	var o: ScfgOverlayCheckout = m["overlay"]
	var ui: FakeUi = m["ui"]
	o.reopen()
	T.eq(ui.calls, [], "nothing before start")
	o.start()
	o.reopen()
	T.eq(ui.count("open"), 1, "opened once")
	T.ok(ui.calls.has(["show", "o1"]), "and shown again")
	return T.done()


# tests/unit/test_overlay.py::test_stop_closes_once_and_pump_does_nothing_after
func test_stop_closes_once_and_pump_does_nothing_after() -> bool:
	var m := _make()
	var o: ScfgOverlayCheckout = m["overlay"]
	var ui: FakeUi = m["ui"]
	o.start()
	o.stop()
	o.stop()
	ui.inbox = [{"type": "tx", "body": {"tx_hash": TX}}]
	o.pump()
	T.eq(ui.calls.filter(func(c: Array) -> bool: return c[0] == "close"), [["close", "o1"]], "closed once")
	T.eq(m["hints"], [], "no hint after stop")
	T.eq(o.running, false, "not running")
	return T.done()


# tests/unit/test_overlay.py::test_a_build_handed_back_after_build_tx_stopped_the_overlay_is_cancelled
func test_a_build_handed_back_after_build_tx_stopped_the_overlay_is_cancelled() -> bool:
	var job := ScfgJob.new()
	var holder := {}
	var m := _make(func(_account: Variant) -> Variant:
		(holder["o"] as ScfgOverlayCheckout).stop()
		return job)
	var o: ScfgOverlayCheckout = m["overlay"]
	holder["o"] = o
	var ui: FakeUi = m["ui"]
	o.start()
	ui.inbox = [{"type": "build", "id": 1, "body": {"account": SOL}}]
	o.pump()
	T.ok(job.cancelled(), "the build nobody answers must not run")
	T.eq(ui.replies(), [], "no reply")
	T.eq(o._builds, {}, "nothing kept for it")
	holder.clear()  # the overlay holds the Callable that holds it: break the cycle
	return T.done()


# tests/unit/test_overlay.py::test_the_rest_of_a_batch_is_ignored_once_a_callback_stopped_the_overlay
func test_the_rest_of_a_batch_is_ignored_once_a_callback_stopped_the_overlay() -> bool:
	var ui := FakeUi.new()
	var hints: Array = []
	var built: Array = []
	var status := {"state": "waiting"}
	var holder := {}
	var o := ScfgOverlayCheckout.new({"ui": ui, "key": "o1", "session_info": {"title": "Chapter 2"},
		"on_tx": func(tx: Variant, _sender: Variant = null) -> void:
			hints.append(tx)
			(holder["o"] as ScfgOverlayCheckout).stop(),
		"get_status": func() -> Dictionary: return status.duplicate(),
		"build_tx": func(account: Variant) -> Variant:
			built.append(account)
			return ScfgJob.new(),
		"clock": F.FakeClock.new(0.0).fn})
	holder["o"] = o
	o.start()
	status["state"] = "confirming"
	ui.inbox = [{"type": "tx", "body": {"tx_hash": TX}}, {"type": "tx", "body": {"tx_hash": "0x" + "cd".repeat(32)}},
		{"type": "build", "id": 2, "body": {"account": SOL}}]
	o.pump()
	T.eq(hints, [TX], "only the first hint")
	T.eq(built, [], "no build after stop")
	T.eq(o._builds, {}, "nothing kept")
	T.ok(not ui.calls.has(["status", "confirming"]), "no status pushed to a closed overlay")
	holder.clear()  # the overlay holds the Callables that hold it: break the cycle
	return T.done()


# Godot only: the callbacks belong to a controller that may have been released; a freed one is
# skipped, never a script error (README "M4 contracts").
func test_callbacks_of_a_released_controller_are_skipped() -> bool:
	var owner := _Owner.new()
	var ui := FakeUi.new()
	var o := ScfgOverlayCheckout.new({"ui": ui, "key": "o1", "session_info": {"title": "T"},
		"on_tx": owner.on_tx, "get_status": owner.status, "build_tx": owner.build})
	o.start()
	T.eq(ui.calls.filter(func(c: Array) -> bool: return c[0] == "status"), [["status", "waiting"]], "status from a live owner")
	owner = null  # freed: the Callables are invalid now
	ui.inbox = [{"type": "tx", "body": {"tx_hash": TX}}, {"type": "build", "id": 4, "body": {"account": SOL}}]
	o.pump()
	T.ok(ui.replies().has(["reply", 4, 503, {"error": "unavailable"}]), "a build of a released owner is unavailable (%s)" % [ui.replies()])
	o.stop()
	T.eq(ui.count("close"), 1, "and it still closes")
	return T.done()


class _Owner:
	extends RefCounted
	var hints: Array = []

	func on_tx(tx: Variant, sender: Variant = null) -> void:
		hints.append([tx, sender])

	func status() -> Dictionary:
		return {"state": "waiting"}

	func build(_account: Variant) -> Variant:
		return ScfgJob.new()
