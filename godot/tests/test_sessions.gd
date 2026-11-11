## ScfgSessions (sessions.gd): the twin of tests/unit/test_sessions.py, plus M1's checks
## (rpgmaker/src/core/sessions.test.ts): reserved extra fields, the injected normalizer and the
## entropy draw order.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const TX := "0xABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB"


## create_session(store, args) with test_sessions.py's `make` defaults, `kw` overriding them.
func _make(store: Dictionary, kw: Dictionary = {}, entropy: Variant = null) -> Variant:
	var args := {"kind": "unlock", "item": "chapter2", "title": "Chapter 2", "token": "USDC",
		"chain": "arbitrum", "price": "4.99", "code": 37, "start_block": 1000, "now": 500.0, "nonce": 42}
	args.merge(kw, true)
	return ScfgSessions.create_session(store, args, entropy)


func test_constants() -> bool:
	T.eq([ScfgSessions.SESSION_WINDOW_S, ScfgSessions.SCAN_HORIZON_S, ScfgSessions.HINT_TIMEOUT_S], [1800, 86400, 1800],
		"windows")
	T.eq(ScfgSessions.UI_STATUSES, ["open", "pending", "cancelled", "expired"], "ui statuses")
	return T.done()


func test_create_session_record_shape() -> bool:
	var store := {}
	var rec: Variant = _make(store)
	T.ok(rec is Dictionary, "a record")
	if not rec is Dictionary:
		return T.done()
	T.ok(is_same(store[rec["id"]], rec), "the store holds the record itself")
	T.ok((rec["id"] as String).begins_with("ps_"), "id prefix")
	T.eq(rec["units"], 4_993_742, "units")
	T.eq(rec["price"], "4.99", "price")
	T.eq([rec["cursor"], rec["start_block"]], [1000, 1000], "cursor starts at start_block")
	T.eq([rec["state"], rec["ui_status"]], ["awaiting", "open"], "states")
	T.eq([rec["hint_tx"], rec["match"]], [null, null], "no hint, no match")
	T.eq((rec as Dictionary).keys(), ["id", "kind", "item", "title", "token", "chain", "price", "code", "nonce", "units",
		"start_block", "cursor", "created_at", "state", "ui_status", "hint_tx", "hint_at", "match"], "Python's field order")
	return T.done()


func test_record_contains_only_plain_types() -> bool:
	# persistent is a plain JSON record: a JSON round trip keeps it (numbers by value)
	var rec: Variant = _make({})
	T.json_eq(JSON.parse_string(JSON.stringify(rec)), rec, "JSON round trip")
	for k: String in rec:
		T.ok(typeof(rec[k]) in [TYPE_NIL, TYPE_STRING, TYPE_INT, TYPE_FLOAT], "%s is a plain value" % k)
	return T.done()


func test_random_nonce_and_unique_ids() -> bool:
	var store := {}
	var nonces := {}
	for i in 50:
		var rec: Variant = _make(store, {"nonce": null})
		T.ok(rec is Dictionary and rec["nonce"] is int and 0 <= rec["nonce"] and rec["nonce"] <= 99, "nonce in 0..99")
		if rec is Dictionary:
			nonces[rec["nonce"]] = true
	T.eq(store.size(), 50, "50 distinct ids")
	T.ok(nonces.size() > 1, "the nonce is random")
	return T.done()


func test_create_rejects_bad_kind_and_price() -> bool:
	T.is_err(_make({}, {"kind": "refund"}), "ValueError", "kind")
	T.is_err(_make({}, {"price": "4.999"}), "ValueError", "sub-cent price")
	T.is_err(_make({}, {"price": 4.99}), "TypeError", "a float price")
	var store := {}
	T.is_err(_make(store, {"kind": "refund"}), "ValueError", "nothing stored on error")
	T.eq(store, {}, "the store is untouched")
	return T.done()


func test_create_rejects_reserved_extra_fields() -> bool:
	var store := {}
	T.is_err(_make(store, {"extra": {"units": 1}}), "ValueError", "a reserved field")
	T.eq(store, {}, "nothing stored")
	var rec: Variant = _make(store, {"extra": {"reference": "abc"}})
	T.eq(rec["reference"], "abc", "an extra field is merged in")
	T.eq((rec as Dictionary).keys()[-1], "reference", "after the Python fields")
	T.is_err(_make({}, {"colour": "red"}), "TypeError", "an unexpected keyword argument")
	var missing := {"kind": "tip"}
	T.is_err(ScfgSessions.create_session({}, missing), "TypeError", "a missing keyword argument")
	return T.done()


func test_tip_session_encodes_code_zero() -> bool:
	var rec: Variant = _make({}, {"kind": "tip", "item": null, "code": 0, "price": "3", "nonce": 7})
	T.eq(rec["units"], 3_000_007, "units")
	T.eq(str(ScfgSessions.session_price(rec)), "3.00", "session_price")
	return T.done()


func test_get_remove_list() -> bool:
	var store := {}
	var a: Variant = _make(store, {"now": 10.0})
	var b: Variant = _make(store, {"now": 5.0})
	T.ok(is_same(ScfgSessions.get_session(store, a["id"]), a), "get_session")
	T.eq(ScfgSessions.get_session(store, "nope"), null, "an unknown id is None")
	var listed: Variant = ScfgSessions.list_sessions(store)
	T.eq((listed as Array).map(func(r: Dictionary) -> String: return r["id"]), [b["id"], a["id"]], "oldest first")
	ScfgSessions.remove_session(store, a["id"])
	ScfgSessions.remove_session(store, a["id"])  # idempotent
	T.eq(store.keys(), [b["id"]], "removed")
	return T.done()


func test_hints() -> bool:
	var store := {}
	var rec: Variant = _make(store)
	T.eq(ScfgSessions.set_hint(store, rec["id"], "  " + TX + " ", 600.0), null, "set_hint")
	T.eq([rec["hint_tx"], rec["hint_at"]], [TX.to_lower(), 600.0], "hint stored normalized")
	ScfgSessions.clear_hint(rec)
	T.eq([rec["hint_tx"], rec["hint_at"]], [null, null], "cleared")
	return T.done()


func test_bad_tx_hash() -> bool:
	for bad: Variant in ["", "0x1234", "0x" + "zz".repeat(32), "ab".repeat(32), null]:
		T.is_err(ScfgSessions.normalize_tx_hash(bad), "ValueError", "bad %s" % [bad])
	return T.done()


func test_set_hint_unknown_session() -> bool:
	T.is_err(ScfgSessions.set_hint({}, "ps_missing", TX, 0.0), "KeyError", "unknown session")
	return T.done()


func test_set_hint_uses_the_given_normalizer() -> bool:
	var store := {}
	var rec: Variant = _make(store)
	var upper := func(t: Variant) -> Variant: return "N:" + str(t)
	ScfgSessions.set_hint(store, rec["id"], "abc", 7.0, upper)
	T.eq(rec["hint_tx"], "N:abc", "the normalizer's result")
	var refuse := func(_t: Variant) -> Variant: return ScfgErr.make("ValueError", "no")
	T.is_err(ScfgSessions.set_hint(store, rec["id"], "x", 8.0, refuse), "ValueError", "its error")
	T.eq([rec["hint_tx"], rec["hint_at"]], ["N:abc", 7.0], "a refused hint changes nothing")
	return T.done()


func test_ui_status() -> bool:
	var store := {}
	var rec: Variant = _make(store)
	ScfgSessions.set_ui_status(store, rec["id"], "cancelled")
	T.eq(rec["ui_status"], "cancelled", "set")
	T.is_err(ScfgSessions.set_ui_status(store, rec["id"], "paid"), "ValueError", "not a ui status")
	T.is_err(ScfgSessions.set_ui_status(store, rec["id"], 5), "ValueError", "a non-str status")
	T.is_err(ScfgSessions.set_ui_status(store, "ps_gone", "open"), "KeyError", "an unknown session")
	return T.done()


func test_window_and_horizon() -> bool:
	var rec: Variant = _make({}, {"now": 0.0})
	T.eq(ScfgSessions.is_past_window(rec, ScfgSessions.SESSION_WINDOW_S - 1), false, "window - 1")
	T.eq(ScfgSessions.is_past_window(rec, ScfgSessions.SESSION_WINDOW_S), true, "window")
	T.eq(ScfgSessions.is_past_horizon(rec, ScfgSessions.SCAN_HORIZON_S - 1), false, "horizon - 1")
	T.eq(ScfgSessions.is_past_horizon(rec, ScfgSessions.SCAN_HORIZON_S), true, "horizon")
	T.is_err(ScfgSessions.is_past_window({}, 0), "KeyError", "no created_at")
	T.is_err(ScfgSessions.is_past_window({"created_at": "0"}, 0), "TypeError", "a str created_at")
	return T.done()


func test_create_session_draws_nonce_then_id() -> bool:
	var e := F.RecordingEntropy.new()
	var rec: Variant = _make({}, {"nonce": null}, e)
	T.eq(e.draws, ["randbelow(100)", "token_hex(8)"], "Python's draw order")
	T.eq(rec["id"], "ps_" + "ab".repeat(8), "the id from token_hex")
	T.eq([rec["nonce"], rec["units"]], [42, 4_993_742], "the nonce from randbelow")
	return T.done()


func test_given_nonce_draws_only_the_id() -> bool:
	var e := F.RecordingEntropy.new()
	_make({}, {"nonce": 7}, e)
	T.eq(e.draws, ["token_hex(8)"], "only the id")
	return T.done()


func test_new_session_id_format() -> bool:
	var sid: Variant = ScfgSessions.new_session_id()
	T.ok(sid is String and (sid as String).length() == 19 and (sid as String).begins_with("ps_"), "ps_ + 16 hex")
	T.ok(RegEx.create_from_string("^ps_[0-9a-f]{16}$").search(sid) != null, "lowercase hex")
	T.ok(ScfgSessions.new_session_id() != sid, "fresh each time")
	return T.done()


func test_default_entropy_ranges() -> bool:
	var e := ScfgSessions.OsEntropy.new()
	var seen := {}
	for i in 200:
		var n: Variant = e.randbelow(3)
		T.ok(n is int and n >= 0 and n < 3, "randbelow(3) in range")
		seen[n] = true
	T.eq(seen.size(), 3, "every value occurs")
	T.ok((e.token_bytes(32) as PackedByteArray).size() == 32, "token_bytes(32)")
	T.ok((e.token_hex(4) as String).length() == 8, "token_hex(4)")
	T.is_err(e.randbelow(0), "ValueError", "randbelow(0)")
	return T.done()


# tests/unit/test_sessions.py::test_session_price_is_read_as_a_stored_amount
func test_session_price_is_read_as_a_stored_amount() -> bool:
	T.eq(str(ScfgSessions.session_price({"price": "4.99"})), "4.99", "a price")
	for price: String in ["1E+999999", "1E+41", "Infinity"]:
		T.is_err(ScfgSessions.session_price({"price": price}), "ValueError", price)
		T.is_err(ScfgWatcher.min_amount({"kind": "unlock", "item": "gone", "price": price}, {}), "ValueError",
			"min_amount %s" % price)
	return T.done()
