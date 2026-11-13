## ScfgWebMirror (addons/scfg/web_mirror.gd): the receipts' copy in the page's localStorage on a
## web export, on FakeJs's fake localStorage (its storage scripts run there).
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const KEY := "scfg:SCFG Demo:25cc291fc16215469f2e86aff3472ce019ae62e216ee8312ffebc5f3228e72d7"


static func _receipt(tx: String, item: String = "chapter2") -> Dictionary:
	return {"chain": "arbitrum", "tx_hash": tx, "log_index": 3, "block": 990, "kind": "unlock", "item": item,
		"amount": "4.99", "token": "USDC", "payer": "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed",
		"recipient": "0x52908400098527886e0f7030069857d2e4169ee7", "source": "payment"}


static func _doc(receipts: Dictionary) -> String:
	return JSON.stringify({"version": 1, "receipts": receipts})


func test_the_key_follows_the_project_name_and_storage_id() -> bool:
	T.eq(ScfgWebMirror.key_for("SCFG Demo"), KEY, "scfg:<name>:<sha256 of the name> without a storage id")
	T.eq(ScfgWebMirror.key_for("SCFG Demo", ""), KEY, "an empty storage id is no storage id")
	T.eq(ScfgWebMirror.key_for("SCFG Demo", "com.example.mygame"),
		"scfg:SCFG Demo:6dc5a1c640e1624934d78faae75e8040502c833d7d206ab552d8f2f37c939c65", "<sha256 of the storage id>")
	T.ok(ScfgWebMirror.key_for("SCFG Demo", "a") != ScfgWebMirror.key_for("SCFG Demo", "b"),
		"two games with one name and different storage ids do not share a key")
	T.eq(ScfgWebMirror.project_key(),
		"scfg:scfg-dev:51faacf069774fe50b2caa16226886f8ae9136bd54d2e3b1ceb7d2ff419f450c", "this project's (no storage id)")
	ProjectSettings.set_setting(ScfgWebMirror.STORAGE_ID_SETTING, "com.example.mygame")
	T.eq(ScfgWebMirror.project_key(),
		"scfg:scfg-dev:6dc5a1c640e1624934d78faae75e8040502c833d7d206ab552d8f2f37c939c65", "scfg/web/storage_id when set")
	ProjectSettings.set_setting(ScfgWebMirror.STORAGE_ID_SETTING, 7)
	T.eq(ScfgWebMirror.project_key(), ScfgWebMirror.key_for("scfg-dev"), "a storage id that is not a String is ignored")
	ProjectSettings.set_setting(ScfgWebMirror.STORAGE_ID_SETTING, null)  # (removes it)
	T.eq(ScfgWebMirror.project_key(), ScfgWebMirror.key_for("scfg-dev"), "and back")
	return T.done()


func test_write_then_read_round_trips_receipts_with_ints() -> bool:
	var js := F.FakeJs.new()
	var m := ScfgWebMirror.new(js, KEY)
	var receipts := {"arbitrum:0xab:3": _receipt("0xab"), "arbitrum:0xcd:3": _receipt("0xcd", "tip")}
	T.eq(m.write(receipts), null, "written")
	var stored: Variant = (js.local_storage as Dictionary).get(KEY)
	T.ok(stored is String, "one text under the key (%s)" % [js.local_storage])
	var doc: Variant = ScfgPy.from_json(JSON.parse_string(str(stored)))
	T.eq(doc, {"version": 1, "receipts": receipts}, "the document: version and receipts only")
	var got := m.read()
	T.eq([got["warning"], got["available"]], [null, true], "read back")
	T.eq(got["receipts"], receipts, "the same receipts")
	T.eq((got["receipts"] as Dictionary).keys(), receipts.keys(), "in store order")
	T.eq(typeof(got["receipts"]["arbitrum:0xab:3"]["log_index"]), TYPE_INT, "ints stay ints (ScfgPy.from_json)")
	T.eq(js.scripts, [] as Array[String], "nothing else ran: no page script is needed")
	return T.done()


func test_key_and_value_cross_as_json_literals() -> bool:
	var js := F.FakeJs.new()
	var key := "scfg:it's \"x\"\n</script>:ab"
	var m := ScfgWebMirror.new(js, key)
	var nasty := _receipt("0xab", "x'); alert(1); //\"\\\u2028\u2029</script>")
	T.ok(str(nasty["item"]).contains(char(0x2028)) and str(nasty["item"]).contains(char(0x2029)),
		"the item holds U+2028 and U+2029 (line terminators in JavaScript before ES2019)")
	T.eq(m.write({"k": nasty}), null, "written")
	var text := JSON.stringify({"version": 1, "receipts": {"k": nasty}}, "", false, true)
	T.eq(js.storage_scripts, [ScfgWebMirror.WRITE_JS + "(" + JSON.stringify(key) + ", " + JSON.stringify(text) + ")"] as Array[String],
		"constant code called on two JSON literals")
	T.eq(m.read()["receipts"], {"k": nasty}, "and read back whole")
	T.eq(js.storage_scripts[1], ScfgWebMirror.READ_JS + "(" + JSON.stringify(key) + ")", "the read: the key as a literal")
	for code: String in [ScfgWebMirror.READ_JS, ScfgWebMirror.WRITE_JS]:
		T.ok(code.begins_with("(function (") and code.ends_with("})"), "a function expression")
		T.ok(code.contains("try {") and code.contains("catch (e)"), "that catches everything")
	return T.done()


func test_nothing_stored_is_no_receipts_and_no_warning() -> bool:
	var got := ScfgWebMirror.new(F.FakeJs.new(), KEY).read()
	T.eq(got, {"receipts": {}, "warning": null, "available": true}, "a first launch")
	return T.done()


func test_a_value_that_is_not_the_document_is_ignored_with_a_warning() -> bool:
	for bad: String in ["{\"version\": 1, \"receipts\": ", "[1, 2]", "\"text\"", _doc({}).replace("1", "2"),
			"{\"version\": 1, \"receipts\": [1]}", "{\"version\": 1.5, \"receipts\": {}}", "{\"receipts\": {}}",
			"{\"version\": 1}"]:
		var js := F.FakeJs.new()
		js.local_storage[KEY] = bad
		var got := ScfgWebMirror.new(js, KEY).read()
		T.eq([got["receipts"], got["available"]], [{}, true], "no receipts from %s" % bad)
		var w: Variant = got["warning"]
		T.ok(w is String and (w as String).contains(KEY) and (w as String).contains("ignored"),
			"a warning naming the key (%s)" % [w])
	return T.done()


func test_an_unusable_local_storage_is_a_warning_never_an_error() -> bool:
	var receipts := {"arbitrum:0xab:3": _receipt("0xab")}
	# no localStorage in the page
	var none := F.FakeJs.new()
	none.local_storage = null
	var m := ScfgWebMirror.new(none, KEY)
	var got := m.read()
	T.eq([got["receipts"], got["available"]], [{}, false], "nothing, and not available")
	T.ok(got["warning"] is String and (got["warning"] as String).contains("localStorage"), "a warning (%s)" % [got["warning"]])
	var w: Variant = m.write(receipts)
	T.ok(w is String and (w as String).contains("no localStorage"), "a write is a warning (%s)" % [w])
	# blocked site data: every call throws
	var blocked := F.FakeJs.new()
	blocked.storage_throws = "SecurityError"
	got = ScfgWebMirror.new(blocked, KEY).read()
	T.eq(got["available"], false, "blocked: not available")
	T.ok(str(got["warning"]).contains("SecurityError"), "the browser's reason is named (%s)" % [got["warning"]])
	T.ok(str(ScfgWebMirror.new(blocked, KEY).write(receipts)).contains("SecurityError"), "a write too")
	# a full storage: only the write fails
	var full := F.FakeJs.new()
	full.write_throws = "QuotaExceededError"
	m = ScfgWebMirror.new(full, KEY)
	w = m.write(receipts)
	T.ok(w is String and (w as String).contains("QuotaExceededError") and (w as String).contains(KEY), "a full storage (%s)" % [w])
	T.eq(m.read(), {"receipts": {}, "warning": null, "available": true}, "and reads still work")
	# a page that gives no answer at all (JavaScriptBridge.eval answering null)
	var mute := ScfgWebMirror.new(Mute.new(), KEY)
	got = mute.read()
	T.eq([got["receipts"], got["available"]], [{}, false], "no answer: not available")
	T.ok(got["warning"] is String, "with a warning (%s)" % [got["warning"]])
	T.ok(mute.write(receipts) is String, "a write is a warning")
	return T.done()


## A page whose eval answers null to everything.
class Mute:
	extends RefCounted

	func eval(_code: String) -> Variant:
		return null


func test_update_merges_into_the_copy() -> bool:
	var js := F.FakeJs.new()
	var m := ScfgWebMirror.new(js, KEY)
	var a := _receipt("0xaa")
	var b := _receipt("0xbb")
	var b2 := _receipt("0xbb", "tip")
	var c := _receipt("0xcc")
	js.local_storage[KEY] = _doc({"a": a, "b": b})
	T.eq(m.update({"b": b2, "c": c}, {}), null, "merged")
	T.eq(m.read()["receipts"], {"a": a, "b": b2, "c": c}, "the copy's other receipts stay; this game's version wins")
	T.eq((m.read()["receipts"] as Dictionary).keys(), ["a", "b", "c"], "in the copy's order, new ones last")
	T.eq(m.update({"c": c}, {"b": true}), null, "a dropped key")
	T.eq(m.read()["receipts"], {"a": a, "c": c}, "leaves the copy")
	var writes := js.storage_scripts.filter(func(s: String) -> bool: return s.begins_with(ScfgWebMirror.WRITE_JS)).size()
	T.eq(m.update({"c": c}, {}), null, "nothing new")
	T.eq(js.storage_scripts.filter(func(s: String) -> bool: return s.begins_with(ScfgWebMirror.WRITE_JS)).size(), writes,
		"is not written again")
	js.local_storage[KEY] = "{broken"
	T.eq(m.update({"c": c}, {}), null, "a copy that is not the document")
	T.eq(m.read(), {"receipts": {"c": c}, "warning": null, "available": true}, "counts as empty and is replaced")
	T.eq(m.write({}), null, "write() replaces")
	T.eq(m.read()["receipts"], {}, "with exactly what it is given")
	return T.done()
