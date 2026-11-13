## ScfgSolanaBackend watching (advance, transfer_to): tests/unit/test_solana_watch.py, plus the
## JS core's edges (rpgmaker/src/core/backends/solana.test.ts): the payer rule, net gains and
## malformed balance rows, a dropped hint's verdict cleared by a reference payment, a pending
## hint suspending the scan, an expired hint's outage, the node's confirmations, a missing and a
## malformed session.
##
## Python monkeypatches the module constants SIG_PAGE and MAX_SIG_PAGES; here a backend instance
## carries its own (`sig_page`, `max_sig_pages`), and FakeSolanaChain.patched stands in for
## monkeypatching a method.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const MINT := "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
const DEV := "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
const OLD := "4SELZ24sTinbBYo27VvsUAHepTRXGVBghc8WTUpvx6Pn"
const PAYER := "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe"
const REF := "3RnSVDFx2gbsEz5MGXRb8GaEiug7tvzAQzMN18SHVJpw"
const DEST := "OWNERATA"
const UNITS := 4_993_742

static var CHAIN: Dictionary = _chain()
static var ITEMS := {"chapter2": ScfgModels.make_item({"item_id": "chapter2", "code": 37,  # err-ok: valid fields
	"price": ScfgDec.parse("4.99"), "token": "USDC", "title": "Chapter 2", "min_price": ScfgDec.parse("4.99")})}
static var B := ScfgSolanaBackend.new()


static func _chain() -> Dictionary:
	var c: Dictionary = (ScfgChains.MAINNET["solana"] as Dictionary).duplicate()
	c["rpc"] = ["https://x.example"]
	c["confirmations"] = ScfgSolanaBackend.FINAL_CONFIRMATIONS
	c["max_log_range"] = 0
	c["explorer_tx"] = "https://solscan.io/tx/{}"
	c["tokens"] = {"USDC": ScfgChains.make_token(MINT)}
	return c


## [pending, receipts, chain, rec] (test_solana_watch.py `setup`).
func _setup(kw: Dictionary = {}) -> Array:
	var pending := {}
	var opts := {"kind": "unlock", "item": "chapter2", "title": "Chapter 2", "token": "USDC", "chain": "solana",
		"price": "4.99", "code": 37, "nonce": 42, "start_block": 900, "now": 0.0,
		"extra": {"reference": REF, "dest": DEST, "scan_cursor": null}}
	opts.merge(kw, true)
	var rec: Variant = ScfgSessions.create_session(pending, opts)
	return [pending, {}, F.FakeSolanaChain.new(1000), rec]


func _step(s: Array, now: float = 1.0, b: ScfgSolanaBackend = B) -> Variant:
	return await b.advance(s[0], s[1], s[3]["id"], {"rpc": s[2], "chain": CHAIN, "wallet": DEV, "items": ITEMS,
		"now": now})


func _rec(s: Array) -> Dictionary:
	return s[0][s[3]["id"]]


func _pay(chain: F.FakeSolanaChain, n: int, kw: Dictionary = {}) -> Dictionary:
	var opts := {"mint": MINT, "payer": PAYER, "owner": DEV, "amount": UNITS}
	opts.merge(kw, true)
	return chain.pay(F.sig(n), opts)


func _hint(s: Array, tx: String, at: float = 0.0) -> void:
	ScfgSessions.set_hint(s[0], s[3]["id"], tx, at, B.normalize_tx)


func _count(calls: Array[String], name: String) -> int:
	return calls.count(name)


func _small_pages() -> ScfgSolanaBackend:
	var b := ScfgSolanaBackend.new()
	b.sig_page = 10
	b.max_sig_pages = 3
	return b


func test_registered_and_normalizes_signatures() -> bool:
	var reg: Variant = ScfgBackends.backend_for_family("solana")
	T.ok(reg is ScfgSolanaBackend, "registered")
	T.eq(reg.family if reg is ScfgSolanaBackend else reg, "solana", "its family")
	T.ok(ScfgBackends.backend_for_family("solana") == reg, "one instance per family")
	var s := F.sig(5)
	T.eq(B.normalize_tx(" " + s), s, "stripped")
	T.eq(s, ScfgSolanaBase58.normalize_signature(s), "normalize_signature")
	T.is_err(B.normalize_tx("0x" + "ab".repeat(32)), "ValueError", "an EVM hash")
	T.eq(ScfgBackends.normalize_any_tx(" " + s), s, "normalize_any_tx reaches it")
	return T.done()


func test_token_accepts_base58_mints_and_keeps_evm_checksums() -> bool:
	T.eq(ScfgChains.make_token(MINT)["address"], MINT, "a mint")
	var arb: String = ScfgChains.MAINNET["arbitrum"]["tokens"]["USDC"]["address"]
	T.eq(ScfgChains.make_token(arb.to_lower())["address"], arb, "an EVM checksum")
	T.is_err(ScfgChains.make_token("not-an-address"), "ValueError", "neither")
	return T.done()


func test_transfer_to_reads_balance_changes() -> bool:
	var chain := F.FakeSolanaChain.new()
	var tx := _pay(chain, 1, {"slot": 950})
	var t: Variant = ScfgSolanaBackend.transfer_to(tx, {"sig": F.sig(1), "mint": MINT, "owner": DEV})
	T.ok(t is Dictionary, "a Transfer")
	if t is Dictionary:
		T.eq(t.keys(), ScfgTransfers.TRANSFER_FIELDS, "Transfer's fields")
		T.ok(ScfgPy.py_eq(t["value"], UNITS), "value")
		T.eq([t["sender"], t["recipient"], t["log_index"], t["block_number"], t["token"], t["tx_hash"]],
			[PAYER, DEV, 2, 950, MINT, F.sig(1)], "fields")
	T.eq(ScfgSolanaBackend.transfer_to(tx, {"sig": F.sig(1), "mint": MINT, "owner": PAYER}), null, "the payer gained nothing")
	T.eq(ScfgSolanaBackend.transfer_to(tx, {"sig": F.sig(1), "mint": "OTHERMINT", "owner": DEV}), null, "another mint")
	var failed := _pay(chain, 2, {"err": {"InstructionError": [0, "Custom"]}})
	T.eq(ScfgSolanaBackend.transfer_to(failed, {"sig": F.sig(2), "mint": MINT, "owner": DEV}), null, "a failed transaction")
	return T.done()


func _row(i: Variant, who: Variant, v: Variant) -> Dictionary:
	return {"accountIndex": i, "mint": MINT, "owner": who, "uiTokenAmount": {"amount": v}}


func test_transfer_to_payer_is_the_largest_drop_else_the_fee_payer() -> bool:
	var tx := {
		"slot": "77",
		"transaction": {"message": {"accountKeys": [{"pubkey": OLD}, "x"]}},
		"meta": {"err": null,
			"preTokenBalances": [_row(3, OLD, "10"), _row(1, PAYER, "10"), _row(4, REF, "10")],
			"postTokenBalances": [_row(3, OLD, "7"), _row(1, PAYER, "3"), _row(4, REF, "3"), _row(5, DEV, "7"),
				_row(6, DEV, "6")]},
	}
	var t: Variant = ScfgSolanaBackend.transfer_to(tx, {"sig": "s", "mint": MINT, "owner": DEV})
	# PAYER and REF both drop 7: Python's min() keeps the first in accountIndex order (1 before 4)
	T.ok(t is Dictionary and ScfgPy.py_eq(t["value"], 13), "the owner's net gain over both accounts")
	if t is Dictionary:
		T.eq([t["sender"], t["log_index"], t["block_number"]], [PAYER, 5, 77], "payer, first gaining index, int(slot)")
	var no_drops := tx.duplicate(true)
	no_drops["meta"] = {"err": null, "preTokenBalances": [], "postTokenBalances": [_row(2, DEV, "5")]}
	T.eq(ScfgSolanaBackend.transfer_to(no_drops, {"sig": "s", "mint": MINT, "owner": DEV})["sender"], OLD,
		"accountKeys[0].pubkey")
	var plain_key := no_drops.duplicate(true)
	plain_key["transaction"] = {"message": {"accountKeys": [PAYER]}}
	T.eq(ScfgSolanaBackend.transfer_to(plain_key, {"sig": "s", "mint": MINT, "owner": DEV})["sender"], PAYER,
		"a plain key")
	var no_keys := no_drops.duplicate(true)
	no_keys["transaction"] = null
	T.eq(ScfgSolanaBackend.transfer_to(no_keys, {"sig": "s", "mint": MINT, "owner": DEV})["sender"], null, "none")
	var str_keys := no_drops.duplicate(true)
	str_keys["transaction"] = {"message": {"accountKeys": "Kx"}}
	T.eq(ScfgSolanaBackend.transfer_to(str_keys, {"sig": "s", "mint": MINT, "owner": DEV})["sender"], "K",
		"Python indexes a str: its first character")
	var dict_keys := no_drops.duplicate(true)
	dict_keys["transaction"] = {"message": {"accountKeys": {"a": 1}}}
	T.is_err(ScfgSolanaBackend.transfer_to(dict_keys, {"sig": "s", "mint": MINT, "owner": DEV}), "KeyError",
		"a dict has no key 0")
	var bad_entry := no_drops.duplicate(true)
	bad_entry["transaction"] = {"message": {"accountKeys": [5]}}
	T.is_err(ScfgSolanaBackend.transfer_to(bad_entry, {"sig": "s", "mint": MINT, "owner": DEV}), "AttributeError",
		"an int entry has no .get")
	return T.done()


func test_transfer_to_net_gain_and_malformed_rows() -> bool:
	var a := {"sig": "s", "mint": MINT, "owner": DEV}
	var loss := {"slot": 1, "meta": {"preTokenBalances": [_row(2, DEV, "10")],
		"postTokenBalances": [_row(2, DEV, "4"), _row(3, DEV, "5")]}}
	T.eq(ScfgSolanaBackend.transfer_to(loss, a), null, "a net loss across the owner's accounts is no payment")
	var forms := {"slot": 1, "meta": {"preTokenBalances": [], "postTokenBalances": [_row(2, DEV, " +1_000 ")]}}
	T.ok(ScfgPy.py_eq(ScfgSolanaBackend.transfer_to(forms, a)["value"], 1000), "Python int() forms")
	var big := {"slot": 1, "meta": {"preTokenBalances": [], "postTokenBalances": [_row(2, DEV, "18446744073709551615")]}}
	var t: Variant = ScfgSolanaBackend.transfer_to(big, a)
	T.ok(t is Dictionary and t["value"] is ScfgBigInt and (t["value"] as ScfgBigInt).to_decimal() == "18446744073709551615",
		"u64 amounts beyond 2^63 stay exact")
	var negative := {"slot": 1, "meta": {"preTokenBalances": [_row(2, DEV, "-5")], "postTokenBalances": [_row(2, DEV, "3")]}}
	T.ok(ScfgPy.py_eq(ScfgSolanaBackend.transfer_to(negative, a)["value"], 8), "signed arithmetic")
	var floats := {"slot": 950.0, "meta": {"preTokenBalances": [], "postTokenBalances": [_row(2.0, DEV, "1")]}}
	var ft: Variant = ScfgSolanaBackend.transfer_to(floats, a)
	T.eq([ft["log_index"], ft["block_number"]] if ft is Dictionary else ft, [2, 950],
		"JSON numbers (floats here) are Python's ints")
	T.is_err(ScfgSolanaBackend.transfer_to({"slot": 1, "meta": {"postTokenBalances": [_row(2, DEV, "x")]}}, a),
		"ValueError", "an amount that is not a number")
	T.is_err(ScfgSolanaBackend.transfer_to({"slot": 1, "meta": {"postTokenBalances": [{"mint": MINT, "owner": DEV,
		"uiTokenAmount": {"amount": "1"}}]}}, a), "KeyError", "no accountIndex")
	T.is_err(ScfgSolanaBackend.transfer_to({"meta": {"postTokenBalances": [_row(2, DEV, "1")]}}, a), "KeyError", "no slot")
	T.is_err(ScfgSolanaBackend.transfer_to({"meta": {"postTokenBalances": "garbage"}}, a), "AttributeError",
		"a str's characters have no .get")
	T.is_err(ScfgSolanaBackend.transfer_to({"meta": {"postTokenBalances": 5}}, a), "TypeError", "an int is not iterable")
	T.is_err(ScfgSolanaBackend.transfer_to({"meta": "x"}, a), "AttributeError", "a str meta")
	T.is_err(ScfgSolanaBackend.transfer_to([1], a), "AttributeError", "a list transaction")
	var other := {"slot": 1, "meta": {"postTokenBalances": [{"mint": "M", "accountIndex": [1]}]}}
	T.eq(ScfgSolanaBackend.transfer_to(other, a), null, "other mints are never read")
	var unhashable_owner := {"slot": 1, "meta": {"preTokenBalances": [_row(1, [1], "5")],
		"postTokenBalances": [_row(1, [1], "0"), _row(2, DEV, "5")]}}
	T.is_err(ScfgSolanaBackend.transfer_to(unhashable_owner, a), "TypeError", "a list owner cannot be a dict key")
	T.eq(ScfgSolanaBackend.transfer_to({"slot": 1, "meta": {"err": "x", "postTokenBalances": "garbage"}}, a), null,
		"an err is read first")
	return T.done()


func test_waits_when_nothing_arrived() -> bool:
	var s := _setup()
	var out: Variant = await _step(s)
	T.eq(out["status"], "waiting", "waiting")
	T.eq(s[2].calls, ["get_signatures_for_address", "get_signatures_for_address"], "the reference, then one scan page")
	return T.done()


func test_reference_payment_confirms_then_finalizes_into_a_receipt() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"slot": 950, "addresses": [REF, DEST]})
	var out: Variant = await _step(s)
	T.eq([out["status"], out["confirmations"], out["required"]], ["confirming", 1, 32], "confirming")
	s[2].finalize(F.sig(1))
	out = await _step(s)
	T.eq(out["status"], "paid", "paid")
	var result: Dictionary = out["result"]
	T.eq([result["tx_hash"], result["chain"], result["payer"], str(result["amount"])], [F.sig(1), "solana", PAYER,
		"4.993742"], "the result")
	var key: Variant = ScfgReceipts.receipt_key("solana", F.sig(1), 2)
	T.ok(s[1].has(key), "a receipt")
	if s[1].has(key):
		T.eq([s[1][key]["block"], s[1][key]["recipient"], s[1][key]["source"]], [950, DEV, "payment"], "its fields")
	T.ok(not s[0].has(s[3]["id"]), "the session is closed")
	return T.done()


func test_reference_matches_any_amount_and_detects_underpayment() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"amount": 5_000_000, "addresses": [REF]})
	T.eq((await _step(s))["status"], "confirming", "a round amount by reference")
	s = _setup()
	_pay(s[2], 2, {"amount": 1_000_000, "addresses": [REF]})
	var out: Variant = await _step(s)
	T.eq(out["status"], "underpaid", "underpaid")
	T.ok(not s[0].has(s[3]["id"]), "closed")
	T.eq(str(out["result"]["amount"]), "1.000000", "what was paid")
	return T.done()


func test_scan_matches_exact_amount_only_and_moves_cursor() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"amount": 5_000_000, "addresses": [DEST]})
	T.eq((await _step(s))["status"], "waiting", "another amount")
	T.eq(_rec(s)["scan_cursor"], F.sig(1), "the cursor moved")
	_pay(s[2], 2, {"addresses": [DEST]})
	T.eq((await _step(s))["status"], "confirming", "the exact amount")
	T.eq(_rec(s)["match"]["tx_hash"], F.sig(2), "matched")
	return T.done()


func test_scan_spends_at_most_five_transactions_per_step() -> bool:
	var s := _setup()
	for n in range(1, 8):
		_pay(s[2], n, {"amount": 1_230_000, "addresses": [DEST]})
	_pay(s[2], 99, {"addresses": [DEST]})
	T.eq((await _step(s))["status"], "waiting", "budget spent")
	T.eq(_count(s[2].calls, "get_transaction"), 5, "five transactions")
	T.eq((await _step(s))["status"], "confirming", "found on the next step")
	return T.done()


## >100 new signatures since the last step must not hide a payment.
func test_scan_pages_back_past_a_full_page() -> bool:
	var s := _setup()
	_pay(s[2], 500, {"addresses": [DEST]})
	for n in range(1, 250):  # newer, unrelated traffic
		(s[2].addresses[DEST] as Array).insert(0, {"signature": F.sig(n), "slot": 990, "err": null})
	T.eq((await _step(s))["status"], "confirming", "found")
	T.ok(_count(s[2].calls, "get_signatures_for_address") >= 3, "paged")
	return T.done()


func test_scan_skips_failed_signatures_without_fetching_them() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"addresses": [DEST], "err": {"InstructionError": [0, "Custom"]}})
	T.eq((await _step(s))["status"], "waiting", "waiting")
	T.ok(not "get_transaction" in s[2].calls, "not fetched")
	T.eq(_rec(s)["scan_cursor"], F.sig(1), "but passed")
	return T.done()


func test_already_receipted_payment_is_not_matched_again() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"addresses": [REF]})
	s[1][ScfgReceipts.receipt_key("solana", F.sig(1), 2)] = {"chain": "solana"}
	T.eq((await _step(s))["status"], "waiting", "already a receipt")
	return T.done()


func test_bad_hints_are_dropped_with_a_reason() -> bool:
	var cases := [
		[{"err": {"InstructionError": [0, "Custom"]}}, "tx_reverted"],
		[{"owner": PAYER}, "tx_not_a_payment"],
		[{"slot": 100}, "tx_not_a_payment"],  # before the session started
	]
	for c: Array in cases:
		var s := _setup()
		_pay(s[2], 1, c[0])
		_hint(s, F.sig(1))
		var out: Variant = await _step(s)
		T.eq([out["status"], out["error"], out["hint_tx"]], ["waiting", c[1], F.sig(1)], c[1])
		T.eq(_rec(s)["hint_tx"], null, "%s: the hint is cleared" % c[1])
	return T.done()


func test_hint_not_yet_visible_waits_then_expires() -> bool:
	var s := _setup()
	_hint(s, F.sig(1))
	T.eq((await _step(s, 10.0))["status"], "waiting", "not visible yet")
	T.eq(_rec(s)["hint_tx"], F.sig(1), "kept")
	await _step(s, ScfgSessions.HINT_TIMEOUT_S + 1)
	T.eq(_rec(s)["hint_tx"], null, "expired")
	return T.done()


func test_good_hint_matches_round_amount() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"amount": 5_000_000})
	_hint(s, F.sig(1))
	T.eq((await _step(s))["status"], "confirming", "a hinted round amount")
	return T.done()


## A lagging endpoint's null status must not lose the payment.
func test_vanished_match_becomes_a_hint_and_is_found_again() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"addresses": [DEST]})
	T.eq((await _step(s))["status"], "confirming", "confirming")
	var saved: Variant = s[2].statuses[F.sig(1)]
	s[2].statuses.erase(F.sig(1))
	var out: Variant = await _step(s, 5.0)
	T.eq(out["status"], "reorged", "reorged")
	T.eq([_rec(s)["state"], _rec(s)["hint_tx"], _rec(s)["hint_at"], _rec(s)["match"]], ["awaiting", F.sig(1), 5.0, null],
		"kept as a hint")
	s[2].statuses[F.sig(1)] = saved  # the endpoint caught up
	T.eq((await _step(s, 6.0))["status"], "confirming", "found again")
	s[2].finalize(F.sig(1))
	T.eq((await _step(s, 7.0))["status"], "paid", "paid")
	return T.done()


func test_network_outage_changes_nothing() -> bool:
	var s := _setup()
	s[2].down = true
	var before: Dictionary = _rec(s).duplicate(true)
	var out: Variant = await _step(s)
	T.eq(out["error"], "network_unavailable", "network_unavailable")
	T.eq(_rec(s), before, "the session is untouched")
	return T.done()


func test_horizon_closes_the_session() -> bool:
	var s := _setup()
	T.eq((await _step(s, ScfgSessions.SCAN_HORIZON_S + 1))["status"], "closed", "closed")
	T.ok(not s[0].has(s[3]["id"]), "removed")
	return T.done()


func test_wrong_mint_on_the_reference_is_ignored() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"mint": "So11111111111111111111111111111111111111112", "addresses": [REF]})
	T.eq((await _step(s))["status"], "waiting", "another mint")
	return T.done()


## sig(1) is the old cursor; `spam` unrelated transfers newer than it, with the exact payment
## placed after `payment_at` of them (0 = just after the cursor).
func _busy_account(chain: F.FakeSolanaChain, spam: int, payment_at: int) -> void:
	_pay(chain, 1, {"amount": 1_230_000, "addresses": [DEST]})
	for n in spam + 1:
		if n == payment_at:
			_pay(chain, 9000, {"addresses": [DEST]})
		if n < spam:
			_pay(chain, 100 + n, {"amount": 1_230_000, "addresses": [DEST]})


## The first outcome that is not "waiting" (null after `limit` steps).
func _steps_until_confirming(s: Array, b: ScfgSolanaBackend = B, limit: int = 300) -> Variant:
	for n in limit:
		var out: Variant = await _step(s, float(n), b)
		if not out is Dictionary or out["status"] != "waiting":
			return out
	return null


## A busy account (> SIG_PAGE * MAX_SIG_PAGES new signatures) only delays detection.
func test_scan_crosses_a_backlog_longer_than_one_walk() -> bool:
	var s := _setup()
	_busy_account(s[2], 1050, 0)
	_rec(s)["scan_cursor"] = F.sig(1)
	var out: Variant = await _steps_until_confirming(s)
	T.ok(out is Dictionary and out["status"] == "confirming", "confirming")
	T.eq(_rec(s)["match"]["tx_hash"] if _rec(s)["match"] != null else null, F.sig(9000), "the payment")
	return T.done()


func test_scan_never_skips_signatures_when_it_falls_far_behind() -> bool:
	for payment_at: int in [0, 40, 80, 104, 105]:
		var b := _small_pages()
		var s := _setup()
		_busy_account(s[2], 105, payment_at)  # 106+ signatures: several walks deep
		_rec(s)["scan_cursor"] = F.sig(1)
		var out: Variant = await _steps_until_confirming(s, b)
		T.ok(out is Dictionary and out["status"] == "confirming", "[%d] confirming" % payment_at)
		T.eq(_rec(s)["match"]["tx_hash"] if _rec(s)["match"] != null else null, F.sig(9000), "[%d] the payment" % payment_at)
		T.ok("get_transaction" in s[2].calls, "[%d] examined" % payment_at)
	return T.done()


func test_scan_walk_resumes_where_it_stopped_after_new_traffic() -> bool:
	var b := _small_pages()
	var s := _setup()
	_busy_account(s[2], 70, 35)
	_rec(s)["scan_cursor"] = F.sig(1)
	for n in 4:  # walked back and started on the oldest window
		T.eq((await _step(s, float(n), b))["status"], "waiting", "step %d" % n)
	for k in 40:  # more traffic arrives mid-backlog
		_pay(s[2], 5000 + k, {"amount": 1_230_000, "addresses": [DEST]})
	await _steps_until_confirming(s, b)
	T.eq(_rec(s)["match"]["tx_hash"] if _rec(s)["match"] != null else null, F.sig(9000), "the payment")
	return T.done()


func test_outage_during_a_scan_walk_leaves_the_cursors_unchanged() -> bool:
	var b := _small_pages()
	var s := _setup()
	_busy_account(s[2], 105, 50)
	_rec(s)["scan_cursor"] = F.sig(1)
	T.eq((await _step(s, 1.0, b))["status"], "waiting", "first walk: moves scan_before")
	T.ok(_rec(s)["scan_before"] != null, "scan_before is set")
	var before: Dictionary = _rec(s).duplicate(true)
	var chain: F.FakeSolanaChain = s[2]
	var calls := []
	chain.patched["get_signatures_for_address"] = func(address: Variant, opts: Dictionary) -> Variant:
		calls.append(1)
		if calls.size() == 2:  # dies mid-walk
			return ScfgErr.make("RpcUnavailable", "fake outage")
		return await chain.real_get_signatures_for_address(address, opts)
	var out: Variant = await _step(s, 1.0, b)
	chain.patched.clear()  # the lambda holds the chain: break the cycle
	T.eq([out["error"], calls.size()], ["network_unavailable", 2], "the outage")
	T.eq(_rec(s), before, "the cursors are unchanged")
	return T.done()


## A wallet returns the signature before the transaction lands; dropped ones are routine.
func test_hint_that_never_lands_expires_after_the_solana_timeout() -> bool:
	T.eq(ScfgSolanaBackend.SOLANA_HINT_TIMEOUT_S, 180, "180 s")
	var s := _setup()
	_hint(s, F.sig(1))
	var out: Variant = await _step(s, ScfgSolanaBackend.SOLANA_HINT_TIMEOUT_S - 1)
	T.eq([out["status"], out["error"]], ["waiting", null], "not yet")
	T.eq(_rec(s)["hint_tx"], F.sig(1), "kept")
	out = await _step(s, ScfgSolanaBackend.SOLANA_HINT_TIMEOUT_S)
	T.eq([out["status"], out["error"], out["hint_tx"]], ["waiting", "tx_not_landed", F.sig(1)], "expired")
	T.eq(_rec(s)["hint_tx"], null, "cleared")
	return T.done()


func test_reference_payment_matches_while_a_hint_is_pending() -> bool:
	var s := _setup()
	_hint(s, F.sig(1))  # never lands
	_pay(s[2], 2, {"addresses": [REF, DEST]})
	var out: Variant = await _step(s, 10.0)
	T.eq([out["status"], out["error"]], ["confirming", null], "confirming")
	T.eq(_rec(s)["match"]["tx_hash"], F.sig(2), "the reference payment")
	T.eq(_rec(s)["hint_tx"], null, "the hint is cleared")
	return T.done()


## A hint dropped this step and a reference payment found in the same step: the payment wins.
func test_reference_payment_clears_the_verdict_on_a_dropped_hint() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"err": {"InstructionError": [0, "Custom"]}})
	_hint(s, F.sig(1))
	_pay(s[2], 2, {"addresses": [REF]})
	var out: Variant = await _step(s, 10.0)
	T.eq([out["status"], out["error"], out["hint_tx"]], ["confirming", null, null], "the verdict is cleared")
	return T.done()


func test_pending_hint_suspends_the_scan() -> bool:
	var s := _setup()
	_hint(s, F.sig(1))  # not visible yet
	_pay(s[2], 2, {"addresses": [DEST]})
	T.eq((await _step(s, 10.0))["status"], "waiting", "waiting")
	T.eq(_count(s[2].calls, "get_signatures_for_address"), 1, "the reference only")
	return T.done()


func test_hint_whose_lookup_keeps_failing_still_expires() -> bool:
	var s := _setup()
	_hint(s, F.sig(1))
	s[2].patched["get_transaction"] = func(_s: Variant, _c: String) -> Variant:
		return ScfgErr.make("RpcError", "Transaction version (9) is not supported", -32015)
	var out: Variant = await _step(s, 10.0)
	T.eq(out["error"], "network_unavailable", "an RpcError before the timeout")
	T.eq(_rec(s)["hint_tx"], F.sig(1), "kept")
	out = await _step(s, ScfgSolanaBackend.SOLANA_HINT_TIMEOUT_S + 1)
	T.eq([out["status"], out["error"]], ["waiting", "tx_not_landed"], "expired anyway")
	T.eq(_rec(s)["hint_tx"], null, "cleared")
	return T.done()


## Only an RpcError is forgiven once the hint is past its timeout; an outage is not a verdict.
func test_expired_hint_lookup_outage_is_still_network_trouble() -> bool:
	var s := _setup()
	_hint(s, F.sig(1))
	s[2].patched["get_transaction"] = func(_s: Variant, _c: String) -> Variant:
		return ScfgErr.make("RpcUnavailable", "down")
	var out: Variant = await _step(s, ScfgSolanaBackend.SOLANA_HINT_TIMEOUT_S + 1)
	T.eq(out["error"], "network_unavailable", "network trouble")
	T.eq(_rec(s)["hint_tx"], F.sig(1), "kept")
	return T.done()


## Later ticks are plain "waiting" (not forwarded), so the expiry tick must say it.
func test_hint_expiry_is_reported_even_if_the_reference_lookup_fails() -> bool:
	for failure: String in ["unavailable", "rpc_error"]:
		var s := _setup()
		_hint(s, F.sig(1))
		s[2].patched["get_signatures_for_address"] = func(_a: Variant, _o: Dictionary) -> Variant:
			if failure == "unavailable":
				return ScfgErr.make("RpcUnavailable", "all endpoints down")
			return ScfgErr.make("RpcError", "node is behind", -32005)
		var out: Variant = await _step(s, ScfgSolanaBackend.SOLANA_HINT_TIMEOUT_S)
		T.eq([out["status"], out["error"], out["hint_tx"]], ["waiting", "tx_not_landed", F.sig(1)],
			"[%s] which hint didn't land" % failure)
		T.eq(_rec(s)["hint_tx"], null, "[%s] cleared" % failure)
		out = await _step(s, ScfgSolanaBackend.SOLANA_HINT_TIMEOUT_S + 1)
		T.eq([out["status"], out["error"]], ["waiting", "network_unavailable"], "[%s] then plain trouble" % failure)
	return T.done()


func test_hint_verdicts_name_the_hint_they_are_about() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"err": {"InstructionError": [0, "Custom"]}})
	_hint(s, F.sig(1))
	var out: Variant = await _step(s, 1.0)
	T.eq([out["error"], out["hint_tx"]], ["tx_reverted", F.sig(1)], "the verdict")
	out = await _step(s, 2.0)
	T.eq([out["error"], out["hint_tx"]], [null, null], "said once")
	return T.done()


## Without a usable cursor the walk must not crawl the token account's whole history.
func test_scan_walk_stops_at_signatures_older_than_the_session() -> bool:
	for cursor: Variant in [null, F.sig(12345)]:
		var s := _setup()
		for n in range(1, 251):  # long history from before the session
			_pay(s[2], n, {"slot": 500 + n, "addresses": [DEST]})
		_pay(s[2], 999, {"slot": 950, "addresses": [DEST]})
		_rec(s)["scan_cursor"] = cursor
		var out: Variant = await _step(s)
		T.eq(out["status"], "confirming", "[%s] confirming" % cursor)
		T.eq(_rec(s)["match"]["tx_hash"] if _rec(s)["match"] != null else null, F.sig(999), "[%s] the payment" % cursor)
		T.eq(_count(s[2].calls, "get_transaction"), 1, "[%s] one transaction" % cursor)
		T.ok(_count(s[2].calls, "get_signatures_for_address") <= 2, "[%s] the reference and one page" % cursor)
	return T.done()


func test_confirming_reports_the_node_confirmations() -> bool:
	var s := _setup()
	_pay(s[2], 1, {"addresses": [REF]})
	await _step(s)
	var st: Dictionary = (s[2].statuses[F.sig(1)] as Dictionary).duplicate()
	st["confirmations"] = "7"
	s[2].statuses[F.sig(1)] = st
	T.eq((await _step(s))["confirmations"], 7, "int() of the node's count")
	st = st.duplicate()
	st["confirmations"] = null
	s[2].statuses[F.sig(1)] = st
	T.eq((await _step(s))["confirmations"], 0, "none")
	st = st.duplicate()
	st["confirmations"] = 3.0
	s[2].statuses[F.sig(1)] = st
	var out: Variant = await _step(s)
	T.eq(out["confirmations"], 3, "a JSON number is an int")
	s[2].statuses[F.sig(1)] = "pending"
	T.is_err(await _step(s), "AttributeError", "a status that is not a dict")
	return T.done()


func test_missing_session() -> bool:
	var s := _setup()
	var out: Variant = await B.advance(s[0], s[1], "ps_nope", {"rpc": s[2], "chain": CHAIN, "wallet": DEV, "items": ITEMS,
		"now": 1})
	T.eq(out, {"session_id": "ps_nope", "status": "missing", "confirmations": 0, "required": 32, "result": null,
		"error": null, "hint_tx": null}, "missing")
	T.eq(s[2].calls, [], "no network")
	return T.done()


func test_malformed_session_raises_instead_of_waiting() -> bool:
	var s := _setup()
	_rec(s).erase("scan_cursor")
	T.is_err(await _step(s), "KeyError", "no scan_cursor (the Engine contains it as internal_error)")
	s = _setup()
	_rec(s)["token"] = "NOPE"
	T.is_err(await _step(s), "KeyError", "an unknown token, before the try")
	s = _setup()
	_rec(s)["start_block"] = null
	_pay(s[2], 1, {"addresses": [REF]})
	T.is_err(await _step(s), "TypeError", "a start_block that is not a number")
	return T.done()
