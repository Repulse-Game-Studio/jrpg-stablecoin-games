## Developer configuration: validation of configure()/add_item() arguments. Mirrors
## scfg_core/config.py.
##
## Every mistake is a ConfigError at startup, so it surfaces while the developer is testing
## instead of when a player tries to pay. Error codes, error classes and the order of the checks
## are Python's.
##
## Python keyword arguments are one trailing `opts` Dictionary (keys are the keyword names): an
## unexpected key, or a missing required one, is a TypeError, as for a Python call with a bad
## signature.
##
## Representation (see also ScfgChains): a Config is a Dictionary with Python's dataclass fields
## in Python's order: wallet (String or null), testnet, chains (Array of String), tokens (Array of
## String), methods ({name: {option: value}}), rpc_urls ({chain: Array of String}), confirmations
## ({chain: int}), previous_wallets, solana_wallet, previous_solana_wallets, logo, accent,
## custom_chains ({key: Chain}) and items ({item_id: Item}). `add_item` mutates `items` in place,
## as Python's does.
##
## Python's semantics on loosely typed input are kept where a caller can reach them:
## - `bool` and `int` are different types (GDScript has both), and so are `int` and `float`: an
##   integral float such as 5.0 is Python's float, never an int;
## - an iterable argument (`chains`, `tokens`, `previous_wallets`, `custom_chains`,
##   `methods.qr.chains`) may be an Array, a String (its characters) or a Dictionary (its keys);
##   anything else is a TypeError, and a dict or set lookup with an Array or Dictionary key is
##   an "unhashable" TypeError;
## - `.items()` on a value that is not a Dictionary is an AttributeError;
## - the id, symbol, colour and local-http patterns match the whole string (no trailing newline);
##   `\d` in the local-http pattern is any Unicode decimal digit.
class_name ScfgConfig
extends RefCounted

const TOKENS: Array[String] = ["USDC", "USDT"]
const DEFAULT_METHODS := {
	"browser": {"enabled": true, "timeout_min": 30},
	"qr": {"enabled": true, "wallet_button": true, "chains": null},
	"manual": {"enabled": true, "allow_tx_paste": true},
}
const FEE_HINTS: Array[String] = ["lowest", "low", "high"]
## rpc.MIN_LOG_RANGE (config.py imports it): the smallest max_log_range a custom chain may declare.
const MIN_LOG_RANGE := ScfgRpcClient.MIN_LOG_RANGE

const DEFAULT_ACCENT := "#0e7c74"
const INK := "#17202e"
## Raster formats only: an SVG served from the checkout page's origin could run script.
const LOGO_TYPES := {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".webp": "image/webp"}

const CONFIG_FIELDS: Array[String] = ["wallet", "testnet", "chains", "tokens", "methods", "rpc_urls", "confirmations",
	"previous_wallets", "solana_wallet", "previous_solana_wallets", "logo", "accent", "custom_chains", "items"]

const _BUILD_CHAIN_REQUIRED: Array[String] = ["name", "chain_id", "rpc", "tokens", "confirmations", "explorer_tx"]
const _BUILD_CHAIN_OPTIONAL: Array[String] = ["native_symbol", "fee_hint", "max_log_range", "testnet"]
const _BUILD_CONFIG_KEYS: Array[String] = ["wallet", "testnet", "chains", "tokens", "methods", "rpc_urls",
	"confirmations", "previous_wallets", "logo", "accent", "custom_chains", "solana_wallet"]
const _ADD_ITEM_REQUIRED: Array[String] = ["code", "price", "token", "title"]


# --- Config accessors (the Config dataclass' methods and properties) ----------------------------


## Every chain this game knows on its network: built-in, then the developer's own.
static func registry(config: Dictionary) -> Dictionary:
	var out: Dictionary = ScfgChains.get_chains(config["testnet"]).duplicate()
	out.merge(config["custom_chains"])
	return out


## EVM: current wallet first, then wallets used by earlier versions of the game.
static func accepted_wallets(config: Dictionary) -> Array:
	var out := []
	if ScfgPy.py_truthy(config["wallet"]):
		out.append(config["wallet"])
	out.append_array(config["previous_wallets"])
	return out


static func accepted_wallets_for(config: Dictionary, family: String) -> Array:
	if family == "solana":
		var out := []
		if ScfgPy.py_truthy(config["solana_wallet"]):
			out.append(config["solana_wallet"])
		out.append_array(config["previous_solana_wallets"])
		return out
	return accepted_wallets(config)


static func wallet_for(config: Dictionary, chain: Dictionary) -> Variant:
	return config["solana_wallet"] if chain["family"] == "solana" else config["wallet"]


## The item with this code, or null.
static func item_by_code(config: Dictionary, code: Variant) -> Variant:
	for item: Dictionary in (config["items"] as Dictionary).values():
		if item["code"] == code:
			return item
	return null


# --- helpers ---------------------------------------------------------------------------------


## A ConfigError with its code (always returned, so the callers are fallible).
## @fallible
static func config_error(code: String, message: String) -> Variant:
	return ScfgErr.make("ConfigError", message, code)


static func _is_str(v: Variant) -> bool:
	return v is String or v is StringName


## Python `isinstance(v, int)` and not a bool.
static func _is_int(v: Variant) -> bool:
	return typeof(v) == TYPE_INT or v is ScfgBigInt


## `value in <tuple or list of str>`: only a str can be equal to one. Public: ScfgPayments uses it.
static func in_strs(items: Array, value: Variant) -> bool:
	return _is_str(value) and items.has(str(value))


## Python's `tuple(x)` / iteration over a JSON-like value: an Array's items, a str's characters,
## a Dictionary's keys; anything else is a TypeError. A new Array.
## @fallible
static func _iterate(value: Variant) -> Variant:
	if value is Array:
		return (value as Array).duplicate()
	if _is_str(value):
		var out := []
		var s := str(value)
		for i in s.length():
			out.append(s[i])
		return out
	if value is Dictionary:
		return (value as Dictionary).keys()
	return ScfgErr.make("TypeError", "'%s' object is not iterable" % ScfgPy.py_type(value))


## A dict or set lookup key that Python cannot hash (list, dict) is a TypeError; null otherwise.
## @fallible
static func _unhashable(key: Variant) -> Variant:
	if key is Array or key is Dictionary:
		return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(key))
	return null


## `(...).items()` on a value Python would call it on: a Dictionary, else an AttributeError.
## @fallible
static func _as_dict(value: Variant) -> Variant:
	if value is Dictionary:
		return value
	return ScfgErr.make("AttributeError", "'%s' object has no attribute 'items'" % ScfgPy.py_type(value))


## The keyword-argument check of a Python call: a TypeError for a key outside `allowed + required`
## and for a required one that is missing; null when fine.
## @fallible
static func _check_opts(opts: Variant, required: Array[String], optional: Array[String], fn: String) -> Variant:
	if not opts is Dictionary:
		return ScfgErr.make("TypeError", "%s() options must be a Dictionary" % fn)
	for k: Variant in opts:
		if not (_is_str(k) and (str(k) in required or str(k) in optional)):
			return ScfgErr.make("TypeError", "%s() got an unexpected keyword argument %s" % [fn, ScfgPy.py_repr(k)])
	var missing := PackedStringArray()
	for k in required:
		if not opts.has(k):
			missing.append("'%s'" % k)
	if not missing.is_empty():
		return ScfgErr.make("TypeError", "%s() missing %d required keyword-only argument%s: %s" % [
			fn, missing.size(), "" if missing.size() == 1 else "s", " and ".join(missing)])
	return null


## Python's `re.fullmatch(r"[<class>]{1,max}", s)`: the whole string, no trailing newline.
static func _id_match(s: String, max_len: int, upper: bool) -> bool:
	if s.length() < 1 or s.length() > max_len:
		return false
	for i in s.length():
		var c := s.unicode_at(i)
		var ok := (c >= 0x61 and c <= 0x7a) or (c >= 0x30 and c <= 0x39)
		if upper:
			ok = ok or (c >= 0x41 and c <= 0x5a)
		else:
			ok = ok or c == 0x5f or c == 0x2d
		if not ok:
			return false
	return true


## `^http://(127\.0\.0\.1|localhost)(:\d{1,5})?(/|\Z)`: `\d` is any Unicode decimal digit; the
## URL ends there (no trailing newline) or continues with a path.
static func _local_http(u: String) -> bool:
	if not u.begins_with("http://"):
		return false
	var rest := u.substr(7)
	if rest.begins_with("127.0.0.1"):
		rest = rest.substr(9)
	elif rest.begins_with("localhost"):
		rest = rest.substr(9)
	else:
		return false
	if rest.begins_with(":"):
		var n := 1
		while n < rest.length() and ScfgPy.decimal_digit(rest.unicode_at(n)) >= 0:
			n += 1
		var digits := n - 1
		if digits < 1 or digits > 5:
			return false
		rest = rest.substr(n)
	return rest == "" or rest.begins_with("/")  # `\Z`: the URL ends, or a path follows


# --- wallets and urls ------------------------------------------------------------------------


## @fallible
static func _address(value: Variant, what: String) -> Variant:
	var out: Variant = ScfgEvm.normalize_address(value)
	if out is ScfgErr and (out as ScfgErr).is_a("ValueError"):
		return config_error("address.invalid", "%s: %s" % [what, (out as ScfgErr).message])
	return out


## @fallible
static func _solana_address(value: Variant, what: String) -> Variant:
	var address: Variant = ScfgSolanaBase58.normalize_pubkey(value)
	if address is ScfgErr:
		if (address as ScfgErr).is_a("ValueError"):
			return config_error("solana_address.invalid", "%s: not a Solana address: %s" % [what, ScfgPy.py_repr(value)])
		return address
	var raw: Variant = ScfgSolanaBase58.b58decode(address)
	if raw is ScfgErr:
		return raw
	if not ScfgEd25519.is_on_curve(raw):
		return config_error("solana_address.not_on_curve", "%s: %s looks like a token account or program address; use your wallet address" % [
			what, address])
	return address


## `(family, normalized address)`: 0x addresses are EVM, anything else Solana.
## @fallible
static func _any_wallet(value: Variant, what: String) -> Variant:
	if _is_str(value) and ScfgPy.py_strip(str(value)).begins_with("0x"):
		var evm: Variant = _address(ScfgPy.py_strip(str(value)), what)
		if evm is ScfgErr:
			return evm
		return ["evm", evm]
	var sol: Variant = _solana_address(value, what)
	if sol is ScfgErr:
		return sol
	return ["solana", sol]


## @fallible
static func _rpc_urls(value: Variant, what: String) -> Variant:
	var urls: Variant = [value] if _is_str(value) else value
	if not urls is Array or (urls as Array).is_empty():
		return config_error("rpc.bad_list", "%s must be a URL or a non-empty list of URLs" % what)
	for u: Variant in urls:
		if not _is_str(u) or not (str(u).begins_with("https://") or _local_http(str(u))):
			return config_error("rpc.not_https", "rpc url must start with https://: %s" % ScfgPy.py_repr(u))
	return (urls as Array).duplicate()


## @fallible
static func _positive_int(value: Variant, what: String, minimum: int = 1) -> Variant:
	if typeof(value) == TYPE_BOOL or not _is_int(value) or (typeof(value) == TYPE_INT and value < minimum):
		return config_error("int.below_minimum", "%s must be an int >= %d" % [what, minimum])
	return value


# --- build_chain -----------------------------------------------------------------------------


## A developer-defined EVM chain (scfg.add_chain). Validated like the rest of the configuration,
## so a typo fails at startup rather than when a player pays.
##
## `opts`: name, chain_id, rpc, tokens, confirmations and explorer_tx are required;
## native_symbol ("ETH"), fee_hint ("low"), max_log_range (1000) and testnet (false) are
## optional. Only EVM chains work: payments are found through ERC-20 Transfer logs over Ethereum
## JSON-RPC. The tokens must have 6 decimals, because the item code and nonce live in the last
## four decimals of the amount. Returns a Chain Dictionary.
## @fallible
static func build_chain(key: Variant, opts: Variant) -> Variant:
	var bad_opts: Variant = _check_opts(opts, _BUILD_CHAIN_REQUIRED, _BUILD_CHAIN_OPTIONAL, "build_chain")
	if bad_opts is ScfgErr:
		return bad_opts
	var o: Dictionary = opts
	var name: Variant = o["name"]
	var chain_id: Variant = o["chain_id"]
	var rpc: Variant = o["rpc"]
	var tokens: Variant = o["tokens"]
	var confirmations: Variant = o["confirmations"]
	var explorer_tx: Variant = o["explorer_tx"]
	var native_symbol: Variant = o.get("native_symbol", "ETH")
	var fee_hint: Variant = o.get("fee_hint", "low")
	var max_log_range: Variant = o.get("max_log_range", 1000)
	var testnet: Variant = o.get("testnet", false)
	if not _is_str(key) or not _id_match(str(key), 32, false):
		return config_error("chain.invalid_key", "invalid chain key %s (use a-z, 0-9, _ and -)" % ScfgPy.py_repr(key))
	var what := "chain %s" % ScfgPy.py_repr(key)
	if not _is_str(name) or ScfgPy.py_strip(str(name)) == "" or str(name).length() > 32:
		return config_error("chain.invalid_name", "%s: name must be a non-empty string of at most 32 characters" % what)
	chain_id = _positive_int(chain_id, "%s: chain_id" % what)
	if chain_id is ScfgErr:
		return chain_id
	rpc = _rpc_urls(rpc, "%s: rpc" % what)
	if rpc is ScfgErr:
		return rpc
	if not tokens is Dictionary or (tokens as Dictionary).is_empty():
		return config_error("chain.tokens_invalid", "%s: tokens must be a dict like {'USDC': '0x...'}" % what)
	var parsed := {}
	for symbol: Variant in tokens:
		if not in_strs(TOKENS, symbol):
			return config_error("chain.unknown_token", "%s: unknown token %s (supported: USDC, USDT)" % [what, ScfgPy.py_repr(symbol)])
		var address: Variant = _address(tokens[symbol], "%s: %s address" % [what, symbol])
		if address is ScfgErr:
			return address
		var token: Variant = ScfgChains.make_token(address)
		if token is ScfgErr:
			return token
		parsed[str(symbol)] = token
	confirmations = _positive_int(confirmations, "%s: confirmations" % what)
	if confirmations is ScfgErr:
		return confirmations
	if not _is_str(explorer_tx) or not str(explorer_tx).begins_with("https://") or _count_braces(str(explorer_tx)) != 1:
		return config_error("chain.bad_explorer_tx", "%s: explorer_tx must be an https:// URL with {} where the transaction hash goes, like 'https://snowtrace.io/tx/{}'" % what)
	var formatted: Variant = ScfgPy.py_format1(str(explorer_tx), "0x")
	if formatted is ScfgErr:
		if (formatted as ScfgErr).is_a("IndexError") or (formatted as ScfgErr).is_a("KeyError") \
				or (formatted as ScfgErr).is_a("ValueError"):
			return config_error("chain.bad_explorer_tx_braces", "%s: explorer_tx may contain no braces besides {}" % what)
		return formatted
	if not _is_str(native_symbol) or not _id_match(str(native_symbol), 11, true):
		return config_error("chain.bad_native_symbol", "%s: native_symbol must be letters/digits, like 'AVAX'" % what)
	if not in_strs(FEE_HINTS, fee_hint):
		return config_error("chain.bad_fee_hint", "%s: fee_hint must be one of %s" % [what, ", ".join(FEE_HINTS)])
	max_log_range = _positive_int(max_log_range, "%s: max_log_range" % what, MIN_LOG_RANGE)
	if max_log_range is ScfgErr:
		return max_log_range
	return {
		"key": str(key), "chain_id": chain_id, "name": ScfgPy.py_strip(str(name)), "rpc": rpc,
		"confirmations": confirmations, "max_log_range": max_log_range, "explorer_tx": str(explorer_tx),
		"native_symbol": str(native_symbol), "fee_hint": str(fee_hint), "tokens": parsed,
		"testnet": ScfgPy.py_truthy(testnet), "family": "evm", "wallet_chain": "", "native_token": "",
		"min_fee_wei": 0,
	}


## Python's `s.count("{}")`: non-overlapping occurrences.
static func _count_braces(s: String) -> int:
	var n := 0
	var i := 0
	while i < s.length() - 1:
		if s[i] == "{" and s[i + 1] == "}":
			n += 1
			i += 2
		else:
			i += 1
	return n


## Python `isinstance(x, Chain)` for plain-data chains: a Dictionary with every Chain field.
static func _is_chain(x: Variant) -> bool:
	if not x is Dictionary:
		return false
	var d: Dictionary = x
	for f in ScfgChains.CHAIN_FIELDS:
		if not d.has(f):
			return false
	return _is_str(d["key"]) and _is_int(d["chain_id"]) and d["tokens"] is Dictionary \
		and typeof(d["testnet"]) == TYPE_BOOL


## `(chains for this network keyed by key, keys only defined for the other network)`.
## @fallible
static func _custom_chains(custom: Variant, testnet: bool) -> Variant:
	var builtin: Dictionary = ScfgChains.get_chains(testnet)
	var mine := {}
	var other := {}
	var chains: Variant = []
	if ScfgPy.py_truthy(custom):
		chains = _iterate(custom)
		if chains is ScfgErr:
			return chains
	for chain: Variant in chains:
		if not _is_chain(chain):
			return config_error("chain.not_from_add_chain", "custom chains must be created with scfg.add_chain()")
		var c: Dictionary = chain
		if ScfgChains.get_chains(false).has(c["key"]) or ScfgChains.get_chains(true).has(c["key"]):
			return config_error("chain.is_builtin", "chain %s is built in; use rpc_urls/confirmations to change it, or pick another key" % ScfgPy.py_repr(c["key"]))
		if c["testnet"] != testnet:
			other[c["key"]] = true
			continue
		if mine.has(c["key"]):
			return config_error("chain.duplicate", "chain %s added twice" % ScfgPy.py_repr(c["key"]))
		for known: Dictionary in builtin.values() + mine.values():
			if known["chain_id"] == c["chain_id"]:
				return config_error("chain.duplicate_chain_id", "chain %s has the same chain_id (%s) as %s" % [
					ScfgPy.py_repr(c["key"]), c["chain_id"], ScfgPy.py_repr(known["key"])])
		mine[c["key"]] = c
	for k: Variant in mine:
		other.erase(k)
	return [mine, other]


# --- appearance ------------------------------------------------------------------------------


## @fallible
static func _logo(value: Variant) -> Variant:
	if value == null:
		return null
	var s := str(value)
	if not _is_str(value) or s == "" or s.contains("\\") or s.contains(":") or s.begins_with("/") \
			or ".." in Array(s.split("/")):
		return config_error("logo.bad_path", "logo must be a path inside the game folder, like 'images/logo.png': %s" % ScfgPy.py_repr(value))
	var kind: Variant = logo_content_type(s)
	if kind is ScfgErr:
		return kind
	return s


## The image type of a logo path by its extension; anything but .png, .jpg, .jpeg or .webp is a
## ConfigError (`logo.bad_type`).
## @fallible
static func logo_content_type(path: Variant) -> Variant:
	if not _is_str(path):
		return ScfgErr.make("TypeError", "argument of type '%s' is not iterable" % ScfgPy.py_type(path))
	var s := str(path)
	var ext := s.substr(s.rfind(".")).to_lower() if s.contains(".") else ""
	if not LOGO_TYPES.has(ext):
		return config_error("logo.bad_type", "logo must be a .png, .jpg or .webp image: %s" % ScfgPy.py_repr(s))
	return LOGO_TYPES[ext]


## @fallible
static func _accent(value: Variant) -> Variant:
	if value == null:
		return DEFAULT_ACCENT
	if not _is_str(value) or not _hex_color(str(value)):
		return config_error("accent.invalid", "accent must be a color like '#0e7c74': %s" % ScfgPy.py_repr(value))
	var digits := str(value).substr(1)
	if digits.length() == 3:
		var doubled := ""
		for i in 3:
			doubled += digits[i] + digits[i]
		digits = doubled
	return "#" + digits.substr(0, 6).to_lower()  # alpha, if any, is dropped


## `#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})`, fullmatch: no trailing newline.
static func _hex_color(s: String) -> bool:
	if not s.begins_with("#") or not s.length() in [4, 7, 9]:
		return false
	for i in range(1, s.length()):
		if not ScfgBytes.is_hex_digit(s.unicode_at(i)):
			return false
	return true


## WCAG relative luminance of a #rrggbb color.
static func _luminance(color: String) -> float:
	var channel := func(i: int) -> float:
		var c := color.substr(i, 2).hex_to_int() / 255.0
		return c / 12.92 if c <= 0.03928 else pow((c + 0.055) / 1.055, 2.4)
	return 0.2126 * channel.call(1) + 0.7152 * channel.call(3) + 0.0722 * channel.call(5)


## White or ink, whichever has the higher WCAG contrast on `color` (a valid #rrggbb).
static func text_on(color: String) -> String:
	var lum := _luminance(color)
	var white := 1.05 / (lum + 0.05)
	var ink := (lum + 0.05) / (_luminance(INK) + 0.05)
	return "#ffffff" if white >= ink else INK


# --- methods ---------------------------------------------------------------------------------


## @fallible
static func _merge_methods(methods: Variant, chains: Array) -> Variant:
	var merged: Dictionary = DEFAULT_METHODS.duplicate(true)
	if ScfgPy.py_truthy(methods):
		var table: Variant = _as_dict(methods)
		if table is ScfgErr:
			return table
		for name: Variant in table:
			if not (_is_str(name) and merged.has(str(name))):
				return config_error("methods.unknown_method", "unknown method: %s" % ScfgPy.py_repr(name))
			var options: Variant = _as_dict(table[name])
			if options is ScfgErr:
				return options
			for key: Variant in options:
				if not (_is_str(key) and (merged[str(name)] as Dictionary).has(str(key))):
					return config_error("methods.unknown_option", "unknown option %s for method %s" % [
						ScfgPy.py_repr(key), ScfgPy.py_repr(name)])
				merged[str(name)][str(key)] = options[key]
	for name: String in merged:
		if typeof(merged[name]["enabled"]) != TYPE_BOOL:
			return config_error("methods.enabled_not_bool", "methods.%s.enabled must be True or False" % name)
	var timeout: Variant = merged["browser"]["timeout_min"]
	var numeric := typeof(timeout) == TYPE_INT or typeof(timeout) == TYPE_FLOAT or timeout is ScfgBigInt
	var positive := false
	if numeric:
		positive = (not (timeout as ScfgBigInt).is_zero()) if timeout is ScfgBigInt else timeout > 0
	if not positive:
		return config_error("methods.bad_timeout", "methods.browser.timeout_min must be a number of minutes above 0")
	var any_enabled := false
	for name: String in merged:
		if merged[name]["enabled"]:
			any_enabled = true
	if not any_enabled:
		return config_error("methods.none_enabled", "at least one payment method must be enabled")
	var qr_chains: Variant = merged["qr"]["chains"]
	if qr_chains != null:
		var items: Variant = _iterate(qr_chains)
		if items is ScfgErr:
			return items
		for c: Variant in items:
			if not in_strs(chains, c):
				return config_error("methods.qr_disabled_chain", "methods.qr.chains contains disabled chain %s" % ScfgPy.py_repr(c))
	return merged


# --- build_config ----------------------------------------------------------------------------


## The developer's configuration, validated. `opts` holds the keyword arguments (all optional):
## wallet, testnet, chains, tokens, methods, rpc_urls, confirmations, previous_wallets, logo,
## accent, custom_chains (Chains from build_chain; those of the other network are ignored) and
## solana_wallet. Returns a Config Dictionary.
## @fallible
static func build_config(opts: Variant = {}) -> Variant:
	var bad_opts: Variant = _check_opts(opts, [], _BUILD_CONFIG_KEYS, "build_config")
	if bad_opts is ScfgErr:
		return bad_opts
	var o: Dictionary = opts
	var testnet := ScfgPy.py_truthy(o.get("testnet", false))
	var split: Variant = _custom_chains(o.get("custom_chains"), testnet)
	if split is ScfgErr:
		return split
	var custom: Dictionary = split[0]
	var other_network: Dictionary = split[1]
	var reg: Dictionary = ScfgChains.get_chains(testnet).duplicate()
	reg.merge(custom)
	var wallet_arg: Variant = o.get("wallet")
	var solana_arg: Variant = o.get("solana_wallet")
	if wallet_arg == null and solana_arg == null:
		return config_error("wallet.missing", "configure() needs wallet= (an EVM 0x address) and/or solana_wallet=")
	var wallet: Variant = null
	if wallet_arg != null:
		wallet = _address(wallet_arg, "wallet")
		if wallet is ScfgErr:
			return wallet
	var sol: Variant = null
	if solana_arg != null:
		sol = _solana_address(solana_arg, "solana_wallet")
		if sol is ScfgErr:
			return sol
	var prev: Array = []
	var prev_sol: Array = []
	var previous: Variant = []
	if ScfgPy.py_truthy(o.get("previous_wallets")):
		previous = _iterate(o["previous_wallets"])
		if previous is ScfgErr:
			return previous
	for w: Variant in previous:
		var routed: Variant = _any_wallet(w, "previous_wallets")
		if routed is ScfgErr:
			return routed
		var is_sol: bool = routed[0] == "solana"
		var current: Variant = sol if is_sol else wallet
		var seen: Array = prev_sol if is_sol else prev
		if routed[1] != current and not seen.has(routed[1]):
			seen.append(routed[1])

	var chains: Variant
	if o.get("chains") == null:
		chains = []
		if wallet != null:
			chains.append_array(ScfgChains.DISPLAY_ORDER)
			chains.append_array(custom.keys())
		if sol != null:
			for k: String in reg:
				if reg[k]["family"] == "solana":
					chains.append(k)
	else:
		chains = _iterate(o["chains"])
		if chains is ScfgErr:
			return chains
	if (chains as Array).is_empty():
		return config_error("chains.empty", "chains must not be empty")
	for c: Variant in chains:
		var hash_err: Variant = _unhashable(c)
		if hash_err is ScfgErr:
			return hash_err
		if _is_str(c) and other_network.has(str(c)):
			var net := "mainnet" if testnet else "testnet"
			return config_error("chains.wrong_network", "chain %s was only added for %s; add it again with testnet=%s to use it here" % [
				ScfgPy.py_repr(c), net, "True" if testnet else "False"])
		if not (_is_str(c) and reg.has(str(c))):
			return config_error("chains.unknown", "unknown chain: %s" % ScfgPy.py_repr(c))
		var family: String = reg[str(c)]["family"]
		if family == "solana" and sol == null:
			return config_error("chain.needs_solana_wallet", "chain %s needs solana_wallet=" % ScfgPy.py_repr(c))
		if family == "evm" and wallet == null:
			return config_error("chain.needs_evm_wallet", "chain %s needs wallet= (an EVM 0x address)" % ScfgPy.py_repr(c))
	var distinct := {}
	for c: Variant in chains:
		distinct[c] = true
	if distinct.size() != (chains as Array).size():
		return config_error("chains.duplicate", "duplicate chain in chains")

	var tokens: Variant
	if o.get("tokens") == null:
		tokens = TOKENS.duplicate()
	else:
		tokens = _iterate(o["tokens"])
		if tokens is ScfgErr:
			return tokens
	if (tokens as Array).is_empty():
		return config_error("tokens.empty", "tokens must not be empty")
	for t: Variant in tokens:
		if not in_strs(TOKENS, t):
			return config_error("tokens.unknown", "unknown token: %s" % ScfgPy.py_repr(t))

	var merged: Variant = _merge_methods(o.get("methods"), chains)
	if merged is ScfgErr:
		return merged

	var urls := {}
	if ScfgPy.py_truthy(o.get("rpc_urls")):
		var url_table: Variant = _as_dict(o["rpc_urls"])
		if url_table is ScfgErr:
			return url_table
		for c: Variant in url_table:
			if not in_strs(chains, c):
				return config_error("rpc_urls.unknown_chain", "rpc_urls for unknown or disabled chain %s" % ScfgPy.py_repr(c))
			var list: Variant = _rpc_urls(url_table[c], "rpc_urls[%s]" % ScfgPy.py_repr(c))
			if list is ScfgErr:
				return list
			urls[str(c)] = list

	var confs := {}
	if ScfgPy.py_truthy(o.get("confirmations")):
		var conf_table: Variant = _as_dict(o["confirmations"])
		if conf_table is ScfgErr:
			return conf_table
		for c: Variant in conf_table:
			if not in_strs(chains, c):
				return config_error("confirmations.unknown_chain", "confirmations for unknown or disabled chain %s" % ScfgPy.py_repr(c))
			if reg[str(c)]["family"] == "solana":
				return config_error("confirmations.solana_fixed", "Solana payments wait for finalization; confirmations can't be set for %s" % ScfgPy.py_repr(c))
			var n: Variant = conf_table[c]
			if typeof(n) == TYPE_BOOL or not _is_int(n) or (typeof(n) == TYPE_INT and n < 1):
				return config_error("confirmations.invalid", "confirmations must be an int >= 1")
			confs[str(c)] = n

	var logo: Variant = _logo(o.get("logo"))
	if logo is ScfgErr:
		return logo
	var accent: Variant = _accent(o.get("accent"))
	if accent is ScfgErr:
		return accent
	var config := {
		"wallet": wallet, "testnet": testnet, "chains": chains, "tokens": tokens, "methods": merged,
		"rpc_urls": urls, "confirmations": confs, "previous_wallets": prev, "solana_wallet": sol,
		"previous_solana_wallets": prev_sol, "logo": logo, "accent": accent, "custom_chains": custom, "items": {},
	}
	if enabled_chains(config).is_empty():
		return config_error("config.no_usable_token", "no enabled chain supports any enabled token")
	return config


## Chains the player may pay on, in display order, filtered to enabled tokens, with developer RPC
## URLs tried first and confirmation overrides applied. New Chain Dictionaries; the registry's
## are not touched.
static func enabled_chains(config: Dictionary) -> Dictionary:
	var reg := registry(config)
	var out := {}
	for key: String in config["chains"]:
		var base: Dictionary = reg[key]
		var tokens := {}
		for t: String in base["tokens"]:
			if (config["tokens"] as Array).has(t):
				tokens[t] = base["tokens"][t]
		if tokens.is_empty():
			continue
		var chain := base.duplicate()
		chain["tokens"] = tokens
		chain["rpc"] = (config["rpc_urls"].get(key, []) as Array) + (base["rpc"] as Array)
		chain["confirmations"] = config["confirmations"].get(key, base["confirmations"])
		out[key] = chain
	return out


## The keys of the enabled chains that carry `token`. A token that is a list or dict is
## unhashable, a TypeError.
## @fallible
static func chains_for_token(config: Dictionary, token: Variant) -> Variant:
	var hash_err: Variant = _unhashable(token)
	if hash_err is ScfgErr:
		return hash_err
	var out := []
	var enabled := enabled_chains(config)
	for k: String in enabled:
		if in_strs(enabled[k]["tokens"].keys(), token):
			out.append(k)
	return out


# --- add_item --------------------------------------------------------------------------------


## `except (TypeError, ValueError)` around parse_amount: those become item.invalid_price.
## @fallible
static func _price_error(item_id: Variant, err: ScfgErr) -> Variant:
	if err.is_a("TypeError") or err.is_a("ValueError"):
		return config_error("item.invalid_price", "item %s: %s" % [ScfgPy.py_repr(item_id), err.message])
	return err


## Declare a paid item and return it (an Item Dictionary, also stored in `config["items"]`).
## `opts`: code, price, token and title are required; min_price is optional.
## @fallible
static func add_item(config: Dictionary, item_id: Variant, opts: Variant) -> Variant:
	var bad_opts: Variant = _check_opts(opts, _ADD_ITEM_REQUIRED, ["min_price"], "add_item")
	if bad_opts is ScfgErr:
		return bad_opts
	var o: Dictionary = opts
	var code: Variant = o["code"]
	var price: Variant = o["price"]
	var token: Variant = o["token"]
	var title: Variant = o["title"]
	var min_price: Variant = o.get("min_price")
	if not _is_str(item_id) or not _id_match(str(item_id), 64, false):
		return config_error("item.invalid_id", "invalid item id %s (use a-z, 0-9, _ and -)" % ScfgPy.py_repr(item_id))
	if (config["items"] as Dictionary).has(str(item_id)):
		return config_error("item.duplicate_id", "duplicate item id %s" % ScfgPy.py_repr(item_id))
	if typeof(code) != TYPE_INT or code < 1 or code > 99:
		return config_error("item.invalid_code", "item code must be an int in 1..99")
	if item_by_code(config, code) != null:
		return config_error("item.duplicate_code", "duplicate item code %d" % code)
	var p: Variant = ScfgAmounts.parse_amount(price)
	if p is ScfgErr:
		return _price_error(item_id, p)
	var mp: Variant = p
	if min_price != null:
		mp = ScfgAmounts.parse_amount(min_price)
		if mp is ScfgErr:
			return _price_error(item_id, mp)
	if (mp as ScfgDec).cmp(p) > 0:
		return config_error("item.min_price_above_price", "min_price must not exceed price")
	var carrying: Variant = []
	if in_strs(config["tokens"], token):
		carrying = chains_for_token(config, token)
		if carrying is ScfgErr:
			return carrying
	if (carrying as Array).is_empty():
		return config_error("token.unavailable", "token %s is not available on any enabled chain" % ScfgPy.py_repr(token))
	if not _is_str(title) or ScfgPy.py_strip(str(title)) == "" or str(title).length() > 80:
		return config_error("item.invalid_title", "title must be a non-empty string of at most 80 characters")
	var item := {"item_id": str(item_id), "code": code, "price": p, "token": str(token), "title": str(title),
		"min_price": mp}
	config["items"][str(item_id)] = item
	return item
