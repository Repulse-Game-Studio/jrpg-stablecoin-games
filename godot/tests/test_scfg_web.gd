## The Scfg autoload on a web export (addons/scfg/scfg.gd, the WEB EXPORT SEAMs): the checkout is
## the in-page overlay, the browser wallet is the page's has_wallet, and copy, paste and links go
## through the page. No browser runs here: the test forces the web platform and gives the
## autoload a fake `js` (the `_setup` keys platform and js), as Ren'Py's WEB branches of
## scfg_api.rpy are driven by a fake emscripten module. The real JavaScriptBridge is exercised by
## the web e2e.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")
const FLOWS := preload("res://tests/test_scfg_flows.gd")

const TX := "0xabababababababababababababababababababababababababababababababab"


## FLOWS._make with the web platform and a fake js (`js`, or a new one). `wallet`: what
## hasWallet answers.
func _web(name: String, wallet: int = 1, persistent: bool = true, js: F.FakeJs = null) -> Dictionary:
	if js == null:
		js = F.FakeJs.new()
	js.answers["scfgBridge.hasWallet("] = []
	for i in 20:
		js.answers["scfgBridge.hasWallet("].append(wallet)
	var platform := FLOWS.FakePlatform.new()
	platform.web = true
	platform.persistent = persistent
	var env: Dictionary = await FLOWS.new()._make(name, {"platform": platform, "js": js})
	env["js"] = js
	return env


func _drop(env: Dictionary) -> void:
	await FLOWS.new()._drop(env)


func test_web_checkout_is_the_overlay_not_a_server() -> bool:
	var env := await _web("overlay")
	var s: Node = env["node"]
	var js: F.FakeJs = env["js"]
	var platform: FLOWS.FakePlatform = env["platform"]
	var flows := FLOWS.new()
	T.eq(js.scripts.size(), 0, "no page script before the first checkout")
	flows._pay(s, "unlock", {"item": "chapter2"})
	var ctl := await flows._session(s, env["host"])
	T.eq(js.scripts.slice(0, 4).map(func(x: String) -> bool: return x.length() > 20), [true, true, true, true], "the four scripts were loaded")
	T.ok(js.scripts[4].begins_with("scfgBridge.setPage("), "then the page")
	T.eq(js.scripts_for("scfgBridge.hasWallet(").size() > 0, true, "the controller asked the page for a wallet")
	T.ok(ctl.methods().has("browser"), "a wallet in the page offers the browser checkout (%s)" % [ctl.methods()])
	s.open_browser(ctl)
	var opens := js.scripts_for("scfgBridge.openCheckout(")
	T.eq(opens.size(), 1, "the overlay opened")
	T.ok(opens[0].begins_with("scfgBridge.openCheckout(\"o"), "under a key o<n> (%s)" % opens[0].left(40))
	T.ok(opens[0].contains("\"Chapter 2\""), "with the session's title")
	T.ok(opens[0].ends_with(", null)"), "and no logo")
	T.eq(platform.opened, [], "no browser window on the web: the overlay is already showing")
	# the page posts a tx hash: it reaches the session as a hint, through the pump
	js.answers["scfgBridge.take("] = ["[{\"type\":\"tx\",\"body\":{\"tx_hash\":\"%s\"}}]" % TX]
	s._pump()
	await s._worker.run_once()
	s._pump()
	var hinted := false
	for sid: Variant in s.pending():
		hinted = hinted or s.pending()[sid].get("hint_tx") == TX
	T.ok(hinted, "the page's tx hash became the session's hint (%s)" % [s.pending()])
	T.ok(js.scripts_for("scfgBridge.setStatus(").size() >= 1, "the status was pushed to the page")
	# closing the sheet closes the overlay
	(env["host"].sheets[-1] as Node).emit_signal("closed", "cancel")
	await (Engine.get_main_loop() as SceneTree).process_frame
	T.eq(js.scripts_for("scfgBridge.closeCheckout(").size(), 1, "the overlay closed with the sheet")
	await _drop(env)
	return T.done()


func test_no_wallet_in_the_page_means_no_browser_checkout() -> bool:
	var env := await _web("no_wallet", 0)
	var s: Node = env["node"]
	var flows := FLOWS.new()
	flows._pay(s, "unlock", {"item": "chapter2"})
	var ctl := await flows._session(s, env["host"])
	T.ok(not ctl.methods().has("browser"), "no browser checkout without a wallet (%s)" % [ctl.methods()])
	await _drop(env)
	return T.done()


func test_desktop_never_touches_the_page() -> bool:
	var js := F.FakeJs.new()
	var env: Dictionary = await FLOWS.new()._make("desktop", {"js": js})
	var s: Node = env["node"]
	var flows := FLOWS.new()
	flows._pay(s, "unlock", {"item": "chapter2"})
	var ctl := await flows._session(s, env["host"])
	s.copy(TX)
	s.open_explorer("https://arbiscan.io/tx/" + TX)
	s.paste_into(ctl)
	T.eq(js.scripts, [] as Array[String], "no script ran")
	T.eq(js.storage_scripts, [] as Array[String], "and localStorage was never used")
	T.eq(s._page, null, "and no page was built")
	T.eq(env["platform"].clipboard, TX, "the engine's clipboard")
	await _drop(env)
	return T.done()


func test_links_and_copy_go_through_the_page() -> bool:
	var env := await _web("links")
	var s: Node = env["node"]
	var js: F.FakeJs = env["js"]
	var platform: FLOWS.FakePlatform = env["platform"]
	s.open_explorer("https://arbiscan.io/tx/1")
	T.eq(js.scripts_for("scfgBridge.openUrl(").size(), 1, "open_explorer opens from the page")
	T.eq(js.scripts_for("scfgBridge.openUrl(")[0], "scfgBridge.openUrl(\"https://arbiscan.io/tx/1\")", "with the url as a literal")
	T.eq(platform.opened, [], "not the engine's shell_open")
	s.copy(TX)
	T.eq(js.scripts_for("scfgBridge.copy(")[0], "scfgBridge.copy(\"%s\")" % TX, "copy goes to the page")
	T.eq(platform.clipboard, "", "not the engine's clipboard")
	# the browser refuses the copy: the player is told, once
	env["host"].notes.clear()
	js.answers["scfgBridge.takeCopyFailed("] = [1, 0]
	s._pump()
	T.eq(env["host"].notes, ["Couldn't copy: your browser blocked the clipboard."], "told once")
	s._pump()
	T.eq(env["host"].notes.size(), 1, "and not again")
	# a failure after the 5 second watch is not reported
	env["clock"].advance(6.0)
	js.answers["scfgBridge.takeCopyFailed("] = [1]
	s._pump()
	T.eq(env["host"].notes.size(), 1, "a late answer is ignored")
	await _drop(env)
	return T.done()


func test_paste_comes_from_the_page_asynchronously() -> bool:
	var env := await _web("paste")
	var s: Node = env["node"]
	var js: F.FakeJs = env["js"]
	var flows := FLOWS.new()
	flows._pay(s, "unlock", {"item": "chapter2"})
	var ctl := await flows._session(s, env["host"])
	ctl.error = "invalid_hash"
	s.paste_into(ctl)
	T.eq(js.scripts_for("scfgBridge.requestPaste(").size(), 1, "the page was asked")
	T.eq([ctl.tx_input, ctl.error], ["", "invalid_hash"], "nothing yet")
	js.answers["scfgBridge.takePaste("] = ["null", "\"  0x12ab-CD!ef and more\\n\""]
	s._pump()
	T.eq([ctl.tx_input, ctl.error], ["", "invalid_hash"], "still waiting for the browser")
	s._pump()
	T.eq([ctl.tx_input, ctl.error], ["0x12abCDef", null], "the first word, hash characters only (Ren'Py's _clean_paste)")
	# a refusal: an empty answer
	s.paste_into(ctl)
	js.answers["scfgBridge.takePaste("] = ["\"\""]
	s._pump()
	T.eq(ctl.error, "paste_blocked", "the browser refused: paste_blocked")
	T.eq(ctl.tx_input, "0x12abCDef", "the field is kept")
	# the pump asks only while a paste is outstanding
	var asked := js.scripts_for("scfgBridge.takePaste(").size()
	s._pump()
	T.eq(js.scripts_for("scfgBridge.takePaste(").size(), asked, "no polling without a request")
	# a restore screen that moved past its input drops the paste
	flows._restore(s)
	var rc: ScfgRestoreController = (env["host"].sheets[-1] as FLOWS.FakeSheet).controller
	s.paste_into(rc)
	rc.state = "checking"
	js.answers["scfgBridge.takePaste("] = ["\"" + TX + "\""]
	s._pump()
	T.eq(rc.tx_input, "", "dropped once the player pressed Check")
	await _drop(env)
	return T.done()


func test_warns_when_user_storage_is_not_persistent() -> bool:
	var env := await _web("not_persistent", 1, false)
	var s: Node = env["node"]
	T.eq(s._warnings.size(), 1, "one warning (%s)" % [s._warnings])
	if s._warnings.size() == 1:
		T.ok(s._warnings[0].contains("not persistent") and s._warnings[0].contains("receipts"), "it says receipts are lost (%s)" % s._warnings[0])
	await _drop(env)
	var ok_env := await _web("persistent")
	T.eq(ok_env["node"]._warnings, [] as Array[String], "no warning when persistent")
	await _drop(ok_env)
	var desktop: Dictionary = await FLOWS.new()._make("desktop_storage", {})
	desktop["platform"].persistent = false
	T.eq(desktop["node"]._warnings, [] as Array[String], "desktop never asks")
	await _drop(desktop)
	return T.done()


## The page warns about a file missing from the export through the autoload's developer-warning
## path (debug builds only, recorded here in quiet mode).
func test_a_missing_page_file_warns_through_the_autoload() -> bool:
	var env := await _web("missing_file")
	var s: Node = env["node"]
	var page: ScfgWebPage = s._web_page()
	T.eq(page.warn, Callable(s, "_warn"), "the page warns through Scfg._warn")
	page.warn.call("probe")
	T.eq(s._warnings.back(), "scfg: probe", "which records it in quiet mode")
	await _drop(env)
	return T.done()


func test_the_overlay_gets_the_logo_as_a_data_url() -> bool:
	var env := await _web("logo")
	var s: Node = env["node"]
	T.eq(s._logo_data_url(), null, "no logo: null")
	s._logo_res = "res://tests/fixtures/logo.png"
	var url: Variant = s._logo_data_url()
	T.ok(url is String and (url as String).begins_with("data:image/png;base64,iVBOR"), "a PNG data url (%s)" % [str(url).left(40)])
	await _drop(env)
	return T.done()


## The page a web export builds for a checkout (Scfg._server_factory) is the overlay, with what
## the controller hands a Solana checkout: its build_tx (the page asks it for the unsigned
## transfer) and the autoload's clock (a build the Worker has not finished within
## BUILD_TIMEOUT_S on that clock answers 503). An EVM checkout gives no build_tx: a build request
## is then a 404.
func test_the_web_overlay_gets_build_tx_and_the_clock() -> bool:
	var env := await _web("build")
	var s: Node = env["node"]
	var js: F.FakeJs = env["js"]
	var clock: F.FakeClock = env["clock"]
	var account := "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe"
	var asked: Array = []
	var jobs: Array = []
	var build := func(a: String) -> Variant:
		asked.append(a)
		var job := ScfgJob.new()  # the Worker never gets to it
		jobs.append(job)
		return job
	var kw := {"session_info": {"family": "solana"}, "on_tx": func(_t: String, _s: Variant) -> void: pass,
		"get_status": func() -> Dictionary: return {}}
	var page: Variant = s._server_factory(kw.merged({"build_tx": build}))
	T.ok(page is ScfgOverlayCheckout, "the overlay (%s)" % [page])
	if not page is ScfgOverlayCheckout:
		await _drop(env)
		return T.done()
	var overlay: ScfgOverlayCheckout = page
	T.ok(overlay.build_tx == build, "with the controller's build_tx")
	T.eq(overlay.clock.call(), clock.now, "and the autoload's clock")
	T.eq(overlay.start(), null, "started")
	var build_message := "[{\"type\":\"build\",\"id\":%d,\"body\":{\"account\":\"%s\"}}]"
	js.answers["scfgBridge.take("] = [build_message % [7, account]]
	overlay.pump()
	T.eq(asked, [account], "a build request from the page reaches build_tx")
	clock.advance(ScfgPageMessages.BUILD_TIMEOUT_S - 0.5)
	overlay.pump()
	T.eq(js.scripts_for("scfgBridge.reply(").size(), 0, "no answer before the build timeout on the autoload's clock")
	clock.advance(0.5)
	overlay.pump()
	var replies := js.scripts_for("scfgBridge.reply(")
	T.ok(replies.size() == 1 and replies[0].contains(", 7, 503, "), "503 once it passed (%s)" % [replies])
	T.ok(jobs.size() == 1 and (jobs[0] as ScfgJob).cancelled(), "and the build is cancelled")
	overlay.stop()
	# EVM: no build_tx, so the page has no build route
	var evm: Variant = s._server_factory(kw)
	T.ok(evm is ScfgOverlayCheckout and (evm as ScfgOverlayCheckout).build_tx == null, "an EVM overlay has no build_tx")
	if evm is ScfgOverlayCheckout:
		(evm as ScfgOverlayCheckout).start()
		js.answers["scfgBridge.take("] = [build_message % [8, account]]
		(evm as ScfgOverlayCheckout).pump()
		T.ok(js.scripts_for("scfgBridge.reply(").any(func(r: String) -> bool: return r.contains(", 8, 404, ")),
			"a build request is a 404 there")
		(evm as ScfgOverlayCheckout).stop()
	await _drop(env)
	return T.done()


# --- the receipts' copy in localStorage (ScfgWebMirror) --------------------------------------


const SCFG := preload("res://addons/scfg/scfg.gd")
const TX_B := "0xcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"


static func _receipt(tx: String) -> Dictionary:
	return {"chain": "arbitrum", "tx_hash": tx, "log_index": 3, "block": 990, "kind": "unlock", "item": "chapter2",
		"amount": "4.99", "token": "USDC", "payer": "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed",
		"recipient": FLOWS.DEV.to_lower(), "source": "payment"}


static func _rkey(tx: String) -> String:
	return "arbitrum:%s:3" % tx


## The mirror's document in the fake localStorage (null when nothing is stored).
static func _mirror(js: F.FakeJs) -> Variant:
	var text: Variant = (js.local_storage as Dictionary).get(ScfgWebMirror.project_key())
	return ScfgPy.from_json(JSON.parse_string(text)) if text is String else null


## The storage scripts that write.
static func _writes(js: F.FakeJs) -> Array:
	return js.storage_scripts.filter(func(c: String) -> bool: return c.begins_with(ScfgWebMirror.WRITE_JS))


## A configured web autoload on `path` whose Worker has not started: nothing has read the state.
func _web_unstarted(path: String, js: F.FakeJs) -> Dictionary:
	var tree := Engine.get_main_loop() as SceneTree
	if not tree.root.is_inside_tree():
		await tree.process_frame
	var clock := F.FakeClock.new(1000.0)
	var platform := FLOWS.FakePlatform.new()
	platform.web = true
	var factory := func(_chain: Dictionary) -> Variant: return F.FakeChain.new(1000)
	var node: Node = SCFG.new()
	node._setup({"store_path": path, "clock": clock.fn, "scheduler": FLOWS.ManualTimer.new(),
		"transport": F.FakeTransport.new({}), "engine_opts": {"rpc_factory": factory, "clock": clock.fn},
		"quiet": true, "platform": platform, "js": js})
	tree.root.add_child(node)
	T.eq(node.configure({"wallet": FLOWS.DEV}), null, "configure")
	T.eq(node.add_item("chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"}), null, "add_item")
	return {"node": node, "host": FLOWS.FakeHost.new(), "js": js}


## Runs `fn(engine)` in one worker pass, then one pump frame.
func _run(s: Node, fn: Callable) -> Variant:
	var w: ScfgWorker = s._worker
	var job := w.submit(fn)
	await w.run_once()
	s._pump()
	return job.result()


func _add_receipt(s: Node, tx: String) -> Variant:
	return await _run(s, func(e: ScfgEngine) -> Variant:
		return ScfgReceipts.add_receipt(e.receipts, _receipt(tx)))


func test_every_web_save_mirrors_the_receipts_to_local_storage() -> bool:
	var env := await _web("mirror_save")
	var s: Node = env["node"]
	var js: F.FakeJs = env["js"]
	var key := ScfgWebMirror.project_key()
	T.eq(_mirror(js), null, "no receipts yet: nothing to copy")
	await _add_receipt(s, FLOWS.TX)
	T.json_eq(_mirror(js), {"version": 1, "receipts": s.receipts()}, "a new receipt is copied with the save")
	T.eq((s.receipts() as Dictionary).size(), 1, "(one receipt)")
	var sid: Variant = (await _run(s, func(e: ScfgEngine) -> Variant:
		return await e.start_session("unlock", "arbitrum", {"item": "chapter2"})))["id"]
	T.ok((ScfgStore.load(s._store_path)["pending"] as Dictionary).has(sid), "a new session is saved to state.json")
	T.eq((_mirror(js) as Dictionary).keys(), ["version", "receipts"], "the copy holds the receipts only")
	T.eq(_writes(js).size(), 1, "a save that changed no receipt does not write the copy again")
	var ops: Array = js.storage_scripts.map(func(c: String) -> String:
		return "write" if c.begins_with(ScfgWebMirror.WRITE_JS) else "read")
	T.ok(ops.size() >= 3 and ops[0] == "read" and ops.count("write") == 1
		and range(ops.size()).all(func(i: int) -> bool: return ops[i] == "read" or (i > 0 and ops[i - 1] == "read")),
		"each save reads the copy before it writes (%s)" % [ops])
	await _add_receipt(s, TX_B)
	T.json_eq(_mirror(js), {"version": 1, "receipts": s.receipts()}, "the next receipt too")
	T.eq((_mirror(js)["receipts"] as Dictionary).size(), 2, "(two receipts)")
	s.clear_local_data()
	T.eq(_mirror(js), {"version": 1, "receipts": {}}, "clear_local_data() empties the copy too")
	for code: String in js.storage_scripts:
		T.ok(code == ScfgWebMirror.READ_JS + "(" + JSON.stringify(key) + ")"
			or code.begins_with(ScfgWebMirror.WRITE_JS + "(" + JSON.stringify(key) + ", \""),
			"every call is the constant code on JSON literals (%s)" % code.left(60))
	T.eq(s._warnings, [] as Array[String], "no warning")
	await _drop(env)
	return T.done()


## Another game on the same origin deleted state.json from IndexedDB (or changed it): the receipts
## come back from the copy, merged into what state.json still holds, and are saved back at once.
func test_receipts_lost_from_user_storage_come_back_from_the_mirror() -> bool:
	var path: String = FLOWS.new()._path("mirror_merge")
	var a := _receipt(FLOWS.TX)
	var b := _receipt(TX_B)
	var b_copy := b.duplicate()
	b_copy["block"] = 991
	var pending := {"ps_1": {"id": "ps_1", "state": "awaiting", "cursor": 900}}
	T.eq(ScfgStore.save(path, pending, {_rkey(TX_B): b}), null, "state.json holds one receipt")
	var js := F.FakeJs.new()
	js.local_storage[ScfgWebMirror.project_key()] = JSON.stringify({"version": 1,
		"receipts": {_rkey(FLOWS.TX): a, _rkey(TX_B): b_copy}})
	var env := await _web_unstarted(path, js)
	var s: Node = env["node"]
	var want := {_rkey(TX_B): b, _rkey(FLOWS.TX): a}
	T.eq(s.receipts(), want, "the copy's other receipt is added; state.json's wins a conflict")
	T.eq((s.receipts() as Dictionary).keys(), want.keys(), "state.json's first, then the copy's")
	T.eq(typeof(s.receipts()[_rkey(FLOWS.TX)]["block"]), TYPE_INT, "normalised: ints stay ints")
	T.eq(s.owns("chapter2"), true, "owned before the Worker starts")
	var disk := ScfgStore.load(path)
	T.eq(disk["receipts"], want, "saved back to state.json at once")
	T.eq(disk["pending"], pending, "with its pending sessions as they were")
	T.eq(_mirror(js), {"version": 1, "receipts": want}, "and the copy now holds state.json's version")
	T.eq(s._warnings, [] as Array[String], "no warning")
	s._pump()
	T.eq((s._worker as ScfgWorker).snapshot["receipts"], want, "the Engine starts with them")
	await _drop(env)
	# state.json gone altogether (pending sessions are not copied: Restore covers those)
	DirAccess.remove_absolute(path)
	var js2 := F.FakeJs.new()
	js2.local_storage[ScfgWebMirror.project_key()] = JSON.stringify({"version": 1, "receipts": {_rkey(FLOWS.TX): a}})
	var again := await _web_unstarted(path, js2)
	T.eq(again["node"].receipts(), {_rkey(FLOWS.TX): a}, "a deleted state.json: the receipts come back")
	T.eq(again["node"].pending(), {}, "(pending sessions do not)")
	T.eq(ScfgStore.load(path)["receipts"], {_rkey(FLOWS.TX): a}, "and are saved to a new state.json")
	await _drop(again)
	return T.done()


## A receipt from the copy is checked like any stored one: the launch re-verification removes it
## when the chain does not have it, or when its own fields are invalid, and the copy follows.
func test_merged_receipts_are_reverified_like_stored_ones() -> bool:
	var js := F.FakeJs.new()
	var bad := _receipt(TX_B)
	bad["log_index"] = "three"
	js.local_storage[ScfgWebMirror.project_key()] = JSON.stringify({"version": 1,
		"receipts": {_rkey(FLOWS.TX): _receipt(FLOWS.TX), "bad": bad}})
	var env := await _web("mirror_reverify", 1, true, js)  # the first pass re-verifies
	var s: Node = env["node"]
	T.eq(s.receipts(), {}, "both removed: the chain does not have the one, the other is malformed")
	T.eq(_mirror(js), {"version": 1, "receipts": {}}, "and the copy follows")
	T.eq(ScfgStore.load(s._store_path)["receipts"], {}, "as does state.json")
	await _drop(env)
	return T.done()


func test_local_storage_trouble_never_breaks_saving() -> bool:
	for trouble: Array in [["none", null, "", ""], ["blocked", {}, "SecurityError", ""], ["full", {}, "", "QuotaExceededError"]]:
		var js := F.FakeJs.new()
		js.local_storage = trouble[1]
		js.storage_throws = trouble[2]
		js.write_throws = trouble[3]
		var env := await _web("mirror_trouble_" + trouble[0], 1, true, js)
		var s: Node = env["node"]
		await _add_receipt(s, FLOWS.TX)
		await _add_receipt(s, TX_B)
		T.eq((ScfgStore.load(s._store_path)["receipts"] as Dictionary).size(), 2, "%s: state.json is saved" % trouble[0])
		T.eq(s._dirty, false, "%s: and clean" % trouble[0])
		var about: Array = s._warnings.filter(func(w: String) -> bool: return w.contains("localStorage"))
		T.eq(about.size(), 1, "%s: one warning (%s)" % [trouble[0], s._warnings])
		T.eq(s._warnings.size(), 1, "%s: and no other" % trouble[0])
		T.eq(s._reported, [] as Array[String], "%s: never an error" % trouble[0])
		if trouble[0] != "full":
			T.eq(_writes(js).size(), 0, "%s: no write is tried once the read failed" % trouble[0])
		await _drop(env)
	return T.done()


func test_a_broken_copy_is_ignored_with_a_warning_and_replaced() -> bool:
	var path: String = FLOWS.new()._path("mirror_broken")
	T.eq(ScfgStore.save(path, {}, {_rkey(FLOWS.TX): _receipt(FLOWS.TX)}), null, "state.json holds one receipt")
	var js := F.FakeJs.new()
	js.local_storage[ScfgWebMirror.project_key()] = "{\"version\": 1, \"receipts\": "
	var env := await _web_unstarted(path, js)
	var s: Node = env["node"]
	T.eq(s.receipts(), {_rkey(FLOWS.TX): _receipt(FLOWS.TX)}, "state.json's receipts")
	T.eq(s._warnings.size(), 1, "one warning (%s)" % [s._warnings])
	T.ok(s._warnings.size() == 1 and s._warnings[0].contains("ignored"), "it says the copy is ignored")
	T.eq(_mirror(js), {"version": 1, "receipts": s.receipts()}, "the copy is written again from state.json")
	await _drop(env)
	return T.done()


## A state.json that cannot be read right now is never saved over (store.gd); its receipts are
## unknown, so the copy is neither merged nor replaced either.
func test_an_unreadable_state_file_leaves_the_copy_alone() -> bool:
	var path: String = FLOWS.new()._path("mirror_unreadable")
	T.eq(ScfgStore.save(path, {}, {_rkey(TX_B): _receipt(TX_B)}), null, "a good file")
	FileAccess.set_unix_permissions(path, 0)
	if FileAccess.open(path, FileAccess.READ) != null:
		FileAccess.set_unix_permissions(path, FileAccess.UNIX_READ_OWNER | FileAccess.UNIX_WRITE_OWNER)
		T.skip("this process reads files without permission (root)")
		return T.done()
	var js := F.FakeJs.new()
	var copy := JSON.stringify({"version": 1, "receipts": {_rkey(FLOWS.TX): _receipt(FLOWS.TX)}})
	js.local_storage[ScfgWebMirror.project_key()] = copy
	var env := await _web_unstarted(path, js)
	var s: Node = env["node"]
	s._pump()
	T.eq(s._worker, null, "payments do not start")
	s.clear_local_data()
	s._dirty = true
	await _drop(env)
	FileAccess.set_unix_permissions(path, FileAccess.UNIX_READ_OWNER | FileAccess.UNIX_WRITE_OWNER)
	T.eq(js.storage_scripts, [] as Array[String], "localStorage was not touched")
	T.eq(js.local_storage[ScfgWebMirror.project_key()], copy, "the copy is as it was")
	return T.done()


static func _tx(c: String) -> String:
	return "0x" + c.repeat(32)


## Runs `fn(engine)` in one worker pass of `s`, then one pump frame.
func _remove_receipt(s: Node, tx: String) -> void:
	await _run(s, func(e: ScfgEngine) -> Variant:
		e.receipts.erase(_rkey(tx))
		return null)


## One game open in two tabs: both loaded the same state. Each tab's save merges into the copy
## instead of replacing it, so a receipt one tab saved survives the other tab's later saves; a
## receipt a tab removed itself (re-verification, clear_local_data) leaves the copy.
func test_two_tabs_of_one_game_keep_each_others_receipts_in_the_copy() -> bool:
	var js_a := F.FakeJs.new()
	var js_b := F.FakeJs.new()
	js_b.local_storage = js_a.local_storage  # one origin: one localStorage
	var a_env := await _web("tabs_a", 1, true, js_a)
	var b_env := await _web("tabs_b", 1, true, js_b)
	var a: Node = a_env["node"]
	var b: Node = b_env["node"]
	var r1 := _tx("11")
	var r2 := _tx("22")
	var x := _tx("33")
	var r3 := _tx("44")
	await _add_receipt(a, r1)
	T.eq((_mirror(js_a)["receipts"] as Dictionary).keys(), [_rkey(r1)], "tab A saved R1")
	await _add_receipt(b, r2)
	T.eq((_mirror(js_a)["receipts"] as Dictionary).keys(), [_rkey(r1), _rkey(r2)],
		"tab B, which never saw R1, keeps it while adding R2")
	await _add_receipt(a, x)
	await _remove_receipt(a, x)
	T.eq((_mirror(js_a)["receipts"] as Dictionary).keys(), [_rkey(r1), _rkey(r2)],
		"a receipt tab A removed itself leaves the copy, and B's R2 stays")
	await _add_receipt(b, r3)
	T.eq((_mirror(js_a)["receipts"] as Dictionary).keys(), [_rkey(r1), _rkey(r2), _rkey(r3)],
		"B's next save still keeps A's R1")
	T.json_eq(_mirror(js_a)["receipts"][_rkey(r2)], b.receipts()[_rkey(r2)], "each receipt as its tab saved it")
	a.clear_local_data()
	T.eq(_mirror(js_a), {"version": 1, "receipts": {}}, "clear_local_data() empties the copy")
	T.eq(a._warnings + b._warnings, [] as Array[String], "no warning")
	await _drop(a_env)
	await _drop(b_env)
	return T.done()
