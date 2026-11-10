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


## FLOWS._make with the web platform and a fake js. `wallet`: what hasWallet answers.
func _web(name: String, wallet: int = 1, persistent: bool = true) -> Dictionary:
	var js := F.FakeJs.new()
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
