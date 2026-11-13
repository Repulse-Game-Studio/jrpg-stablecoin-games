## Registry of supported chains and stablecoin contracts. Mirrors scfg_core/chains.py.
##
## Token addresses come from Circle's and Tether's official contract lists and are pinned by
## tests. Solana mints come from those same lists and are compared exactly (base58), never
## case-folded. Tokens are always identified by contract address, never by symbol, so a look-alike
## token called "USDC" can never be mistaken for the real one. Testnet entries use the same keys
## so a game can switch with `testnet = true`.
##
## Developers can add further EVM chains of their own (ScfgConfig.build_chain, exposed as
## scfg.add_chain); those never enter these dictionaries, they live on the Config.
##
## Log sources: on most chains a token's Transfer events come from its own contract. On Arc, USDC
## is the native gas coin: a plain native send logs a Transfer only at a system emitter address
## (18 decimals), while an ERC-20 transfer() logs there *and* at the 0x3600 interface (6
## decimals). Such a token names the emitter as its log_address, and `parse_transfer` reads only
## that stream, floored to 6-decimal units, so every payment is seen exactly once however the
## player sent it.
##
## Representation: Python's frozen dataclasses are plain Dictionaries with the Python field
## names in Python's order (GDScript dictionaries keep insertion order).
## - Token: address, decimals, log_address, log_decimals.
## - Chain: key, chain_id, name, rpc (Array of String), confirmations, max_log_range,
##   explorer_tx, native_symbol, fee_hint, tokens ({symbol: Token}), testnet, family,
##   wallet_chain, native_token, min_fee_wei.
## Treat a Token or Chain as immutable, as Python's frozen ones are (copy before changing).
## MAINNET and TESTNET themselves are mutable on purpose: the integration driver patches their
## entries (rpc lists) to point at a local node, as it patches Python's.
class_name ScfgChains
extends RefCounted

const ARC_USDC := "0x3600000000000000000000000000000000000000"
const ARC_SYSTEM_EMITTER := "0xffffFFFfFFffffffffffffffFfFFFfffFFFfFFfE"
## Arc's mempool silently drops anything below 20 gwei.
const ARC_MIN_FEE_WEI := 20_000_000_000

const TOKEN_FIELDS: Array[String] = ["address", "decimals", "log_address", "log_decimals"]
const CHAIN_FIELDS: Array[String] = ["key", "chain_id", "name", "rpc", "confirmations", "max_log_range",
	"explorer_tx", "native_symbol", "fee_hint", "tokens", "testnet", "family", "wallet_chain", "native_token",
	"min_fee_wei"]

const DISPLAY_ORDER: Array[String] = ["arbitrum", "base", "arc", "polygon", "ethereum"]

static var MAINNET: Dictionary = _mainnet()
static var TESTNET: Dictionary = _testnet()


## `Token(address, decimals=6, log_address="", log_decimals=0)` with its __post_init__: an EVM
## address (one starting with "0x") is EIP-55 checksummed (a bad one is a ValueError), anything
## else is a base58 Solana mint, normalized; a non-str address is an AttributeError (Python calls
## `.startswith`). A log_address is checksummed too, and then log_decimals must be at least
## decimals (a ValueError). `opts` holds the keyword arguments decimals, log_address and
## log_decimals; any other key is a TypeError.
## @fallible
static func make_token(address: Variant, opts: Dictionary = {}) -> Variant:
	for k: Variant in opts:
		if not (k is String and k in ["decimals", "log_address", "log_decimals"]):
			return ScfgErr.make("TypeError", "Token.__init__() got an unexpected keyword argument %s" % ScfgPy.py_repr(k))
	if not (address is String or address is StringName):
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'startswith'" % ScfgPy.py_type(address))
	var decimals: Variant = opts.get("decimals", 6)
	var log_address: Variant = opts.get("log_address", "")
	var log_decimals: Variant = opts.get("log_decimals", 0)
	var normalized: Variant
	if str(address).begins_with("0x"):
		var evm: Variant = ScfgEvm.to_checksum_address(str(address))
		if evm is ScfgErr:
			return evm
		normalized = evm
	else:
		var sol: Variant = ScfgSolanaBase58.normalize_pubkey(str(address))
		if sol is ScfgErr:
			return sol
		normalized = sol
	if ScfgPy.py_truthy(log_address):
		var checked: Variant = ScfgEvm.to_checksum_address(log_address)
		if checked is ScfgErr:
			return checked
		log_address = checked
		if not (typeof(log_decimals) in [TYPE_INT, TYPE_FLOAT] and typeof(decimals) in [TYPE_INT, TYPE_FLOAT]):
			return ScfgErr.make("TypeError", "'<' not supported between instances of '%s' and '%s'" % [
				ScfgPy.py_type(log_decimals), ScfgPy.py_type(decimals)])
		if log_decimals < decimals:
			return ScfgErr.make("ValueError", "log_decimals must be at least decimals")
	return {"address": normalized, "decimals": decimals, "log_address": log_address, "log_decimals": log_decimals}


## Python's `Token.log_source`: the contract address whose Transfer events record this token's
## payments.
static func log_source(token: Dictionary) -> String:
	var own: Variant = token.get("log_address", "")
	return str(own) if ScfgPy.py_truthy(own) else str(token["address"])


## `get_chains(testnet)`: the TESTNET registry for a truthy argument, else MAINNET (the very
## dictionaries, not copies).
static func get_chains(testnet: Variant) -> Dictionary:
	return TESTNET if ScfgPy.py_truthy(testnet) else MAINNET


## Python's `Chain.explorer_url(tx_hash)`: `explorer_tx.format(tx_hash)`. A template that
## cannot be formatted is the error Python raises (build_chain refuses those).
## @fallible
static func explorer_url(chain: Dictionary, tx_hash: Variant) -> Variant:
	return ScfgPy.py_format1(chain["explorer_tx"], str(tx_hash))


## Python's `Chain.parse_transfer(log)`: a raw log as a payment in one of this chain's tokens, or
## null. Only logs from a token's log source count. Values logged with more decimals than the
## token has are floored to its units (an ScfgBigInt division: Arc values pass 2^63); a log that
## floors to 0 is not a payment. The result always carries the token's canonical address, and is
## a new Dictionary (the TransferLog fields of ScfgEvm.parse_transfer_log).
## @fallible
static func parse_transfer(chain: Dictionary, log: Variant) -> Variant:
	var t: Variant = ScfgEvm.parse_transfer_log(log)
	if t is ScfgErr or t == null:
		return t
	var found: Variant = null
	for candidate: Dictionary in (chain["tokens"] as Dictionary).values():
		if log_source(candidate) == t["token"]:
			found = candidate
			break
	if found == null:
		return null
	var token: Dictionary = found
	if not ScfgPy.py_truthy(token["log_address"]):
		return t
	var shift: int = token["log_decimals"] - token["decimals"]
	var divisor: Variant = ScfgBigInt.from_decimal("1" + "0".repeat(shift))
	if divisor is ScfgErr:
		return divisor
	var qr: Variant = (t["value"] as ScfgBigInt).divmod(divisor)  # err-ok: the divisor is a power of ten
	var value: ScfgBigInt = qr[0]
	if value.is_zero():
		return null
	var out: Dictionary = (t as Dictionary).duplicate()
	out["token"] = token["address"]
	out["value"] = value
	return out


static func _token(address: String, opts: Dictionary = {}) -> Dictionary:
	var token: Variant = make_token(address, opts)  # err-ok: the registry's pinned addresses are valid
	return token


static func _chain(fields: Dictionary) -> Dictionary:
	var out := {}
	var defaults := {"tokens": {}, "testnet": false, "family": "evm", "wallet_chain": "", "native_token": "",
		"min_fee_wei": 0}
	for f: String in CHAIN_FIELDS:
		out[f] = fields[f] if fields.has(f) else defaults[f]
	return out


static func _mainnet() -> Dictionary:
	return {
		"ethereum": _chain({
			"key": "ethereum", "chain_id": 1, "name": "Ethereum",
			"rpc": ["https://ethereum-rpc.publicnode.com", "https://eth.drpc.org"],
			"confirmations": 12, "max_log_range": 125,
			"explorer_tx": "https://etherscan.io/tx/{}", "native_symbol": "ETH", "fee_hint": "high",
			"tokens": {
				"USDC": _token("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"),
				"USDT": _token("0xdAC17F958D2ee523a2206206994597C13D831ec7"),
			},
		}),
		"arbitrum": _chain({
			"key": "arbitrum", "chain_id": 42161, "name": "Arbitrum",
			# publicnode only serves very recent Arbitrum blocks without a token: last resort.
			"rpc": ["https://arb1.arbitrum.io/rpc", "https://arbitrum.drpc.org", "https://1rpc.io/arb",
				"https://arbitrum-one-rpc.publicnode.com"],
			"confirmations": 20, "max_log_range": 2000,
			"explorer_tx": "https://arbiscan.io/tx/{}", "native_symbol": "ETH", "fee_hint": "low",
			"tokens": {
				"USDC": _token("0xaf88d065e77c8cC2239327C5EDb3A432268e5831"),
				"USDT": _token("0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9"),
			},
		}),
		"polygon": _chain({
			"key": "polygon", "chain_id": 137, "name": "Polygon",
			"rpc": ["https://polygon-bor-rpc.publicnode.com", "https://polygon.drpc.org", "https://1rpc.io/matic"],
			"confirmations": 32, "max_log_range": 500,
			"explorer_tx": "https://polygonscan.com/tx/{}", "native_symbol": "POL", "fee_hint": "lowest",
			"tokens": {
				"USDC": _token("0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359"),
				"USDT": _token("0xc2132D05D31c914a87C6611C10748AEb04B58e8F"),
			},
		}),
		"base": _chain({
			"key": "base", "chain_id": 8453, "name": "Base",
			"rpc": ["https://mainnet.base.org", "https://base-rpc.publicnode.com", "https://base.drpc.org",
				"https://1rpc.io/base"],
			"confirmations": 10, "max_log_range": 125,
			"explorer_tx": "https://basescan.org/tx/{}", "native_symbol": "ETH", "fee_hint": "low",
			"tokens": {"USDC": _token("0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913")},
		}),
		"arc": _chain({
			"key": "arc", "chain_id": 5042, "name": "Arc",
			# dRPC caps getLogs at a few dozen blocks, so it is never first, and never last
			# either: the last endpoint is the one handed to a wallet that adds the network.
			"rpc": ["https://rpc.mainnet.arc.io", "https://rpc.quicknode.mainnet.arc.io",
				"https://rpc.drpc.mainnet.arc.io", "https://rpc.blockdaemon.mainnet.arc.io"],
			"confirmations": 1, "max_log_range": 9_999,  # BFT finality: one block is final
			"explorer_tx": "https://explorer.arc.io/tx/{}", "native_symbol": "USDC", "fee_hint": "lowest",
			"native_token": "USDC", "min_fee_wei": ARC_MIN_FEE_WEI,
			"tokens": {"USDC": _token(ARC_USDC, {"log_address": ARC_SYSTEM_EMITTER, "log_decimals": 18})},
		}),
		"solana": _chain({
			"key": "solana", "chain_id": 0, "name": "Solana",
			# publicnode answers fast but keeps ~2 days of history; mainnet-beta keeps all of it.
			"rpc": ["https://solana-rpc.publicnode.com", "https://api.mainnet-beta.solana.com"],
			"confirmations": 32, "max_log_range": 0,
			"explorer_tx": "https://solscan.io/tx/{}", "native_symbol": "SOL", "fee_hint": "lowest",
			"family": "solana", "wallet_chain": "solana:mainnet",
			"tokens": {
				"USDC": _token("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"),
				"USDT": _token("Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB"),
			},
		}),
	}


static func _testnet() -> Dictionary:
	return {
		"ethereum": _chain({
			"key": "ethereum", "chain_id": 11155111, "name": "Sepolia",
			"rpc": ["https://ethereum-sepolia-rpc.publicnode.com", "https://1rpc.io/sepolia"],
			"confirmations": 3, "max_log_range": 2000,
			"explorer_tx": "https://sepolia.etherscan.io/tx/{}", "native_symbol": "ETH", "fee_hint": "high",
			"testnet": true,
			"tokens": {"USDC": _token("0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238")},
		}),
		"arbitrum": _chain({
			"key": "arbitrum", "chain_id": 421614, "name": "Arbitrum Sepolia",
			"rpc": ["https://sepolia-rollup.arbitrum.io/rpc", "https://arbitrum-sepolia-rpc.publicnode.com"],
			"confirmations": 3, "max_log_range": 2000,
			"explorer_tx": "https://sepolia.arbiscan.io/tx/{}", "native_symbol": "ETH", "fee_hint": "low",
			"testnet": true,
			"tokens": {"USDC": _token("0x75faf114eafb1BDbe2F0316DF893fd58CE46AA4d")},
		}),
		"polygon": _chain({
			"key": "polygon", "chain_id": 80002, "name": "Polygon Amoy",
			"rpc": ["https://polygon-amoy-bor-rpc.publicnode.com"],
			"confirmations": 3, "max_log_range": 2000,
			"explorer_tx": "https://amoy.polygonscan.com/tx/{}", "native_symbol": "POL", "fee_hint": "lowest",
			"testnet": true,
			"tokens": {"USDC": _token("0x41E94Eb019C0762f9Bfcf9Fb1E58725BfB0e7582")},
		}),
		"base": _chain({
			"key": "base", "chain_id": 84532, "name": "Base Sepolia",
			"rpc": ["https://base-sepolia-rpc.publicnode.com", "https://sepolia.base.org"],
			"confirmations": 3, "max_log_range": 2000,
			"explorer_tx": "https://sepolia.basescan.org/tx/{}", "native_symbol": "ETH", "fee_hint": "low",
			"testnet": true,
			"tokens": {"USDC": _token("0x036CbD53842c5426634e7929541eC2318f3dCF7e")},
		}),
		"arc": _chain({
			"key": "arc", "chain_id": 5042002, "name": "Arc Testnet",
			# Blockdaemon's testnet endpoint answers gzip unasked (urllib can't read it): left out.
			"rpc": ["https://rpc.testnet.arc.io", "https://rpc.drpc.testnet.arc.io",
				"https://rpc.quicknode.testnet.arc.io"],
			"confirmations": 1, "max_log_range": 9_999,
			"explorer_tx": "https://explorer.testnet.arc.io/tx/{}", "native_symbol": "USDC",
			"fee_hint": "lowest", "testnet": true, "native_token": "USDC", "min_fee_wei": ARC_MIN_FEE_WEI,
			"tokens": {"USDC": _token(ARC_USDC, {"log_address": ARC_SYSTEM_EMITTER, "log_decimals": 18})},
		}),
		"solana": _chain({
			"key": "solana", "chain_id": 0, "name": "Solana Devnet",
			"rpc": ["https://api.devnet.solana.com"],
			"confirmations": 32, "max_log_range": 0,
			"explorer_tx": "https://solscan.io/tx/{}?cluster=devnet", "native_symbol": "SOL",
			"fee_hint": "lowest", "testnet": true, "family": "solana", "wallet_chain": "solana:devnet",
			"tokens": {"USDC": _token("4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU")},
		}),
	}
