extends RefCounted

const T := preload("res://tests/t.gd")

# Official EIP-55 test vectors
const EIP55: Array[String] = [
	"0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed",
	"0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
	"0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB",
	"0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb",
]
const USDC := "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"
const TOPIC := "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef"


func _log() -> Dictionary:
	return {
		"address": USDC.to_lower(),
		"blockHash": "0x" + "CD".repeat(32),
		"blockNumber": "0x12d687",
		"data": "0x00000000000000000000000000000000000000000000000000000000004c2430",
		"logIndex": "0x7",
		"removed": false,
		"topics": [TOPIC, "0x000000000000000000000000" + EIP55[0].substr(2).to_lower(),
			"0x000000000000000000000000" + EIP55[1].substr(2).to_lower()],
		"transactionHash": "0x" + "AB".repeat(32),
	}


func _flip_case(ch: String) -> String:
	return ch.to_upper() if ch != ch.to_upper() else ch.to_lower()


func test_transfer_topic_constant() -> bool:
	T.eq(ScfgEvm.TRANSFER_TOPIC, TOPIC, "TRANSFER_TOPIC")
	T.eq(ScfgEvm.TRANSFER_TOPIC,
		"0x" + ScfgBytes.bytes_to_hex(ScfgKeccak.keccak256("Transfer(address,address,uint256)".to_ascii_buffer())),
		"it is keccak256 of the event signature")
	return T.done()


func test_checksum_from_lowercase_and_uppercase_body() -> bool:
	for a in EIP55:
		T.eq(ScfgEvm.to_checksum_address(a.to_lower()), a, "lowercase %s" % a)
		T.eq(ScfgEvm.to_checksum_address("0x" + a.substr(2).to_upper()), a, "uppercase body %s" % a)
		T.eq(ScfgEvm.to_checksum_address(a), a, "already checksummed %s" % a)
	return T.done()


func test_checksum_rejects_malformed() -> bool:
	for bad: Variant in ["", "0x", "5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed",
			"0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAe", "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAedd",
			"0xZZAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", "0X5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed",
			" 0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed\n\n",
			"0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAe\n", null, 42, true, 4.5, [], {}]:
		T.is_err(ScfgEvm.to_checksum_address(bad), "ValueError", "to_checksum_address(%s)" % ScfgPy.py_repr(bad))
	return T.done()


## The whole string must be the address (Python's fullmatch): a trailing newline is refused.
func test_checksum_rejects_a_trailing_newline() -> bool:
	for bad: String in ["0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed\n", "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed\n",
			"\n0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed\r\n"]:
		T.is_err(ScfgEvm.to_checksum_address(bad), "ValueError", "to_checksum_address(%s)" % ScfgPy.py_repr(bad))
		T.is_err(ScfgEvm.normalize_address(bad), "ValueError", "normalize_address(%s)" % ScfgPy.py_repr(bad))
		T.eq(ScfgEvm.is_checksum_address(bad), false, "is_checksum_address(%s)" % ScfgPy.py_repr(bad))
	return T.done()


func test_is_checksum_address() -> bool:
	T.eq(ScfgEvm.is_checksum_address(EIP55[0]), true, "checksummed")
	T.eq(ScfgEvm.is_checksum_address(EIP55[0].to_lower()), false, "lowercase")
	T.eq(ScfgEvm.is_checksum_address("nonsense"), false, "nonsense")
	T.eq(ScfgEvm.is_checksum_address(5), false, "a non-string never raises")
	T.eq(ScfgEvm.is_checksum_address(null), false, "None never raises")
	return T.done()


func test_normalize_address() -> bool:
	T.eq(ScfgEvm.normalize_address(EIP55[1].to_lower()), EIP55[1], "single case accepted")
	T.eq(ScfgEvm.normalize_address("0x" + EIP55[1].substr(2).to_upper()), EIP55[1], "upper body accepted")
	T.eq(ScfgEvm.normalize_address(EIP55[2]), EIP55[2], "correct mixed case accepted")
	var wrong := EIP55[0].substr(0, 3) + _flip_case(EIP55[0][3]) + EIP55[0].substr(4)
	var e: Variant = ScfgEvm.normalize_address(wrong)
	T.is_err(e, "ValueError", "a flipped letter is a wrong checksum")
	T.ok(e is ScfgErr and "checksum" in (e as ScfgErr).message, "the message names the checksum")
	T.is_err(ScfgEvm.normalize_address(7), "ValueError", "int")
	T.is_err(ScfgEvm.normalize_address(""), "ValueError", "empty")
	return T.done()


func test_address_topic_is_left_padded_lowercase() -> bool:
	var topic: Variant = ScfgEvm.address_topic(EIP55[0])
	T.eq(topic, "0x000000000000000000000000" + EIP55[0].substr(2).to_lower(), "padded")
	T.eq((topic as String).length(), 66, "66 characters")
	T.eq(ScfgEvm.address_topic("0x" + "0".repeat(40)), "0x" + "0".repeat(64), "zero address")
	T.is_err(ScfgEvm.address_topic("0x1234"), "ValueError", "short")
	return T.done()


func test_parse_transfer_log_fields_in_python_order() -> bool:
	var t: Variant = ScfgEvm.parse_transfer_log(_log())
	T.ok(t is Dictionary, "a Dictionary")
	if not t is Dictionary:
		return T.done()
	T.eq(t.keys(), ["token", "sender", "recipient", "value", "tx_hash", "log_index", "block_number",
		"block_hash"], "the dataclass fields, in order")
	T.eq(t["token"], USDC, "token checksummed")
	T.eq(t["sender"], EIP55[0], "sender checksummed")
	T.eq(t["recipient"], EIP55[1], "recipient checksummed")
	T.ok(t["value"] is ScfgBigInt and (t["value"] as ScfgBigInt).to_decimal() == "4990000", "value is a ScfgBigInt")
	T.eq(t["tx_hash"], "0x" + "ab".repeat(32), "tx_hash lowercased")
	T.eq(t["log_index"], 7, "log_index")
	T.eq(t["block_number"], 1234567, "block_number")
	T.eq(t["block_hash"], "0x" + "cd".repeat(32), "block_hash lowercased")
	return T.done()


func test_parse_transfer_log_skips_other_events() -> bool:
	var l := _log()
	l["topics"] = [TOPIC.to_upper().replace("0X", "0x")] + (_log()["topics"] as Array).slice(1)
	T.ok(ScfgEvm.parse_transfer_log(l) is Dictionary, "topic0 compares case-insensitively")
	l = _log()
	l["topics"] = (_log()["topics"] as Array).slice(0, 2)
	T.eq(ScfgEvm.parse_transfer_log(l), null, "two topics")
	l["topics"] = []
	T.eq(ScfgEvm.parse_transfer_log(l), null, "no topics")
	l["topics"] = null
	T.eq(ScfgEvm.parse_transfer_log(l), null, "topics: null is empty")
	l["topics"] = _log()["topics"] + ["0x" + "0".repeat(63) + "5"]
	T.eq(ScfgEvm.parse_transfer_log(l), null, "an ERC-721 Transfer has 4 topics")
	l = _log()
	l["topics"] = ["0x" + "11".repeat(32)] + (_log()["topics"] as Array).slice(1)
	T.eq(ScfgEvm.parse_transfer_log(l), null, "another event")
	l = _log()
	l["removed"] = true
	T.eq(ScfgEvm.parse_transfer_log(l), null, "removed")
	l["removed"] = 1
	T.eq(ScfgEvm.parse_transfer_log(l), null, "removed is tested for truthiness")
	l["removed"] = null
	T.ok(ScfgEvm.parse_transfer_log(l) is Dictionary, "removed: null is falsy")
	# removed is checked before anything else can fail
	l = {"removed": true, "topics": [TOPIC, "x", "y"]}
	T.eq(ScfgEvm.parse_transfer_log(l), null, "removed wins over malformed fields")
	return T.done()


func test_parse_transfer_log_data() -> bool:
	var l := _log()
	# anything but one 32-byte word ("0x" + 64 hex digits) is not a transfer, never an error
	for d: Variant in ["0x", "", null, 0, false, 5, "0x01", "0xzz", "zz", "0x 1", "1.5", "0x1g",
			"0x" + "00".repeat(31), "0x" + "00".repeat(33), "0X" + "00".repeat(32), "00".repeat(33),
			"0x" + "0_".repeat(32), "0x" + "00".repeat(31) + " 1", &"0x01", ["0x" + "00".repeat(32)]]:
		l["data"] = d
		T.eq(ScfgEvm.parse_transfer_log(l), null, "data %s" % ScfgPy.py_repr(d))
	l.erase("data")
	T.eq(ScfgEvm.parse_transfer_log(l), null, "no data")
	l["data"] = "0x" + "0".repeat(64)
	T.ok((ScfgEvm.parse_transfer_log(l)["value"] as ScfgBigInt).is_zero(), "a zero word")
	l["data"] = "0x" + "F".repeat(64)
	T.eq((ScfgEvm.parse_transfer_log(l)["value"] as ScfgBigInt).to_hex(), "f".repeat(64), "upper-case hex")
	l["data"] = "0x" + "f".repeat(64)
	T.eq((ScfgEvm.parse_transfer_log(l)["value"] as ScfgBigInt).to_decimal(),
		"115792089237316195423570985008687907853269984665640564039457584007913129639935", "uint256 max")
	l["data"] = "0x000000000000000000000000ffffffffffffffffffffffffffffffffffffffff"
	T.eq((ScfgEvm.parse_transfer_log(l)["value"] as ScfgBigInt).to_hex(), "f".repeat(40), "a 160-bit value")
	return T.done()


## M1 lessons: a non-string hash must not slip through as a value, and indices at or above 2^53
## must not lose precision.
func test_parse_transfer_log_rejects_bad_field_types() -> bool:
	for field: String in ["transactionHash", "blockHash"]:
		for bad: Variant in [null, 5, true, ["0xab"], {}]:
			var l := _log()
			l[field] = bad
			T.is_err(ScfgEvm.parse_transfer_log(l), "TypeError", "%s = %s" % [field, ScfgPy.py_repr(bad)])
	for field: String in ["logIndex", "blockNumber", "address"]:
		for bad: Variant in [null, 5, true, 1.5]:
			var l := _log()
			l[field] = bad
			var r: Variant = ScfgEvm.parse_transfer_log(l)
			T.ok(r is ScfgErr and ((r as ScfgErr).is_a("TypeError") or (r as ScfgErr).is_a("ValueError")),
				"%s = %s is rejected (%s)" % [field, ScfgPy.py_repr(bad), r])
	for field: String in ["address", "transactionHash", "logIndex", "blockNumber", "blockHash"]:
		var l := _log()
		l.erase(field)
		T.is_err(ScfgEvm.parse_transfer_log(l), "KeyError", "missing %s" % field)
	var l := _log()
	l.erase("topics")
	T.eq(ScfgEvm.parse_transfer_log(l), null, "missing topics is an empty list")
	l = _log()
	l["topics"] = [TOPIC, "0x1234", (_log()["topics"] as Array)[2]]
	T.is_err(ScfgEvm.parse_transfer_log(l), "ValueError", "a topic too short to hold an address")
	l["topics"] = [TOPIC, 5, (_log()["topics"] as Array)[2]]
	T.ok(ScfgEvm.parse_transfer_log(l) is ScfgErr, "a non-string topic")
	T.is_err(ScfgEvm.parse_transfer_log(5), "AttributeError", "a log that is not an object")
	return T.done()


func test_parse_transfer_log_index_and_block_limits() -> bool:
	var l := _log()
	l["logIndex"] = "0x1fffffffffffff"  # 2^53 - 1
	l["blockNumber"] = "0x1fffffffffffff"
	var t: Variant = ScfgEvm.parse_transfer_log(l)
	T.ok(t is Dictionary and t["log_index"] == 9007199254740991 and t["block_number"] == 9007199254740991,
		"2^53 - 1 is exact")
	l["logIndex"] = "0x20000000000000"  # 2^53
	T.is_err(ScfgEvm.parse_transfer_log(l), "ValueError", "logIndex 2^53")
	l["logIndex"] = "0x7"
	l["blockNumber"] = "0x20000000000000"
	T.is_err(ScfgEvm.parse_transfer_log(l), "ValueError", "blockNumber 2^53")
	l["blockNumber"] = "0x" + "f".repeat(20)  # beyond 64 bits
	T.is_err(ScfgEvm.parse_transfer_log(l), "ValueError", "blockNumber beyond 2^64")
	l["blockNumber"] = "12"  # int(text, 16) takes digits without a prefix
	T.eq(ScfgEvm.parse_transfer_log(l)["block_number"], 18, "no 0x prefix")
	return T.done()


func test_parse_transfer_log_invalid_addresses() -> bool:
	var l := _log()
	l["address"] = "0x1234"
	T.is_err(ScfgEvm.parse_transfer_log(l), "ValueError", "short contract address")
	l = _log()
	l["address"] = USDC.to_upper().replace("0X", "0x")
	T.eq(ScfgEvm.parse_transfer_log(l)["token"], USDC, "uppercase address is checksummed")
	return T.done()
