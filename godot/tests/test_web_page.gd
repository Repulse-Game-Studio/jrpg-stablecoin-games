## ScfgWebPage (addons/scfg/web_page.gd): the game's side of the web export, every call into the
## page's JavaScript (addons/scfg/web/bridge.js) behind a small method. Twin of
## tests/unit/test_web_page.py; `js` is a fake that records the scripts it is given.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const HOSTILE := "\"); alert(1); (\""


static func _assets(name: String) -> PackedByteArray:
	var files := {"bridge.js": "/*bridge*/", "evm.js": "/*evm*/", "solana.js": "/*sol*/",
		"app.js": "/*app*/", "index.html": "<main id=\"x\">\"</main>"}
	return String(files[name]).to_utf8_buffer()


func _page(js: F.FakeJs) -> ScfgWebPage:
	return ScfgWebPage.new(js, _assets)


# tests/unit/test_web_page.py::test_install_loads_bridge_first_then_the_page_once
func test_install_loads_bridge_first_then_the_page_once() -> bool:
	var js := F.FakeJs.new()
	var page := _page(js)
	page.install()
	page.install()
	T.eq(js.scripts.slice(0, 4), ["/*bridge*/", "/*evm*/", "/*sol*/", "/*app*/"] as Array[String], "bridge.js first")
	T.eq(js.scripts[4], "scfgBridge.setPage(%s)" % JSON.stringify("<main id=\"x\">\"</main>"), "then the page, as a literal")
	T.eq(js.scripts.size(), 5, "once")
	return T.done()


func test_install_reads_the_shared_page_files() -> bool:
	var js := F.FakeJs.new()
	var page := ScfgWebPage.new(js, func(name: String) -> PackedByteArray:
		return FileAccess.get_file_as_bytes("res://addons/scfg/web/" + name))
	page.install()
	T.eq(js.scripts.size(), 5, "five scripts")
	T.ok(js.scripts[0].contains("window.scfgBridge"), "bridge.js first")
	T.ok(js.scripts[3].length() > 100, "app.js")
	T.ok(js.scripts[4].begins_with("scfgBridge.setPage(\"<!DOCTYPE html>") or js.scripts[4].begins_with("scfgBridge.setPage(\"<!doctype html>"),
		"index.html (%s)" % js.scripts[4].left(60))
	return T.done()


## A page file left out of the export (the preset lacks the addons/scfg/web/* filter): install()
## names it and the filter through the developer-warning callable, and still loads the rest.
func test_install_warns_about_a_missing_page_file() -> bool:
	var js := F.FakeJs.new()
	var warnings: Array[String] = []
	var page := ScfgWebPage.new(js, func(name: String) -> Variant:
		return null if name == "evm.js" else _assets(name), warnings.append)
	page.install()
	T.eq(warnings.size(), 1, "one warning (%s)" % [warnings])
	T.ok(warnings[0].contains("addons/scfg/web/evm.js"), "it names the missing file (%s)" % warnings[0])
	T.ok(warnings[0].contains("addons/scfg/web/*"), "and the export filter that brings it")
	T.eq(js.scripts.size(), 4, "the other files are still loaded (no empty script for evm.js)")
	T.eq(js.scripts[0], "/*bridge*/", "bridge.js")
	T.ok(js.scripts[3].begins_with("scfgBridge.setPage("), "the page")
	page.install()
	T.eq(warnings.size(), 1, "installed once, warned once")
	return T.done()


## Every file present: no warning.
func test_install_with_every_file_does_not_warn() -> bool:
	var warnings: Array[String] = []
	var page := ScfgWebPage.new(F.FakeJs.new(), _assets, warnings.append)
	page.install()
	T.eq(warnings, [] as Array[String], "no warning")
	return T.done()


# tests/unit/test_web_page.py::test_values_cross_as_json_literals_never_as_code, for the overlay
# (the page's only game data): a hostile session title.
func test_values_reach_js_as_json_literals() -> bool:
	var js := F.FakeJs.new()
	var page := _page(js)
	var info := {"title": HOSTILE, "chain_name": "Arb\nitrum"}
	page.open("o1", info, "data:image/png;base64,AA")
	var literal := JSON.stringify(info)
	T.eq(js.scripts, ["scfgBridge.openCheckout(\"o1\", %s, \"data:image/png;base64,AA\")" % literal] as Array[String],
		"the exact code the page ran")
	T.ok(not js.scripts[0].contains(HOSTILE), "the hostile title never appears raw")
	T.ok(literal.contains("\\\"); alert(1); (\\\""), "it is inside the JSON string literal, quotes escaped (%s)" % literal)
	T.eq(JSON.parse_string(literal)["title"], HOSTILE, "and the literal reads back as the same text")
	# every other value that comes from the game
	page.set_status("o\")x", {"state": HOSTILE})
	page.copy(HOSTILE)
	page.open_url(HOSTILE)
	page.reply("k", 3, 200, {"error": HOSTILE})
	for s: String in js.scripts.slice(1):
		T.ok(not s.contains(HOSTILE), "no raw hostile text in: %s" % s)
	T.eq(js.scripts[2], "scfgBridge.copy(%s)" % JSON.stringify(HOSTILE), "copy")
	T.eq(js.scripts[3], "scfgBridge.openUrl(%s)" % JSON.stringify(HOSTILE), "open_url")
	return T.done()


# tests/unit/test_web_page.py::test_has_wallet_asks_for_the_chain_family
func test_has_wallet_asks_for_the_chain_family() -> bool:
	var js := F.FakeJs.new()
	js.answers["scfgBridge.hasWallet("] = [1, 0.0, null]  # the page answers numbers (floats too)
	var page := _page(js)
	var sol := {"family": "solana", "wallet_chain": "solana:mainnet"}
	var evm := {"family": "evm", "wallet_chain": ""}
	T.eq(page.has_wallet(sol), true, "a Solana wallet")
	T.eq(page.has_wallet(evm), false, "no EVM wallet")
	T.eq(page.has_wallet(evm), false, "an eval with no value is no wallet")
	T.eq(js.scripts[0], "scfgBridge.hasWallet(\"solana\", \"solana:mainnet\")", "family and wallet chain")
	return T.done()


# tests/unit/test_web_page.py::test_overlay_calls
func test_overlay_calls() -> bool:
	var js := F.FakeJs.new()
	js.answers["scfgBridge.take("] = ["[{\"type\": \"tx\"}]"]
	var page := _page(js)
	page.open("o1", {"title": "T"}, null)
	page.show("o1")
	page.set_status("o1", {"state": "waiting"})
	page.reply("o1", 2, 200, {"transaction": "AA"})
	T.eq(page.take("o1"), [{"type": "tx"}], "take")
	page.close("o1")
	T.eq(js.scripts, [
		"scfgBridge.openCheckout(\"o1\", {\"title\":\"T\"}, null)",
		"scfgBridge.show(\"o1\")",
		"scfgBridge.setStatus(\"o1\", {\"state\":\"waiting\"})",
		"scfgBridge.reply(\"o1\", 2, 200, {\"transaction\":\"AA\"})",
		"scfgBridge.take(\"o1\")",
		"scfgBridge.closeCheckout(\"o1\")",
	] as Array[String], "the scripts")
	return T.done()


func test_take_reads_json_messages_with_ints_restored() -> bool:
	var js := F.FakeJs.new()
	js.answers["scfgBridge.take("] = ["[{\"type\":\"build\",\"id\":7,\"body\":{\"account\":\"x\"}}]", "not json", "{\"a\":1}", null]
	var page := _page(js)
	var got: Array = page.take("o1")
	T.eq(got, [{"type": "build", "id": 7, "body": {"account": "x"}}], "parsed")
	T.eq(typeof(got[0]["id"]), TYPE_INT, "the id is an int again (ScfgPy.from_json), not a float")
	T.eq(page.take("o1"), [], "garbage is no messages")
	T.eq(page.take("o1"), [], "an object is no list of messages")
	T.eq(page.take("o1"), [], "no value is no messages")
	return T.done()


# tests/unit/test_web_page.py::test_clipboard_and_links
func test_clipboard_and_links() -> bool:
	var js := F.FakeJs.new()
	js.answers["scfgBridge.takePaste("] = ["null", "\"0xab\"", "\"\"", "garbage", null]
	var page := _page(js)
	page.copy("0xabc")
	page.request_paste()
	T.eq(page.take_paste(), null, "not answered yet")
	T.eq(page.take_paste(), "0xab", "the clipboard text")
	T.eq(page.take_paste(), "", "the browser refused")
	T.eq(page.take_paste(), "", "an answer that is not a string reads as a refusal")
	T.eq(page.take_paste(), null, "no value: not answered")
	page.open_url("https://arbiscan.io/tx/1")
	T.eq(js.scripts[0], "scfgBridge.copy(\"0xabc\")", "copy")
	T.eq(js.scripts[1], "scfgBridge.requestPaste()", "requestPaste")
	T.eq(js.scripts[-1], "scfgBridge.openUrl(\"https://arbiscan.io/tx/1\")", "openUrl")
	return T.done()


# tests/unit/test_web_page.py::test_take_copy_failed_reads_the_one_shot_flag
func test_take_copy_failed_reads_the_one_shot_flag() -> bool:
	var js := F.FakeJs.new()
	js.answers["scfgBridge.takeCopyFailed("] = [1, 0.0, null]
	var page := _page(js)
	T.eq(page.take_copy_failed(), true, "failed once")
	T.eq(page.take_copy_failed(), false, "then not")
	T.eq(page.take_copy_failed(), false, "no value: not failed")
	T.eq(js.scripts, ["scfgBridge.takeCopyFailed()", "scfgBridge.takeCopyFailed()", "scfgBridge.takeCopyFailed()"] as Array[String], "the scripts")
	return T.done()
