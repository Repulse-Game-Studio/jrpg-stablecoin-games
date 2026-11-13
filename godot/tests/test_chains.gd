extends RefCounted

const T := preload("res://tests/t.gd")

const PINNED := {
	"ethereum:USDC": "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48",
	"ethereum:USDT": "0xdAC17F958D2ee523a2206206994597C13D831ec7",
	"arbitrum:USDC": "0xaf88d065e77c8cC2239327C5EDb3A432268e5831",
	"arbitrum:USDT": "0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9",
	"polygon:USDC": "0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359",
	"polygon:USDT": "0xc2132D05D31c914a87C6611C10748AEb04B58e8F",
	"base:USDC": "0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913",
	"arc:USDC": "0x3600000000000000000000000000000000000000",
}
const CHAIN_FIELDS := ["key", "chain_id", "name", "rpc", "confirmations", "max_log_range", "explorer_tx",
	"native_symbol", "fee_hint", "tokens", "testnet", "family", "wallet_chain", "native_token", "min_fee_wei"]


func _evm(registry: Dictionary) -> Dictionary:
	var out := {}
	for k: String in registry:
		if registry[k]["family"] == "evm":
			out[k] = registry[k]
	return out


func test_chain_dictionaries_carry_python_fields_in_order() -> bool:
	for net: Dictionary in [ScfgChains.MAINNET, ScfgChains.TESTNET]:
		for k: String in net:
			T.eq(net[k].keys(), CHAIN_FIELDS, "%s fields" % k)
			for t: Dictionary in net[k]["tokens"].values():
				T.eq(t.keys(), ["address", "decimals", "log_address", "log_decimals"], "token fields")
	T.eq(ScfgChains.MAINNET.keys(), ["ethereum", "arbitrum", "polygon", "base", "arc", "solana"], "mainnet order")
	T.eq(ScfgChains.TESTNET.keys(), ["ethereum", "arbitrum", "polygon", "base", "arc", "solana"], "testnet order")
	return T.done()


func test_chain_ids() -> bool:
	var main := {}
	var test := {}
	for k: String in _evm(ScfgChains.MAINNET):
		main[k] = ScfgChains.MAINNET[k]["chain_id"]
	for k: String in _evm(ScfgChains.TESTNET):
		test[k] = ScfgChains.TESTNET[k]["chain_id"]
	T.eq(main, {"ethereum": 1, "arbitrum": 42161, "polygon": 137, "base": 8453, "arc": 5042}, "mainnet ids")
	T.eq(test, {"ethereum": 11155111, "arbitrum": 421614, "polygon": 80002, "base": 84532, "arc": 5042002},
		"testnet ids use the same keys")
	return T.done()


func test_pinned_mainnet_tokens() -> bool:
	var actual := {}
	var evm := _evm(ScfgChains.MAINNET)
	for k: String in evm:
		for sym: String in evm[k]["tokens"]:
			actual["%s:%s" % [k, sym]] = evm[k]["tokens"][sym]["address"]
	T.eq(actual, PINNED, "the mainnet token set is exactly the pinned one")
	T.eq(ScfgChains.MAINNET["base"]["tokens"].keys(), ["USDC"], "Base has no USDT")
	return T.done()


func test_chain_invariants() -> bool:
	var chains: Array = _evm(ScfgChains.MAINNET).values() + _evm(ScfgChains.TESTNET).values()
	for c: Dictionary in chains:
		T.ok(c["rpc"].size() > 0, "%s has rpc" % c["name"])
		for u: String in c["rpc"]:
			T.ok(u.begins_with("https://"), "%s rpc is https" % c["name"])
		T.ok(c["confirmations"] >= 1 and c["max_log_range"] >= 10, "%s limits" % c["name"])
		T.ok(c["explorer_tx"].contains("{}"), "%s explorer_tx" % c["name"])
		for t: Dictionary in c["tokens"].values():
			T.ok(ScfgEvm.is_checksum_address(t["address"]), "%s token is checksummed" % c["name"])
			T.eq(t["decimals"], 6, "6 decimals")
	return T.done()


func test_display_order() -> bool:
	T.eq(ScfgChains.DISPLAY_ORDER, ["arbitrum", "base", "arc", "polygon", "ethereum"], "Arc after the familiar L2s")
	var keys := _evm(ScfgChains.MAINNET).keys()
	keys.sort()
	var order := ScfgChains.DISPLAY_ORDER.duplicate()
	order.sort()
	T.eq(order, keys, "display order covers every EVM chain")
	T.ok(not "solana" in ScfgChains.DISPLAY_ORDER, "Solana is not in the EVM display order")
	return T.done()


func test_arc_usdc_logs_come_from_the_system_emitter() -> bool:
	T.eq(ScfgChains.ARC_USDC, "0x3600000000000000000000000000000000000000", "ARC_USDC")
	T.eq(ScfgChains.ARC_SYSTEM_EMITTER, "0xffffFFFfFFffffffffffffffFfFFFfffFFFfFFfE", "ARC_SYSTEM_EMITTER")
	T.eq(ScfgChains.ARC_MIN_FEE_WEI, 20000000000, "ARC_MIN_FEE_WEI")
	for testnet: bool in [false, true]:
		var arc: Dictionary = ScfgChains.get_chains(testnet)["arc"]
		var usdc: Dictionary = arc["tokens"]["USDC"]
		T.eq(usdc["address"], ScfgChains.ARC_USDC, "address")
		T.eq(ScfgChains.log_source(usdc), ScfgChains.ARC_SYSTEM_EMITTER, "log_source")
		T.eq([usdc["decimals"], usdc["log_decimals"]], [6, 18], "decimals")
		T.eq(arc["tokens"].keys(), ["USDC"], "only USDC")
		T.eq([arc["native_token"], arc["min_fee_wei"], arc["native_symbol"], arc["fee_hint"], arc["confirmations"]],
			["USDC", 20000000000, "USDC", "lowest", 1], "fee floor and native token")
		T.eq(arc["max_log_range"], 9999, "max_log_range")
		var host := "explorer.testnet.arc.io" if testnet else "explorer.arc.io"
		T.eq(ScfgChains.explorer_url(arc, "0xab"), "https://%s/tx/0xab" % host, "explorer_url")
		T.ok(not (arc["rpc"][-1] as String).contains("drpc"), "the last endpoint is never dRPC")
	T.eq(ScfgChains.TESTNET["arc"]["rpc"], ["https://rpc.testnet.arc.io", "https://rpc.drpc.testnet.arc.io",
		"https://rpc.quicknode.testnet.arc.io"], "testnet rpc")
	T.eq(ScfgChains.MAINNET["arc"]["rpc"], ["https://rpc.mainnet.arc.io", "https://rpc.quicknode.mainnet.arc.io",
		"https://rpc.drpc.mainnet.arc.io", "https://rpc.blockdaemon.mainnet.arc.io"], "mainnet rpc")
	return T.done()


func test_other_chains_have_no_log_source_or_fee_floor() -> bool:
	for net: Dictionary in [ScfgChains.MAINNET, ScfgChains.TESTNET]:
		for c: Dictionary in net.values():
			if c["key"] == "arc":
				continue
			T.eq([c["native_token"], c["min_fee_wei"]], ["", 0], "%s" % c["name"])
			for t: Dictionary in c["tokens"].values():
				T.eq(ScfgChains.log_source(t), t["address"], "log_source is the address")
	return T.done()


func test_get_chains_returns_the_mutable_registries() -> bool:
	T.ok(ScfgChains.get_chains(false) == ScfgChains.MAINNET and is_same(ScfgChains.get_chains(false), ScfgChains.MAINNET),
		"mainnet is the registry itself")
	T.ok(is_same(ScfgChains.get_chains(true), ScfgChains.TESTNET), "testnet is the registry itself")
	# the integration driver patches entries in place
	var saved: Variant = ScfgChains.MAINNET["base"]["rpc"]
	ScfgChains.MAINNET["base"]["rpc"] = ["http://127.0.0.1:8545"]
	T.eq(ScfgChains.get_chains(false)["base"]["rpc"], ["http://127.0.0.1:8545"], "a patched entry is visible")
	ScfgChains.MAINNET["base"]["rpc"] = saved
	return T.done()


func test_token_address_is_normalized_to_checksum() -> bool:
	var t: Variant = ScfgChains.make_token("0xaf88d065e77c8cc2239327c5edb3a432268e5831")
	T.ok(t is Dictionary, "a lowercase address is accepted")
	if t is Dictionary:
		T.eq(t["address"], PINNED["arbitrum:USDC"], "normalized")
		T.eq(t, {"address": PINNED["arbitrum:USDC"], "decimals": 6, "log_address": "", "log_decimals": 0}, "defaults")
	T.is_err(ScfgChains.make_token("not an address"), "ValueError", "bad EVM-looking address")
	T.is_err(ScfgChains.make_token("0x123"), "ValueError", "short address")
	var arc: Variant = ScfgChains.make_token(ScfgChains.ARC_USDC, {"log_address": ScfgChains.ARC_SYSTEM_EMITTER.to_lower(),
		"log_decimals": 18})
	T.ok(arc is Dictionary and arc["log_address"] == ScfgChains.ARC_SYSTEM_EMITTER, "log_address is checksummed too")
	T.is_err(ScfgChains.make_token(ScfgChains.ARC_USDC, {"log_address": ScfgChains.ARC_SYSTEM_EMITTER, "log_decimals": 3}),
		"ValueError", "log_decimals below decimals")
	T.is_err(ScfgChains.make_token(5), "AttributeError", "a non-str address, as .startswith in Python")
	T.is_err(ScfgChains.make_token(PINNED["arbitrum:USDC"], {"bogus": 1}), "TypeError", "unexpected keyword")
	return T.done()


func test_solana_mints_are_pinned() -> bool:
	var actual := {}
	for net: Array in [[false, ScfgChains.MAINNET["solana"]], [true, ScfgChains.TESTNET["solana"]]]:
		for sym: String in net[1]["tokens"]:
			actual["%s:%s" % [net[0], sym]] = net[1]["tokens"][sym]["address"]
	T.eq(actual, {
		"false:USDC": "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
		"false:USDT": "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB",
		"true:USDC": "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU"}, "mints")
	return T.done()


func test_solana_entries() -> bool:
	for testnet: bool in [false, true]:
		var c: Dictionary = ScfgChains.get_chains(testnet)["solana"]
		T.eq([c["family"], c["testnet"], c["native_symbol"], c["fee_hint"], c["confirmations"]],
			["solana", testnet, "SOL", "lowest", 32], "solana fields")
		T.eq(c["wallet_chain"], "solana:devnet" if testnet else "solana:mainnet", "wallet_chain")
		T.eq(c["rpc"], ["https://api.devnet.solana.com"] if testnet else
			["https://solana-rpc.publicnode.com", "https://api.mainnet-beta.solana.com"], "rpc")
		T.eq(ScfgChains.explorer_url(c, "SIG"), "https://solscan.io/tx/SIG?cluster=devnet" if testnet
			else "https://solscan.io/tx/SIG", "explorer_url")
		T.eq([c["chain_id"], c["max_log_range"]], [0, 0], "no chain id or log range")
	return T.done()


func _log(address: String, value_hex: String, topic0: String = ScfgEvm.TRANSFER_TOPIC) -> Dictionary:
	return {"address": address, "topics": [topic0, ScfgEvm.address_topic("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"),
		ScfgEvm.address_topic("0x52908400098527886E0F7030069857D2E4169EE7")],
		"data": "0x" + value_hex.lpad(64, "0"), "transactionHash": "0x" + "ab".repeat(32), "logIndex": "0x3",
		"blockNumber": "0x309", "blockHash": "0x" + "11".repeat(32)}


func test_parse_transfer_floors_arc_values_to_token_units() -> bool:
	var arc: Dictionary = ScfgChains.MAINNET["arc"]
	# 4.993742 USDC logged with 18 decimals, plus dust below one unit: floors to 4993742
	var value: Variant = ScfgBigInt.from_decimal("4993742000000999999")
	var t: Variant = ScfgChains.parse_transfer(arc, _log(ScfgChains.ARC_SYSTEM_EMITTER, (value as ScfgBigInt).to_hex()))
	T.ok(t is Dictionary, "an emitter log is a payment")
	if t is Dictionary:
		T.eq(t.keys(), ["token", "sender", "recipient", "value", "tx_hash", "log_index", "block_number", "block_hash"],
			"TransferLog fields")
		T.eq(t["token"], ScfgChains.ARC_USDC, "the token's canonical address")
		T.ok(t["value"] is ScfgBigInt and (t["value"] as ScfgBigInt).to_decimal() == "4993742", "floored value")
		T.eq([t["log_index"], t["block_number"]], [3, 777], "ints")
	# below one unit: not a payment
	T.eq(ScfgChains.parse_transfer(arc, _log(ScfgChains.ARC_SYSTEM_EMITTER, "e8d4a50fff")), null, "dust floors to 0")
	T.eq(ScfgChains.parse_transfer(arc, _log(ScfgChains.ARC_SYSTEM_EMITTER, "0")), null, "zero")
	# the 0x3600 interface log (6 decimals) is not read: only the emitter stream counts
	T.eq(ScfgChains.parse_transfer(arc, _log(ScfgChains.ARC_USDC, "4c4b40")), null, "interface logs are ignored")
	# above 2^63: no wrap
	var big: Variant = ScfgBigInt.from_decimal("123456789012345678901234567890")
	var t2: Variant = ScfgChains.parse_transfer(arc, _log(ScfgChains.ARC_SYSTEM_EMITTER, (big as ScfgBigInt).to_hex()))
	T.ok(t2 is Dictionary and (t2["value"] as ScfgBigInt).to_decimal() == "123456789012345678", "bigint floor")
	return T.done()


func test_parse_transfer_on_a_plain_chain() -> bool:
	var base: Dictionary = ScfgChains.MAINNET["base"]
	var usdc: String = base["tokens"]["USDC"]["address"]
	var t: Variant = ScfgChains.parse_transfer(base, _log(usdc, "4c4b40"))
	T.ok(t is Dictionary and (t["value"] as ScfgBigInt).to_decimal() == "5000000" and t["token"] == usdc,
		"an own-contract log is returned as parsed")
	T.eq(ScfgChains.parse_transfer(base, _log("0xdAC17F958D2ee523a2206206994597C13D831ec7", "4c4b40")), null,
		"another contract")
	T.eq(ScfgChains.parse_transfer(base, _log(usdc, "4c4b40", "0x" + "00".repeat(32))), null, "not a Transfer")
	T.is_err(ScfgChains.parse_transfer(base, 5), "AttributeError", "a non-object log")
	return T.done()
