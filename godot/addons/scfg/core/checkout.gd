## State behind the payment screen, independent of any engine. Mirrors scfg_core/checkout.py (and
## the TS core's rpgmaker/src/core/checkout.ts).
##
## The Godot sheet (ui/sheet.gd) only displays this object's fields and calls its methods. Engine
## work goes through `submit` (the ScfgWorker's), and results come back through `poll()` (jobs) and
## `on_outcome()` (watcher outcomes). Nothing here touches the SceneTree or the network.
##
## States: choose -> starting -> waiting -> confirming -> paid
##                                      \-> underpaid | expired
##
## The sheet shows one step at a time (`step`):
## amount (tips only) -> network -> method -> browser | qr | manual -> waiting -> done.
## Steps with a single option are skipped both ways. `back()` works until a transaction is seen; a
## seen payment moves the sheet to waiting and a final state to done.
##
## `ScfgCheckoutController.new(opts)` takes Python's keyword arguments as keys of `opts`:
##   - config, spec, submit (required): the Config, the PaymentSpec (ScfgPayments.prepare_payment)
##     and `submit(fn: Callable) -> ScfgJob`. `fn(engine)` may be a coroutine (the Worker awaits it);
##   - mobile (false);
##   - server_factory: a Callable taking one Dictionary `{session_info, on_tx, get_status}` plus
##     `build_tx` on Solana, returning the page object (or an ScfgErr). The page has `start()`
##     (may return an ScfgErr), `stop()`, `url` and `running`, and optionally `reopen()` and
##     `pump()`. `session_info` is the exact dict Python hands the page (same keys and order,
##     `amount_units` and `min_fee_wei` Strings, `chain_id_hex` as Python's hex()).
##     `on_tx(tx_hash, sender = null)` takes a normalised id; call it from the game loop, never
##     from inside a worker command. `get_status()` is status_info(). `build_tx(account)` returns
##     the Worker's ScfgJob that builds the unsigned Solana transfer;
##   - unready: a Callable returning `worker.snapshot["unready"]`: an Array of [chain_key, token]
##     pairs (or a Dictionary keyed by them, like ScfgEngine.unready). Nothing is blocked by default;
##   - game_name ("Game" when empty), browser_wallet (a Callable taking a Chain, for the web build).
## A constructor cannot return an error: the (unreachable) failure of the first network step lands
## in `init_error`.
##
## Every injected Callable except the `fn` handed to `submit` must be synchronous (submit,
## server_factory, unready, browser_wallet, and the page's start(), stop(), reopen() and pump()):
## the controller calls them without `await`.
## The callbacks given to the page are bound to this controller but do not keep it alive: call
## close() (or cancel()/finish()) before releasing it; the page checks `is_valid()` before a call.
##
## Python's `@property` members are properties here; methods keep their names. A method Python can
## raise from returns its value or an ScfgErr (`## @fallible`). A cancelled job lands in poll()'s
## failure branch (start_failed).
class_name ScfgCheckoutController
extends RefCounted

const FINAL_STATES := ["paid", "underpaid", "expired"]
## Errors that judge the reported transaction (the hint) and drop it.
const HINT_VERDICTS := ["tx_reverted", "tx_not_a_payment", "tx_not_landed"]
## Order of the steps; the three method pages share a place.
const STEP_ORDER := {"amount": 0, "network": 1, "method": 2, "browser": 3, "qr": 3, "manual": 3,
	"waiting": 4, "done": 5}

static var MESSAGES := {
	"network_unavailable": "Can't reach the network. Retrying...",
	"tx_reverted": "That transaction failed on-chain; no funds were sent.",
	"tx_not_a_payment": "That transaction isn't a payment for this purchase.",
	"tx_not_landed": "That transaction didn't reach the network. You can pay again.",
	"invalid_hash": "That doesn't look like a transaction hash or signature.",
	"invalid_amount": "Enter an amount of at least %s with up to 2 decimals." % str(ScfgPayments.MIN_TIP),
	"browser_unavailable": "Couldn't start the browser checkout. Use another method.",
	"start_failed": "Couldn't start the payment. Check your connection and try again.",
	"solana_unavailable": "Payments on Solana aren't available right now.",
	"internal_error": "Something went wrong while checking the payment. Retrying...",
	"paste_blocked": "Your browser blocked pasting. Type it instead: the game also finds the payment without it.",
}

var config: Variant
var spec: Variant
var submit: Callable
var mobile: bool
var server_factory: Variant
var unready: Variant
var browser_wallet: Variant
var game_name: String
var amount: ScfgDec
var custom_amount: String = ""
var tx_input: String = ""
var chain_key: Variant = null
var session: Variant = null
var method: Variant = null
var state: String = "choose"
var error: Variant = null
var confirmations: int = 0
var required: int = 0
var hint_tx: Variant = null
var dropped_tx: Variant = null  # the hint that never landed (tx_not_landed)
var result: Variant = null
var browser_failed: bool = false
var step: String
var nav_dir: int = 1  # 1: the step came in going forward, -1: going back
## The error the constructor's first network step ended in (unreachable), or null.
var init_error: Variant = null
var _future: Variant = null
var _server: Variant = null
var _picked_network: bool = false  # move on from the network step once the session starts

# --- read-only views ---------------------------------------------------------

var session_id: Variant:
	get:
		return session["id"] if ScfgPy.py_truthy(session) else null

var chain: Variant:
	get:
		if not ScfgPy.py_truthy(chain_key):
			return null
		return ScfgConfig.enabled_chains(config).get(chain_key)

var family: Variant:
	get:
		var c: Variant = chain
		return c["family"] if c != null else null

## The exact amount to send, with the item code and nonce: '4.993742'.
var pay_amount_text: Variant:
	get:
		var text: Variant = _pay_amount()  # err-ok: the units of a session the Engine made always format
		return text

## The price as the player thinks of it: '4.99'.
var price_text: String:
	get:
		return str(amount)

var token_address: Variant:
	get:
		var c: Variant = chain
		return c["tokens"][spec["token"]]["address"] if c != null else null

var recipient: Variant:
	get:
		var c: Variant = chain
		if c != null:
			return ScfgConfig.wallet_for(config, c)
		return config["wallet"] if ScfgPy.py_truthy(config["wallet"]) else config["solana_wallet"]

## A transaction for this payment exists, so going back could start a second one.
var tx_seen: bool:
	get:
		return ScfgPy.py_truthy(hint_tx) or state == "confirming"

var can_go_back: bool:
	get:
		if step == "done" or tx_seen:
			return false
		return _previous_step() != null


func _init(opts: Dictionary) -> void:
	config = opts.get("config")
	spec = opts.get("spec")
	submit = opts.get("submit", Callable())
	mobile = bool(opts.get("mobile", false))
	server_factory = opts.get("server_factory")
	unready = opts.get("unready")  # null: nothing blocked
	browser_wallet = opts.get("browser_wallet")
	var name: Variant = opts.get("game_name", "Game")
	game_name = str(name) if ScfgPy.py_truthy(name) else "Game"
	amount = spec["price"]
	step = "amount" if spec["kind"] == "tip" else "network"
	if step == "network":
		var failed: Variant = _enter_network()
		if failed is ScfgErr:
			init_error = failed


## @fallible
func _pay_amount() -> Variant:
	if ScfgPy.py_truthy(session):
		return ScfgAmounts.format_units(session["units"])
	return str(amount)


## The routes the snapshot marks unusable, as [chain_key, token] pairs.
func _unready_pairs() -> Array:
	if not (unready is Callable and (unready as Callable).is_valid()):
		return []
	var raw: Variant = (unready as Callable).call()
	if raw is Dictionary:
		return (raw as Dictionary).keys()
	return Array(raw) if raw is Array else []


func chain_options() -> Array:
	var chains := ScfgConfig.enabled_chains(config)
	var blocked := _unready_pairs()
	var out := []
	for k: Variant in spec["chains"]:
		if chains.has(k) and not blocked.has([k, spec["token"]]):
			out.append([k, chains[k]["name"], chains[k]["fee_hint"]])
	return out


func tip_presets() -> Array:
	return ScfgPayments.TIP_PRESETS


func methods() -> Array:
	var m: Dictionary = config["methods"]
	var out := []
	if ScfgPy.py_truthy(m["browser"]["enabled"]) and not browser_failed and _browser_possible():
		out.append("browser")
	var qr_chains: Variant = m["qr"]["chains"]
	if ScfgPy.py_truthy(m["qr"]["enabled"]) and (qr_chains == null or (qr_chains as Array).has(chain_key)):
		out.append("qr")
	if ScfgPy.py_truthy(m["manual"]["enabled"]):
		out.append("manual")
	return out


## Desktop: a computer's browser can always open the page. Web build: only if a wallet that can
## pay on the chosen network is in the page (phones included: a wallet app's own browser injects
## one).
func _browser_possible() -> bool:
	if browser_wallet != null:
		var c: Variant = chain
		return c != null and ScfgPy.py_truthy((browser_wallet as Callable).call(c))
	return not mobile


## The payment request a wallet scans or opens.
## @fallible
func payment_uri() -> Variant:
	if not ScfgPy.py_truthy(session):
		return null
	if family == "solana":
		return ScfgSolanaPaylinks.solana_pay_uri({"recipient": recipient, "units": session["units"],
			"mint": token_address, "reference": session["reference"], "label": game_name, "message": spec["title"]})
	return ScfgPaylinks.eip681_transfer_uri({"token_address": token_address, "chain_id": chain["chain_id"],
		"recipient": recipient, "units": session["units"]})


## @fallible
func explorer_url() -> Variant:
	var tx: Variant = hint_tx
	if result != null and ScfgPy.py_truthy(result.get("tx_hash")):
		tx = result["tx_hash"]
	var c: Variant = chain
	if ScfgPy.py_truthy(tx) and c != null:
		return ScfgChains.explorer_url(c, tx)
	return null


func message() -> Variant:
	if ScfgPy.py_truthy(error):
		return MESSAGES.get(error)
	if state == "choose" and ScfgPy.py_truthy(spec["chains"]) and chain_options().is_empty():
		return MESSAGES["solana_unavailable"]
	return null


## Shown in DEV MODE only: setup problems the developer must fix.
func dev_warnings() -> Array:
	var registry := ScfgConfig.registry(config)
	var pairs := _unready_pairs()
	pairs.sort_custom(func(a: Array, b: Array) -> bool: return a[0] < b[0] or (a[0] == b[0] and a[1] < b[1]))
	var out := []
	for pair: Array in pairs:
		if registry.has(pair[0]) and registry[pair[0]]["family"] == "solana":
			out.append("Your Solana wallet can't receive %s yet: receive any amount once." % pair[1])
	return out


## Status for the localhost page. `dropped_tx` names the transaction a `tx_not_landed` is about, so
## the page offers Pay again only if it is the one the page sent.
func status_info() -> Dictionary:
	return {"state": state, "confirmations": confirmations, "required": required, "tx_hash": hint_tx,
		"error": error, "dropped_tx": dropped_tx}


## The steps this payment walks through, without the single-option ones.
func step_path() -> Array:
	var path := ["amount"] if spec["kind"] == "tip" else []
	if chain_options().size() != 1:
		path.append("network")
	if methods().size() != 1:
		path.append("method")
	path.append(method if ScfgPy.py_truthy(method) else "browser")
	path.append("waiting")
	return path


## [index of the current step, number of steps] for the progress dots.
func step_dots() -> Array:
	var path := step_path()
	var here: int = STEP_ORDER[step]
	var before := 0
	for s: String in path:
		if STEP_ORDER[s] < here:
			before += 1
	return [before, path.size()]


# --- actions -----------------------------------------------------------------

## Start a session on `key`, discarding the one running. A chain the payment does not offer is a
## ValueError.
## @fallible
func select_chain(key: Variant) -> Variant:
	if not ScfgConfig.in_strs(spec["chains"], key):
		return ScfgErr.make("ValueError", "chain %s not available for this payment" % ScfgPy.py_repr(key))
	_discard_session()
	chain_key = key
	state = "starting"
	error = null
	var kind: Variant = spec["kind"]
	var opts := {"item": spec["item"], "amount": amount, "token": spec["token"], "title": spec["title"]}
	_future = submit.call(func(e: Variant) -> Variant: return await e.start_session(kind, key, opts))  # err-ok: an ScfgErr fails the job, which the caller reads
	var available := methods()
	if not available.has(method):
		method = available[0] if not available.is_empty() else null
	return null


## Network step: start a session on `key`, or keep the one already running.
## @fallible
func choose_network(key: Variant) -> Variant:
	step = "network"
	nav_dir = 1
	if key == chain_key and ScfgPy.py_truthy(session) and state == "waiting":
		_after_network()
		return null
	_picked_network = true  # poll() moves on once the session has started
	if key != chain_key or state != "starting":
		return select_chain(key)
	return null


func choose_method(name: String) -> void:
	if methods().has(name):
		method = name
		error = null
		_go(name, 1)


## Leave the amount step (tips), applying a typed amount first. False when the step stays.
## @fallible
func next_step() -> Variant:
	if step != "amount":
		return false
	if ScfgPy.py_truthy(ScfgPy.py_strip(custom_amount)):
		var applied: Variant = apply_custom_amount()
		if applied is ScfgErr:
			return applied
		if not applied:
			return false
	var entered: Variant = _enter_network()
	if entered is ScfgErr:
		return entered
	return true


func back() -> void:
	if can_go_back:
		error = null
		_go(_previous_step(), -1)


## The player says they sent the payment from the QR or manual page.
func mark_sent() -> void:
	if step == "qr" or step == "manual":
		error = null
		_go("waiting", 1)


## Browser page: start the localhost page and move to waiting; returns its URL (or null).
func open_browser() -> Variant:
	var url: Variant = start_browser()
	if ScfgPy.py_truthy(url):
		_go("waiting", 1)
	elif browser_failed and step == "browser":
		var available := methods()
		_go("method" if available.size() != 1 else available[0], -1)
	return url


## Change the tip. False (with `error` set) for an unusable amount, and for an unlock.
## @fallible
func set_tip_amount(value: Variant) -> Variant:
	if spec["kind"] != "tip":
		return false
	var parsed: Variant = ScfgAmounts.parse_amount(value)
	if parsed is ScfgErr:
		if (parsed as ScfgErr).is_a("TypeError") or (parsed as ScfgErr).is_a("ValueError"):
			error = "invalid_amount"
			return false
		return parsed
	var new_amount: ScfgDec = parsed
	if new_amount.cmp(ScfgPayments.MIN_TIP) < 0:
		error = "invalid_amount"
		return false
	error = null
	if new_amount.cmp(amount) == 0 and ScfgPy.py_truthy(chain_key):
		return true  # same amount: keep the session already started
	amount = new_amount
	if ScfgPy.py_truthy(chain_key):
		var restarted: Variant = select_chain(chain_key)
		if restarted is ScfgErr:
			return restarted
	return true


## @fallible
func apply_custom_amount() -> Variant:
	return set_tip_amount(ScfgPy.py_strip(custom_amount))


## The player pasted a transaction id (`text`, else `tx_input`). False (with `error` set) when it
## is not one in the chain family's format.
## @fallible
func submit_hash(text: Variant = null) -> Variant:
	var c: Variant = chain
	if not ScfgPy.py_truthy(session) or c == null:
		return false
	var backend: Variant = ScfgBackends.backend_for(c)
	if backend is ScfgErr:
		return backend
	var tx: Variant = backend.normalize_tx(tx_input if text == null else text)
	if tx is ScfgErr:
		if (tx as ScfgErr).is_a("ValueError"):
			error = "invalid_hash"
			return false
		return tx
	_report_hint(tx)
	tx_input = ""
	return true


func _report_hint(tx_hash: Variant, _sender: Variant = null) -> void:
	var sid: Variant = session_id
	hint_tx = tx_hash
	error = null
	dropped_tx = null
	submit.call(func(e: Variant) -> Variant: return await e.report_hint(sid, tx_hash))  # err-ok: the job is dropped; an ScfgErr fails it silently, as Python's fire-and-forget does
	if step != "done":
		_go("waiting", 1)


## Start the localhost page for this session; returns its URL or null.
func start_browser() -> Variant:
	if not ScfgPy.py_truthy(session) or not server_factory is Callable:
		return null
	if _server == null:
		var c: Dictionary = chain
		var info := {
			"title": spec["title"], "kind": spec["kind"], "token": spec["token"],
			"family": c["family"], "decimals": 6,
			"amount_units": str(session["units"]), "amount_text": pay_amount_text,
			"price_text": price_text,
			"recipient": recipient, "chain_name": c["name"],
			"native_symbol": c["native_symbol"],
			"explorer_tx": c["explorer_tx"], "testnet": config["testnet"],
			"logo": config["logo"] != null, "accent": config["accent"],
			"on_accent": ScfgConfig.text_on(config["accent"]),
		}
		if c["family"] == "solana":
			info["wallet_chain"] = c["wallet_chain"]
			info["mint"] = token_address
			info["reference"] = session["reference"]
		else:
			info["chain_id"] = c["chain_id"]
			info["chain_id_hex"] = "0x%x" % c["chain_id"]
			info["rpc_url"] = c["rpc"][-1]
			info["token_address"] = token_address
			# Arc: the token is also the gas coin, with a fee floor.
			info["gas_is_token"] = c["native_token"] == spec["token"]
			info["min_fee_wei"] = str(c["min_fee_wei"])
		var kwargs := {"session_info": info, "on_tx": _report_hint, "get_status": status_info}
		if c["family"] == "solana":
			kwargs["build_tx"] = _submit_build
		var server: Object = (server_factory as Callable).call(kwargs)
		var started: Variant = null
		if server != null and not server is ScfgErr:
			started = server.start()
		if server == null or server is ScfgErr or started is ScfgErr:
			_server = null
			browser_failed = true
			error = "browser_unavailable"
			if method == "browser":
				var available := methods()
				method = available[0] if not available.is_empty() else null
			return null
		_server = server
	elif _server.has_method("reopen"):
		_server.reopen()
	return _server.url


## Ask the Worker for the unsigned Solana transfer; returns its Job. The desktop page's server
## waits on it, the web overlay polls it.
func _submit_build(account: Variant) -> Variant:
	var sid: Variant = session_id
	return submit.call(func(e: Variant) -> Variant: return await e.build_solana_payment(sid, account))  # err-ok: an ScfgErr fails the job, which the caller reads


func cancel() -> Variant:
	var sid: Variant = session_id
	if ScfgPy.py_truthy(sid) and not FINAL_STATES.has(state):
		submit.call(func(e: Variant) -> Variant: return await e.set_ui_status(sid, "cancelled"))  # err-ok: the job is dropped; an ScfgErr fails it silently, as Python's fire-and-forget does
	close()
	if result == null:
		result = _result("cancelled")
	return result


func keep_playing() -> Variant:
	var sid: Variant = session_id
	if ScfgPy.py_truthy(sid):
		submit.call(func(e: Variant) -> Variant: return await e.set_ui_status(sid, "pending"))  # err-ok: the job is dropped; an ScfgErr fails it silently, as Python's fire-and-forget does
	return _result("pending")


## Close after a final state (paid/underpaid/expired).
func finish() -> Variant:
	close()
	return result if result != null else _result("cancelled")


func close() -> void:
	_abandon_start()
	if _server != null:
		_server.stop()
		_server = null


# --- updates -------------------------------------------------------------

## Pick up the result of start_session (call often from the UI).
func poll() -> void:
	if _server != null and _server.has_method("pump"):
		_server.pump()
	var fut: Variant = _future
	if fut == null or not fut.done():
		return
	_future = null
	var exc: Variant = fut.exception()  # err-ok: the job's failure, handled below
	var started: Variant = fut.result() if exc == null else null  # err-ok: exception() is null, so this is the value
	# A success that is not a session (Python would have raised on it) is a failed start too.
	if exc != null or not started is Dictionary:
		state = "choose"
		chain_key = null
		var network: bool = exc is ScfgErr and (exc as ScfgErr).is_a("RpcUnavailable")
		error = "network_unavailable" if network else "start_failed"
		if step != "amount" and step != "network":
			_go("network", -1)
		return
	session = started
	state = "waiting"
	var c: Variant = chain
	required = c["confirmations"] if c != null else 0
	if step == "network" and _picked_network:
		_after_network()


## A StepOutcome from the Worker's drain_outcomes() (or Engine.tick()).
func on_outcome(outcome: Variant) -> void:
	if not ScfgPy.py_truthy(session) or outcome.get("session_id") != session_id:
		return
	var out_error: Variant = outcome.get("error")
	if HINT_VERDICTS.has(out_error):
		_hint_verdict(outcome)
	elif ScfgPy.py_truthy(out_error):
		error = out_error
	var status: Variant = outcome.get("status")
	if status == "confirming":
		state = "confirming"
		confirmations = outcome["confirmations"]
		required = outcome["required"]
		if out_error == null:
			error = null
	elif status == "reorged":
		state = "waiting"
		confirmations = 0
	elif status == "paid" or status == "underpaid":
		state = status
		result = outcome.get("result")
		error = null
	elif status == "expired" and state != "paid" and state != "underpaid":
		state = "expired"
		result = outcome.get("result")
	if FINAL_STATES.has(state):
		if step != "done":
			_go("done", 1)
	elif tx_seen and step != "waiting":
		_go("waiting", 1)


# --- internals -----------------------------------------------------------

func _go(to: String, direction: int) -> void:
	step = to
	nav_dir = direction
	_picked_network = false


func _previous_step() -> Variant:
	var here: int = STEP_ORDER[step]
	var earlier := []
	for s: String in step_path():
		if STEP_ORDER[s] < here:
			earlier.append(s)
	return earlier[-1] if not earlier.is_empty() else null


## Go forward to the network step, or past it when there is only one network.
## @fallible
func _enter_network() -> Variant:
	var options := chain_options()
	if options.size() == 1:
		return choose_network(options[0][0])
	_go("network", 1)
	return null


## The session is running: go to the method step, or straight to the only method.
func _after_network() -> void:
	var available := methods()
	if not available.has(method):
		method = available[0] if not available.is_empty() else null
	_go(method if available.size() == 1 else "method", 1)


func _hint_verdict(outcome: Variant) -> void:
	var about: Variant = outcome.get("hint_tx")
	if ScfgPy.py_truthy(about) and ScfgPy.py_truthy(hint_tx) and about != hint_tx:
		return  # judged an older hint; the newer one reported since is still pending
	error = outcome.get("error")
	dropped_tx = (about if ScfgPy.py_truthy(about) else hint_tx) if error == "tx_not_landed" else null
	hint_tx = null


## Stop waiting for a session start the player moved away from, and discard that session once it
## exists: it would be watched for 24 h and reported expired.
func _abandon_start() -> void:
	var fut: Variant = _future
	_future = null
	if not fut is ScfgJob:
		return
	var sub := submit
	# Future.add_done_callback: runs at once when the job is done, else when it settles. The
	# callback holds the job weakly, so a start that never finishes leaves no cycle behind.
	var discard := func(job: Variant) -> void:
		if job == null or job.cancelled() or job.exception() != null:  # err-ok: a failed start has no session to discard
			return
		var started: Variant = job.result()  # err-ok: exception() is null, so this is the value
		if started is Dictionary and (started as Dictionary).has("id"):
			var sid: Variant = started["id"]
			sub.call(func(e: Variant) -> Variant: return await e.discard_session(sid))  # err-ok: the job is dropped; an ScfgErr fails it silently, as Python's fire-and-forget does
	if (fut as ScfgJob).done():
		discard.call(fut)
	else:
		var ref: WeakRef = weakref(fut)
		(fut as ScfgJob).finished.connect(func() -> void: discard.call(ref.get_ref()), CONNECT_ONE_SHOT)


func _discard_session() -> void:
	if ScfgPy.py_truthy(session) and (state == "waiting" or state == "starting"):
		var sid: Variant = session_id
		submit.call(func(e: Variant) -> Variant: return await e.discard_session(sid))  # err-ok: the job is dropped; an ScfgErr fails it silently, as Python's fire-and-forget does
	close()
	session = null
	hint_tx = null
	dropped_tx = null
	confirmations = 0


func _result(status: String) -> Variant:
	var made: Variant = ScfgModels.make_payment_result({"status": status, "kind": spec["kind"],  # err-ok: known fields only
		"item": spec["item"], "token": spec["token"], "chain": chain_key, "session_id": session_id})
	return made
