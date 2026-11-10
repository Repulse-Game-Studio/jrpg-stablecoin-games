## The Scfg autoload (addons/scfg/scfg.gd): configuration, queries, the pump's persistence and
## the editor plugin. Behaviour authority: plugin/game/scfg/scfg_api.rpy.
##
## Each test builds its own instance of the autoload script (never the project's `Scfg`
## singleton) through its test hook `_setup`: a state file under user://scfg-tests/api/, a fake
## clock for the save interval and the Engine, a manual worker scheduler (a run happens only
## when the test awaits `run_once()`), an RPC factory of FakeChains and a fake transport, and
## `quiet` so developer errors are recorded in `_reported` instead of printed.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")
const SCFG := preload("res://addons/scfg/scfg.gd")
const HTTP := preload("res://tests/test_http_transport.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const DIR := "user://scfg-tests/api"
const LOGO := "res://tests/fixtures/logo.png"


## A manual scheduler: nothing fires unless a test calls fire().
class ManualTimer:
	extends RefCounted
	var pending: Variant = null

	func schedule(cb: Callable, seconds: float) -> Variant:
		pending = {"cb": cb, "seconds": seconds}
		return pending

	func cancel(handle: Variant) -> void:
		if is_same(pending, handle):
			pending = null


## A FakeChain whose every call, once `boom` is set, waits a few frames and then fails with a
## RuntimeError: an error the Engine contains and reports to its on_error.
class BoomChain:
	extends F.FakeChain
	var boom := false

	func _check(name: String) -> Variant:
		if boom:
			for i in 5:
				await (Engine.get_main_loop() as SceneTree).process_frame
			return ScfgErr.make("RuntimeError", "boom after a few frames")
		return await super(name)


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


## {node, chains, clock}: an autoload instance under the tree root. A coroutine (the runner
## starts before the root is inside the tree).
func _make(path: String) -> Dictionary:
	var tree := Engine.get_main_loop() as SceneTree
	if not tree.root.is_inside_tree():
		await tree.process_frame
	var chains := {}
	var factory := func(chain: Dictionary) -> Variant:
		if not chains.has(chain["key"]):
			chains[chain["key"]] = BoomChain.new(1000)
		chains[chain["key"]].chain_obj = chain
		return chains[chain["key"]]
	var clock := F.FakeClock.new(1000.0)
	var node: Node = SCFG.new()
	node._setup({"store_path": path, "clock": clock.fn, "scheduler": ManualTimer.new(),
		"transport": F.FakeTransport.new({}), "engine_opts": {"rpc_factory": factory, "clock": clock.fn},
		"quiet": true})
	tree.root.add_child(node)
	return {"node": node, "chains": chains, "clock": clock}


func _configured(path: String) -> Dictionary:
	var env := await _make(path)
	var s: Node = env["node"]
	T.eq(s.configure({"wallet": DEV}), null, "configure")
	T.eq(s.add_item("chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"}), null, "add_item")
	return env


func _drop(env: Dictionary) -> void:
	var s: Node = env["node"]
	s.queue_free()
	await (Engine.get_main_loop() as SceneTree).process_frame


static func _receipt(tx: String) -> Dictionary:
	return {"chain": "arbitrum", "tx_hash": tx, "log_index": 3, "block": 990, "kind": "unlock", "item": "chapter2",
		"amount": "4.99", "token": "USDC", "payer": "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed",
		"recipient": DEV.to_lower(), "source": "payment"}


## The first pump frame starts the Worker; its first pass (launch re-verification of the stored
## receipts, which a FakeChain does not know) runs here, before a test adds receipts.
func _start(s: Node) -> void:
	s._pump()
	await (s._worker as ScfgWorker).run_once()
	s._pump()


## Runs `fn(engine)` in one worker pass, then one pump frame.
func _run(s: Node, fn: Callable) -> Variant:
	var w: ScfgWorker = s._worker
	var job := w.submit(fn)
	await w.run_once()
	s._pump()
	return job.result()


func _start_session(s: Node) -> Variant:
	return await _run(s, func(e: ScfgEngine) -> Variant:
		return await e.start_session("unlock", "arbitrum", {"item": "chapter2"}))


func _add_receipt(s: Node, tx: String) -> Variant:
	return await _run(s, func(e: ScfgEngine) -> Variant:
		return ScfgReceipts.add_receipt(e.receipts, _receipt(tx)))


func _on_disk(path: String) -> Dictionary:
	return ScfgStore.load(path)


# --- configuration ---------------------------------------------------------------------------


func test_configure_validates_like_renpy() -> bool:
	var env := await _make(_path("configure"))
	var s: Node = env["node"]
	var got: Variant = s.configure({"wallet": "0x123"})
	T.is_err(got, "ConfigError", "a bad wallet")
	T.eq(got.code if got is ScfgErr else null, "address.invalid", "Ren'Py's code")
	T.eq(s._reported.size(), 1, "reported once (push_error)")
	T.ok(s._reported.size() == 1 and (s._reported[0] as String).begins_with("scfg: wallet: "),
		"the report is `scfg: ` + the message (%s)" % [s._reported])
	T.eq(s._config, null, "not configured")
	got = s.configure({})
	T.eq(got.code if got is ScfgErr else null, "wallet.missing", "no wallet")
	got = s.configure({"wallet": DEV, "colour": "red"})
	T.is_err(got, "TypeError", "an unknown option")
	got = s.configure({"wallet": DEV, "custom_chains": []})
	T.is_err(got, "TypeError", "custom_chains is add_chain's, not an option")
	T.eq(s.add_item("chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"}).code, "configure.not_called",
		"add_item before configure")
	T.eq(s.configure({"wallet": DEV, "testnet": true}), null, "a good config")
	T.eq(s._config["wallet"], DEV, "kept")
	T.eq(s.add_item("chapter2", {"code": 0, "price": "4.99", "token": "USDC", "title": "Chapter 2"}).code, "item.invalid_code",
		"add_item validates")
	T.eq(s.supported(), true, "supported() is always true")
	await _drop(env)
	return T.done()


func test_add_chain_after_configure_is_an_error() -> bool:
	var env := await _make(_path("add_chain"))
	var s: Node = env["node"]
	var chain := {"name": "My L2", "chain_id": 777001, "rpc": ["https://rpc.my-l2.example"],
		"tokens": {"USDC": "0x52908400098527886E0F7030069857D2E4169EE7"}, "confirmations": 3,
		"explorer_tx": "https://scan.my-l2.example/tx/{}"}
	T.eq(s.add_chain("myl2", chain), null, "add_chain before configure")
	var bad: Variant = s.add_chain("bad key!", chain)
	T.eq(bad.code if bad is ScfgErr else null, "chain.invalid_key", "add_chain validates")
	T.eq(s.configure({"wallet": DEV, "chains": ["base", "myl2"]}), null, "the custom chain is usable")
	var late: Variant = s.add_chain("other", chain)
	T.is_err(late, "ConfigError", "add_chain after configure")
	T.eq(late.code if late is ScfgErr else null, "configure.after_add_chain", "Ren'Py's code")
	T.ok(s._reported.size() >= 2 and (s._reported[-1] as String).begins_with("scfg: "), "reported")
	await _drop(env)
	return T.done()


func test_logo_res_path() -> bool:
	var env := await _make(_path("logo"))
	var s: Node = env["node"]
	T.eq(s.configure({"wallet": DEV, "logo": LOGO}), null, "a res:// logo")
	T.eq(s._config["logo"], "tests/fixtures/logo.png", "the core gets the path without res://")
	T.eq(s._logo_path(), LOGO, "resolved back to res://")
	var file: Variant = s._logo_file()
	T.ok(file is Array and file.size() == 2, "the logo's bytes and type")
	if file is Array and file.size() == 2:
		T.eq(file[0], FileAccess.get_file_as_bytes(LOGO), "the file's bytes")
		T.eq(file[1], "image/png", "its content type")
	T.eq(s.configure({"wallet": DEV, "logo": "tests/fixtures/logo.png"}), null, "a project-relative path")
	T.eq(s._logo_path(), LOGO, "is under res:// too")
	var missing: Variant = s.configure({"wallet": DEV, "logo": "res://tests/fixtures/nope.png"})
	T.eq(missing.code if missing is ScfgErr else null, "logo.file_missing", "a missing file")
	var bad: Variant = s.configure({"wallet": DEV, "logo": "user://logo.png"})
	T.eq(bad.code if bad is ScfgErr else null, "logo.bad_path", "user:// is not in the game")
	bad = s.configure({"wallet": DEV, "logo": "res://tests/fixtures/logo.svg"})
	T.eq(bad.code if bad is ScfgErr else null, "logo.bad_type", "raster images only")
	bad = s.configure({"wallet": DEV, "logo": "res://../logo.png"})
	T.eq(bad.code if bad is ScfgErr else null, "logo.bad_path", "no escaping the project")
	T.eq(s.configure({"wallet": DEV}), null, "no logo")
	T.eq([s._logo_path(), s._logo_file()], [null, null], "none")
	await _drop(env)
	return T.done()


# --- queries ---------------------------------------------------------------------------------


func test_owns_after_receipt() -> bool:
	var path := _path("owns")
	var env := await _configured(path)
	var s: Node = env["node"]
	T.eq(s.owns("chapter2"), false, "not owned before the worker starts")
	await _start(s)
	T.ok(s._worker is ScfgWorker, "the first pump starts the worker")
	T.eq(s.owns("chapter2"), false, "not owned yet")
	var changed := [0]
	s.receipts_changed.connect(func() -> void: changed[0] += 1)
	T.eq(await _add_receipt(s, "0x" + "ab".repeat(32)), true, "a receipt is stored")
	T.eq(s.owns("chapter2"), true, "owned")
	T.eq(changed[0], 1, "receipts_changed once")
	T.eq(s.receipts().size(), 1, "receipts()")
	T.eq(s.has_tipped(), false, "no tip")
	T.eq(s.tip_total(), "0", "tip_total() is Python's str(Decimal)")
	T.eq(s.owns("nothing"), false, "an unknown item")
	T.eq(s._reported, ["scfg: unknown item 'nothing'; declare it with Scfg.add_item()"], "is reported")
	await _run(s, func(e: ScfgEngine) -> Variant:
		var tip := _receipt("0x" + "cd".repeat(32))
		tip["kind"] = "tip"
		tip["item"] = null
		tip["amount"] = "2.50"
		return ScfgReceipts.add_receipt(e.receipts, tip))
	T.eq([s.has_tipped(), s.tip_total()], [true, "2.50"], "a tip")
	var exported: Variant = s.export_receipts(DIR.path_join("owns/export.json"))
	T.eq(exported, ProjectSettings.globalize_path(DIR.path_join("owns/export.json")), "export_receipts returns the file's path")
	var listed: Variant = JSON.parse_string(FileAccess.get_file_as_string(DIR.path_join("owns/export.json")))
	T.ok(listed is Array and listed.size() == 2, "the receipts as a JSON list")
	var reported: int = s._reported.size()
	var failed: Variant = s.export_receipts(DIR.path_join("owns"))  # a directory
	T.is_err(failed, "RuntimeError", "a failed export is an error")
	T.eq(s._reported.size(), reported + 1, "and is reported")
	s.clear_local_data()
	T.eq([s.owns("chapter2"), s.receipts(), s.pending()], [false, {}, {}], "clear_local_data forgets them at once")
	T.eq(_on_disk(path)["receipts"], {}, "and on disk")
	await s._worker.run_once()
	s._pump()
	T.eq([s.owns("chapter2"), s.receipts()], [false, {}], "the engine is wiped too")
	T.eq(_on_disk(path)["receipts"], {}, "still empty on disk")
	await _drop(env)
	return T.done()


func test_state_survives_restart() -> bool:
	var path := _path("restart")
	var env := await _configured(path)
	var s: Node = env["node"]
	await _start(s)
	await _add_receipt(s, "0x" + "ab".repeat(32))
	var sid: Variant = (await _start_session(s))["id"]
	var receipts: Dictionary = s.receipts()
	var pending: Dictionary = s.pending()
	T.eq(receipts.size(), 1, "one receipt")
	T.ok(pending.has(sid), "one session")
	await _drop(env)
	var again := await _configured(path)
	var b: Node = again["node"]
	# (by JSON value: the reloaded state is normalised, so the integral float created_at
	# comes back as an int, as it does for the Engine)
	T.json_eq(b.receipts(), receipts, "the same receipts after a restart")
	T.json_eq(b.pending(), pending, "the same pending sessions")
	T.eq(b.owns("chapter2"), true, "owned before the worker starts")
	b._pump()
	T.eq(b.owns("chapter2"), true, "and from the worker's snapshot")
	T.json_eq((b._worker as ScfgWorker).snapshot["receipts"], receipts, "the Engine got them")
	await _drop(again)
	return T.done()


# --- persistence -----------------------------------------------------------------------------


func test_new_session_is_saved_at_once() -> bool:
	var path := _path("new_session")
	var env := await _configured(path)
	var s: Node = env["node"]
	var clock: F.FakeClock = env["clock"]
	await _start(s)
	var first: Variant = (await _start_session(s))["id"]
	T.ok(_on_disk(path)["pending"].has(first), "the first session is on disk")
	clock.advance(1.0)  # well inside the save interval
	var second: Variant = (await _start_session(s))["id"]
	T.eq((_on_disk(path)["pending"] as Dictionary).keys(), [first, second], "a new session is written at once")
	clock.advance(1.0)
	await _run(s, func(e: ScfgEngine) -> Variant: return e.report_hint(second, "0x" + "ef".repeat(32)))
	T.eq(_on_disk(path)["pending"][second]["hint_tx"], "0x" + "ef".repeat(32), "a tx hint is written at once")
	clock.advance(1.0)
	await _run(s, func(e: ScfgEngine) -> Variant: return e.discard_session(first))
	T.eq((_on_disk(path)["pending"] as Dictionary).keys(), [second], "a removed session is written at once")
	await _drop(env)
	return T.done()


func test_scan_progress_is_saved_at_most_every_10_s() -> bool:
	var path := _path("scan")
	var env := await _configured(path)
	var s: Node = env["node"]
	var clock: F.FakeClock = env["clock"]
	await _start(s)
	var sid: Variant = (await _start_session(s))["id"]
	var chain: F.FakeChain = env["chains"]["arbitrum"]
	var saved_cursor: Variant = _on_disk(path)["pending"][sid]["cursor"]
	clock.advance(4.0)
	chain.head += 50
	await s._worker.run_once()
	s._pump()
	var cursor: Variant = s.pending()[sid]["cursor"]
	T.ok(cursor != saved_cursor, "the scan moved the cursor (%s -> %s)" % [saved_cursor, cursor])
	T.eq(_on_disk(path)["pending"][sid]["cursor"], saved_cursor, "scan progress waits for the interval")
	clock.advance(5.9)
	chain.head += 50
	await s._worker.run_once()
	s._pump()
	T.eq(_on_disk(path)["pending"][sid]["cursor"], saved_cursor, "still waiting at 9.9 s")
	clock.advance(0.1)
	chain.head += 50
	await s._worker.run_once()
	s._pump()
	T.eq(_on_disk(path)["pending"][sid]["cursor"], s.pending()[sid]["cursor"], "written once 10 s passed")
	# progress not yet written when the autoload leaves the tree (the game quits) is flushed
	clock.advance(1.0)
	chain.head += 50
	await s._worker.run_once()
	s._pump()
	var last: Variant = s.pending()[sid]["cursor"]
	T.ok(_on_disk(path)["pending"][sid]["cursor"] != last, "unsaved progress")
	await _drop(env)
	T.eq(_on_disk(path)["pending"][sid]["cursor"], last, "flushed on exit")
	return T.done()


func test_session_state_change_is_saved_at_once() -> bool:
	var path := _path("state_change")
	var env := await _configured(path)
	var s: Node = env["node"]
	var clock: F.FakeClock = env["clock"]
	await _start(s)
	var sid: Variant = (await _start_session(s))["id"]
	clock.advance(1.0)
	await _run(s, func(e: ScfgEngine) -> Variant: return e.set_ui_status(sid, "pending"))
	T.eq(_on_disk(path)["pending"][sid]["ui_status"], "open", "a ui_status change alone waits for the interval")
	clock.advance(1.0)
	await _run(s, func(e: ScfgEngine) -> Variant:
		e.pending[sid]["state"] = "confirming"
		return null)
	T.eq(_on_disk(path)["pending"][sid]["state"], "confirming", "a state change is written at once")
	T.eq(_on_disk(path)["pending"][sid]["ui_status"], "pending", "with everything else mirrored so far")
	await _drop(env)
	return T.done()


func test_a_pass_in_flight_when_the_autoload_is_freed_does_not_call_it() -> bool:
	# A quit with a pass in flight: the pass resumes after the node is gone, and the Engine
	# reports the contained step error to its on_error, which must no longer point at the node.
	var path := _path("freed")
	var env := await _configured(path)
	var s: Node = env["node"]
	await _start(s)
	await _start_session(s)
	var chain: BoomChain = env["chains"]["arbitrum"]
	chain.boom = true
	var w: ScfgWorker = s._worker
	var box := {}
	var run := func() -> void:
		box["got"] = await w.run_once()
		box["done"] = true
	var dropped_before: int = SCFG.errors_after_exit
	run.call()
	await (Engine.get_main_loop() as SceneTree).process_frame
	T.eq(box.get("done", false), false, "the pass is in flight")
	s.free()
	for i in 30:
		if box.get("done", false):
			break
		await (Engine.get_main_loop() as SceneTree).process_frame
	T.eq(box.get("done"), true, "the pass finished after the node was freed")
	T.ok(SCFG.errors_after_exit > dropped_before, "its contained error went to the detached handler, not the freed node")
	T.eq(w.running, false, "the Worker was stopped")
	return T.done()


func test_quitting_mid_request_ends_the_pass_before_the_node_goes() -> bool:
	# A real quit tears the tree down with no further frame: a pass waiting on an HTTP request
	# would never resume and its coroutines leaked ("ObjectDB instances leaked at exit", found by
	# the Godot e2e). Leaving the tree closes the autoload's own transport, so the request ends
	# with HostGone and the pass finishes inside _exit_tree.
	var path := _path("quit_mid_request")
	var tree := Engine.get_main_loop() as SceneTree
	if not tree.root.is_inside_tree():
		await tree.process_frame
	var responder := HTTP.Responder.new()
	tree.root.add_child(responder)
	T.ok(responder.start(), "the responder listens")
	# A stored receipt: the first pass re-verifies it on chain (as on every launch after a purchase).
	var tx := "0x" + "ab".repeat(32)
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string(JSON.stringify({"version": 1, "pending": {}, "receipts": {"arbitrum:%s:3" % tx: _receipt(tx)}}))
	f.close()
	var s: Node = SCFG.new()
	s._setup({"store_path": path, "scheduler": ManualTimer.new(), "quiet": true})  # the real transport
	tree.root.add_child(s)
	T.eq(s.configure({"wallet": DEV, "chains": ["arbitrum"], "rpc_urls": {"arbitrum": [responder.url("/slow")]}}),
		null, "configure")
	s._pump()
	var w: ScfgWorker = s._worker
	var box := {}
	var run := func() -> void:
		box["got"] = await w.run_once()
		box["done"] = true
	run.call()
	for i in 300:
		if responder.requests.size() > 0:
			break
		await tree.process_frame
	T.eq(responder.requests.size(), 1, "a request is in flight")
	T.eq(box.get("done", false), false, "the pass waits on it")
	s.free()
	T.eq(box.get("done", false), true, "the pass ended inside _exit_tree")
	await tree.process_frame
	T.eq(responder.requests.size(), 1, "nothing more was sent (no failover to the public endpoints)")
	for c in responder.conns:
		(c["peer"] as StreamPeerTCP).disconnect_from_host()
	responder.server.stop()
	responder.queue_free()
	await tree.process_frame
	return T.done()


func test_corrupt_state_file_starts_empty_and_warns() -> bool:
	var path := _path("corrupt")
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_string("{\"version\": 1, \"pending\": {\"ps_1\": ")
	f.close()
	var env := await _configured(path)
	var s: Node = env["node"]
	T.eq([s.receipts(), s.pending(), s.owns("chapter2")], [{}, {}, false], "empty stores")
	T.eq(s._warnings.size(), 1, "one warning")
	T.ok(s._warnings.size() == 1 and (s._warnings[0] as String).contains(path), "it names the file (%s)" % [s._warnings])
	T.ok(FileAccess.file_exists(path + ".corrupt"), "the bad file is kept aside")
	await _start(s)
	await _add_receipt(s, "0x" + "ab".repeat(32))
	T.eq((_on_disk(path)["receipts"] as Dictionary).size(), 1, "a fresh state file")
	await _drop(env)
	return T.done()


## A save that failed (a full disk, a locked file) is not left until the next change or the quit:
## it is tried again once PENDING_SAVE_INTERVAL passed.
func test_a_failed_save_is_retried() -> bool:
	var path := _path("retry")
	var env := await _configured(path)
	var s: Node = env["node"]
	var clock: F.FakeClock = env["clock"]
	await _start(s)
	DirAccess.make_dir_recursive_absolute(path + ".tmp")  # a directory where the save writes: it fails
	var sid: Variant = (await _start_session(s))["id"]
	T.ok(not (_on_disk(path)["pending"] as Dictionary).has(sid), "the save failed")
	T.ok(s._reported.any(func(l: String) -> bool: return l.begins_with("scfg: cannot save the payment state:")),
		"and was reported (%s)" % [s._reported])
	DirAccess.remove_absolute(path + ".tmp")
	clock.advance(9.0)
	s._pump()
	T.ok(not (_on_disk(path)["pending"] as Dictionary).has(sid), "not retried inside the interval")
	clock.advance(1.0)
	s._pump()
	T.ok((_on_disk(path)["pending"] as Dictionary).has(sid), "retried once the interval passed, with no new snapshot")
	T.eq(s._dirty, false, "and clean")
	await _drop(env)
	return T.done()


## A state file the game cannot read right now (permissions here; a sharing violation, an
## antivirus lock or an I/O error for a player) is not corrupt: it stays where it is, payments do
## not start this run (reported as the stored receipts being unreadable), and nothing is saved
## over it, so the next launch reads it again.
func test_unreadable_state_file_is_never_saved_over() -> bool:
	var path := _path("unreadable")
	T.eq(ScfgStore.save(path, {}, {"arbitrum:0xab:3": _receipt("0x" + "ab".repeat(32))}), null, "a good file")
	var before := FileAccess.get_file_as_bytes(path)
	FileAccess.set_unix_permissions(path, 0)
	if FileAccess.open(path, FileAccess.READ) != null:
		FileAccess.set_unix_permissions(path, FileAccess.UNIX_READ_OWNER | FileAccess.UNIX_WRITE_OWNER)
		T.skip("this process reads files without permission (root)")
		return T.done()
	var env := await _configured(path)
	var s: Node = env["node"]
	s._pump()
	T.eq(s._worker, null, "payments do not start")
	T.ok(s._reported.any(func(l: String) -> bool: return l.begins_with("scfg: cannot read the stored receipts:")),
		"reported (%s)" % [s._reported])
	var box := {}
	var run := func() -> void:
		box["result"] = await s.pay("unlock", {"item": "chapter2"})
	run.call()
	await (Engine.get_main_loop() as SceneTree).process_frame
	var r: ScfgPaymentResult = box.get("result")
	T.eq([r.status, r.error] if r != null else null, ["failed", "RuntimeError"], "pay() resolves failed at once")
	s.clear_local_data()
	s._dirty = true
	await _drop(env)
	FileAccess.set_unix_permissions(path, FileAccess.UNIX_READ_OWNER | FileAccess.UNIX_WRITE_OWNER)
	T.eq(FileAccess.get_file_as_bytes(path), before, "the file is untouched (not saved over, even at exit)")
	T.ok(not FileAccess.file_exists(path + ".corrupt"), "and not moved aside")
	T.eq((_on_disk(path)["receipts"] as Dictionary).size(), 1, "the next launch reads the receipt")
	return T.done()


# --- results ---------------------------------------------------------------------------------


func test_payment_result_from_core() -> bool:
	var core: Variant = ScfgModels.make_payment_result({"status": "paid", "kind": "unlock", "item": "chapter2",
		"amount": ScfgDec.parse("4.990037"), "token": "USDC", "chain": "arbitrum", "tx_hash": "0xab", "payer": "0xcd",
		"session_id": "ps_1"})
	var r := ScfgPaymentResult.from_core(core)
	T.eq([r.status, r.kind, r.item, r.amount, r.token, r.chain, r.tx_hash, r.payer, r.session_id, r.error],
		["paid", "unlock", "chapter2", "4.990037", "USDC", "arbitrum", "0xab", "0xcd", "ps_1", null], "every field")
	var bare := ScfgPaymentResult.from_core(ScfgModels.make_payment_result({"status": "cancelled", "kind": "tip"}))
	T.eq([bare.status, bare.kind, bare.amount, bare.session_id], ["cancelled", "tip", null, null], "defaults")
	var rr := ScfgRestoreResult.from_core(ScfgModels.make_restore_result({"status": "restored", "restored": [{"item": "chapter2"}]}))
	T.eq([rr.status, rr.restored, rr.error], ["restored", [{"item": "chapter2"}], null], "a restore result")
	return T.done()


func test_a_result_without_callback_reaches_payment_resolved() -> bool:
	var env := await _configured(_path("deliver"))
	var s: Node = env["node"]
	var got: Array = []
	s.payment_resolved.connect(func(r: ScfgPaymentResult) -> void: got.append(r))
	s._deliver(ScfgModels.make_payment_result({"status": "expired", "kind": "unlock", "item": "chapter2", "session_id": "ps_1"}))
	T.eq(got.size(), 1, "emitted")
	T.ok(got.size() == 1 and got[0] is ScfgPaymentResult and got[0].status == "expired", "a typed result")
	var called: Array = []
	s._callbacks["ps_2"] = func(r: ScfgPaymentResult) -> void: called.append(r.status)
	s._deliver(ScfgModels.make_payment_result({"status": "paid", "kind": "unlock", "item": "chapter2", "session_id": "ps_2"}))
	T.eq([called, got.size()], [["paid"], 1], "a session's callback gets it instead")
	T.ok(not s._callbacks.has("ps_2"), "a final result drops the callback")
	await _drop(env)
	return T.done()


# --- the editor plugin -----------------------------------------------------------------------


func test_plugin_registers_the_scfg_autoload() -> bool:
	var cfg := ConfigFile.new()
	T.eq(cfg.load("res://addons/scfg/plugin.cfg"), OK, "plugin.cfg")
	T.eq(cfg.get_value("plugin", "name", ""), "Stablecoin for Games (SCFG)", "display name")
	T.eq(cfg.get_value("plugin", "script", ""), "plugin.gd", "script")
	var plugin := load("res://addons/scfg/plugin.gd") as GDScript
	T.ok(plugin != null and plugin.can_instantiate(), "plugin.gd compiles")
	if plugin != null:
		T.eq(plugin.get_instance_base_type(), &"EditorPlugin", "an EditorPlugin")
		var methods: Array = plugin.get_script_method_list().map(func(m: Dictionary) -> String: return m["name"])
		T.ok("_enable_plugin" in methods and "_disable_plugin" in methods, "enable and disable hooks (%s)" % [methods])
		T.ok(plugin.source_code.contains("add_autoload_singleton(AUTOLOAD_NAME")
			and plugin.source_code.contains("remove_autoload_singleton(AUTOLOAD_NAME"), "they add and remove the autoload")
		T.ok(plugin.source_code.contains("const AUTOLOAD_NAME := \"Scfg\""), "named Scfg")
	T.eq(ProjectSettings.get_setting("autoload/Scfg", ""), "*res://addons/scfg/scfg.gd", "the dev project registers it")
	return T.done()
