## Pending payment sessions. Mirrors scfg_core/sessions.py.
##
## A session is one payment attempt, created when the player picks a network. It is stored as a
## plain JSON record (the frozen `pending` schema, conformance/schema/state.json) so it survives
## restarts and the watcher can keep looking for the payment for up to 24 hours. A store is a
## Dictionary {session id: record}.
##
## Randomness goes through an injected entropy object (Python's `secrets` module): any object with
## `token_hex(n)`, `randbelow(n)` and `token_bytes(n)`, each returning its value or an ScfgErr.
## null means OsEntropy (the OS random source). The draw order equals Python's: `randbelow(100)`
## for the nonce, then `token_hex(8)` for the id (the Solana backend adds `token_bytes(32)`).
##
## Record fields are read the way Python indexes them (ScfgPy.py_get): a missing field is a
## KeyError, and a number field that is not a number is a TypeError, never a coerced guess.
class_name ScfgSessions
extends RefCounted

const SESSION_WINDOW_S := 30 * 60
const SCAN_HORIZON_S := 24 * 3600
const HINT_TIMEOUT_S := 30 * 60
const UI_STATUSES: Array[String] = ["open", "pending", "cancelled", "expired"]
const _CREATE_REQUIRED: Array[String] = ["kind", "item", "title", "token", "chain", "price", "code", "start_block",
	"now"]
const _CREATE_OPTIONAL: Array[String] = ["nonce", "extra"]


## Python's `secrets` over the OS random source (Crypto.generate_random_bytes).
class OsEntropy:
	extends RefCounted
	var _crypto := Crypto.new()

	## `secrets.token_bytes(n)`. A negative n is a ValueError.
	## @fallible
	func token_bytes(n: int) -> Variant:
		if n < 0:
			return ScfgErr.make("ValueError", "negative count")
		if n == 0:
			return PackedByteArray()
		return _crypto.generate_random_bytes(n)

	## `secrets.token_hex(n)`: 2n lowercase hex characters.
	## @fallible
	func token_hex(n: int) -> Variant:
		var b: Variant = token_bytes(n)
		if b is ScfgErr:
			return b
		return ScfgBytes.bytes_to_hex(b)

	## `secrets.randbelow(n)`: uniform in [0, n), by rejection sampling over the bits n needs
	## (Python's _randbelow). n must be positive (a ValueError otherwise) and below 2^62.
	## @fallible
	func randbelow(n: int) -> Variant:
		if n <= 0:
			return ScfgErr.make("ValueError", "Upper bound must be positive.")
		if n >= 1 << 62:
			return ScfgErr.make("OverflowError", "randbelow bound beyond 2^62 is not supported by the port")
		var k := 0
		while (1 << k) < n:
			k += 1
		if k == 0:
			return 0
		var nbytes := (k + 7) / 8
		var x := n
		while x >= n:
			var b: PackedByteArray = _crypto.generate_random_bytes(nbytes)
			x = 0
			for byte in b:
				x = (x << 8) | byte
			x &= (1 << k) - 1
		return x


## `"ps_" + secrets.token_hex(8)`.
## @fallible
static func new_session_id(entropy: Variant = null) -> Variant:
	var e: Variant = entropy if entropy != null else OsEntropy.new()
	var h: Variant = e.token_hex(8)
	if h is ScfgErr:
		return h
	return "ps_" + str(h)


## Python `normalize_tx_hash`: `str.strip()`, then `^0x[0-9a-fA-F]{64}$`, then lowercase. A
## non-string or a malformed hash is a ValueError.
## @fallible
static func normalize_tx_hash(tx_hash: Variant) -> Variant:
	var bad := ScfgErr.make("ValueError", "not a transaction hash: %s" % ScfgPy.py_repr(tx_hash))
	if not (tx_hash is String or tx_hash is StringName):
		return bad
	var s := ScfgPy.py_strip(str(tx_hash))
	if s.length() != 66 or not s.begins_with("0x"):
		return bad
	for i in range(2, 66):
		var c := s.unicode_at(i)
		if not ((c >= 0x30 and c <= 0x39) or (c >= 0x61 and c <= 0x66) or (c >= 0x41 and c <= 0x46)):
			return bad
	return s.to_lower()


## Python `create_session(store, *, kind, item, title, token, chain, price, code, start_block,
## now, nonce=None, extra=None)`: the keyword arguments travel in `opts` (an unexpected or missing
## one is a TypeError). Stores and returns the new record, in Python's field order, with `extra`'s
## fields merged in after them (a reserved field name is a ValueError).
## @fallible
static func create_session(store: Dictionary, opts: Dictionary, entropy: Variant = null) -> Variant:
	for k: Variant in opts:
		if not (k is String and (k in _CREATE_REQUIRED or k in _CREATE_OPTIONAL)):
			return ScfgErr.make("TypeError", "create_session() got an unexpected keyword argument %s" % ScfgPy.py_repr(k))
	for k: String in _CREATE_REQUIRED:
		if not opts.has(k):
			return ScfgErr.make("TypeError", "create_session() missing 1 required keyword-only argument: '%s'" % k)
	var e: Variant = entropy if entropy != null else OsEntropy.new()
	var kind: Variant = opts["kind"]
	if not ScfgConfig.in_strs(["unlock", "tip"], kind):
		return ScfgErr.make("ValueError", "invalid kind %s" % ScfgPy.py_repr(kind))
	var nonce: Variant = opts.get("nonce")
	if nonce == null:
		nonce = e.randbelow(100)
		if nonce is ScfgErr:
			return nonce
	var p: Variant = ScfgAmounts.parse_amount(opts["price"])
	if p is ScfgErr:
		return p
	var sid: Variant = new_session_id(e)
	if sid is ScfgErr:
		return sid
	var units: Variant = ScfgAmounts.encode(p, opts["code"], nonce)
	if units is ScfgErr:
		return units
	var start_block: Variant = opts["start_block"]
	var rec := {
		"id": sid, "kind": kind, "item": opts["item"], "title": opts["title"], "token": opts["token"],
		"chain": opts["chain"], "price": str(p), "code": opts["code"], "nonce": nonce, "units": units,
		"start_block": start_block, "cursor": start_block, "created_at": opts["now"],
		"state": "awaiting", "ui_status": "open",
		"hint_tx": null, "hint_at": null, "match": null,
	}
	var extra: Variant = opts.get("extra")
	if ScfgPy.py_truthy(extra):
		if not extra is Dictionary:
			return ScfgErr.make("TypeError", "extra must be a dict, not %s" % ScfgPy.py_type(extra))
		for key: Variant in extra:
			if rec.has(key):
				return ScfgErr.make("ValueError", "session field %s is reserved" % ScfgPy.py_repr(key))
		rec.merge(extra)
	store[sid] = rec
	return rec


## `store.get(sid)`: the record or null.
static func get_session(store: Dictionary, sid: Variant) -> Variant:
	return store.get(sid)


## `store.pop(sid, None)`.
static func remove_session(store: Dictionary, sid: Variant) -> void:
	store.erase(sid)


## The records, oldest first (a stable sort by created_at, like Python's `sorted`). A record
## without created_at is a KeyError, a non-number one a TypeError.
## @fallible
static func list_sessions(store: Dictionary) -> Variant:
	var keyed := []
	var i := 0
	for rec: Variant in store.values():
		var t: Variant = number_field(rec, "created_at")
		if t is ScfgErr:
			return t
		keyed.append([t, i, rec])
		i += 1
	keyed.sort_custom(func(a: Array, b: Array) -> bool: return a[0] < b[0] or (a[0] == b[0] and a[1] < b[1]))
	return keyed.map(func(k: Array) -> Variant: return k[2])


## Record the reported transaction id: `normalize(tx_hash)` (normalize_tx_hash by default; a
## Callable returning the id or an ScfgErr) and the time. An unknown session is a KeyError; a
## refused id changes nothing.
## @fallible
static func set_hint(store: Dictionary, sid: Variant, tx_hash: Variant, now: Variant,
		normalize: Callable = Callable()) -> Variant:
	var rec: Variant = ScfgPy.py_get(store, sid)
	if rec is ScfgErr:
		return rec
	var h: Variant
	if normalize.is_valid():
		h = normalize.call(tx_hash)
	else:
		h = normalize_tx_hash(tx_hash)
	if h is ScfgErr:
		return h
	if not rec is Dictionary:
		return ScfgErr.make("TypeError", "'%s' object does not support item assignment" % ScfgPy.py_type(rec))
	rec["hint_tx"] = h
	rec["hint_at"] = now
	return null


static func clear_hint(rec: Dictionary) -> void:
	rec["hint_tx"] = null
	rec["hint_at"] = null


## A status other than "open", "pending", "cancelled", "expired" is a ValueError; an unknown
## session a KeyError.
## @fallible
static func set_ui_status(store: Dictionary, sid: Variant, status: Variant) -> Variant:
	if not ScfgConfig.in_strs(UI_STATUSES, status):
		return ScfgErr.make("ValueError", "invalid ui status %s" % ScfgPy.py_repr(status))
	var rec: Variant = ScfgPy.py_get(store, sid)
	if rec is ScfgErr:
		return rec
	if not rec is Dictionary:
		return ScfgErr.make("TypeError", "'%s' object does not support item assignment" % ScfgPy.py_type(rec))
	rec["ui_status"] = status
	return null


## `rec[key]` used as a number in Python's arithmetic and comparisons: an int or float (a bool
## is an int). A missing key is a KeyError, None or a str a TypeError. Port limit: an int beyond
## 64 bits (an ScfgBigInt) is an OverflowError.
## @fallible
static func number_field(rec: Variant, key: String) -> Variant:
	var v: Variant = ScfgPy.py_get(rec, key)
	if v is ScfgErr:
		return v
	if v is bool:
		return 1 if v else 0
	if v is int or v is float:
		return v
	if v is ScfgBigInt:
		return ScfgErr.make("OverflowError", "%s %s is beyond the port's 64-bit ints" % [key, (v as ScfgBigInt).to_decimal()])
	return ScfgErr.make("TypeError", "unsupported operand type(s) for -: 'float' and '%s' (%s)" % [ScfgPy.py_type(v), key])


## @fallible
static func is_past_window(rec: Variant, now: Variant) -> Variant:
	var t: Variant = number_field(rec, "created_at")
	if t is ScfgErr:
		return t
	return now - t >= SESSION_WINDOW_S


## @fallible
static func is_past_horizon(rec: Variant, now: Variant) -> Variant:
	var t: Variant = number_field(rec, "created_at")
	if t is ScfgErr:
		return t
	return now - t >= SCAN_HORIZON_S


## `Decimal(rec["price"])`. Port limit: a stored NaN or Infinity (the core never stores one) is
## a ValueError, as in the TypeScript core.
## @fallible
static func session_price(rec: Variant) -> Variant:
	var price: Variant = ScfgPy.py_get(rec, "price")
	if price is ScfgErr:
		return price
	var d: Variant = ScfgDec.from_value(price)
	if d is ScfgErr:
		return d
	if not d is ScfgDec:
		return ScfgErr.make("ValueError", "not a finite decimal: %s" % ScfgPy.py_repr(price))
	return d
