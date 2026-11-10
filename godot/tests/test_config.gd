## Known answers from tests/unit/test_config.py, test_config_solana.py and test_custom_chains.py
## (the code-pinning ones), plus the Python edge cases the vectors do not reach. Every odd-input
## expectation was produced by running scfg_core under CPython 3.12.
extends RefCounted

const T := preload("res://tests/t.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const OLD := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const SOL := "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
const SOL_OLD := "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe"
const TOKEN_ACCOUNT := "C4PRXFV6Gf5mytVZb6RoeLsG8CjcFWzR2EJ3dvwPTUJH"
const AVAX_USDC := "0xB97EF9Ef8734C71904D8002F8b6Bc66Dd9c48a6E"


func _cfg(extra: Dictionary = {}) -> Variant:
	var o := {"wallet": DEV}
	o.merge(extra, true)
	return ScfgConfig.build_config(o)


## The Config, or an empty Dictionary (and a failed check) when the build failed.
func _good(extra: Dictionary = {}) -> Dictionary:
	var c: Variant = _cfg(extra)
	if c is Dictionary:
		return c
	T.ok(false, "build_config(%s) failed: %s" % [ScfgPy.py_repr(extra), str(c)])
	return {}


func _code(v: Variant) -> Variant:
	return v.code if v is ScfgErr and v.is_a("ConfigError") else (v.kind if v is ScfgErr else "<ok>")


func _bad(extra: Dictionary, code: String, msg: String = "") -> void:
	var v: Variant = _cfg(extra)
	T.eq(_code(v), code, "%s %s" % [msg, ScfgPy.py_repr(extra)])


func _kind(v: Variant) -> String:
	return v.kind if v is ScfgErr else "<ok>"


func _avax(over: Dictionary = {}) -> Variant:
	var o := {"name": "Avalanche", "chain_id": 43114, "rpc": ["https://my-node.example/avax",
		"https://api.avax.network/ext/bc/C/rpc"], "tokens": {"USDC": AVAX_USDC}, "confirmations": 5,
		"explorer_tx": "https://snowtrace.io/tx/{}", "native_symbol": "AVAX"}
	o.merge(over, true)
	var key: Variant = o.get("key", "avalanche")
	o.erase("key")
	return ScfgConfig.build_chain(key, o)


# --- wallet -----------------------------------------------------------------------------------


func test_wallet_is_normalized() -> bool:
	T.eq(_good({"wallet": DEV.to_lower()})["wallet"], DEV, "from lowercase")
	for bad: Variant in ["", "0x123", DEV.substr(0, DEV.length() - 1) + "e", 5, false, "not an address"]:
		_bad({"wallet": bad}, "address.invalid", "bad wallet")
	var none_wallet: Variant = ScfgConfig.build_config({"wallet": null})
	T.eq(_code(none_wallet), "wallet.missing", "wallet=None and no solana wallet")
	T.eq(_code(ScfgConfig.build_config({})), "wallet.missing", "no wallet at all")
	T.is_err(ScfgConfig.build_config({"wallet": DEV, "bogus": 1}), "TypeError", "an unexpected keyword")
	T.is_err(ScfgConfig.build_config({}), "ValueError", "a ConfigError is a ValueError")
	# the regex' `$` also matches before one trailing newline; the checksum is then taken over 41 chars
	T.eq(_good({"wallet": DEV + "\n"})["wallet"], "0x52908400098527886E0f7030069857D2e4169ee7\n", "trailing newline")
	return T.done()


func test_accepted_wallets_and_previous_wallets() -> bool:
	var c := _good({"previous_wallets": [OLD.to_lower()]})
	T.eq(ScfgConfig.accepted_wallets(c), [DEV, OLD], "current first, then previous")
	T.eq(c["previous_wallets"], [OLD], "stored normalized")
	var both := _good({"solana_wallet": SOL, "previous_wallets": [OLD.to_lower(), SOL_OLD]})
	T.eq(ScfgConfig.accepted_wallets_for(both, "evm"), [DEV, OLD], "evm")
	T.eq(ScfgConfig.accepted_wallets_for(both, "solana"), [SOL, SOL_OLD], "solana")
	var dedup := _good({"solana_wallet": SOL, "previous_wallets": [DEV.to_lower(), OLD, OLD.to_lower(), SOL, SOL_OLD, SOL_OLD]})
	T.eq(ScfgConfig.accepted_wallets_for(dedup, "evm"), [DEV, OLD], "the current wallet and repeats are dropped (evm)")
	T.eq(ScfgConfig.accepted_wallets_for(dedup, "solana"), [SOL, SOL_OLD], "(solana)")
	var padded := _good({"previous_wallets": ["  " + OLD + " "]})
	T.eq(padded["previous_wallets"], [OLD], "an 0x address is stripped before it is parsed")
	_bad({"previous_wallets": ["0xnothex"]}, "address.invalid")
	_bad({"previous_wallets": [TOKEN_ACCOUNT]}, "solana_address.not_on_curve")
	_bad({"previous_wallets": ["short"]}, "solana_address.invalid")
	_bad({"previous_wallets": [5]}, "solana_address.invalid")
	_bad({"previous_wallets": [null]}, "solana_address.invalid")
	_bad({"previous_wallets": "abc"}, "solana_address.invalid", "a str iterates its characters")
	_bad({"previous_wallets": {"0xnothex": 1}}, "address.invalid", "a dict iterates its keys")
	_bad({"previous_wallets": 5}, "TypeError", "not iterable")
	T.eq(_good({"previous_wallets": []})["previous_wallets"], [], "empty")
	return T.done()


# --- defaults, chains, tokens ---------------------------------------------------------------


func test_defaults() -> bool:
	var c := _good()
	T.eq(c.keys(), ["wallet", "testnet", "chains", "tokens", "methods", "rpc_urls", "confirmations",
		"previous_wallets", "solana_wallet", "previous_solana_wallets", "logo", "accent", "custom_chains", "items"],
		"Config fields in Python's order")
	T.eq(c["testnet"], false, "testnet")
	T.eq(c["chains"], ["arbitrum", "base", "arc", "polygon", "ethereum"], "chains")
	T.eq(c["tokens"], ["USDC", "USDT"], "tokens")
	T.eq(c["methods"], ScfgConfig.DEFAULT_METHODS, "methods")
	T.eq([c["rpc_urls"], c["confirmations"], c["items"], c["custom_chains"]], [{}, {}, {}, {}], "empty tables")
	T.eq([c["logo"], c["accent"], c["solana_wallet"]], [null, ScfgConfig.DEFAULT_ACCENT, null], "appearance")
	T.ok("arc" in c["chains"] and ScfgConfig.enabled_chains(c).has("arc"), "Arc is on by default")
	return T.done()


func test_default_methods_are_not_shared() -> bool:
	var a := _good()
	a["methods"]["qr"]["enabled"] = false
	T.eq(_good()["methods"]["qr"]["enabled"], true, "a second config is untouched")
	T.eq(ScfgConfig.DEFAULT_METHODS["qr"]["enabled"], true, "DEFAULT_METHODS is untouched")
	return T.done()


func test_chains_and_tokens() -> bool:
	_bad({"chains": []}, "chains.empty")
	_bad({"chains": ["tron"]}, "chains.unknown")
	_bad({"chains": ["base", "base"]}, "chains.duplicate")
	_bad({"chains": [["base"]]}, "TypeError", "an unhashable chain")
	_bad({"chains": [5]}, "chains.unknown")
	_bad({"chains": 5}, "TypeError")
	_bad({"chains": "base"}, "chains.unknown", "a str iterates its characters")
	_bad({"chains": ""}, "chains.empty")
	T.eq(_good({"chains": {"base": 1}})["chains"], ["base"], "a dict iterates its keys")
	T.eq(_good({"chains": null})["chains"], ["arbitrum", "base", "arc", "polygon", "ethereum"], "None is the default")
	_bad({"tokens": []}, "tokens.empty")
	_bad({"tokens": ["DAI"]}, "tokens.unknown")
	_bad({"tokens": [["USDC"]]}, "tokens.unknown")
	_bad({"tokens": [1.0]}, "tokens.unknown")
	_bad({"tokens": "USDC"}, "tokens.unknown", "characters")
	_bad({"tokens": 5}, "TypeError")
	_bad({"chains": ["base"], "tokens": ["USDT"]}, "config.no_usable_token")
	_bad({"testnet": true, "tokens": ["USDT"]}, "config.no_usable_token")
	_bad({"chains": ["arc"], "tokens": ["USDT"]}, "config.no_usable_token")
	T.eq(_good({"testnet": 1})["testnet"], true, "testnet is bool()ed")
	T.eq(_good({"testnet": "no"})["testnet"], true, "any truthy value")
	T.eq(_good({"testnet": []})["testnet"], false, "an empty list is falsy")
	return T.done()


func test_enabled_chains_respects_order_tokens_and_overrides() -> bool:
	var c := _good({"chains": ["polygon", "base"], "tokens": ["USDT"],
		"rpc_urls": {"polygon": ["https://my.rpc/key"]}, "confirmations": {"polygon": 64}})
	var chains: Dictionary = ScfgConfig.enabled_chains(c)
	T.eq(chains.keys(), ["polygon"], "base dropped: no USDT on base")
	var poly: Dictionary = chains["polygon"]
	T.eq(poly["tokens"].keys(), ["USDT"], "tokens")
	T.eq(poly["rpc"][0], "https://my.rpc/key", "the developer URL first")
	T.eq(poly["rpc"].size(), 4, "built-ins follow")
	T.eq(poly["confirmations"], 64, "override")
	T.eq(poly.keys(), ScfgChains.MAINNET["polygon"].keys(), "same fields in the same order")
	T.eq(ScfgChains.MAINNET["polygon"]["rpc"].size(), 3, "the registry itself is untouched")
	T.eq(ScfgChains.MAINNET["polygon"]["tokens"].keys(), ["USDC", "USDT"], "(tokens too)")
	T.eq(ScfgConfig.enabled_chains(_good({"rpc_urls": {"base": "https://one.rpc"}}))["base"]["rpc"][0], "https://one.rpc",
		"a str URL is accepted")
	T.eq(ScfgConfig.chains_for_token(_good(), "USDT"), ["arbitrum", "polygon", "ethereum"], "chains_for_token")
	T.eq(ScfgConfig.chains_for_token(_good(), "USDC"), ["arbitrum", "base", "arc", "polygon", "ethereum"], "(USDC)")
	T.is_err(ScfgConfig.chains_for_token(_good(), ["USDC"]), "TypeError", "an unhashable token")
	T.eq(ScfgConfig.registry(_good()).keys(), ["ethereum", "arbitrum", "polygon", "base", "arc", "solana"], "registry")
	return T.done()


func test_rpc_urls() -> bool:
	for urls: Variant in [{"tron": ["https://x"]}, {"base": ["http://insecure"]}, {"base": []}, {"base": 5},
			{"base": "http://example.org"}, {"base": ["https://ok.example.org", "ftp://x"]},
			{"base": "http://127.0.0.1.evil.org"}, {"base": {"https://a": 1}}, {"base": [["https://x"]]},
			{"base": "http://localhost:123456"}, {"base": "http://localhost:"}, {"base": "http://localhostx"},
			{"base": "http://localhost:8545\n\n"}]:
		T.is_err(_cfg({"rpc_urls": urls}), "ConfigError", "rpc_urls %s" % ScfgPy.py_repr(urls))
	_bad({"rpc_urls": {"base": []}}, "rpc.bad_list")
	_bad({"rpc_urls": {"base": "http://example.org"}}, "rpc.not_https")
	_bad({"rpc_urls": {"tron": ["https://x"]}}, "rpc_urls.unknown_chain")
	_bad({"rpc_urls": {"polygon": "https://x.example.org"}, "chains": ["base"]}, "rpc_urls.unknown_chain")
	_bad({"rpc_urls": ["x"]}, "AttributeError", "a list has no .items()")
	for ok: Variant in ["http://127.0.0.1:8545", "http://localhost:8545/", "http://127.0.0.1", "http://localhost:12345/x",
			"http://localhost:8545\n", "http://localhost:\u0661\u0662\u0663"]:
		T.eq(_good({"rpc_urls": {"base": ok}})["rpc_urls"], {"base": [ok]}, "plain http on this computer: %s" % ScfgPy.py_repr(ok))
	T.eq(_good({"rpc_urls": ""})["rpc_urls"], {}, "falsy")
	return T.done()


func test_confirmations() -> bool:
	for confs: Variant in [{"base": 0}, {"base": true}, {"base": "3"}, {"base": 2.0}, {"base": 2.5}]:
		_bad({"confirmations": confs}, "confirmations.invalid", "confirmations")
	_bad({"confirmations": {"nope": 3}}, "confirmations.unknown_chain")
	_bad({"confirmations": {"polygon": 2}, "chains": ["base"]}, "confirmations.unknown_chain")
	_bad({"confirmations": {"solana": 5}, "solana_wallet": SOL}, "confirmations.solana_fixed")
	_bad({"confirmations": [1]}, "AttributeError")
	T.eq(_good({"confirmations": {"base": 3, "ethereum": 1}})["confirmations"], {"base": 3, "ethereum": 1}, "ok")
	return T.done()


# --- methods ----------------------------------------------------------------------------------


func test_methods() -> bool:
	var c := _good({"methods": {"browser": {"enabled": false}}})
	T.eq(c["methods"]["browser"], {"enabled": false, "timeout_min": 30}, "partial options merge")
	T.eq(c["methods"]["qr"]["enabled"], true, "the others keep their defaults")
	T.eq(c["methods"].keys(), ["browser", "qr", "manual"], "method order")
	T.eq(c["methods"]["qr"].keys(), ["enabled", "wallet_button", "chains"], "option order")
	_bad({"methods": {"carrier_pigeon": {"enabled": true}}}, "methods.unknown_method")
	_bad({"methods": {"qr": {"colour": "red"}}}, "methods.unknown_option")
	_bad({"methods": {"qr": {"enabled": "yes"}}}, "methods.enabled_not_bool")
	_bad({"methods": {"manual": {"enabled": 1}}}, "methods.enabled_not_bool")
	_bad({"methods": {"qr": {"enabled": null}}}, "methods.enabled_not_bool")
	_bad({"methods": {"browser": {"enabled": false}, "qr": {"enabled": false}, "manual": {"enabled": false}}},
		"methods.none_enabled")
	for timeout: Variant in ["30", 0, -5, true, null, NAN]:
		_bad({"methods": {"browser": {"timeout_min": timeout}}}, "methods.bad_timeout", "timeout")
	T.eq(_good({"methods": {"browser": {"timeout_min": 7.5}}})["methods"]["browser"]["timeout_min"], 7.5, "a float")
	_bad({"methods": {"qr": {"chains": ["tron"]}}}, "methods.qr_disabled_chain")
	_bad({"chains": ["base"], "methods": {"qr": {"chains": ["polygon"]}}}, "methods.qr_disabled_chain")
	_bad({"methods": {"qr": {"chains": "base"}}}, "methods.qr_disabled_chain", "characters")
	_bad({"methods": {"qr": {"chains": [["base"]]}}}, "methods.qr_disabled_chain", "no hash needed here")
	_bad({"methods": {"qr": {"chains": 5}}}, "TypeError")
	T.eq(_good({"methods": {"qr": {"chains": ["base"]}}})["methods"]["qr"]["chains"], ["base"], "a subset")
	T.eq(_good({"methods": {"qr": {"chains": []}}})["methods"]["qr"]["chains"], [], "empty")
	T.eq(_good({"methods": {"qr": {"chains": {"base": 1}}}})["methods"]["qr"]["chains"], {"base": 1}, "kept as given")
	_bad({"methods": ["x"]}, "AttributeError")
	_bad({"methods": {"qr": "x"}}, "AttributeError")
	_bad({"methods": {"qr": null}}, "AttributeError")
	T.eq(_good({"methods": {"qr": {}}})["methods"], ScfgConfig.DEFAULT_METHODS, "an empty option set")
	T.eq(_good({"methods": ""})["methods"], ScfgConfig.DEFAULT_METHODS, "falsy methods")
	return T.done()


# --- appearance -------------------------------------------------------------------------------


func test_logo_and_accent() -> bool:
	for logo: String in ["images/logo.png", "gui/Logo.JPG", "logo.jpeg", "a/b.webp", ".png", "a/..b.png"]:
		T.eq(_good({"logo": logo})["logo"], logo, "logo %s" % logo)
	for logo: Variant in ["logo.svg", "logo", "logo.gif", "a.PNG\n", "a.b/c"]:
		_bad({"logo": logo}, "logo.bad_type", "logo")
	for logo: Variant in ["", 5, 0, "../logo.png", "/abs/logo.png", "c:\\logo.png", "images\\logo.png", "a/../b.png", "C:logo.png"]:
		_bad({"logo": logo}, "logo.bad_path", "logo")
	T.eq(ScfgConfig.logo_content_type("images/logo.PNG"), "image/png", "png")
	T.eq(ScfgConfig.logo_content_type("x.jpg"), "image/jpeg", "jpg")
	T.eq(ScfgConfig.logo_content_type("x.jpeg"), "image/jpeg", "jpeg")
	T.eq(ScfgConfig.logo_content_type("x.webp"), "image/webp", "webp")
	T.is_err(ScfgConfig.logo_content_type("x.svg"), "ConfigError", "svg")
	T.eq(_code(ScfgConfig.logo_content_type("noext")), "logo.bad_type", "no extension")
	for pair: Array in [["#0E7C74", "#0e7c74"], ["#09c", "#0099cc"], ["#0099ccff", "#0099cc"], [null, "#0e7c74"],
			["#abc\n", "#abc\n"], ["#AABBCC\n", "#aabbcc"]]:
		T.eq(_good({"accent": pair[0]})["accent"], pair[1], "accent %s" % ScfgPy.py_repr(pair[0]))
	for bad: Variant in ["0099cc", "#0099c", "#gggggg", "teal", 0x0099CC, "#0099cc80 ", "#abcdefa", "", 0, "red", "#12"]:
		_bad({"accent": bad}, "accent.invalid", "accent")
	return T.done()


func test_text_on_accent_stays_readable() -> bool:
	for pair: Array in [["#0e7c74", "#ffffff"], ["#17202e", "#ffffff"], ["#ffcc00", "#17202e"], ["#ffffff", "#17202e"],
			["#000000", "#ffffff"]]:
		T.eq(ScfgConfig.text_on(pair[0]), pair[1], "text on %s" % pair[0])
	T.eq(ScfgConfig.INK, "#17202e", "INK")
	return T.done()


# --- solana -----------------------------------------------------------------------------------


func test_solana_only_game() -> bool:
	var c: Variant = ScfgConfig.build_config({"solana_wallet": SOL})
	T.ok(c is Dictionary, "a Solana-only game")
	if c is Dictionary:
		T.eq([c["wallet"], c["solana_wallet"], c["chains"]], [null, SOL, ["solana"]], "fields")
		T.eq(ScfgConfig.accepted_wallets(c), [], "no EVM wallets")
		T.eq(ScfgConfig.accepted_wallets_for(c, "solana"), [SOL], "solana wallets")
		T.eq(ScfgConfig.wallet_for(c, ScfgChains.MAINNET["solana"]), SOL, "wallet_for solana")
		T.eq(ScfgConfig.wallet_for(c, ScfgChains.MAINNET["base"]), null, "wallet_for evm")
	return T.done()


func test_both_families_default_to_evm_then_solana() -> bool:
	var c := _good({"solana_wallet": SOL})
	T.eq(c["chains"], ["arbitrum", "base", "arc", "polygon", "ethereum", "solana"], "chains")
	T.eq(ScfgConfig.wallet_for(c, ScfgChains.MAINNET["base"]), DEV, "evm wallet")
	var avax: Variant = _avax()
	var c2 := _good({"solana_wallet": SOL, "custom_chains": [avax]})
	T.eq(c2["chains"].slice(-2), ["avalanche", "solana"], "custom EVM chains come before Solana")
	return T.done()


func test_solana_wallet_rules() -> bool:
	for bad: Variant in ["", "0x123", "not base58 0OIl", "1".repeat(31), 5]:
		_bad({"solana_wallet": bad}, "solana_address.invalid", "solana wallet")
	_bad({"solana_wallet": TOKEN_ACCOUNT}, "solana_address.not_on_curve")
	T.eq(_code(ScfgConfig.build_config({"solana_wallet": TOKEN_ACCOUNT})), "solana_address.not_on_curve", "alone")
	T.eq((ScfgConfig.build_config({"solana_wallet": TOKEN_ACCOUNT}) as ScfgErr).message.contains("token account"), true,
		"the hint")
	_bad({"chains": ["solana"]}, "chain.needs_solana_wallet")
	T.eq(_code(ScfgConfig.build_config({"solana_wallet": SOL, "chains": ["base"]})), "chain.needs_evm_wallet", "evm chain")
	var testnet: Variant = ScfgConfig.build_config({"solana_wallet": SOL, "testnet": true,
		"rpc_urls": {"solana": "https://my-node.example"}})
	T.ok(testnet is Dictionary, "devnet game")
	if testnet is Dictionary:
		var chain: Dictionary = ScfgConfig.enabled_chains(testnet)["solana"]
		T.eq(chain["rpc"], ["https://my-node.example", "https://api.devnet.solana.com"], "override first")
		T.eq(_code(ScfgConfig.add_item(testnet, "ch2", {"code": 1, "price": "1.00", "token": "USDC", "title": "Chapter 2"})),
			"<ok>", "USDC on devnet")
		T.eq(_code(ScfgConfig.add_item(testnet, "ost", {"code": 2, "price": "1.00", "token": "USDT", "title": "OST"})),
			"token.unavailable", "no USDT on devnet")
	T.eq(_code(ScfgConfig.build_config({"solana_wallet": SOL + "\n"})), "<ok>", "a trailing newline is stripped")
	T.eq(_code(ScfgConfig.build_config({"solana_wallet": " " + SOL + " "})), "<ok>", "padded is stripped")
	return T.done()


# --- build_chain ------------------------------------------------------------------------------


func test_build_chain_fields() -> bool:
	var c: Variant = _avax()
	T.ok(c is Dictionary, "build_chain")
	if c is Dictionary:
		T.eq(c.keys(), ["key", "chain_id", "name", "rpc", "confirmations", "max_log_range", "explorer_tx", "native_symbol",
			"fee_hint", "tokens", "testnet", "family", "wallet_chain", "native_token", "min_fee_wei"], "fields in order")
		T.eq([c["key"], c["chain_id"], c["name"], c["confirmations"], c["native_symbol"], c["fee_hint"]],
			["avalanche", 43114, "Avalanche", 5, "AVAX", "low"], "values")
		T.eq([c["max_log_range"], c["testnet"], c["family"], c["wallet_chain"], c["native_token"], c["min_fee_wei"]],
			[1000, false, "evm", "", "", 0], "defaults")
		T.eq(c["rpc"], ["https://my-node.example/avax", "https://api.avax.network/ext/bc/C/rpc"], "rpc")
		T.eq(c["tokens"]["USDC"], {"address": AVAX_USDC, "decimals": 6, "log_address": "", "log_decimals": 0}, "token")
		var tx := "0x" + "cd".repeat(32)
		T.eq(ScfgChains.explorer_url(c, tx), "https://snowtrace.io/tx/" + tx, "explorer_url")
	var single: Variant = _avax({"rpc": "https://api.avax.network/ext/bc/C/rpc", "tokens": {"USDC": AVAX_USDC.to_lower()}})
	T.ok(single is Dictionary and single["rpc"] == ["https://api.avax.network/ext/bc/C/rpc"]
		and single["tokens"]["USDC"]["address"] == AVAX_USDC, "a single rpc string and a lowercase token")
	for url: String in ["http://127.0.0.1:8545", "http://localhost:8545/", "http://127.0.0.1"]:
		var h: Variant = _avax({"rpc": url})
		T.ok(h is Dictionary and h["rpc"] == [url], "plain http on this computer")
	T.eq(_avax({"name": " \u00a0N\u3000"})["name"], "N", "name is stripped by Python's str.strip")
	T.eq(_avax({"name": "\ud83d\ude00".repeat(32)}) is Dictionary, true, "32 astral characters")
	T.eq(_avax({"testnet": "x"})["testnet"], true, "bool(testnet)")
	T.eq(_avax({"testnet": []})["testnet"], false, "bool([])")
	T.eq(_avax({"max_log_range": 10})["max_log_range"], 10, "the minimum")
	T.eq(_avax({"native_symbol": "ETH\n"})["native_symbol"], "ETH\n", "`$` accepts one trailing newline")
	T.eq(_avax({"key": "ab\n"})["key"], "ab\n", "(also the key)")
	T.eq(_avax({"tokens": {"USDT": AVAX_USDC, "USDC": AVAX_USDC}})["tokens"].keys(), ["USDT", "USDC"], "token order")
	T.eq(_avax({"tokens": {"USDC": AVAX_USDC.to_lower() + "\n"}})["tokens"]["USDC"]["address"], "0xB97EF9EF8734C71904d8002F8b6Bc66DD9C48A6e\n",
		"a newline-terminated address carries the newline")
	return T.done()


func test_build_chain_rejects() -> bool:
	var rows := [
		[{"key": "Avalanche"}, "chain.invalid_key"], [{"key": "a:b"}, "chain.invalid_key"], [{"key": ""}, "chain.invalid_key"],
		[{"key": "x".repeat(33)}, "chain.invalid_key"], [{"key": 5}, "chain.invalid_key"], [{"key": null}, "chain.invalid_key"],
		[{"key": ["a"]}, "chain.invalid_key"], [{"key": "\n"}, "chain.invalid_key"],
		[{"name": ""}, "chain.invalid_name"], [{"name": "x".repeat(33)}, "chain.invalid_name"],
		[{"name": null}, "chain.invalid_name"], [{"name": "   "}, "chain.invalid_name"],
		[{"name": "\ud83d\ude00".repeat(33)}, "chain.invalid_name"],
		[{"chain_id": 0}, "int.below_minimum"], [{"chain_id": true}, "int.below_minimum"],
		[{"chain_id": "43114"}, "int.below_minimum"], [{"chain_id": 2.0}, "int.below_minimum"],
		[{"rpc": []}, "rpc.bad_list"], [{"rpc": "http://my-node.example"}, "rpc.not_https"],
		[{"rpc": "http://127.0.0.1.evil.com"}, "rpc.not_https"], [{"rpc": ["https://ok.example", 5]}, "rpc.not_https"],
		[{"rpc": null}, "rpc.bad_list"], [{"rpc": {"https://a": 1}}, "rpc.bad_list"],
		[{"tokens": {}}, "chain.tokens_invalid"], [{"tokens": [["USDC", AVAX_USDC]]}, "chain.tokens_invalid"],
		[{"tokens": {"DAI": AVAX_USDC}}, "chain.unknown_token"], [{"tokens": {"USDC": "0x123"}}, "address.invalid"],
		[{"tokens": {"USDC": AVAX_USDC.substr(0, 41) + "e"}}, "address.invalid"], [{"tokens": {"USDC": 5}}, "address.invalid"],
		[{"tokens": {"USDC": null}}, "address.invalid"],
		[{"confirmations": 0}, "int.below_minimum"], [{"confirmations": null}, "int.below_minimum"],
		[{"confirmations": 2.0}, "int.below_minimum"],
		[{"explorer_tx": "https://snowtrace.io/tx/"}, "chain.bad_explorer_tx"],
		[{"explorer_tx": "http://snowtrace.io/tx/{}"}, "chain.bad_explorer_tx"],
		[{"explorer_tx": "https://x.example/{}/{}"}, "chain.bad_explorer_tx"],
		[{"explorer_tx": "https://x.example/{{}}{}"}, "chain.bad_explorer_tx"],
		[{"explorer_tx": "https://x.example/{!r}"}, "chain.bad_explorer_tx"], [{"explorer_tx": 5}, "chain.bad_explorer_tx"],
		[{"explorer_tx": "https://x.example/{0}{}"}, "chain.bad_explorer_tx_braces"],
		[{"explorer_tx": "https://x.example/{tx}?{}"}, "chain.bad_explorer_tx_braces"],
		[{"explorer_tx": "https://x.example/{}/{"}, "chain.bad_explorer_tx_braces"],
		[{"explorer_tx": "https://x.example/{}}"}, "chain.bad_explorer_tx_braces"],
		[{"explorer_tx": "https://x.example/{}{:x}"}, "chain.bad_explorer_tx_braces"],
		[{"explorer_tx": "https://x.example/{}{.upper}"}, "chain.bad_explorer_tx_braces"],
		[{"explorer_tx": "https://x.example/{[x]}{}"}, "TypeError"],
		[{"explorer_tx": "https://x.example/{.nope}{}"}, "AttributeError"],
		[{"native_symbol": ""}, "chain.bad_native_symbol"], [{"native_symbol": "A V"}, "chain.bad_native_symbol"],
		[{"native_symbol": "A".repeat(12)}, "chain.bad_native_symbol"], [{"native_symbol": 5}, "chain.bad_native_symbol"],
		[{"fee_hint": "cheap"}, "chain.bad_fee_hint"], [{"fee_hint": ["low"]}, "chain.bad_fee_hint"],
		[{"fee_hint": null}, "chain.bad_fee_hint"],
		[{"max_log_range": 5}, "int.below_minimum"], [{"max_log_range": 1.5}, "int.below_minimum"],
	]
	for row: Array in rows:
		T.eq(_code(_avax(row[0])), row[1], "build_chain(%s)" % ScfgPy.py_repr(row[0]))
	# Python's checks run in this order: key, name, chain_id, rpc, tokens, confirmations, explorer_tx, ...
	T.eq(_code(_avax({"name": "", "chain_id": 0})), "chain.invalid_name", "name before chain_id")
	T.eq(_code(_avax({"chain_id": 0, "rpc": []})), "int.below_minimum", "chain_id before rpc")
	T.eq(_code(_avax({"confirmations": 0, "explorer_tx": "x"})), "int.below_minimum", "confirmations before explorer_tx")
	T.eq(_code(_avax({"native_symbol": "", "fee_hint": "x"})), "chain.bad_native_symbol", "native_symbol before fee_hint")
	T.is_err(ScfgConfig.build_chain("avalanche", {"name": "A"}), "TypeError", "missing keywords")
	T.is_err(ScfgConfig.build_chain("avalanche", {"bogus": 1}), "TypeError", "unexpected keyword")
	T.is_err(ScfgConfig.build_chain("a", {"name": "A", "chain_id": 1, "rpc": "https://a", "tokens": {"USDC": AVAX_USDC},
		"confirmations": 1, "explorer_tx": "https://x/{}", "bogus": 1}), "TypeError", "unexpected keyword with the rest valid")
	return T.done()


# --- custom chains in a config ----------------------------------------------------------------


func test_custom_chain_is_enabled_by_default_after_built_ins() -> bool:
	var c := _good({"custom_chains": [_avax()]})
	T.eq(c["chains"], ["arbitrum", "base", "arc", "polygon", "ethereum", "avalanche"], "chains")
	T.eq(ScfgConfig.enabled_chains(c).keys()[-1], "avalanche", "enabled last")
	T.eq(ScfgConfig.registry(c)["avalanche"]["chain_id"], 43114, "registry")
	T.eq(c["custom_chains"].keys(), ["avalanche"], "custom_chains keyed by key")
	return T.done()


func test_custom_chain_only_and_overrides() -> bool:
	var c := _good({"chains": ["avalanche"], "custom_chains": [_avax()], "rpc_urls": {"avalanche": "https://first.example"},
		"confirmations": {"avalanche": 12}, "methods": {"qr": {"chains": ["avalanche"]}}})
	var chain: Dictionary = ScfgConfig.enabled_chains(c)["avalanche"]
	T.eq(chain["rpc"][0], "https://first.example", "rpc first")
	T.eq(chain["confirmations"], 12, "confirmations")
	T.eq(ScfgConfig.enabled_chains(c).keys(), ["avalanche"], "only the custom chain")
	var item: Variant = ScfgConfig.add_item(c, "ch2", {"code": 1, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	T.ok(item is Dictionary and item["token"] == "USDC", "a custom chain can sell items")
	T.eq(_code(ScfgConfig.add_item(c, "ost", {"code": 2, "price": "2.00", "token": "USDT", "title": "OST"})), "token.unavailable",
		"the custom chain only carries USDC")
	return T.done()


func test_custom_chain_belongs_to_one_network() -> bool:
	var fuji: Variant = _avax({"chain_id": 43113, "name": "Fuji", "testnet": true})
	T.ok(not _good({"custom_chains": [fuji]})["custom_chains"].has("avalanche"), "ignored on the other network")
	var c := _good({"testnet": true, "custom_chains": [_avax(), fuji]})
	T.eq(ScfgConfig.registry(c)["avalanche"]["name"], "Fuji", "the testnet one")
	var e1: Variant = _cfg({"chains": ["avalanche"], "custom_chains": [fuji]})
	T.eq(_code(e1), "chains.wrong_network", "wrong network")
	T.ok((e1 as ScfgErr).message.contains("only added for testnet") and (e1 as ScfgErr).message.contains("testnet=False"),
		"message: " + (e1 as ScfgErr).message)
	var e2: Variant = _cfg({"testnet": true, "chains": ["avalanche"], "custom_chains": [_avax()]})
	T.ok(_code(e2) == "chains.wrong_network" and (e2 as ScfgErr).message.contains("only added for mainnet"), "mainnet")
	var fork := _good({"testnet": true, "custom_chains": [_avax({"key": "fork", "chain_id": 1, "testnet": true})]})
	T.eq(ScfgConfig.registry(fork)["fork"]["chain_id"], 1, "a mainnet chain id is free on testnet")
	var both := _good({"custom_chains": [_avax(), _avax({"testnet": true, "chain_id": 43113})]})
	T.eq(both["custom_chains"].keys(), ["avalanche"], "the same key once per network")
	return T.done()


func test_bad_custom_chains() -> bool:
	var rows := [
		[[_avax({"key": "arbitrum"})], "chain.is_builtin"],
		[[_avax({"key": "ethereum", "testnet": true, "chain_id": 1234})], "chain.is_builtin"],
		[[_avax(), _avax({"chain_id": 99})], "chain.duplicate"],
		[[_avax({"chain_id": 42161})], "chain.duplicate_chain_id"],
		[[_avax(), _avax({"key": "other"})], "chain.duplicate_chain_id"],
		[[ScfgChains.MAINNET["arbitrum"]], "chain.is_builtin"],
		[["avalanche"], "chain.not_from_add_chain"], [[5], "chain.not_from_add_chain"], [[null], "chain.not_from_add_chain"],
		[[{"key": "x"}], "chain.not_from_add_chain"], [[[]], "chain.not_from_add_chain"],
		["ab", "chain.not_from_add_chain"], [{"a": 1}, "chain.not_from_add_chain"], [5, "TypeError"],
	]
	for row: Array in rows:
		_bad({"custom_chains": row[0]}, row[1], "custom_chains")
	T.eq(_good({"custom_chains": ""})["custom_chains"], {}, "falsy")
	T.eq(_good({"custom_chains": null})["custom_chains"], {}, "None")
	var arc: Variant = _cfg({"custom_chains": [_avax({"key": "arc", "chain_id": 5042})]})
	T.ok(arc is ScfgErr and arc.code == "chain.is_builtin" and arc.message.contains("built in"), "Arc is built in")
	return T.done()


# --- add_item ---------------------------------------------------------------------------------


func _item(over: Dictionary = {}) -> Dictionary:
	var o := {"code": 1, "price": "4.99", "token": "USDC", "title": "Chapter 2"}
	o.merge(over, true)
	return o


func test_add_item() -> bool:
	var c := _good()
	var item: Variant = ScfgConfig.add_item(c, "chapter2", _item())
	T.ok(item is Dictionary, "add_item")
	if item is Dictionary:
		T.eq(item.keys(), ["item_id", "code", "price", "token", "title", "min_price"], "Item fields")
		T.eq([item["item_id"], item["code"], str(item["price"]), item["token"], item["title"], str(item["min_price"])],
			["chapter2", 1, "4.99", "USDC", "Chapter 2", "4.99"], "values")
		T.ok(item["price"] is ScfgDec, "price is a Decimal")
		T.ok(is_same(c["items"]["chapter2"], item), "stored in config.items")
	var ost: Variant = ScfgConfig.add_item(c, "ost", _item({"code": 2, "price": "2.00", "token": "USDT", "title": "OST", "min_price": "1.50"}))
	T.eq(str(ost["min_price"]), "1.50", "min_price")
	T.eq(ScfgConfig.item_by_code(c, 2)["item_id"], "ost", "item_by_code")
	T.eq(ScfgConfig.item_by_code(c, 3), null, "no such code")
	T.eq(str(_okitem({"price": 5})["price"]), "5.00", "an int price")
	T.eq(str(_okitem({"price": " 4.99 "})["price"]), "4.99", "a padded price")
	T.eq(str(_okitem({"price": "0.5E1"})["price"]), "5.00", "exponent form")
	T.eq(_okitem({"title": "\ud83d\ude00".repeat(80)})["title"].length(), 80, "80 astral characters")
	T.eq(_okitem({"item_id": "ab\n"})["item_id"], "ab\n", "`$` accepts one trailing newline")
	return T.done()


func _okitem(over: Dictionary) -> Dictionary:
	var o := _item(over)
	var id: Variant = o.get("item_id", "x")
	o.erase("item_id")
	var v: Variant = ScfgConfig.add_item(_good(), id, o)
	if v is Dictionary:
		return v
	T.ok(false, "add_item(%s) failed: %s" % [ScfgPy.py_repr(over), str(v)])
	return {}


func test_add_item_rejects() -> bool:
	var rows := [
		["Chapter 2", {}, "item.invalid_id"], ["", {}, "item.invalid_id"], ["x".repeat(65), {}, "item.invalid_id"],
		[5, {}, "item.invalid_id"], [null, {}, "item.invalid_id"], [["a"], {}, "item.invalid_id"], ["\n", {}, "item.invalid_id"],
		["x", {"code": 0}, "item.invalid_code"], ["x", {"code": 100}, "item.invalid_code"],
		["x", {"code": -1}, "item.invalid_code"], ["x", {"code": true}, "item.invalid_code"],
		["x", {"code": "1"}, "item.invalid_code"], ["x", {"code": 2.0}, "item.invalid_code"],
		["x", {"code": null}, "item.invalid_code"], ["x", {"code": []}, "item.invalid_code"],
		["x", {"price": 4.99}, "item.invalid_price"], ["x", {"price": "4.999"}, "item.invalid_price"],
		["x", {"price": "abc"}, "item.invalid_price"], ["x", {"price": ""}, "item.invalid_price"],
		["x", {"price": 0}, "item.invalid_price"], ["x", {"price": "-1"}, "item.invalid_price"],
		["x", {"price": "1000000.01"}, "item.invalid_price"], ["x", {"price": true}, "item.invalid_price"],
		["x", {"price": null}, "item.invalid_price"], ["x", {"price": []}, "item.invalid_price"],
		["x", {"price": "nan"}, "item.invalid_price"], ["x", {"price": "inf"}, "item.invalid_price"],
		["x", {"price": "snan"}, "item.invalid_price"], ["x", {"price": "1E+7"}, "item.invalid_price"],
		["x", {"min_price": "abc"}, "item.invalid_price"], ["x", {"min_price": 0}, "item.invalid_price"],
		["x", {"min_price": []}, "item.invalid_price"], ["x", {"min_price": 1.5}, "item.invalid_price"],
		["x", {"price": "2", "min_price": "3"}, "item.min_price_above_price"], ["x", {"min_price": "5.00"}, "item.min_price_above_price"],
		["x", {"token": "DAI"}, "token.unavailable"], ["x", {"token": null}, "token.unavailable"],
		["x", {"token": ["USDC"]}, "token.unavailable"], ["x", {"token": {}}, "token.unavailable"], ["x", {"token": 5}, "token.unavailable"],
		["x", {"title": ""}, "item.invalid_title"], ["x", {"title": "   "}, "item.invalid_title"],
		["x", {"title": "\u00a0"}, "item.invalid_title"], ["x", {"title": "T".repeat(81)}, "item.invalid_title"],
		["x", {"title": "\ud83d\ude00".repeat(81)}, "item.invalid_title"], ["x", {"title": 5}, "item.invalid_title"],
		["x", {"title": ["a"]}, "item.invalid_title"],
	]
	for row: Array in rows:
		T.eq(_code(ScfgConfig.add_item(_good(), row[0], _item(row[1]))), row[2],
			"add_item(%s, %s)" % [ScfgPy.py_repr(row[0]), ScfgPy.py_repr(row[1])])
	# check order: id, duplicate id, code, duplicate code, price, min_price, token, title
	var c := _good()
	ScfgConfig.add_item(c, "a", _item({"code": 1, "price": "1"}))
	T.eq(_code(ScfgConfig.add_item(c, "a", _item({"code": 0}))), "item.duplicate_id", "duplicate id before code")
	T.eq(_code(ScfgConfig.add_item(c, "b", _item({"code": 1, "price": "x"}))), "item.duplicate_code", "duplicate code before price")
	T.eq(_code(ScfgConfig.add_item(c, "b", _item({"code": 2, "token": "DAI", "title": ""}))), "token.unavailable", "token before title")
	T.eq(_code(ScfgConfig.add_item(_good({"tokens": ["USDC"]}), "a", _item({"token": "USDT"}))), "token.unavailable", "token not enabled")
	T.eq(_code(ScfgConfig.add_item(_good({"chains": ["base"]}), "a", _item({"token": "USDT"}))), "token.unavailable", "no chain carries it")
	T.eq(c["items"].keys(), ["a"], "failed adds leave the items alone")
	T.is_err(ScfgConfig.add_item(c, "z", {"code": 1, "price": "1", "token": "USDC"}), "TypeError", "title is required")
	T.is_err(ScfgConfig.add_item(c, "z", _item({"foo": 1})), "TypeError", "an unexpected keyword")
	return T.done()


## `value in <tuple of str>`, public because ScfgPayments uses it.
func test_in_strs() -> bool:
	T.ok(ScfgConfig.in_strs(["USDC", "USDT"], "USDC"), "a member")
	T.ok(ScfgConfig.in_strs(["USDC"], &"USDC"), "a StringName is a str")
	T.ok(not ScfgConfig.in_strs(["USDC"], "DAI"), "not a member")
	T.ok(not ScfgConfig.in_strs(["5"], 5), "only a str can be equal to a str")
	return T.done()
