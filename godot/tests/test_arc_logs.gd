## Arc: tests/unit/test_arc_logs.py (Chain.parse_transfer, the watcher, restore and
## re-verification on Arc), plus test_arc_floor_keeps_bigint_precision (Review Focus 4).
##
## USDC is Arc's native gas coin, so its transfers are read from the system emitter. A plain
## native send logs a Transfer only at the emitter (18 decimals); an ERC-20 transfer() on the 0x3600
## interface logs there too, plus its own 6-decimal Transfer. parse_transfer keeps exactly one per
## payment: the emitter's, floored to 6-decimal units and stamped with the canonical USDC address.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const TX := "0xabababababababababababababababababababababababababababababababab"
const UNITS := 4_993_742  # 4.99 + code 37 + nonce 42
const NATIVE := 1_000_000_000_000  # native (18-decimal) wei per 6-decimal unit

static var ARC: Dictionary = ScfgChains.MAINNET["arc"]
static var ITEMS := {"chapter2": ScfgModels.make_item({"item_id": "chapter2", "code": 37,  # err-ok: valid fields
	"price": ScfgDec.parse("4.99"), "token": "USDC", "title": "Chapter 2", "min_price": ScfgDec.parse("4.99")})}


## `units * NATIVE + extra` as an ScfgBigInt (exact beyond 2^63).
static func _wei(units: int, extra: int = 0) -> ScfgBigInt:
	var v: ScfgBigInt = ScfgBigInt.from_int(units).mul(ScfgBigInt.from_int(NATIVE))  # err-ok: non-negative ints
	return v.add(ScfgBigInt.from_int(extra))  # err-ok: non-negative ints


func _emitter_log(value: Variant, opts: Dictionary = {}) -> Dictionary:
	var o := {"block": 100, "tx_hash": TX}
	o.merge(opts, true)
	return F.transfer_log(ScfgChains.ARC_SYSTEM_EMITTER, PAYER, DEV, value, o)


func _value(t: Variant) -> String:
	return (t["value"] as ScfgBigInt).to_decimal() if t is Dictionary else "<%s>" % [t]


func test_parse_transfer_floors_native_value() -> bool:
	var t: Variant = ScfgChains.parse_transfer(ARC, _emitter_log(ScfgBigInt.from_decimal("1230017900000000000"),
		{"log_index": 4}))
	T.eq(_value(t), "1230017", "floored")
	T.eq(t["token"], ScfgChains.ARC_USDC, "the canonical address")
	T.eq([t["sender"], t["recipient"], t["log_index"]], [PAYER, DEV, 4], "fields")
	return T.done()


func test_parse_transfer_ignores_sub_micro_value() -> bool:
	# less than one 6-decimal unit floors to nothing: never a payment
	T.eq(ScfgChains.parse_transfer(ARC, _emitter_log(NATIVE - 1)), null, "dust")
	return T.done()


func test_parse_transfer_ignores_interface_log_on_arc() -> bool:
	# the 0x3600 log of an ERC-20 transfer() duplicates the emitter's: never counted
	var log := F.transfer_log(ScfgChains.ARC_USDC, PAYER, DEV, 1_230_017, {"block": 100, "tx_hash": TX})
	T.eq(ScfgChains.parse_transfer(ARC, log), null, "interface log")
	return T.done()


func test_parse_transfer_ignores_unknown_contract() -> bool:
	var log := F.transfer_log("0x1111111111111111111111111111111111111111", PAYER, DEV, 5, {"block": 100, "tx_hash": TX})
	T.eq(ScfgChains.parse_transfer(ARC, log), null, "on Arc")
	T.eq(ScfgChains.parse_transfer(ScfgChains.MAINNET["base"], log), null, "on Base")
	return T.done()


func test_parse_transfer_plain_chain_unchanged() -> bool:
	var base: Dictionary = ScfgChains.MAINNET["base"]
	var usdc: String = base["tokens"]["USDC"]["address"]
	var t: Variant = ScfgChains.parse_transfer(base, F.transfer_log(usdc, PAYER, DEV, 4_993_742,
		{"block": 7, "tx_hash": TX, "log_index": 2}))
	T.eq([t["token"], _value(t), t["log_index"], t["block_number"]], [usdc, "4993742", 2, 7], "unchanged")
	return T.done()


func test_parse_transfer_skips_non_transfer_logs() -> bool:
	var log := _emitter_log(5 * NATIVE)
	log["topics"] = (log["topics"] as Array).slice(0, 2)
	T.eq(ScfgChains.parse_transfer(ARC, log), null, "two topics")
	return T.done()


func test_arc_floor_keeps_bigint_precision() -> bool:
	# 19993754 * 10^12 is above 2^63 (an int would wrap); flooring must keep every digit
	var raw: Variant = ScfgBigInt.from_decimal("19993754000000000000")
	T.ok((raw as ScfgBigInt).bit_length() > 63, "the raw value is beyond 64-bit ints")
	var t: Variant = ScfgChains.parse_transfer(ARC, _emitter_log(raw, {"log_index": 1}))
	T.eq(_value(t), "19993754", "floored exactly")
	var plus_dust: Variant = ScfgChains.parse_transfer(ARC, _emitter_log(_wei(19993754, NATIVE - 1)))
	T.eq(_value(plus_dust), "19993754", "dust below one unit is dropped")
	# and through the watcher: the hinted value lands in the match as the exact int
	var s := _session()
	_hint(s, [_emitter_log(raw, {"block": 998, "log_index": 5})])
	var out: Variant = await _step(s)
	T.eq(out["status"], "paid", "paid")
	T.eq(str(out["result"]["amount"]), "19.993754", "amount")
	var stored: Dictionary = s[1][ScfgReceipts.receipt_key("arc", TX, 5)]
	T.eq(stored["amount"], "19.993754", "the receipt's amount")
	return T.done()


# --- the watcher on Arc ---------------------------------------------------------------------


## The two logs one ERC-20 transfer() produces on Arc: the emitter's first, then 0x3600's.
func _arc_transfer_tx(units: int, opts: Dictionary = {}) -> Array:
	var block: int = opts.get("block", 998)
	var log_index: int = opts.get("log_index", 5)
	var native: Variant = opts.get("native", _wei(units))
	return [
		_emitter_log(native, {"block": block, "log_index": log_index}),
		F.transfer_log(ScfgChains.ARC_USDC, PAYER, DEV, units, {"block": block, "tx_hash": TX, "log_index": log_index + 1}),
	]


## [pending, receipts, rpc, rec]
func _session(head: int = 1000) -> Array:
	var pending := {}
	var rec: Variant = ScfgSessions.create_session(pending, {"kind": "unlock", "item": "chapter2", "title": "Chapter 2",
		"token": "USDC", "chain": "arc", "price": "4.99", "code": 37, "nonce": 42, "start_block": 995, "now": 0.0})
	T.eq(rec["units"], UNITS, "units")
	return [pending, {}, F.FakeChain.new(head), rec]


func _step(s: Array, now: float = 10.0) -> Variant:
	return await ScfgWatcher.advance(s[0], s[1], s[3]["id"], {"rpc": s[2], "chain": ARC, "wallet": DEV, "items": ITEMS,
		"now": now})


func _hint(s: Array, logs: Array) -> void:
	(s[2] as F.FakeChain).receipts[TX] = {"status": "0x1", "logs": logs}
	ScfgSessions.set_hint(s[0], s[3]["id"], TX, 1.0)


func test_arc_native_send_detected_by_scan() -> bool:
	var s := _session()
	(s[2] as F.FakeChain).logs.append(_emitter_log(_wei(UNITS), {"block": 998, "log_index": 3}))  # a wallet "Send"
	var out: Variant = await _step(s)
	T.eq((s[2] as F.FakeChain).last_get_logs[0], ScfgChains.ARC_SYSTEM_EMITTER, "scans the emitter")
	T.eq(out["status"], "paid", "one confirmation is final on Arc")
	T.eq(str(out["result"]["amount"]), "4.993742", "amount")
	T.eq((s[1] as Dictionary).keys(), [ScfgReceipts.receipt_key("arc", TX, 3)], "one receipt")
	return T.done()


func test_arc_native_send_with_dust_matches_session() -> bool:
	var s := _session()
	(s[2] as F.FakeChain).logs.append(_emitter_log(_wei(UNITS, NATIVE - 1), {"block": 998}))
	T.eq((await _step(s))["status"], "paid", "dust floors away")
	return T.done()


func test_arc_transfer_counts_once_in_scan() -> bool:
	var s := _session()
	(s[2] as F.FakeChain).logs.append_array(_arc_transfer_tx(UNITS))
	T.eq((await _step(s))["status"], "paid", "paid")
	T.eq((s[1] as Dictionary).keys(), [ScfgReceipts.receipt_key("arc", TX, 5)], "once")
	return T.done()


func test_arc_interface_log_alone_is_not_seen_by_scan() -> bool:
	var s := _session()
	(s[2] as F.FakeChain).logs.append(_arc_transfer_tx(UNITS)[1])
	T.eq((await _step(s))["status"], "waiting", "not seen")
	return T.done()


func test_arc_hint_verifies_from_emitter_log() -> bool:
	var s := _session()
	_hint(s, _arc_transfer_tx(UNITS + 1000))  # an exchange-style amount
	var out: Variant = await _step(s)
	T.eq(out["status"], "paid", "paid")
	T.eq(str(out["result"]["amount"]), "4.994742", "amount")
	T.eq((s[1] as Dictionary).keys(), [ScfgReceipts.receipt_key("arc", TX, 5)], "the emitter log's receipt")
	return T.done()


func test_arc_hint_underpaid_uses_floored_value() -> bool:
	var s := _session()
	_hint(s, _arc_transfer_tx(0, {"native": _wei(4_989_999, NATIVE - 1)}))
	var out: Variant = await _step(s)
	T.eq(out["status"], "underpaid", "underpaid")
	T.eq(str(out["result"]["amount"]), "4.989999", "floored amount")
	return T.done()


func test_arc_hint_with_only_interface_log_is_not_a_payment() -> bool:
	var s := _session()
	_hint(s, _arc_transfer_tx(UNITS).slice(1))
	T.eq((await _step(s))["error"], "tx_not_a_payment", "not a payment")
	return T.done()


# --- restore and re-verification on Arc ---------------------------------------------------------


func test_arc_restore_returns_one_candidate() -> bool:
	var config: Variant = ScfgConfig.build_config({"wallet": DEV, "chains": ["arc"]})
	ScfgConfig.add_item(config, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	var rpc := F.FakeChain.new()
	rpc.receipts[TX] = {"status": "0x1", "logs": _arc_transfer_tx(UNITS)}
	var out: Variant = await ScfgRestore.lookup_evm(TX, {"config": config, "chain": ARC, "rpc": rpc, "receipts": {}})
	T.ok(out is Array and out.size() == 1, "one candidate")
	if out is Array and out.size() == 1:
		var c: Dictionary = out[0]
		T.eq([c["token"], _value(c["transfer"]), c["transfer"]["log_index"], c["kind"], c["item"]],
			["USDC", str(UNITS), 5, "unlock", "chapter2"], "the emitter's log, not 0x3600's")
	return T.done()


## The receipt of an Arc payment (test_arc_logs.py arc_receipt).
func _arc_receipt(value: int = UNITS, log_index: int = 5, block: int = 998) -> Variant:
	var t := {"token": ScfgChains.ARC_USDC, "sender": PAYER, "recipient": DEV, "value": ScfgBigInt.from_int(value),
		"tx_hash": TX, "log_index": log_index, "block_number": block, "block_hash": "0x" + "11".repeat(32)}
	return ScfgReceipts.make_receipt({"chain": "arc", "transfer": t, "kind": "unlock", "item": "chapter2",
		"token": "USDC", "source": "payment"})


func _check(r: Variant, rpc: F.FakeChain) -> Variant:
	return await ScfgReverify.check_evm_receipt(r, {"rpc": rpc, "chain": ARC, "wallets": [DEV]})


func test_reverify_keeps_arc_receipt() -> bool:
	var rpc := F.FakeChain.new(1000)
	rpc.logs.append_array(_arc_transfer_tx(UNITS, {"native": _wei(UNITS, 123)}))
	T.eq(await _check(_arc_receipt(), rpc), "keep", "kept")
	T.eq(rpc.last_get_logs[0], ScfgChains.ARC_SYSTEM_EMITTER, "asked the emitter")
	return T.done()


func test_reverify_drops_arc_receipt_with_wrong_amount() -> bool:
	var rpc := F.FakeChain.new(1000)
	rpc.logs.append_array(_arc_transfer_tx(UNITS))
	T.eq(await _check(_arc_receipt(UNITS + 1), rpc), "drop", "dropped")
	return T.done()


## A game that added Arc itself with add_chain("arc", ...) before it was built in stored receipts
## at the 0x3600 log (index emitter + 1). They must survive the update.
func test_reverify_keeps_legacy_custom_arc_receipt() -> bool:
	var rpc := F.FakeChain.new(1000)
	rpc.logs.append_array(_arc_transfer_tx(UNITS))
	T.eq(await _check(_arc_receipt(UNITS, 6), rpc), "keep", "kept")
	T.eq(rpc.calls, ["block_number", "get_logs", "get_logs"], "the emitter, then the token contract")
	T.eq(rpc.last_get_logs[0], ScfgChains.ARC_USDC, "the second ask is the token contract")
	return T.done()


func test_reverify_drops_legacy_arc_receipt_with_wrong_amount() -> bool:
	var rpc := F.FakeChain.new(1000)
	rpc.logs.append_array(_arc_transfer_tx(UNITS))
	T.eq(await _check(_arc_receipt(UNITS + 1, 6), rpc), "drop", "dropped")
	return T.done()


func test_reverify_drops_arc_receipt_with_no_log_at_all() -> bool:
	T.eq(await _check(_arc_receipt(), F.FakeChain.new(1000)), "drop", "dropped")
	return T.done()


## The 0x3600 fallback is only reached after the emitter lookup fails to match, and network
## trouble on it still keeps the receipt.
func test_reverify_keeps_arc_receipt_when_the_second_lookup_has_trouble() -> bool:
	var rpc := F.FakeChain.new(1000)
	var chain := Flaky.new(rpc)
	T.eq(await ScfgReverify.check_evm_receipt(_arc_receipt(), {"rpc": chain, "chain": ARC, "wallets": [DEV]}), "keep",
		"an outage on the second get_logs")
	return T.done()


## Delegates to a FakeChain; the second get_logs answers an RpcUnavailable.
class Flaky:
	extends RefCounted
	var inner: F.FakeChain
	var asks := 0

	func _init(inner_: F.FakeChain) -> void:
		inner = inner_

	func block_number() -> Variant:
		return await inner.block_number()

	func get_logs(address: Variant, topics: Variant, from_block: int, to_block: int) -> Variant:
		asks += 1
		if asks >= 2:
			return ScfgErr.make("RpcUnavailable", "second ask down")
		return await inner.get_logs(address, topics, from_block, to_block)
