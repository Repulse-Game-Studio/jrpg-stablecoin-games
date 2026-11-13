## The game's side of a Godot web export: every call into the page's JavaScript
## (web/bridge.js) behind a small method. The twin of scfg_core/web_page.py.
##
## `js` is any object with `eval(code: String) -> Variant` (the real one is BrowserJs; tests pass a
## fake). Values cross as JSON literals, so game data is never spliced into JavaScript as code;
## answers are JSON text, normalised with ScfgPy.from_json.
##
## ScfgWebPage is the ScfgOverlayCheckout's ui plus the clipboard, links and wallet detection. Every
## call is synchronous: the page answers a paste or a build later, and the autoload polls for it.
## (Ren'Py's start/poll request bridge has no twin: Godot's HTTPRequest already uses fetch.)
class_name ScfgWebPage
extends RefCounted

## bridge.js first: it marks the page.
const SCRIPTS: Array[String] = ["bridge.js", "evm.js", "solana.js", "app.js"]

var js: Variant
## load_asset(name) -> PackedByteArray: a file of the shared page (web/); anything else when the
## file is missing.
var load_asset: Callable
## warn(message): developer advice, such as a page file left out of the export. The autoload
## passes its `_warn` (debug builds only); without one, a debug build calls push_warning.
var warn: Callable
var installed := false


func _init(js_: Variant, load_asset_: Callable, warn_: Callable = Callable()) -> void:
	js = js_
	load_asset = load_asset_
	warn = warn_


## The real `js`: the browser page the export runs in. Only create it on a web export.
class BrowserJs:
	extends RefCounted

	## Runs `code` in the page's global context; the value of its last expression.
	func eval(code: String) -> Variant:
		return JavaScriptBridge.eval(code, true)


static func _lit(value: Variant) -> String:
	return JSON.stringify(value)


## The text of the page file `name`, or null (after a warning) when the export lacks it.
func _text(name: String) -> Variant:
	var bytes: Variant = load_asset.call(name)
	if bytes is PackedByteArray:
		return (bytes as PackedByteArray).get_string_from_utf8()
	_warn("the checkout page file addons/scfg/web/%s is missing from this export, so the browser checkout cannot work. Add addons/scfg/web/* to the export preset's \"Filters to export non-resource files/folders\" (see \"Web exports\" in the addon's README)." % name)
	return null


func _warn(message: String) -> void:
	if warn.is_valid():
		warn.call(message)
	elif OS.is_debug_build():
		push_warning("scfg: " + message)


## Load bridge.js and the checkout page's scripts into the page, once. A file the export lacks
## is skipped with a warning.
func install() -> void:
	if installed:
		return
	for name in SCRIPTS:
		var code: Variant = _text(name)
		if code != null:
			js.eval(code)
	var page: Variant = _text("index.html")
	js.eval("scfgBridge.setPage(%s)" % _lit(page if page != null else ""))
	installed = true


# --- wallets -----------------------------------------------------------------------------------


## Is there a wallet in the page that can pay on this chain (a core chain Dictionary)?
func has_wallet(chain: Dictionary) -> bool:
	var answer: Variant = js.eval("scfgBridge.hasWallet(%s, %s)" % [_lit(chain["family"]), _lit(chain["wallet_chain"])])
	return _truthy(answer)


# --- checkout overlay (ScfgOverlayCheckout's ui) -------------------------------------------------


func open(key: String, session_info: Dictionary, logo_url: Variant) -> void:
	js.eval("scfgBridge.openCheckout(%s, %s, %s)" % [_lit(key), _lit(session_info), _lit(logo_url)])


func show(key: String) -> void:
	js.eval("scfgBridge.show(%s)" % _lit(key))


func close(key: String) -> void:
	js.eval("scfgBridge.closeCheckout(%s)" % _lit(key))


## The messages the overlay's page posted since the last call (an Array; [] for anything else).
func take(key: String) -> Array:
	var parsed: Variant = _parse(js.eval("scfgBridge.take(%s)" % _lit(key)))
	return parsed if parsed is Array else []


func set_status(key: String, status: Dictionary) -> void:
	js.eval("scfgBridge.setStatus(%s, %s)" % [_lit(key), _lit(status)])


func reply(key: String, request_id: int, status: int, body: Dictionary) -> void:
	js.eval("scfgBridge.reply(%s, %d, %d, %s)" % [_lit(key), request_id, status, _lit(body)])


# --- clipboard and links -------------------------------------------------------------------------


func copy(text: String) -> void:
	js.eval("scfgBridge.copy(%s)" % _lit(text))


func request_paste() -> void:
	js.eval("scfgBridge.requestPaste()")


## null until the browser answers; "" if it refused; else the clipboard text.
func take_paste() -> Variant:
	var answer: Variant = js.eval("scfgBridge.takePaste()")
	if not answer is String:
		return null  # the page gave no answer at all
	var parsed: Variant = _parse(answer)
	if parsed == null and (answer as String).strip_edges() == "null":
		return null
	return parsed if parsed is String else ""


## True once after a copy() the browser refused; false otherwise.
func take_copy_failed() -> bool:
	return _truthy(js.eval("scfgBridge.takeCopyFailed()"))


func open_url(url: String) -> void:
	js.eval("scfgBridge.openUrl(%s)" % _lit(url))


# --- values ------------------------------------------------------------------------------------


## JSON text from the page as a value (numbers normalised by ScfgPy.from_json); null for anything
## that is not JSON text.
static func _parse(text: Variant) -> Variant:
	if not text is String:
		return null
	var json := JSON.new()  # (parse_string would log an engine error for text that is not JSON)
	if json.parse(text) != OK:
		return null
	return ScfgPy.from_json(json.data)


## A number or boolean the page answered (JavaScriptBridge gives numbers as float or int).
static func _truthy(answer: Variant) -> bool:
	if answer is bool:
		return answer
	if answer is int or answer is float:
		return answer != 0
	return false
