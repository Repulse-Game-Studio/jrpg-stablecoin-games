extends RefCounted

const T := preload("res://tests/t.gd")

# Keypair.from_seed([i] * 32) public keys; blockhash = Hash([9] * 32)
const PAYER := "AKnL4NNf3DGWZJS6cPknBuEGnVsV4A4m5tgebLHaRSZ9"
const K: Array[String] = [
	"9hSR6S7WPtxmTojgo6GG3k4yDPecgJY292j7xrsUGWBu", "GyGKxMyg1p9SsHfm15MkNUu1u9TN2JtTspcdmrtGUdse",
	"EdmxWPmx2WH6WgFfTdu9xfkYf3k1g5wD1zccTVySEEh1", "8SFqwqnq4whPhs8icwHA2hQg3hUoN1qrCLK1SBx3WKwe"]
const BLOCKHASH := "cGfHiC6Kgg3FpFZvgwGcswsCRtp4aBP2fzuXRQPizuN"
const WRITABLE_SIGNER := "AKkzLhjhyFtM9j7WAhbaqYpFe49cXeJBg2kzLRC2PnNa"
const READONLY_SIGNER := "GmaDrppBC7P5ARKV8g3djiwP89vz1jLK23V2GBjuAEGB"
const PROGRAM := "2KW2XRd9kwqet15Aha2oK3tYvd3nWbTFH1MBiRAv1BE1"


func _hex(b: Variant) -> String:
	return ScfgBytes.bytes_to_hex(b) if b is PackedByteArray else "<%s>" % b


# tests/unit/test_solana_tx.py::test_compact_u16
func test_compact_u16() -> bool:
	var table := [[0, "00"], [127, "7f"], [128, "8001"], [255, "ff01"], [16383, "ff7f"], [16384, "808001"],
		[65534, "feff03"], [65535, "ffff03"]]
	for row: Array in table:
		T.eq(_hex(ScfgSolanaTx.compact_u16(row[0])), row[1], "compact_u16(%d)" % row[0])
	for bad: Variant in [-1, 65536, 100000]:
		T.is_err(ScfgSolanaTx.compact_u16(bad), "ValueError", "compact_u16(%s)" % bad)
	for bad: Variant in ["1", null, 1.5, [1]]:
		T.is_err(ScfgSolanaTx.compact_u16(bad), "TypeError", "compact_u16(%s)" % ScfgPy.py_repr(bad))
	T.eq(_hex(ScfgSolanaTx.compact_u16(true)), "01", "a bool is an int in Python")
	return T.done()


func test_compute_budget_program_id() -> bool:
	T.eq(ScfgSolanaTx.COMPUTE_BUDGET_PROGRAM_ID, "ComputeBudget111111111111111111111111111111", "program id")
	return T.done()


func test_instruction_data() -> bool:
	var limit: Dictionary = ScfgSolanaTx.set_compute_unit_limit(30000)
	T.eq(limit.keys(), ["program_id", "accounts", "data"], "Instruction fields in Python order")
	T.eq(limit["program_id"], ScfgSolanaTx.COMPUTE_BUDGET_PROGRAM_ID, "program")
	T.eq(limit["accounts"], [], "no accounts")
	T.eq(_hex(limit["data"]), "02" + "30750000", "limit data: 2 + u32 LE")
	T.eq(_hex(ScfgSolanaTx.set_compute_unit_price(5000)["data"]), "03" + "8813000000000000", "price data: 3 + u64 LE")
	var ix: Variant = ScfgSolanaTx.transfer_checked({"source": K[0], "mint": K[1], "destination": K[2],
		"owner": K[3], "amount": 4993742, "decimals": 6, "extra": [PAYER]})
	T.ok(ix is Dictionary, "an instruction")
	if ix is Dictionary:
		T.eq(ix["program_id"], ScfgSolanaPda.TOKEN_PROGRAM_ID, "token program")
		T.eq(_hex(ix["data"]), "0cce324c000000000006", "12 + u64 LE amount + decimals")
		var got := []
		for a: Dictionary in ix["accounts"]:
			T.eq(a.keys(), ["pubkey", "is_signer", "is_writable"], "AccountMeta fields in Python order")
			got.append([a["pubkey"], a["is_signer"], a["is_writable"]])
		T.eq(got, [[K[0], false, true], [K[1], false, false], [K[2], false, true], [K[3], true, false],
			[PAYER, false, false]], "accounts: source, mint, destination, owner(signer), extra read-only")
	return T.done()


func test_instruction_number_limits() -> bool:
	T.eq(_hex(ScfgSolanaTx.set_compute_unit_limit(4294967295)["data"]), "02ffffffff", "u32 max")
	T.is_err(ScfgSolanaTx.set_compute_unit_limit(4294967296), "OverflowError", "u32 overflow")
	T.is_err(ScfgSolanaTx.set_compute_unit_limit(-1), "OverflowError", "negative")
	T.is_err(ScfgSolanaTx.set_compute_unit_limit("5"), "AttributeError", "a str has no to_bytes")
	var u64max: Variant = ScfgBigInt.from_decimal("18446744073709551615")  # err-ok: literal
	T.eq(_hex(ScfgSolanaTx.set_compute_unit_price(u64max)["data"]), "03ffffffffffffffff", "u64 max as a ScfgBigInt")
	var u64over: Variant = ScfgBigInt.from_decimal("18446744073709551616")  # err-ok: literal
	T.is_err(ScfgSolanaTx.set_compute_unit_price(u64over), "OverflowError", "u64 overflow")
	T.eq(_hex(ScfgSolanaTx.set_compute_unit_price(9223372036854775807)["data"]), "03ffffffffffffff7f", "int max")
	var args := {"source": K[0], "mint": K[1], "destination": K[2], "owner": K[3], "amount": 1, "decimals": 6}
	var ok_ix: Variant = ScfgSolanaTx.transfer_checked(args)
	T.ok(ok_ix is Dictionary and (ok_ix["accounts"] as Array).size() == 4, "extra defaults to none")
	var bad := args.duplicate()
	bad["decimals"] = 256
	T.is_err(ScfgSolanaTx.transfer_checked(bad), "ValueError", "decimals 256 is not a byte")
	bad["decimals"] = -1
	T.is_err(ScfgSolanaTx.transfer_checked(bad), "ValueError", "decimals -1 is not a byte")
	bad = args.duplicate()
	bad["amount"] = -5
	T.is_err(ScfgSolanaTx.transfer_checked(bad), "OverflowError", "negative amount")
	bad = args.duplicate()
	bad["amount"] = u64over
	T.is_err(ScfgSolanaTx.transfer_checked(bad), "OverflowError", "amount 2^64")
	bad = args.duplicate()
	bad["amount"] = u64max
	T.ok(ScfgSolanaTx.transfer_checked(bad) is Dictionary, "amount 2^64-1")
	bad = args.duplicate()
	bad.erase("mint")
	T.is_err(ScfgSolanaTx.transfer_checked(bad), "TypeError", "a missing keyword argument")
	return T.done()


func _payment_ixs() -> Array:
	return [ScfgSolanaTx.set_compute_unit_limit(30000), ScfgSolanaTx.set_compute_unit_price(1000),
		ScfgSolanaTx.transfer_checked({"source": K[0], "mint": K[1], "destination": K[2], "owner": PAYER,
			"amount": 1000000, "decimals": 6, "extra": [K[3]]})]


const PAYMENT_MESSAGE := "010004078a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c8139770ea87d175f56a35466c34c7ecccb8d8a91b4ee37a25df60f5b8fc9b394ca93ac1705187071d67b83c7ff0efe8108e8ec4530575d7726879333dbdabe7c0306466fe5211732ffecadba72c39be7bc8ce5bbc5f7126b2c439b3a4000000006ddf6e1d765a193d9cbe146ceeb79ac1cb485ed5f5b37913a8cf5857eff00a96e7a1cdd29b0b78fd13af4c5598feff4ef2a97166e3ca6f2e4fbfccd80505bf1ed4928c628d1c2c6eae90338905995612959273a5c63f93636c14614ac8737d1090909090909090909090909090909090909090909090909090909090909090903030005023075000003000903e803000000000000040501060200050a0c40420f000000000006"
const MULTI_SIGNER_MESSAGE := "030102068a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c8a875fff1eb38451577acd5afee405456568dd7c89e090863a0557bc7af49f17ea4a6c63e29c520abef5507b132ec5f9954776aebebe7b92421eea691446d22c8139770ea87d175f56a35466c34c7ecccb8d8a91b4ee37a25df60f5b8fc9b3941398f62c6d1a457c51ba6a4b5f3dbd2f69fca93216218dc8997e416bd17d93caed4928c628d1c2c6eae90338905995612959273a5c63f93636c14614ac8737d1090909090909090909090909090909090909090909090909090909090909090901040501020305000101"


# Message.new_with_blockhash from solders, for the same instructions
func test_payment_message_is_byte_identical_to_solders() -> bool:
	T.eq(_hex(ScfgSolanaTx.compile_message(PAYER, _payment_ixs(), BLOCKHASH)), PAYMENT_MESSAGE, "payment message")
	return T.done()


func test_multi_signer_message_is_byte_identical_to_solders() -> bool:
	# co-signers exercise the writable-signer and readonly-signer groups and their ordering
	var ixs := [ScfgSolanaTx.instruction(PROGRAM, [
		ScfgSolanaTx.account_meta(WRITABLE_SIGNER, true, true),
		ScfgSolanaTx.account_meta(READONLY_SIGNER, true, false),
		ScfgSolanaTx.account_meta(K[0], false, true),
		ScfgSolanaTx.account_meta(K[1], false, false),
		ScfgSolanaTx.account_meta(PAYER, true, false)], PackedByteArray([1]))]
	T.eq(_hex(ScfgSolanaTx.compile_message(PAYER, ixs, BLOCKHASH)), MULTI_SIGNER_MESSAGE, "multi-signer message")
	return T.done()


func test_compile_message_errors() -> bool:
	T.is_err(ScfgSolanaTx.compile_message("abc", [], BLOCKHASH), "ValueError", "bad payer")
	T.is_err(ScfgSolanaTx.compile_message(PAYER, [], "abc"), "ValueError", "bad blockhash")
	T.is_err(ScfgSolanaTx.compile_message(PAYER, [ScfgSolanaTx.instruction("abc", [], PackedByteArray())], BLOCKHASH),
		"ValueError", "bad program id")
	var bad_meta := [ScfgSolanaTx.instruction(PROGRAM, [ScfgSolanaTx.account_meta("abc", false, false)],
		PackedByteArray())]
	T.is_err(ScfgSolanaTx.compile_message(PAYER, bad_meta, BLOCKHASH), "ValueError", "bad account")
	T.eq(_hex(ScfgSolanaTx.compile_message(PAYER, [], BLOCKHASH)).substr(0, 6), "010000", "no instructions: payer only")
	# 300 distinct read-only accounts do not fit a count byte (header byte 3)
	var metas := []
	for i in 300:
		var k := PackedByteArray()
		k.resize(32)
		k[0] = i % 256
		k[1] = i / 256
		k[2] = 1
		metas.append(ScfgSolanaTx.account_meta(ScfgSolanaBase58.b58encode(k), false, false))
	T.is_err(ScfgSolanaTx.compile_message(PAYER, [ScfgSolanaTx.instruction(PROGRAM, metas, PackedByteArray())], BLOCKHASH),
		"ValueError", "more than 255 read-only keys")
	return T.done()


func test_unsigned_transaction() -> bool:
	var msg: PackedByteArray = ScfgBytes.hex_to_bytes(PAYMENT_MESSAGE)  # err-ok: literal
	var tx: Variant = ScfgSolanaTx.unsigned_transaction(msg)
	T.ok(tx is PackedByteArray, "bytes")
	if tx is PackedByteArray:
		T.eq(_hex((tx as PackedByteArray).slice(0, 1)), "01", "one signature slot")
		T.eq((tx as PackedByteArray).slice(1, 65), _zeros(64), "empty signature")
		T.eq((tx as PackedByteArray).slice(65), msg, "then the message")
	var m3: PackedByteArray = ScfgBytes.hex_to_bytes(MULTI_SIGNER_MESSAGE)  # err-ok: literal
	var tx3: Variant = ScfgSolanaTx.unsigned_transaction(m3)
	T.eq((tx3 as PackedByteArray).size(), 1 + 3 * 64 + m3.size(), "three slots")
	T.eq(_hex(ScfgSolanaTx.unsigned_transaction(PackedByteArray([0]))), "0000", "zero signers")
	T.is_err(ScfgSolanaTx.unsigned_transaction(PackedByteArray()), "IndexError", "an empty message (message[0])")
	return T.done()


func _zeros(n: int) -> PackedByteArray:
	var z := PackedByteArray()
	z.resize(n)
	return z


## Malformed Variants are ScfgErr values, never GDScript runtime errors. Python's dataclasses take
## anything and compile_message rejects a non-base58 key as a ValueError (decode_fixed); a value
## that is not an instruction, an account list, an AccountMeta or bytes is a TypeError here
## (Python: AttributeError or TypeError).
func test_malformed_instructions_are_errors_not_crashes() -> bool:
	var meta: Dictionary = ScfgSolanaTx.account_meta(5, true, false)
	T.eq(meta, {"pubkey": 5, "is_signer": true, "is_writable": false}, "account_meta takes any value, as Python")
	T.is_err(ScfgSolanaTx.compile_message(PAYER, [ScfgSolanaTx.instruction(PROGRAM, [meta], PackedByteArray())], BLOCKHASH),
		"ValueError", "a non-string pubkey fails in compile_message (decode_fixed)")
	var ix: Dictionary = ScfgSolanaTx.instruction(null, [], PackedByteArray())
	T.is_err(ScfgSolanaTx.compile_message(PAYER, [ix], BLOCKHASH), "ValueError", "a None program id")
	var good := ScfgSolanaTx.instruction(PROGRAM, [ScfgSolanaTx.account_meta(K[0], false, true)], PackedByteArray([1]))
	var bad_cases := [
		["instructions not a list", "x"],
		["an instruction that is not a dict", [5]],
		["an instruction without accounts", [{"program_id": PROGRAM, "data": PackedByteArray()}]],
		["accounts not a list", [ScfgSolanaTx.instruction(PROGRAM, "x", PackedByteArray())]],
		["an account that is not a dict", [ScfgSolanaTx.instruction(PROGRAM, [5], PackedByteArray())]],
		["an account without is_writable", [ScfgSolanaTx.instruction(PROGRAM, [{"pubkey": K[0], "is_signer": false}],
			PackedByteArray())]],
		["data that is not bytes", [ScfgSolanaTx.instruction(PROGRAM, [], "ab")]],
		["one good, one bad", [good, 7]],
	]
	for row: Array in bad_cases:
		T.is_err(ScfgSolanaTx.compile_message(PAYER, row[1], BLOCKHASH), "TypeError", row[0])
	T.is_err(ScfgSolanaTx.transfer_checked({"source": K[0], "mint": K[1], "destination": K[2], "owner": K[3],
		"amount": 1, "decimals": 6, "extra": 5}), "TypeError", "extra is not iterable")
	var odd: Variant = ScfgSolanaTx.transfer_checked({"source": 5, "mint": K[1], "destination": K[2], "owner": K[3],
		"amount": 1, "decimals": 6})
	T.ok(odd is Dictionary, "transfer_checked builds the instruction whatever the keys are, as Python")
	T.is_err(ScfgSolanaTx.compile_message(PAYER, [odd], BLOCKHASH), "ValueError", "and compile_message rejects it")
	return T.done()
