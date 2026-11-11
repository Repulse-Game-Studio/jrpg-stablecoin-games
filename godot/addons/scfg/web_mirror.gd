## A second copy of the receipts on a web export, in the page's `localStorage`.
##
## Every Godot web export on one origin shares one IndexedDB database (`/userfs`), and each one
## writes that whole database back from the copy it loaded at startup, so another game on the
## same origin can delete `user://scfg/state.json` after this game saved it. localStorage is
## written key by key: another game never touches this key. The autoload merges its receipts into
## the copy at each web save (`update`) and merges the copy back when it first reads the state
## (scfg.gd, WEB: A SECOND COPY OF THE RECEIPTS).
##
## The key is `scfg:<application/config/name>:<sha256 hex of the storage id>`. The storage id is
## the project setting `scfg/web/storage_id` when it is a non-empty String, else
## `application/config/name`, so it stays the same across releases as long as neither changes.
## Two games that share a project name get different keys when they set different storage ids.
## The value is `{"version": 1, "receipts": {...}}` as JSON text: the receipts only. Pending
## sessions stay in state.json alone (a payment whose session was lost is brought back with
## Restore and its transaction hash).
##
## `js` is ScfgWebPage's `js` (any object with `eval(code) -> Variant`; the real one is
## ScfgWebPage.BrowserJs). The JavaScript run here is constant code, a function expression that
## needs nothing of the checkout page; the key and the value cross as its arguments, JSON
## literals, never spliced into code. It catches everything and answers JSON text: a page without
## localStorage, a blocked one (SecurityError) or a full one (QuotaExceededError) is an answer,
## never an exception in the page.
##
## - `read()` returns `{"receipts", "warning", "available"}`. Nothing stored: empty receipts, no
##   warning. A value that is not that document: empty receipts and a warning (the next write
##   replaces it). localStorage unusable (missing, blocked, or no answer from the page):
##   `available` false, with a warning.
## - `write(receipts)` replaces the copy with `receipts` (clear_local_data writes `{}`); null, or a
##   warning String when the browser refused.
## - `update(receipts, dropped)` merges instead (one game open in two tabs: each tab holds only
##   what it loaded plus what it saved since, so a plain write would drop the other tab's new
##   receipts). It reads the copy, removes the keys in `dropped` (receipts this tab removed itself
##   since its last write, such as by re-verification), adds or replaces every receipt of
##   `receipts` (this tab's version wins), and writes the result unless the copy already holds it.
##   A copy that cannot be read as the document counts as empty. Null, or a warning.
##
## The receipts read are normalised with ScfgPy.from_json, as ScfgStore's; the Engine checks them
## like any stored receipt (launch re-verification).
class_name ScfgWebMirror
extends RefCounted

const VERSION := 1
const PREFIX := "scfg:"
## The project setting that tells two games with the same project name apart.
const STORAGE_ID_SETTING := "scfg/web/storage_id"
## `READ_JS(key)`: `{"value": <the stored text or null>}`, or `{"error": "<name>: <message>"}`.
const READ_JS := "(function (k) { try { var s = window.localStorage; if (!s) { return JSON.stringify({ error: 'no localStorage' }); } return JSON.stringify({ value: s.getItem(k) }); } catch (e) { var m = 'error'; try { m = String(e && e.name ? e.name + ': ' + e.message : e); } catch (_) {} return JSON.stringify({ error: m }); } })"
## `WRITE_JS(key, text)`: `{}`, or `{"error": "<name>: <message>"}`.
const WRITE_JS := "(function (k, v) { try { var s = window.localStorage; if (!s) { return JSON.stringify({ error: 'no localStorage' }); } s.setItem(k, v); return '{}'; } catch (e) { var m = 'error'; try { m = String(e && e.name ? e.name + ': ' + e.message : e); } catch (_) {} return JSON.stringify({ error: m }); } })"

var js: Variant
var key: String


func _init(js_: Variant, key_: String) -> void:
	js = js_
	key = key_


## The key for a project name and storage id (see the header).
static func key_for(project_name: String, storage_id: String = "") -> String:
	return "%s%s:%s" % [PREFIX, project_name, (storage_id if storage_id != "" else project_name).sha256_text()]


## This project's key: `application/config/name` and `scfg/web/storage_id`.
static func project_key() -> String:
	var id: Variant = ProjectSettings.get_setting(STORAGE_ID_SETTING, "")
	return key_for(str(ProjectSettings.get_setting("application/config/name", "")), id if id is String else "")


## The receipts stored under `key` (see the header).
func read() -> Dictionary:
	var answer: Variant = _answer(js.eval("%s(%s)" % [READ_JS, JSON.stringify(key)]))
	if answer is String:
		return {"receipts": {}, "available": false,
			"warning": "the page's localStorage cannot be read (%s), so the receipts have no second copy there (localStorage key %s). See \"Web exports\" in the addon's README." % [answer, key]}
	var text: Variant = (answer as Dictionary).get("value")
	if text == null:
		return {"receipts": {}, "warning": null, "available": true}
	var why := ""
	var doc: Variant = null
	var parsed := ScfgCoio.parse_body(str(text).to_utf8_buffer()) if text is String else [false, null]
	if not parsed[0]:
		why = "is not valid JSON"
	else:
		doc = ScfgPy.from_json(parsed[1])
		if not doc is Dictionary:
			why = "is not a JSON object"
		elif not (typeof(doc.get("version")) == TYPE_INT and doc["version"] == VERSION):
			why = "has an unknown version %s" % ScfgPy.py_repr(doc.get("version"))
		elif not doc.get("receipts") is Dictionary:
			why = "has no receipts object"
	if why != "":
		return {"receipts": {}, "available": true,
			"warning": "the receipts' copy in localStorage (key %s) %s; it is ignored and replaced at the next save." % [key, why]}
	return {"receipts": doc["receipts"], "warning": null, "available": true}


## Replaces the copy under `key` with `receipts`; null, or a warning.
func write(receipts: Dictionary) -> Variant:
	# floats at full precision and keys in insertion order, as ScfgStore.save
	var text := JSON.stringify({"version": VERSION, "receipts": receipts}, "", false, true)
	var answer: Variant = _answer(js.eval("%s(%s, %s)" % [WRITE_JS, JSON.stringify(key), JSON.stringify(text)]))
	if answer is String:
		return "the browser did not keep the receipts' copy in localStorage (%s; key %s), so the receipts are only in user://, where another game on this origin can delete them. See \"Web exports\" in the addon's README." % [answer, key]
	return null


## Merges `receipts` into the copy (see the header); null, or a warning.
func update(receipts: Dictionary, dropped: Dictionary) -> Variant:
	var got := read()
	var current: Dictionary = got["receipts"]
	var merged := {}
	for k: Variant in current:
		if not dropped.has(k):
			merged[k] = receipts[k] if receipts.has(k) else current[k]
	for k: Variant in receipts:
		merged[k] = receipts[k]
	if got["warning"] == null and merged == current and merged.keys() == current.keys():
		return null  # the copy already holds exactly this
	return write(merged)


## The page's answer as a Dictionary, or the reason it is not one (a String): the browser's error
## (`{"error": ...}`), or no JSON answer at all.
static func _answer(raw: Variant) -> Variant:
	if not raw is String:
		return "the page gave no answer"
	var json := JSON.new()  # (parse_string would log an engine error for text that is not JSON)
	if json.parse(raw) != OK or not json.data is Dictionary:
		return "the page gave no answer"
	var data: Dictionary = json.data
	if data.has("error"):
		return str(data["error"])
	return data
