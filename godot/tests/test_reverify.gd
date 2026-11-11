## ScfgReverify (reverify.gd): tests/unit/test_reverify.py and the reverify tests of
## test_backends.py, plus M1's pinned quirks (rpgmaker/src/core/reverify.test.ts): the Python
## accept/reject surface of the malformed-entry net, what escapes it, the removed keys in store
## order, and Python's lazy `any()` over the logs.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const OLD := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const PAYER := "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"
const TX := "0xabababababababababababababababababababababababababababababababab"

static var ARB: Dictionary = ScfgChains.MAINNET["arbitrum"]
static var USDC: String = ARB["tokens"]["USDC"]["address"]


## The store of one receipt (test_reverify.py store_with).
func _store_with(o: Dictionary = {}) -> Dictionary:
	var store := {}
	var t := {"token": USDC, "sender": PAYER, "recipient": o.get("recipient", DEV),
		"value": ScfgBigInt.from_int(o.get("value", 4_993_742)), "tx_hash": TX, "log_index": o.get("log_index", 2),
		"block_number": o.get("block", 900), "block_hash": "0x" + "11".repeat(32)}
	ScfgReceipts.add_receipt(store, ScfgReceipts.make_receipt({"chain": o.get("chain", "arbitrum"), "transfer": t,
		"kind": "unlock", "item": "chapter2", "token": o.get("token", "USDC"), "source": "payment"}))
	return store


func _run(store: Dictionary, chain: F.FakeChain, wallets: Variant = [DEV], on_error: Variant = null) -> Variant:
	var opts := {"rpc_for": func(_k: Variant) -> Variant: return chain, "chains": ScfgChains.MAINNET, "wallets": wallets}
	# quiet by default: removals and failed checks are reported
	opts["on_error"] = on_error if on_error != null else func(_e: Variant) -> void: pass
	return await ScfgReverify.reverify_receipts(store, opts)


func _genuine(chain: F.FakeChain, o: Dictionary = {}) -> void:
	var opts := {"block": o.get("block", 900), "tx_hash": o.get("tx_hash", TX), "log_index": o.get("log_index", 2)}
	chain.logs.append(F.transfer_log(USDC, PAYER, DEV, o.get("value", 4_993_742), opts))


func test_genuine_receipt_is_kept() -> bool:
	var store := _store_with()
	var chain := F.FakeChain.new(1000)
	_genuine(chain)
	T.eq(await _run(store, chain), [], "nothing removed")
	T.eq(store.size(), 1, "kept")
	return T.done()


func test_lookup_is_scoped_to_the_receipt_block() -> bool:
	var chain := F.FakeChain.new(1000)
	await _run(_store_with(), chain)
	T.eq([chain.last_get_logs[0], chain.last_get_logs[2], chain.last_get_logs[3]], [USDC, 900, 900], "address and block")
	T.eq(chain.last_get_logs[1][2], ["0x" + "0".repeat(24) + DEV.substr(2).to_lower()], "the wallet topic")
	T.eq(chain.last_get_logs[1][0], ScfgEvm.TRANSFER_TOPIC, "the Transfer topic")
	T.eq(chain.last_get_logs[1][1], null, "any sender")
	return T.done()


func test_mismatching_log_revokes() -> bool:
	for o: Dictionary in [{"value": 4_993_743}, {"log_index": 3}, {"tx_hash": "0x" + "cd".repeat(32)}]:
		var store := _store_with()
		var chain := F.FakeChain.new(1000)
		_genuine(chain, o)
		T.eq((await _run(store, chain) as Array).size(), 1, "revoked: %s" % [o])
		T.eq(store, {}, "removed: %s" % [o])
	return T.done()


func test_invented_receipt_with_no_log_is_revoked() -> bool:
	T.eq((await _run(_store_with(), F.FakeChain.new(1000)) as Array).size(), 1, "revoked")
	return T.done()


func test_outage_keeps_everything() -> bool:
	var store := _store_with()
	var chain := F.FakeChain.new(1000)
	chain.down = true
	T.eq(await _run(store, chain), [], "nothing removed")
	T.eq(store.size(), 1, "kept")
	return T.done()


## An RpcError (a node error) keeps the receipt too, at either call.
func test_node_error_keeps_the_receipt() -> bool:
	var rpc_error := ScfgErr.make("RpcError", "boom", -32000)
	for failing: String in ["block_number", "get_logs"]:
		var store := _store_with()
		var chain := ErroringChain.new(1000, failing, rpc_error)
		T.eq(await _run(store, chain), [], "%s: nothing removed" % failing)
		T.eq(store.size(), 1, "%s: kept" % failing)
	return T.done()


func test_block_slightly_ahead_of_lagging_head_is_kept() -> bool:
	var store := _store_with({"block": 1005})
	T.eq(await _run(store, F.FakeChain.new(1000)), [], "kept")
	T.eq(store.size(), 1, "still there")
	return T.done()


func test_impossible_future_block_is_revoked() -> bool:
	var far := _store_with({"block": 1000 + ScfgReverify.FUTURE_TOLERANCE_BLOCKS + 1})
	T.eq((await _run(far, F.FakeChain.new(1000)) as Array).size(), 1, "one block past the tolerance")
	var edge := _store_with({"block": 1000 + ScfgReverify.FUTURE_TOLERANCE_BLOCKS})
	T.eq(await _run(edge, F.FakeChain.new(1000)), [], "exactly at the tolerance")
	return T.done()


func test_previous_wallet_payments_survive_wallet_change() -> bool:
	var store := _store_with({"recipient": OLD})
	var chain := F.FakeChain.new(1000)
	chain.logs.append(F.transfer_log(USDC, PAYER, OLD, 4_993_742, {"block": 900, "tx_hash": TX, "log_index": 2}))
	T.eq(await _run(store, chain, [DEV, OLD]), [], "both wallets listed")
	T.ok(not (await _run(store, chain, [DEV]) as Array).is_empty(), "the old wallet dropped")
	return T.done()


func test_unknown_chain_is_left_alone() -> bool:
	var store := _store_with({"chain": "tron"})
	T.eq(await _run(store, F.FakeChain.new(1000)), [], "kept")
	T.eq(store.size(), 1, "still there")
	return T.done()


func test_token_not_on_chain_is_revoked() -> bool:
	var store := _store_with({"chain": "base", "token": "USDT"})
	T.eq((await _run(store, F.FakeChain.new(1000)) as Array).size(), 1, "no USDT on base")
	return T.done()


func test_malformed_entries_are_removed() -> bool:
	for junk: Dictionary in [{}, {"chain": "arbitrum"}, {"chain": "arbitrum", "tx_hash": TX, "log_index": "x",
			"block": 1, "amount": "1", "token": "USDC"}]:
		var store := _store_with()
		var chain := F.FakeChain.new(1000)
		_genuine(chain)
		store["junk"] = junk
		T.eq(await _run(store, chain), ["junk"], "junk: %s" % [junk])
		T.eq(store.size(), 1, "the genuine receipt stays")
	return T.done()


# --- the Python accept/reject surface of the malformed-entry net -----------------------------------


func _good(over: Dictionary) -> Dictionary:
	var d := {"chain": "arbitrum", "tx_hash": TX, "log_index": 2, "block": 900, "amount": "4.993742", "token": "USDC"}
	d.merge(over, true)
	return d


func test_malformed_field_forms_follow_python_int_and_decimal() -> bool:
	# dropped: Python raises KeyError / TypeError / ValueError / AttributeError / InvalidOperation
	var dropped: Array[Dictionary] = [
		_good({"tx_hash": 5}), _good({"tx_hash": null}), _good({"log_index": null}), _good({"log_index": [1]}),
		_good({"block": "0x10"}), _good({"amount": "abc"}), _good({"amount": "1.0000001"}), _good({"amount": null}),
		_good({"amount": [1]}), _good({"amount": {}}), _good({"amount": "NaN"}), _good({"amount": "sNaN"}),
		_good({"chain": ["arbitrum"]}), _good({"chain": {}}), _good({"log_index": "x"}), _good({"log_index": NAN}),
	]
	# kept for further judging (no wallet for the family): Python converts them
	var parsed: Array[Dictionary] = [
		_good({"log_index": "2"}), _good({"log_index": " 2 "}), _good({"log_index": 2.5}), _good({"log_index": true}),
		_good({"block": "900"}), _good({"amount": 5}), _good({"amount": 1.5}), _good({"amount": true}),
		_good({"amount": " 4.993742 "}), _good({"log_index": 2.0}), _good({"chain": "tron"}),
	]
	for i in dropped.size() + parsed.size():
		var is_dropped := i < dropped.size()
		var rec: Dictionary = dropped[i] if is_dropped else parsed[i - dropped.size()]
		var store := {"k": rec}
		var errors: Array = []
		var removed: Variant = await ScfgReverify.reverify_receipts(store, {"rpc_for": func(_k: Variant) -> Variant: return null,
			"chains": ScfgChains.MAINNET, "wallets": {"evm": [], "solana": []},
			"on_error": func(e: Variant) -> void: errors.append(e)})
		T.eq(removed, ["k"] if is_dropped else [], str(rec))
		T.eq(errors.size(), 1 if is_dropped else 0, "%s: a removal is reported with its reason" % str(rec))
	return T.done()


func test_a_receipt_that_is_not_a_dict_is_dropped() -> bool:
	for junk: Variant in [null, "x", 5, [1, 2], true]:
		var store := {"k": junk}
		T.eq(await _run(store, F.FakeChain.new(1000)), ["k"], "junk: %s" % [junk])
	return T.done()


func test_a_hashable_unknown_token_name_is_dropped() -> bool:
	T.eq(await _run({"k": _good({"token": 5})}, F.FakeChain.new(1000)), ["k"], "hashable but unknown: dropped")
	T.eq(await _run({"k": _good({"token": null})}, F.FakeChain.new(1000)), ["k"], "None: dropped")
	return T.done()


## tests/unit/test_reverify.py::test_a_receipt_with_invalid_fields_is_removed_and_reported: a receipt
## whose own fields are invalid is removed with its reason reported to on_error, whatever chain it
## names; the run goes on.
func test_a_receipt_with_invalid_fields_is_removed_and_reported() -> bool:
	for corrupt: Dictionary in [{"amount": "Infinity"}, {"amount": "-Infinity"}, {"amount": "1E+999999"},
			{"amount": "1E+999993"}, {"amount": "-1E+999995"}, {"amount": "1E+9999999999999999"}, {"amount": "1E+41"},
			{"token": ["USDC"]}, {"token": {}}, {"item": ["chapter2"]}, {"chain": ["arbitrum"]}, {"tx_hash": 5},
			{"log_index": INF}, {"block": "0x10"}]:
		var chain := F.FakeChain.new(1000)
		_genuine(chain)
		var bad := _good(corrupt)
		var store := {"a:corrupt": bad}
		store.merge(_store_with({"log_index": 2}))
		store.merge(_store_with({"log_index": 3}))
		var errors: Array = []
		var label := ScfgPy.py_repr(corrupt)
		var start := Time.get_ticks_msec()
		T.eq(await _run(store, chain, [DEV], func(e: Variant) -> void: errors.append(e)),
			["a:corrupt", "arbitrum:%s:3" % TX], label)
		T.ok(Time.get_ticks_msec() - start < 5000, "%s: quickly" % label)
		T.eq(store.keys(), ["arbitrum:%s:2" % TX], "%s: removed, the genuine one kept" % label)
		T.eq(errors.size(), 1, "%s: reported" % label)
		if errors.size() == 1:
			T.ok(errors[0] is ScfgErr and (errors[0] as ScfgErr).is_a("ValueError") and str(errors[0]).contains("a:corrupt"),
				"%s: the reason names the receipt (%s)" % [label, str(errors[0])])
	return T.done()


## tests/unit/test_reverify.py::test_a_type_valid_but_malformed_receipt_is_removed_before_any_rpc
func test_a_type_valid_but_malformed_receipt_is_removed_before_any_rpc() -> bool:
	for row: Array in [["arbitrum", {"tx_hash": "0x123"}], ["arbitrum", {"tx_hash": "0x" + "zz".repeat(32)}],
			["arbitrum", {"tx_hash": "ab".repeat(32)}], ["arbitrum", {"tx_hash": TX + " "}],
			["arbitrum", {"tx_hash": "0x" + "ab".repeat(33)}], ["arbitrum", {"log_index": -1}],
			["arbitrum", {"log_index": "-1"}], ["arbitrum", {"block": -5}], ["solana", {"tx_hash": TX}],
			["arbitrum", {"log_index": "9".repeat(4301)}], ["arbitrum", {"block": "9".repeat(100000)}],
			["solana", {"tx_hash": "2".repeat(100000)}],
			["solana", {"tx_hash": "0OIl".repeat(22)}], ["solana", {"tx_hash": "1".repeat(63)}],
			# str.strip() whitespace that JS trim() keeps (TS twin)
			["arbitrum", {"tx_hash": TX + char(0x85)}], ["arbitrum", {"tx_hash": TX + char(0x1c)}],
			["arbitrum", {"tx_hash": char(0x1f) + TX}], ["solana", {"tx_hash": "1".repeat(64) + char(0x85)}]]:
		var node := F.FakeChain.new(1000)
		var bad := _good(row[1])
		bad["chain"] = row[0]
		var store := {"bad": bad}
		var errors: Array = []
		var label := "%s %s" % [row[0], ScfgPy.py_repr(row[1]).substr(0, 60)]
		T.eq(await _run(store, node, [DEV], func(e: Variant) -> void: errors.append(e)), ["bad"], label)
		T.eq(errors.size(), 1, "%s: reported" % label)
		T.eq(node.calls, [], "%s: judged without the network" % label)
	return T.done()


## tests/unit/test_reverify.py::test_a_malformed_receipt_on_an_unknown_chain_is_kept
func test_a_malformed_receipt_on_an_unknown_chain_is_kept() -> bool:
	var store := {"old": _good({"chain": "tron", "tx_hash": "0x123"})}
	T.eq(await _run(store, F.FakeChain.new(1000)), [], "kept: the chain cannot be checked")
	T.eq(store.size(), 1, "still stored")
	return T.done()


## tests/unit/test_reverify.py::test_a_forged_out_of_range_receipt_no_longer_unlocks
func test_a_forged_out_of_range_receipt_no_longer_unlocks() -> bool:
	var forged := _good({"amount": "1E+999999"})
	forged["kind"] = "unlock"
	forged["item"] = "chapter2"
	var store := {"forged": forged}
	var items := {"chapter2": {"item_id": "chapter2", "code": 37, "price": ScfgDec.parse("4.99"), "token": "USDC",
		"title": "Chapter 2", "min_price": ScfgDec.parse("4.99")}}
	var owned: Variant = ScfgReceipts.compute_ownership(store, items)
	T.ok(owned is Array and (owned[0] as Array).is_empty(), "owns nothing")
	T.eq(await _run(store, F.FakeChain.new(1000), [DEV], func(_e: Variant) -> void: pass), ["forged"], "removed")
	T.eq(store, {}, "gone")
	return T.done()


func test_removed_keys_come_back_in_store_order() -> bool:
	var store := {}
	for k: String in ["z", "b", "m", "a"]:
		store[k] = {}
	var keep := _store_with()
	store.merge(keep)
	var chain := F.FakeChain.new(1000)
	_genuine(chain)
	T.eq(await _run(store, chain), ["z", "b", "m", "a"], "store order, not sorted")
	T.eq(store.keys(), keep.keys(), "only the genuine receipt is left")
	return T.done()


## tests/unit/test_reverify.py::test_a_receipt_whose_check_crashes_is_kept_and_reported
func test_a_failing_backend_check_skips_that_receipt() -> bool:
	# a malformed log crashes the backend's check: that receipt is kept and reported, the next judged
	var store := _store_with()
	store.merge(_store_with({"log_index": 3}))
	var chain := F.FakeChain.new(1000)
	var bad := F.transfer_log(USDC, PAYER, DEV, 1, {"block": 900, "tx_hash": TX})
	bad["logIndex"] = "0xzz"
	chain.logs.append(bad)
	var errors: Array = []
	T.eq(await _run(store, chain, [DEV], func(e: Variant) -> void: errors.append(e)), [], "nothing removed")
	T.eq(store.size(), 2, "both kept")
	T.eq(errors.size(), 2, "each crashed check reported")
	if errors.size() == 2:
		T.is_err(errors[0], "ValueError", "the malformed log")
	return T.done()


# --- wallets per family (test_backends.py) ---------------------------------------------------------


func test_reverify_accepts_wallets_per_family() -> bool:
	var store := _store_with()
	var chain := F.FakeChain.new(1000)
	_genuine(chain)
	var removed: Variant = await ScfgReverify.reverify_receipts(store, {"rpc_for": func(_k: Variant) -> Variant: return chain,
		"chains": {"arbitrum": ARB}, "wallets": {"evm": [DEV]}})
	T.eq(removed, [], "kept")
	T.eq(store.size(), 1, "still there")
	return T.done()


## A game that dropped EVM (Solana-only now) can't judge old EVM receipts: keep them.
func test_reverify_keeps_receipts_of_a_family_without_wallets() -> bool:
	var store := _store_with()
	var chain := F.FakeChain.new(1000)  # no logs at all: would revoke if checked
	for wallets: Variant in [{"evm": []}, {}, []]:
		var removed: Variant = await ScfgReverify.reverify_receipts(store, {"rpc_for": func(_k: Variant) -> Variant: return chain,
			"chains": {"arbitrum": ARB}, "wallets": wallets})
		T.eq(removed, [], "kept with wallets %s" % [wallets])
	T.eq(store.size(), 1, "still there")
	T.ok(not "get_logs" in chain.calls, "nothing was asked of the chain")
	return T.done()


# --- check_evm_receipt ------------------------------------------------------------------------


## Python's any() over a generator: a malformed log after the matching one is never parsed.
func test_check_evm_receipt_stops_parsing_at_the_first_match() -> bool:
	var receipt: Dictionary = _store_with().values()[0]
	var bad := F.transfer_log(USDC, PAYER, DEV, 1, {"block": 900, "tx_hash": TX, "log_index": 9})
	bad["logIndex"] = "0xzz"
	var chain := F.FakeChain.new(1000)
	_genuine(chain)
	chain.logs.append(bad)
	T.eq(await ScfgReverify.check_evm_receipt(receipt, {"rpc": chain, "chain": ARB, "wallets": [DEV]}), "keep",
		"the match comes first")
	var first := F.FakeChain.new(1000)
	first.logs.append(bad)
	_genuine(first)
	T.is_err(await ScfgReverify.check_evm_receipt(receipt, {"rpc": first, "chain": ARB, "wallets": [DEV]}), "ValueError",
		"the bad log comes first")
	return T.done()


func test_check_evm_receipt_reads_its_fields_like_python() -> bool:
	var chain := F.FakeChain.new(1000)
	var opts := {"rpc": chain, "chain": ARB, "wallets": [DEV]}
	T.is_err(await ScfgReverify.check_evm_receipt({}, opts), "KeyError", "an empty receipt")
	T.is_err(await ScfgReverify.check_evm_receipt(_good({"tx_hash": 5}), opts), "AttributeError", "an int tx_hash")
	T.is_err(await ScfgReverify.check_evm_receipt(_good({"token": "NOPE"}), opts), "KeyError", "a token the chain lacks")
	T.eq(await ScfgReverify.check_evm_receipt(_good({"tx_hash": TX.to_upper().replace("0X", "0x")}), opts), "drop", "no log")
	T.eq(chain.calls, ["block_number", "get_logs"], "asked once")
	return T.done()


func test_a_block_beyond_64_bits_is_far_in_the_future() -> bool:
	var chain := F.FakeChain.new(1000)
	var huge := _good({"block": "18446744073709551616"})  # 2**64
	T.eq(await ScfgReverify.check_evm_receipt(huge, {"rpc": chain, "chain": ARB, "wallets": [DEV]}), "drop", "impossible")
	return T.done()


## A FakeChain whose method `failing` answers `err`.
class ErroringChain:
	extends F.FakeChain
	var failing: String
	var err: ScfgErr

	func _init(head_: int, failing_: String, err_: ScfgErr) -> void:
		super(head_)
		failing = failing_
		err = err_

	func block_number() -> Variant:
		if failing == "block_number":
			return err
		return await super()

	func get_logs(address: Variant, topics: Variant, from_block: int, to_block: int) -> Variant:
		if failing == "get_logs":
			return err
		return await super(address, topics, from_block, to_block)
