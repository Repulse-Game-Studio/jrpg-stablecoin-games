extends RefCounted

const T := preload("res://tests/t.gd")

const EVM_TX := "0xabababababababababababababababababababababababababababababababab"


func test_normalize_tx_hash() -> bool:
	T.eq(ScfgSessions.normalize_tx_hash(" " + EVM_TX.to_upper().replace("0X", "0x") + "　"), EVM_TX,
		"str.strip() and lower()")
	for bad: Variant in ["0X" + "ab".repeat(32), EVM_TX.substr(2), EVM_TX + "ab", "0x" + "１".repeat(64), 5, null,
			EVM_TX + "\n" + EVM_TX, "0x" + "ab".repeat(16) + " " + "ab".repeat(16)]:
		T.is_err(ScfgSessions.normalize_tx_hash(bad), "ValueError", ScfgPy.py_repr(bad))
	return T.done()


func test_normalize_any_tx_tries_evm_then_solana() -> bool:
	T.eq(ScfgBackends.normalize_any_tx(" " + EVM_TX + " "), EVM_TX, "an EVM hash")
	var sig := ScfgSolanaBase58.b58encode(PackedByteArray(range(64)))
	T.eq(ScfgBackends.normalize_any_tx("\t" + sig + "\n"), sig, "a Solana signature")
	for bad: Variant in ["garbage", 5, null, [], "1".repeat(10000)]:
		var e: Variant = ScfgBackends.normalize_any_tx(bad)
		T.is_err(e, "ValueError", ScfgPy.py_repr(bad))
	return T.done()
