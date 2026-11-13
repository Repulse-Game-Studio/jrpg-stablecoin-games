extends RefCounted

const T := preload("res://tests/t.gd")

const EVM_TX := "0xabababababababababababababababababababababababababababababababab"
const SOL_ACCOUNT := "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3"


# tests/unit/test_page_messages.py::test_tx_message_with_a_checksummed_sender
func test_tx_message_with_a_checksummed_sender() -> bool:
	T.eq(ScfgPageMessages.parse_tx_message({"tx_hash": EVM_TX.to_upper().replace("0X", "0x"),
		"from": "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed"}),
		[EVM_TX, "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"], "normalized hash and checksummed sender")
	return T.done()


# tests/unit/test_page_messages.py::test_a_bad_sender_is_dropped_not_fatal
func test_a_bad_sender_is_dropped_not_fatal() -> bool:
	for data: Dictionary in [{"tx_hash": EVM_TX, "from": "nonsense"}, {"tx_hash": EVM_TX},
			{"tx_hash": EVM_TX, "from": 5}, {"tx_hash": EVM_TX, "from": []}, {"tx_hash": EVM_TX, "from": true},
			{"tx_hash": EVM_TX, "from": "  0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed"}]:
		T.eq(ScfgPageMessages.parse_tx_message(data), [EVM_TX, null], ScfgPy.py_repr(data))
	T.eq(ScfgPageMessages.parse_tx_message({"tx_hash": EVM_TX, "from": " " + SOL_ACCOUNT + " "}),
		[EVM_TX, SOL_ACCOUNT], "a Solana sender is normalized")
	return T.done()


# tests/unit/test_page_messages.py::test_bad_tx_messages_raise
func test_bad_tx_messages_raise() -> bool:
	for data: Variant in [null, [], "x", {}, {"tx_hash": 5}, {"tx_hash": "0x12"}, {"tx_hash": null}]:
		T.is_err(ScfgPageMessages.parse_tx_message(data), "ValueError", ScfgPy.py_repr(data))
	return T.done()


# tests/unit/test_page_messages.py::test_build_message
func test_build_message() -> bool:
	T.eq(ScfgPageMessages.parse_build_message({"account": SOL_ACCOUNT}), SOL_ACCOUNT, "the account")
	T.eq(ScfgPageMessages.parse_build_message({"account": " " + SOL_ACCOUNT + "\n"}), SOL_ACCOUNT, "stripped")
	T.eq(ScfgPageMessages.BUILD_TIMEOUT_S, 15, "BUILD_TIMEOUT_S")
	return T.done()


# tests/unit/test_page_messages.py::test_bad_build_messages_raise
func test_bad_build_messages_raise() -> bool:
	for data: Variant in [null, {}, {"account": 5}, {"account": "0xabc"}, [], {"account": null}]:
		T.is_err(ScfgPageMessages.parse_build_message(data), "ValueError", ScfgPy.py_repr(data))
	return T.done()
