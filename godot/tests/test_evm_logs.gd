## ScfgEvm.parse_transfer_log: the twin of tests/unit/test_evm_logs.py (test_evm.gd holds the
## GDScript-only cases).
extends RefCounted

const T := preload("res://tests/t.gd")

const USDC := "0xaf88d065e77c8cC2239327C5EDb3A432268e5831"
const PAYER := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"


func _make_log(overrides: Dictionary = {}) -> Dictionary:
	var log := {
		"address": USDC.to_lower(),
		"topics": [ScfgEvm.TRANSFER_TOPIC, ScfgEvm.address_topic(PAYER), ScfgEvm.address_topic(DEV)],
		"data": "0x" + ("%x" % 4_993_742).lpad(64, "0"),
		"transactionHash": "0xABCDEF" + "0".repeat(58),
		"logIndex": "0xc",
		"blockNumber": "0xffb2a46",
		"blockHash": "0x" + "11".repeat(32),
		"removed": false,
	}
	log.merge(overrides, true)
	return log


func test_parses_erc20_transfer() -> bool:
	var t: Variant = ScfgEvm.parse_transfer_log(_make_log())
	T.ok(t is Dictionary, "parsed")
	if not t is Dictionary:
		return T.done()
	T.eq([t["token"], t["sender"], t["recipient"]], [USDC, PAYER, DEV], "addresses")
	T.eq((t["value"] as ScfgBigInt).to_decimal(), "4993742", "value")
	T.eq(t["tx_hash"], "0xabcdef" + "0".repeat(58), "tx hash lowercased")
	T.eq([t["log_index"], t["block_number"]], [12, 0xFFB2A46], "log index and block")
	T.eq(t["block_hash"], "0x" + "11".repeat(32), "block hash")
	return T.done()


func test_topic0_comparison_is_case_insensitive() -> bool:
	var upper := ScfgEvm.TRANSFER_TOPIC.to_upper().replace("0X", "0x")
	T.ok(ScfgEvm.parse_transfer_log(_make_log({"topics": [upper, ScfgEvm.address_topic(PAYER),
		ScfgEvm.address_topic(DEV)]})) is Dictionary, "an uppercase topic0")
	return T.done()


func test_ignores_other_events() -> bool:
	T.eq(ScfgEvm.parse_transfer_log(_make_log({"topics": ["0x" + "ab".repeat(32), ScfgEvm.address_topic(PAYER),
		ScfgEvm.address_topic(DEV)]})), null, "another event")
	return T.done()


func test_ignores_erc721_transfer_with_four_topics() -> bool:
	T.eq(ScfgEvm.parse_transfer_log(_make_log({"topics": [ScfgEvm.TRANSFER_TOPIC, ScfgEvm.address_topic(PAYER),
		ScfgEvm.address_topic(DEV), "0x" + "00".repeat(31) + "07"]})), null, "four topics")
	return T.done()


func test_ignores_removed_logs() -> bool:
	T.eq(ScfgEvm.parse_transfer_log(_make_log({"removed": true})), null, "removed")
	return T.done()


func test_missing_removed_field_is_fine() -> bool:
	var log := _make_log()
	log.erase("removed")
	T.ok(ScfgEvm.parse_transfer_log(log) is Dictionary, "no removed field")
	return T.done()


func test_empty_data_means_zero_value() -> bool:
	var t: Variant = ScfgEvm.parse_transfer_log(_make_log({"data": "0x"}))
	T.ok(t is Dictionary and (t["value"] as ScfgBigInt).is_zero(), "0x is zero")
	return T.done()
