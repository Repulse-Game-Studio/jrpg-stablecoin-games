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


func _run(store: Dictionary, chain: F.FakeChain, wallets: Variant = [DEV]) -> Variant:
	return await ScfgReverify.reverify_receipts(store, {"rpc_for": func(_k: Variant) -> Variant: return chain,
		"chains": ScfgChains.MAINNET, "wallets": wallets})


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
		var removed: Variant = await ScfgReverify.reverify_receipts(store, {"rpc_for": func(_k: Variant) -> Variant: return null,
			"chains": ScfgChains.MAINNET, "wallets": {"evm": [], "solana": []}})
		T.eq(removed, ["k"] if is_dropped else [], str(rec))
	return T.done()


func test_a_receipt_that_is_not_a_dict_is_dropped() -> bool:
	for junk: Variant in [null, "x", 5, [1, 2], true]:
		var store := {"k": junk}
		T.eq(await _run(store, F.FakeChain.new(1000)), ["k"], "junk: %s" % [junk])
	return T.done()


## Python: chain.tokens.get([..]) raises TypeError outside the try, so the whole run is rejected.
func test_unhashable_token_name_escapes_the_net() -> bool:
	T.is_err(await _run({"k": _good({"token": ["USDC"]})}, F.FakeChain.new(1000)), "TypeError", "a list token")
	T.is_err(await _run({"k": _good({"token": {}})}, F.FakeChain.new(1000)), "TypeError", "a dict token")
	T.eq(await _run({"k": _good({"token": 5})}, F.FakeChain.new(1000)), ["k"], "hashable but unknown: dropped")
	T.eq(await _run({"k": _good({"token": null})}, F.FakeChain.new(1000)), ["k"], "None: dropped")
	return T.done()


## Python: int(Decimal("Infinity")) is an OverflowError, which the net does not list.
func test_infinite_amount_escapes_the_net() -> bool:
	var store := {"k": _good({"amount": "Infinity"})}
	T.is_err(await _run(store, F.FakeChain.new(1000)), "OverflowError", "Infinity")
	T.is_err(await _run({"k": _good({"amount": "-Infinity"})}, F.FakeChain.new(1000)), "OverflowError", "-Infinity")
	T.eq(store.size(), 1, "the store is not touched by the failing run")
	T.is_err(await _run({"k": _good({"log_index": INF})}, F.FakeChain.new(1000)), "OverflowError", "int(inf)")
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


func test_a_failing_backend_check_fails_the_run() -> bool:
	# the backend's own errors are not part of the net: a malformed log stops everything
	var store := _store_with()
	var chain := F.FakeChain.new(1000)
	var bad := F.transfer_log(USDC, PAYER, DEV, 1, {"block": 900, "tx_hash": TX})
	bad["data"] = "0xzz"
	chain.logs.append(bad)
	T.is_err(await _run(store, chain), "ValueError", "a malformed log")
	T.eq(store.size(), 1, "nothing was removed")
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
	bad["data"] = "0xzz"
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
