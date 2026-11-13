## ScfgWatcher (watcher.gd): the twin of tests/unit/test_watcher_awaiting.py and
## test_watcher_confirming.py, plus the JS core's checks (rpgmaker/src/core/watcher.test.ts):
## Python's lookups and tie-breaks, the 2^53 limit on stored values, and errors as values.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const TX := "0xabababababababababababababababababababababababababababababababab"
const UNITS := 4_993_742

static var CHAIN: Dictionary = ScfgChains.MAINNET["arbitrum"]  # 20 confirmations
static var USDC: String = CHAIN["tokens"]["USDC"]["address"]
static var USDT: String = CHAIN["tokens"]["USDT"]["address"]
static var ITEMS := {"chapter2": ScfgModels.make_item({"item_id": "chapter2", "code": 37,  # err-ok: valid fields
	"price": ScfgDec.parse("4.99"), "token": "USDC", "title": "Chapter 2", "min_price": ScfgDec.parse("4.99")})}


## [pending, receipts, chain, rec] (test_watcher_awaiting.py `setup`).
func _setup(head: int = 1000, kw: Dictionary = {}) -> Array:
	var pending := {}
	var args := {"kind": "unlock", "item": "chapter2", "title": "Chapter 2", "token": "USDC", "chain": "arbitrum",
		"price": "4.99", "code": 37, "nonce": 42, "start_block": 995, "now": 0.0}
	args.merge(kw, true)
	var rec: Variant = ScfgSessions.create_session(pending, args)
	return [pending, {}, F.FakeChain.new(head), rec]


func _step(s: Array, now: float = 10.0, chain: Dictionary = CHAIN) -> Variant:
	return await ScfgWatcher.advance(s[0], s[1], s[3]["id"], {"rpc": s[2], "chain": chain, "wallet": DEV,
		"items": ITEMS, "now": now})


func _log(token: String, value: Variant, block: int, opts: Dictionary = {}) -> Dictionary:
	var o := {"block": block, "tx_hash": TX}
	o.merge(opts, true)
	return F.transfer_log(token, PAYER, DEV, value, o)


func _receipt_with(logs: Array, status: String = "0x1") -> Dictionary:
	return {"status": status, "logs": logs}


func _hinted(s: Array, receipt: Variant) -> void:
	ScfgSessions.set_hint(s[0], s[3]["id"], TX, 0.0)
	(s[2] as F.FakeChain).receipts[TX] = receipt


func test_missing_session() -> bool:
	var out: Variant = await ScfgWatcher.advance({}, {}, "ps_nope", {"rpc": F.FakeChain.new(), "chain": CHAIN,
		"wallet": DEV, "items": ITEMS, "now": 0})
	T.eq(out, {"session_id": "ps_nope", "status": "missing", "confirmations": 0, "required": 20, "result": null,
		"error": null, "hint_tx": null}, "every StepOutcome field, in Python's order")
	T.eq((out as Dictionary).keys(), ScfgWatcher.STEP_OUTCOME_FIELDS, "field order")
	return T.done()


# --- amount scan ------------------------------------------------------------------------------


func test_scan_without_payment_advances_cursor() -> bool:
	var s := _setup()
	var out: Variant = await _step(s)
	T.eq([out["status"], out["error"]], ["waiting", null], "waiting")
	T.eq(s[3]["cursor"], 1001, "cursor")
	var chain: F.FakeChain = s[2]
	T.eq(chain.last_get_logs[0], USDC, "address")
	T.eq(chain.last_get_logs[1], [ScfgEvm.TRANSFER_TOPIC, null, ScfgEvm.address_topic(DEV)], "topics")
	T.eq(chain.last_get_logs.slice(2), [995, 1000], "range")
	return T.done()


func test_scan_does_nothing_when_cursor_ahead_of_head() -> bool:
	var s := _setup(900)  # lagging endpoint
	var out: Variant = await _step(s)
	T.eq(out["status"], "waiting", "waiting")
	T.eq(s[3]["cursor"], 995, "cursor kept")
	T.ok(not "get_logs" in (s[2] as F.FakeChain).calls, "no getLogs")
	return T.done()


func test_scan_is_bounded_per_step() -> bool:
	var s := _setup(995 + 3 * ScfgWatcher.SCAN_STEP_BLOCKS)
	await _step(s)
	T.eq(s[3]["cursor"], 995 + ScfgWatcher.SCAN_STEP_BLOCKS, "one step of blocks")
	return T.done()


func test_scan_matches_exact_amount_and_starts_confirming() -> bool:
	var s := _setup(1000)
	(s[2] as F.FakeChain).logs.append(_log(USDC, UNITS, 998, {"log_index": 2}))
	var out: Variant = await _step(s)
	T.eq(out["status"], "confirming", "confirming")
	T.eq([out["confirmations"], out["required"]], [3, 20], "confirmations")
	T.eq(s[3]["state"], "confirming", "state")
	T.eq(s[3]["match"], {"tx_hash": TX, "log_index": 2, "block": 998, "block_hash": "0x" + ("%x" % 998).lpad(64, "0"),
		"value": UNITS, "payer": PAYER, "recipient": DEV}, "match")
	T.eq((s[3]["match"] as Dictionary).keys(), ["tx_hash", "log_index", "block", "block_hash", "value", "payer",
		"recipient"], "match field order")
	return T.done()


func test_scan_ignores_wrong_amount_token_recipient_and_old_blocks() -> bool:
	var s := _setup(1000)
	var other := "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"
	(s[2] as F.FakeChain).logs.append_array([
		_log(USDC, UNITS + 1, 998),  # amount off by 1
		_log(USDT, UNITS, 998),  # other token
		F.transfer_log(USDC, PAYER, other, UNITS, {"block": 998, "tx_hash": TX}),  # other recipient
		_log(USDC, UNITS, 990),  # before the session
	])
	T.eq((await _step(s))["status"], "waiting", "waiting")
	T.eq(s[3]["match"], null, "no match")
	return T.done()


func test_scan_picks_earliest_match() -> bool:
	var s := _setup(1000)
	(s[2] as F.FakeChain).logs.append_array([
		_log(USDC, UNITS, 999, {"tx_hash": "0x" + "02".repeat(32)}),
		_log(USDC, UNITS, 997, {"tx_hash": "0x" + "01".repeat(32)}),
	])
	await _step(s)
	T.eq(s[3]["match"]["tx_hash"], "0x" + "01".repeat(32), "earliest block")
	return T.done()


func test_scan_breaks_block_ties_by_log_index() -> bool:
	var s := _setup(1000)
	(s[2] as F.FakeChain).logs.append_array([
		_log(USDC, UNITS, 998, {"tx_hash": "0x" + "02".repeat(32), "log_index": 7}),
		_log(USDC, UNITS, 998, {"tx_hash": "0x" + "01".repeat(32), "log_index": 3}),
	])
	await _step(s)
	T.eq(s[3]["match"]["log_index"], 3, "lowest log index in the block")
	return T.done()


func test_scan_skips_transfer_already_used_as_receipt() -> bool:
	var s := _setup(1000)
	(s[2] as F.FakeChain).logs.append(_log(USDC, UNITS, 998, {"log_index": 2}))
	s[1][ScfgReceipts.receipt_key("arbitrum", TX, 2)] = {"kind": "unlock"}
	T.eq((await _step(s))["status"], "waiting", "waiting")
	return T.done()


func test_horizon_closes_session() -> bool:
	var s := _setup()
	var out: Variant = await _step(s, ScfgSessions.SCAN_HORIZON_S)
	T.eq([out["status"], out["result"]], ["closed", null], "closed")
	T.eq(s[0], {}, "removed")
	return T.done()


func test_network_outage_is_reported_and_state_kept() -> bool:
	var s := _setup()
	(s[2] as F.FakeChain).down = true
	var out: Variant = await _step(s)
	T.eq(out["error"], "network_unavailable", "error")
	T.eq([s[3]["cursor"], s[3]["state"]], [995, "awaiting"], "state kept")
	return T.done()


# --- hints (browser page / pasted hash) -------------------------------------------------------


func test_hint_not_mined_yet_keeps_waiting_without_scanning() -> bool:
	var s := _setup()
	ScfgSessions.set_hint(s[0], s[3]["id"], TX, 5.0)
	var out: Variant = await _step(s, 10.0)
	T.eq([out["status"], out["error"]], ["waiting", null], "waiting")
	T.eq(s[3]["hint_tx"], TX, "hint kept")
	T.ok(not "get_logs" in (s[2] as F.FakeChain).calls, "no scan")
	return T.done()


func test_hint_dropped_after_timeout_then_scan_resumes() -> bool:
	var s := _setup()
	ScfgSessions.set_hint(s[0], s[3]["id"], TX, 0.0)
	await _step(s, ScfgSessions.HINT_TIMEOUT_S)
	T.eq(s[3]["hint_tx"], null, "hint dropped")
	T.ok("get_logs" in (s[2] as F.FakeChain).calls, "scanned")
	return T.done()


func test_hint_reverted() -> bool:
	var s := _setup()
	_hinted(s, _receipt_with([], "0x0"))
	var out: Variant = await _step(s)
	T.eq([out["error"], s[3]["hint_tx"]], ["tx_reverted", null], "reverted")
	T.eq(out["hint_tx"], TX, "names the hint judged")
	return T.done()


func test_hint_not_a_payment() -> bool:
	var s := _setup()
	_hinted(s, _receipt_with([_log(USDT, UNITS, 998)]))
	var out: Variant = await _step(s)
	T.eq([out["error"], s[3]["hint_tx"]], ["tx_not_a_payment", null], "not a payment")
	T.eq(out["hint_tx"], TX, "names the hint judged")
	return T.done()


func test_hint_from_before_session_is_rejected() -> bool:
	var s := _setup()
	_hinted(s, _receipt_with([_log(USDC, UNITS, 900)]))
	T.eq((await _step(s))["error"], "tx_not_a_payment", "before start_block")
	return T.done()


func test_hint_receipt_without_logs_is_not_a_payment() -> bool:
	# Python: receipt.get("logs") or []: a missing or null "logs" is an empty list
	for receipt: Dictionary in [{"status": "0x1"}, {"status": "0x1", "logs": null}, {}]:
		var s := _setup()
		_hinted(s, receipt)
		T.eq((await _step(s))["error"], "tx_not_a_payment", "receipt %s" % [receipt])
	return T.done()


func test_hint_malformed_receipt_raises() -> bool:
	# Python: a non-dict receipt has no .get (AttributeError); "abc" logs iterate characters,
	# which are not dicts either. The Engine contains these.
	for receipt: Variant in ["0x1", [1], 5, true]:
		var s := _setup()
		_hinted(s, receipt)
		T.is_err(await _step(s), "AttributeError", "receipt %s" % [receipt])
	var s2 := _setup()
	_hinted(s2, {"status": "0x1", "logs": "abc"})
	T.is_err(await _step(s2), "AttributeError", "str logs")
	var s3 := _setup()
	_hinted(s3, {"status": "0x1", "logs": 5})
	T.is_err(await _step(s3), "TypeError", "int logs are not iterable")
	return T.done()


func test_hint_accepts_any_amount_at_or_above_min_price() -> bool:
	# exchange payer: could not send the encoded decimals, sent a round 5.00
	var s := _setup(1000)
	_hinted(s, _receipt_with([_log(USDC, 5_000_000, 999)]))
	var out: Variant = await _step(s)
	T.eq(out["status"], "confirming", "confirming")
	T.eq(s[3]["match"]["value"], 5_000_000, "value")
	return T.done()


func test_hint_underpaid_unlock_is_final() -> bool:
	var s := _setup()
	_hinted(s, _receipt_with([_log(USDC, 4_000_000, 999)]))
	var out: Variant = await _step(s)
	T.eq(out["status"], "underpaid", "underpaid")
	var r: Dictionary = out["result"]
	T.eq(r["status"], "underpaid", "result status")
	T.eq(str(r["amount"]), "4.000000", "amount")
	T.eq([r["tx_hash"], r["payer"]], [TX, PAYER], "tx and payer")
	T.eq([s[0], s[1]], [{}, {}], "session gone, no receipt")
	return T.done()


func test_hint_tip_accepts_any_positive_amount() -> bool:
	var s := _setup(1000, {"kind": "tip", "item": null, "code": 0, "price": "3.00"})
	_hinted(s, _receipt_with([_log(USDC, 1_000_000, 999)]))
	T.eq((await _step(s))["status"], "confirming", "confirming")
	return T.done()


func test_hint_with_several_transfers_takes_largest_qualifying() -> bool:
	var s := _setup(1000)
	_hinted(s, _receipt_with([_log(USDC, 1_000_000, 999, {"log_index": 0}), _log(USDC, UNITS, 999, {"log_index": 1})]))
	await _step(s)
	T.eq(s[3]["match"]["log_index"], 1, "the largest")
	return T.done()


func test_hint_largest_tie_keeps_the_first() -> bool:
	# Python max() returns the first maximal element
	var s := _setup(1000)
	_hinted(s, _receipt_with([_log(USDC, UNITS, 999, {"log_index": 4}), _log(USDC, UNITS, 999, {"log_index": 1})]))
	await _step(s)
	T.eq(s[3]["match"]["log_index"], 4, "the first of the largest")
	return T.done()


func test_scan_step_is_bounded_by_calls_when_rpc_reports_its_range() -> bool:
	var s := _setup(995 + 100_000)
	(s[2] as F.FakeChain).log_range = 50  # a stingy public endpoint
	await _step(s)
	T.eq(s[3]["cursor"], 995 + 50 * ScfgWatcher.MAX_SCAN_CALLS, "bounded by calls")
	return T.done()


func test_min_amount() -> bool:
	T.eq(str(ScfgWatcher.min_amount({"kind": "unlock", "item": "chapter2", "price": "9.99"}, ITEMS)), "4.99", "the item's")
	T.eq(str(ScfgWatcher.min_amount({"kind": "unlock", "item": "gone", "price": "9.99"}, ITEMS)), "9.99", "the price")
	T.eq(str(ScfgWatcher.min_amount({"kind": "tip", "item": null, "price": "3.00"}, ITEMS)), "0.000001", "a tip")
	T.is_err(ScfgWatcher.min_amount({"kind": "unlock", "item": ["x"], "price": "1"}, ITEMS), "TypeError", "unhashable")
	T.is_err(ScfgWatcher.min_amount({"item": "chapter2"}, ITEMS), "KeyError", "no kind")
	return T.done()


func test_result_from_match() -> bool:
	var rec := {"id": "ps_1", "kind": "unlock", "item": "chapter2", "token": "USDC"}
	var m := {"tx_hash": TX, "payer": PAYER, "value": UNITS}
	var r: Variant = ScfgWatcher.result_from_match(rec, "paid", "arbitrum", m)
	T.eq(str(r["amount"]), "4.993742", "amount")
	r["amount"] = str(r["amount"])
	T.eq(r, {"status": "paid", "kind": "unlock", "item": "chapter2", "amount": "4.993742", "token": "USDC",
		"chain": "arbitrum", "tx_hash": TX, "payer": PAYER, "session_id": "ps_1", "error": null}, "PaymentResult")
	T.is_err(ScfgWatcher.result_from_match(rec, "paid", "arbitrum", {"tx_hash": TX}), "KeyError", "no value")
	return T.done()


func test_session_missing_a_field_raises_key_error() -> bool:
	# Python indexes rec["state"]: a record without it is a KeyError (the Engine contains it)
	var s := _setup()
	(s[3] as Dictionary).erase("state")
	T.is_err(await _step(s), "KeyError", "no state")
	T.eq((s[2] as F.FakeChain).calls, [], "no request")
	return T.done()


func test_scan_reads_units_only_for_a_qualifying_log() -> bool:
	# Python evaluates rec["units"] after _qualifies
	var no_logs := _setup()
	(no_logs[3] as Dictionary).erase("units")
	T.eq((await _step(no_logs))["status"], "waiting", "no logs")
	var wrong := _setup()
	(wrong[3] as Dictionary).erase("units")
	(wrong[2] as F.FakeChain).logs.append(_log(USDT, UNITS, 999))
	T.eq((await _step(wrong))["status"], "waiting", "a log of another token")
	var q := _setup()
	(q[3] as Dictionary).erase("units")
	(q[2] as F.FakeChain).logs.append(_log(USDC, UNITS, 999))
	T.is_err(await _step(q), "KeyError", "a qualifying log")
	return T.done()


func test_awaits_a_chain_that_yields_frames() -> bool:
	var s := _setup(1000)
	(s[2] as F.FakeChain).frames = 2
	(s[2] as F.FakeChain).logs.append(_log(USDC, UNITS, 998, {"log_index": 2}))
	var out: Variant = await _step(s)
	T.eq([out["status"], out["confirmations"]], ["confirming", 3], "a suspending chain gives the same outcome")
	return T.done()


func test_hinted_value_at_2_pow_53_is_refused_not_rounded() -> bool:
	# match.value is a JSON number, so 2^53 units cannot be stored exactly. The watcher
	# returns a ValueError (the Engine contains it) and leaves the session untouched.
	var s := _setup(1000)
	_hinted(s, _receipt_with([_log(USDC, ScfgBigInt.from_decimal("9007199254740992"), 999)]))
	T.is_err(await _step(s), "ValueError", "2^53")
	T.eq([s[3]["state"], s[3]["match"], s[3]["hint_tx"]], ["awaiting", null, TX], "untouched")
	var ok := _setup(1000)
	_hinted(ok, _receipt_with([_log(USDC, ScfgBigInt.from_decimal("9007199254740991"), 999)]))
	T.eq((await _step(ok))["status"], "confirming", "2^53 - 1 is stored")
	T.eq(ok[3]["match"]["value"], 9007199254740991, "as an int")
	var huge := _setup(1000)
	_hinted(huge, _receipt_with([_log(USDC, ScfgBigInt.from_decimal("123456789012345678901234567890"), 999)]))
	T.is_err(await _step(huge), "ValueError", "beyond 2^64")
	return T.done()


# --- confirming (test_watcher_confirming.py) ----------------------------------------------------


func _matched(head: int = 1000, block: int = 998) -> Array:
	var s := _setup(head)
	(s[2] as F.FakeChain).logs.append(_log(USDC, UNITS, block, {"log_index": 2}))
	var out: Variant = await _step(s)
	T.eq(out["status"], "confirming", "matched")
	return s


func test_counts_confirmations_until_required() -> bool:
	var s: Array = await _matched()
	(s[2] as F.FakeChain).head = 1010
	var out: Variant = await _step(s)
	T.eq([out["status"], out["confirmations"], out["required"]], ["confirming", 13, 20], "13 of 20")
	return T.done()


func test_paid_writes_receipt_and_removes_session() -> bool:
	var s: Array = await _matched()
	(s[2] as F.FakeChain).head = 998 + 19  # exactly 20 confirmations
	var out: Variant = await _step(s)
	T.eq(out["status"], "paid", "paid")
	var r: Dictionary = out["result"]
	T.eq([r["status"], r["kind"], r["item"], r["token"], r["chain"]], ["paid", "unlock", "chapter2", "USDC", "arbitrum"],
		"result")
	T.eq([str(r["amount"]), r["tx_hash"], r["payer"]], ["4.993742", TX, PAYER], "amount, tx, payer")
	T.eq(r["session_id"], s[3]["id"], "session id")
	T.eq(s[0], {}, "session removed")
	var stored: Dictionary = s[1][ScfgReceipts.receipt_key("arbitrum", TX, 2)]
	T.eq([stored["source"], stored["block"], stored["amount"]], ["payment", 998, "4.993742"], "receipt")
	return T.done()


func test_confirmations_override_is_respected() -> bool:
	var s: Array = await _matched()
	var fast := CHAIN.duplicate()
	fast["confirmations"] = 3
	var out: Variant = await _step(s, 1, fast)
	T.eq(out["status"], "paid", "paid at 3")
	return T.done()


func test_reorg_returns_to_awaiting_and_rewinds_cursor() -> bool:
	var s: Array = await _matched()
	var chain: F.FakeChain = s[2]
	chain.hashes[998] = "0x" + "ee".repeat(32)  # block 998 replaced
	chain.logs.clear()  # the transfer is no longer in the canonical chain
	chain.head = 1005
	var out: Variant = await _step(s)
	T.eq(out["status"], "reorged", "reorged")
	T.eq([s[3]["state"], s[3]["match"], s[3]["cursor"]], ["awaiting", null, 998], "rewound")
	return T.done()


func test_reorg_then_transfer_reappears_in_new_block() -> bool:
	var s: Array = await _matched()
	var chain: F.FakeChain = s[2]
	chain.hashes[998] = "0x" + "ee".repeat(32)
	chain.logs = [_log(USDC, UNITS, 1001, {"log_index": 0})]
	chain.head = 1002
	T.eq((await _step(s))["status"], "reorged", "reorged")
	T.eq((await _step(s))["status"], "confirming", "found again")
	T.eq(s[3]["match"]["block"], 1001, "in the new block")
	return T.done()


func test_block_hash_gone_is_a_reorg() -> bool:
	var s: Array = await _matched()
	var chain: F.FakeChain = s[2]
	chain.hashes[998] = null  # the node no longer knows the block's hash: None
	T.eq((await _step(s))["status"], "reorged", "a missing block is a reorg")
	return T.done()


func test_lagging_endpoint_below_match_block_is_not_a_reorg() -> bool:
	var s: Array = await _matched()
	(s[2] as F.FakeChain).head = 990  # another endpoint, behind
	var out: Variant = await _step(s)
	T.eq([out["status"], out["confirmations"]], ["confirming", 0], "not a reorg")
	T.eq(s[3]["state"], "confirming", "state kept")
	return T.done()


func test_outage_while_confirming_keeps_match() -> bool:
	var s: Array = await _matched()
	(s[2] as F.FakeChain).down = true
	var out: Variant = await _step(s)
	T.eq(out["error"], "network_unavailable", "error")
	T.ok(s[3]["state"] == "confirming" and s[3]["match"] != null, "match kept")
	return T.done()


func test_cancelled_or_expired_ui_status_does_not_stop_confirmation() -> bool:
	var s: Array = await _matched()
	s[3]["ui_status"] = "cancelled"
	(s[2] as F.FakeChain).head = 1100
	T.eq((await _step(s, 4000.0))["status"], "paid", "paid")
	return T.done()
