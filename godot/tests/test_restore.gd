## ScfgRestore (restore.gd): tests/unit/test_restore.py, plus M1's edges (rpgmaker/src/core/
## restore.test.ts): classify at exactly min_price, the 10**32 refusal, the Solana id ruled out
## without touching it, a malformed log failing the whole lookup, the choices keys.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const OLD := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const PAYER := "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"
const TX := "0xabababababababababababababababababababababababababababababababab"

static var ARB_USDC: String = ScfgChains.MAINNET["arbitrum"]["tokens"]["USDC"]["address"]
static var BASE_USDC: String = ScfgChains.MAINNET["base"]["tokens"]["USDC"]["address"]


func _cfg() -> Dictionary:
	var c: Variant = ScfgConfig.build_config({"wallet": DEV, "previous_wallets": [OLD]})
	ScfgConfig.add_item(c, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	ScfgConfig.add_item(c, "ost", {"code": 2, "price": "2.00", "token": "USDT", "title": "OST"})
	return c


## classify's [kind, item, choices] for `value` units.
func _classify(value: Variant, config: Dictionary = {}) -> Variant:
	return ScfgRestore.classify(config if not config.is_empty() else _cfg(), value)


# --- classify ---------------------------------------------------------------------


func test_classify_item_code() -> bool:
	T.eq(_classify(4_993_742), ["unlock", "chapter2", []], "item code")
	return T.done()


func test_classify_tip_code() -> bool:
	T.eq(_classify(3_000_017), ["tip", null, []], "tip code")
	return T.done()


func test_classify_round_amount_asks_player() -> bool:
	T.eq(_classify(5_000_000), [null, null, ["chapter2", "ost", "tip"]], "round amount")
	T.eq(ScfgRestore.TIP_CHOICE, "tip", "TIP_CHOICE")
	return T.done()


func test_classify_unknown_code_offers_affordable_items() -> bool:
	T.eq(_classify(2_509_942), [null, null, ["ost", "tip"]], "2.50 | 99 | 42, code 99 unknown")
	return T.done()


func test_classify_known_code_but_below_min_price_asks() -> bool:
	T.eq(_classify(1_003_742), [null, null, ["tip"]], "code 37 but only ~1.00 paid")
	return T.done()


## Task 7 review gap of M1: both comparisons are inclusive (Python `amount >= min_price` and
## `min_price <= amount`).
func test_classify_amount_exactly_at_min_price_counts() -> bool:
	var c := _cfg()
	c["items"]["chapter2"]["min_price"] = ScfgDec.parse("4.993742")
	T.eq(_classify(4_993_742, c), ["unlock", "chapter2", []], "code 37, paid exactly min_price")
	c["items"]["chapter2"]["min_price"] = ScfgDec.parse("4.993743")
	T.eq(_classify(4_993_742, c), [null, null, ["ost", "tip"]], "one unit short")
	var r := _cfg()
	r["items"]["chapter2"]["min_price"] = ScfgDec.parse("5.00")
	T.eq(_classify(5_000_000, r)[2], ["chapter2", "ost", "tip"], "a round amount equal to min_price offers the item")
	r["items"]["chapter2"]["min_price"] = ScfgDec.parse("5.000001")
	T.eq(_classify(5_000_000, r)[2], ["ost", "tip"], "one unit less does not")
	r["items"]["ost"]["min_price"] = ScfgDec.parse("2.509942")  # unknown code 99: affordable, inclusive
	T.eq(_classify(2_509_942, r)[2], ["ost", "tip"], "affordable list, inclusive")
	r["items"]["ost"]["min_price"] = ScfgDec.parse("2.509943")
	T.eq(_classify(2_509_942, r)[2], ["tip"], "affordable list, one unit short")
	return T.done()


## Ruling 5: decode raises decimal.InvalidOperation from 10**32 units, which is not a ValueError.
func test_classify_refuses_values_decode_cannot_hold() -> bool:
	T.is_err(_classify(ScfgBigInt.from_decimal("1" + "0".repeat(32))), "InvalidOperation", "10**32 units")
	var just_below: Variant = ScfgBigInt.from_decimal("99999999999999999999999999990000")
	T.eq(_classify(just_below)[0], null, "just below: a whole-cent amount")
	return T.done()


# --- lookup -----------------------------------------------------------------------


## [chains {key: chain}, fakes {key: FakeChain}] (test_restore.py make_chains); `by_chain` maps a
## key to the receipt that chain returns for TX.
func _make_chains(by_chain: Dictionary = {}) -> Array:
	var chains := {}
	var fakes := {}
	for k: String in ["arbitrum", "base"]:
		chains[k] = ScfgChains.MAINNET[k]
		fakes[k] = F.FakeChain.new()
	for k: String in by_chain:
		fakes[k].receipts[TX] = by_chain[k]
	return [chains, fakes]


func _lookup(cf: Array, receipts: Dictionary = {}, tx: Variant = TX, on_error: Variant = null) -> Variant:
	var fakes: Dictionary = cf[1]
	var opts := {"config": _cfg(), "chains": cf[0], "receipts": receipts,
		"rpc_for": func(key: Variant) -> Variant: return fakes[key]}
	if on_error != null:
		opts["on_error"] = on_error
	return await ScfgRestore.lookup(tx, opts)


func _log(token: String, recipient: String, value: Variant, opts: Dictionary = {}) -> Dictionary:
	var o := {"block": 50, "tx_hash": TX}
	o.merge(opts, true)
	return F.transfer_log(token, PAYER, recipient, value, o)


func _is_restore_error(v: Variant, code: String, msg: String) -> void:
	T.is_err(v, "RestoreError", msg)
	if v is ScfgErr:
		T.eq((v as ScfgErr).code, code, "%s: code" % msg)


func test_lookup_finds_chain_and_classifies() -> bool:
	var cf := _make_chains({"base": {"status": "0x1", "logs": [_log(BASE_USDC, DEV, 4_993_742, {"log_index": 1})]}})
	var out: Variant = await _lookup(cf)
	T.ok(out is Array and out.size() == 1, "one candidate")
	if out is Array and out.size() == 1:
		var c: Dictionary = out[0]
		T.eq([c["chain"], c["token"], c["kind"], c["item"], c["already_restored"]],
			["base", "USDC", "unlock", "chapter2", false], "classified")
		T.eq(c.keys(), ["chain", "token", "transfer", "kind", "item", "choices", "already_restored"], "Python's field order")
		T.eq(c["choices"], [], "no choices")
	T.eq((cf[1]["arbitrum"] as F.FakeChain).calls, ["get_transaction_receipt"], "tried first, not found")
	return T.done()


func test_lookup_accepts_previous_wallet() -> bool:
	var cf := _make_chains({"arbitrum": {"status": "0x1", "logs": [_log(ARB_USDC, OLD, 3_000_005)]}})
	var out: Variant = await _lookup(cf)
	T.eq(out[0]["kind"] if out is Array else out, "tip", "a tip to the previous wallet")
	return T.done()


func test_lookup_marks_already_restored() -> bool:
	var cf := _make_chains({"arbitrum": {"status": "0x1", "logs": [_log(ARB_USDC, DEV, 4_993_742, {"log_index": 3})]}})
	var out: Variant = await _lookup(cf, {ScfgReceipts.receipt_key("arbitrum", TX, 3): {}})
	T.eq(out[0]["already_restored"] if out is Array else out, true, "already restored")
	return T.done()


func test_lookup_errors() -> bool:
	var cases := [
		["reverted", {"status": "0x0", "logs": []}, "tx_reverted"],
		["not to the developer", {"status": "0x1", "logs": [_log(ARB_USDC, PAYER, 5_000_000, {"block": 1})]}, "tx_not_a_payment"],
		["no logs", {"status": "0x1", "logs": []}, "tx_not_a_payment"],
		["logs missing", {"status": "0x1"}, "tx_not_a_payment"],
		["zero value", {"status": "0x1", "logs": [_log(ARB_USDC, DEV, 0, {"block": 1})]}, "tx_not_a_payment"],
		["another token", {"status": "0x1", "logs": [_log("0x1111111111111111111111111111111111111111", DEV, 5_000_000)]},
			"tx_not_a_payment"],
	]
	for c: Array in cases:
		var cf := _make_chains({"arbitrum": c[1]})
		_is_restore_error(await _lookup(cf), c[2], c[0])
	return T.done()


func test_lookup_not_found_anywhere() -> bool:
	_is_restore_error(await _lookup(_make_chains()), "tx_not_found", "not found")
	return T.done()


func test_lookup_network_trouble_is_reported_when_not_found() -> bool:
	var cf := _make_chains()
	(cf[1]["base"] as F.FakeChain).down = true
	_is_restore_error(await _lookup(cf), "network_unavailable", "base is down")
	return T.done()


func test_lookup_invalid_hash() -> bool:
	var cf := _make_chains()
	_is_restore_error(await _lookup(cf, {}, "0x123"), "invalid_hash", "not a hash")
	T.eq((cf[1]["arbitrum"] as F.FakeChain).calls, [], "no request")
	return T.done()


func test_lookup_a_found_tx_beats_network_trouble_on_an_earlier_chain() -> bool:
	var cf := _make_chains({"base": {"status": "0x1", "logs": [_log(BASE_USDC, DEV, 4_993_742)]}})
	(cf[1]["arbitrum"] as F.FakeChain).down = true
	var out: Variant = await _lookup(cf)
	T.eq(out[0]["chain"] if out is Array else out, "base", "found on base")
	return T.done()


func test_lookup_node_error_counts_as_network_trouble() -> bool:
	var cf := _make_chains()
	var arb := cf[1]["arbitrum"] as F.FakeChain
	arb.down = true  # RpcUnavailable
	_is_restore_error(await _lookup(cf), "network_unavailable", "an outage")
	var rpc_error := ScfgErr.make("RpcError", "boom", -32000)
	var fakes: Dictionary = cf[1]
	var out: Variant = await ScfgRestore.lookup(TX, {"config": _cfg(), "chains": cf[0], "receipts": {},
		"rpc_for": func(key: Variant) -> Variant: return rpc_error if key == "arbitrum" else fakes[key]})
	_is_restore_error(out, "network_unavailable", "an RpcError from rpc_for itself")
	return T.done()


## The registry holds Solana; its backend refuses the 0x hash and never reaches the network.
func test_lookup_rules_out_solana_for_an_evm_hash_without_touching_it() -> bool:
	var cf := _make_chains()
	cf[0]["solana"] = ScfgChains.MAINNET["solana"]
	var touched := []
	var fakes: Dictionary = cf[1]
	var out: Variant = await ScfgRestore.lookup(TX, {"config": _cfg(), "chains": cf[0], "receipts": {},
		"rpc_for": func(key: Variant) -> Variant:
			touched.append(key)
			return fakes[key] if fakes.has(key) else null})
	_is_restore_error(out, "tx_not_found", "an EVM hash is not a Solana signature")
	T.ok(not "solana" in touched, "the Solana RPC was never built")
	return T.done()


## An EVM backend whose lookup returns `found` (null: a coroutine a script error aborted).
class LookupStub:
	extends ScfgEvmBackend
	var found: Variant = null
	var asked := 0

	func lookup(_tx: Variant, _opts: Dictionary) -> Variant:
		asked += 1
		return found


## Python's None (the chain does not know the tx) is [] in GDScript, so a null `found` can only be
## an aborted coroutine: a RuntimeError for the whole lookup, never "tx_not_found".
func test_lookup_contains_a_null_from_a_backend_as_a_runtime_error() -> bool:
	var saved := ScfgBackends._load().duplicate()
	var stub := LookupStub.new()
	ScfgBackends._BACKENDS["evm"] = stub
	var cf := _make_chains()
	var out: Variant = await _lookup(cf)
	stub.found = []
	var empty: Variant = await _lookup(cf)
	ScfgBackends._BACKENDS.clear()
	ScfgBackends._BACKENDS.merge(saved)
	T.is_err(out, "RuntimeError", "an aborted backend lookup")
	T.ok(out is ScfgErr and not (out as ScfgErr).is_a("RestoreError"), "not a RestoreError")
	T.eq(stub.asked, 1 + (cf[0] as Dictionary).size(), "the first chain stopped the lookup; [] moved on to every chain")
	_is_restore_error(empty, "tx_not_found", "[] everywhere")
	return T.done()


## Only RpcUnavailable and RpcError are swallowed: a bad log is a plain error, not a RestoreError.
func test_lookup_malformed_log_rejects_the_whole_lookup() -> bool:
	var bad := _log(ARB_USDC, DEV, 5_000_000, {"block": 1})
	bad["logIndex"] = "0xzz"
	var out: Variant = await _lookup(_make_chains({"arbitrum": {"status": "0x1", "logs": [bad]}}))
	T.is_err(out, "ValueError", "a malformed log")
	T.ok(out is ScfgErr and not (out as ScfgErr).is_a("RestoreError"), "not a RestoreError")
	return T.done()


## tests/unit/test_restore.py::test_lookup_skips_a_transfer_too_large_to_decode
func test_lookup_skips_a_transfer_too_large_to_decode() -> bool:
	var huge := _log(ARB_USDC, DEV, ScfgBigInt.from_decimal("1" + "0".repeat(32)), {"block": 50, "log_index": 1})
	var fine := _log(ARB_USDC, DEV, 4_993_742, {"block": 50, "log_index": 2})
	var errors: Array = []
	var out: Variant = await _lookup(_make_chains({"arbitrum": {"status": "0x1", "logs": [huge, fine]}}), {}, TX,
		func(e: Variant) -> void: errors.append(e))
	T.ok(out is Array and (out as Array).size() == 1, "only the transfer decode can price is offered")
	if out is Array and (out as Array).size() == 1:
		var c: Dictionary = out[0]
		T.eq([c["transfer"]["log_index"], c["kind"], c["item"]], [2, "unlock", "chapter2"], "the Chapter 2 unlock")
	T.eq(errors.size(), 1, "the skipped one is reported")
	if errors.size() == 1:
		T.is_err(errors[0], "InvalidOperation", "decode's error")
	errors.clear()
	var only := _log(ARB_USDC, DEV, ScfgBigInt.from_decimal("1" + "0".repeat(40)), {"block": 50, "log_index": 1})
	out = await _lookup(_make_chains({"arbitrum": {"status": "0x1", "logs": [only]}}), {}, TX,
		func(e: Variant) -> void: errors.append(e))
	_is_restore_error(out, "tx_not_a_payment", "nothing else in the transaction")
	T.eq(errors.size(), 1, "reported")
	return T.done()


func test_lookup_non_dict_receipt_is_an_attribute_error() -> bool:
	T.is_err(await _lookup(_make_chains({"arbitrum": "0x1"})), "AttributeError", "a str has no .get")
	return T.done()


func test_lookup_non_string_tx_is_not_understood() -> bool:
	_is_restore_error(await _lookup(_make_chains(), {}, 5), "invalid_hash", "an int is not a hash")
	return T.done()


func test_restore_error_is_an_err_with_a_code() -> bool:
	var e: ScfgErr = ScfgRestore.restore_error("tx_reverted")
	T.eq([e.kind, e.code, e.message], ["RestoreError", "tx_reverted", "tx_reverted"], "fields")
	T.ok(not e.is_a("ValueError"), "an Exception, not a ValueError")
	return T.done()


# --- apply ------------------------------------------------------------------------


func test_apply_auto_and_chosen_candidates() -> bool:
	var cf := _make_chains({"arbitrum": {"status": "0x1", "logs": [
		_log(ARB_USDC, DEV, 4_993_742, {"log_index": 0}), _log(ARB_USDC, DEV, 5_000_000, {"log_index": 1})]}})
	var cands: Variant = await _lookup(cf)
	var store := {}
	var added: Variant = ScfgRestore.apply(store, cands, {1: "ost"})
	T.eq((added as Array).map(func(r: Dictionary) -> Array: return [r["kind"], r["item"], r["source"]]),
		[["unlock", "chapter2", "restore"], ["unlock", "ost", "restore"]], "auto and chosen")
	T.eq(ScfgRestore.apply(store, cands, {1: "ost"}), [], "idempotent")
	T.eq(added[1]["amount"], "5.000000", "amount")
	T.eq(store.keys(), [ScfgReceipts.receipt_key("arbitrum", TX, 0), ScfgReceipts.receipt_key("arbitrum", TX, 1)], "stored")
	return T.done()


func test_apply_skips_unchosen_and_rejects_invalid_choice() -> bool:
	var cf := _make_chains({"arbitrum": {"status": "0x1", "logs": [_log(ARB_USDC, DEV, 1_000_000)]}})
	var cands: Variant = await _lookup(cf)
	T.eq(ScfgRestore.apply({}, cands), [], "no choice")
	T.is_err(ScfgRestore.apply({}, cands, {0: "chapter2"}), "ValueError", "1.00 can't buy chapter2")
	T.eq(ScfgRestore.apply({}, cands, {0: ScfgRestore.TIP_CHOICE})[0]["kind"], "tip", "a tip")
	return T.done()


func test_apply_choice_keys_are_candidate_indexes_and_null_means_unchosen() -> bool:
	var cf := _make_chains({"arbitrum": {"status": "0x1", "logs": [_log(ARB_USDC, DEV, 1_000_000)]}})
	var cands: Variant = await _lookup(cf)
	T.eq(ScfgRestore.apply({}, cands, {0: null}), [], "null is unchosen")
	T.eq(ScfgRestore.apply({}, cands, {5: "tip"}), [], "no such candidate: ignored")
	T.eq(ScfgRestore.apply({}, cands, {}), [], "empty choices")
	T.eq((ScfgRestore.apply({}, cands, {0: "tip"}) as Array).size(), 1, "a choice")
	return T.done()


func test_apply_ignores_choices_for_candidates_that_already_have_a_kind() -> bool:
	var cf := _make_chains({"arbitrum": {"status": "0x1", "logs": [_log(ARB_USDC, DEV, 4_993_742)]}})
	var cands: Variant = await _lookup(cf)
	var added: Variant = ScfgRestore.apply({}, cands, {0: "nonsense"})
	T.eq(added[0]["item"], "chapter2", "the candidate's own kind and item win")
	return T.done()
