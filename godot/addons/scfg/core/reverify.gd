## Launch-time re-verification of stored receipts against the chain. Mirrors
## scfg_core/reverify.py.
##
## Each receipt is checked by its chain family's backend:
##
## - EVM: one eth_getLogs call scoped to the receipt's own block. Block-scoped lookups keep
##   working years later, even on nodes that no longer index old transaction hashes.
## - Solana: getTransaction on the receipt's signature, asked of each endpoint in turn; a missing
##   transaction only counts when an endpoint whose history reaches back to the receipt's slot
##   says so.
##
## Only a *definitive* mismatch removes a receipt; network trouble never does.
##
## Pinned quirks of the Python reference (D14, mirrored):
## - the removed keys come back in store order;
## - the malformed-entry net catches KeyError, TypeError, ValueError, AttributeError and
##   InvalidOperation only. An "Infinity" amount (OverflowError), an infinite float log_index or
##   block (OverflowError), a stored tip beyond Decimal's range (Overflow) and an unhashable
##   token name (TypeError, raised outside the net) escape it and fail the whole run.
class_name ScfgReverify
extends RefCounted

const FUTURE_TOLERANCE_BLOCKS := 10_000
const _NET_KINDS: Array[String] = ["KeyError", "TypeError", "ValueError", "AttributeError", "InvalidOperation"]
const _INT63_MAX := 9223372036854775807


## Whether the malformed-entry net of reverify_receipts catches `err`.
static func _caught(err: ScfgErr) -> bool:
	for kind: String in _NET_KINDS:
		if err.is_a(kind):
			return true
	return false


## Python's `str.lower()` on a stored field: a TypeError-free AttributeError for a non-str.
## @fallible
static func _lower(v: Variant) -> Variant:
	if not (v is String or v is StringName):
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'lower'" % ScfgPy.py_type(v))
	return str(v).to_lower()


## "drop" when the malformed-entry net catches `err`, else `err` itself (it escapes the net).
## @fallible
static func _netted(err: ScfgErr) -> Variant:
	if _caught(err):
		return "drop"
	return err


## One check of the net on the stored field `v`: 1 `tx_hash.lower()`, 2 `int(log_index)`,
## 3 `int(block)`, 4 `decimal_to_units(Decimal(amount))`. Returns its result or its error.
## @fallible
static func _check_field(i: int, v: Variant) -> Variant:
	if i == 1:
		return _lower(v)
	if i == 4:
		return ScfgAmounts.decimal_to_units(v)
	return ScfgPy.py_int(v)


## The checks of the malformed-entry net, in Python's order: `chains.get(r["chain"])`,
## `r["tx_hash"].lower()`, `int(r["log_index"])`, `int(r["block"])`,
## `decimal_to_units(Decimal(r["amount"]))` and `r["token"]`. Returns [chain or null, token name],
## "drop" for a receipt the net catches, or the ScfgErr that escapes it.
## @fallible
static func _screen(r: Variant, chains: Dictionary) -> Variant:
	var key: Variant = ScfgPy.py_get(r, "chain")
	if key is ScfgErr:
		return _netted(key)
	if key is Array or key is Dictionary:
		return "drop"  # chains.get([..]): unhashable, a TypeError inside the net
	var found: Variant = chains.get(key)
	var fields := ["tx_hash", "log_index", "block", "amount"]
	for i in fields.size():
		var v: Variant = ScfgPy.py_get(r, fields[i])
		if v is ScfgErr:
			return _netted(v)
		var checked: Variant = _check_field(i + 1, v)
		if checked is ScfgErr:
			return _netted(checked)
	var token_name: Variant = ScfgPy.py_get(r, "token")
	if token_name is ScfgErr:
		return "drop"
	return [found, token_name]


## Remove receipts that the chain contradicts. Returns the removed keys, in store order. `opts`:
## rpc_for (a Callable(key) returning that chain's RPC object or an ScfgErr), chains ({key:
## Chain}) and wallets: an Array (the developer's wallets, for every family) or a Dictionary
## family -> Array. A family with no wallets keeps its receipts: they cannot be judged. A
## coroutine.
## @fallible
## @coroutine
static func reverify_receipts(receipts: Dictionary, opts: Dictionary) -> Variant:
	var rpc_for: Callable = opts["rpc_for"]
	var chains: Dictionary = opts["chains"]
	var wallets: Variant = opts["wallets"]
	var removed := []
	for key: Variant in receipts.keys():
		var r: Variant = receipts[key]
		var screened: Variant = _screen(r, chains)
		if screened is ScfgErr:
			return screened
		if screened is String:
			receipts.erase(key)
			removed.append(key)
			continue
		var chain: Variant = screened[0]
		if chain == null:
			continue  # chain unknown to this build: cannot judge, keep
		var token_name: Variant = screened[1]
		if token_name is Array or token_name is Dictionary:
			return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(token_name))
		if not (token_name is String or token_name is StringName) or not (chain["tokens"] as Dictionary).has(str(token_name)):
			receipts.erase(key)
			removed.append(key)
			continue
		var family_wallets: Variant = wallets
		if wallets is Dictionary:
			family_wallets = (wallets as Dictionary).get(chain["family"], [])
		var listed: Variant = ScfgPy.py_iter(family_wallets)
		if listed is ScfgErr:
			return listed
		if (listed as Array).is_empty():
			continue  # no wallet of this family configured any more: cannot judge, keep
		var backend: Variant = ScfgBackends.backend_for(chain)
		if backend is ScfgErr:
			return backend
		var rpc: Variant = rpc_for.call(chain["key"])
		if rpc is ScfgErr:
			return rpc
		var verdict: Variant = await backend.reverify(r, {"rpc": rpc, "chain": chain, "wallets": (listed as Array).duplicate()})
		if verdict is ScfgErr:
			return verdict
		if ScfgPy.py_eq(verdict, "drop"):
			receipts.erase(key)
			removed.append(key)
	return removed


## Whether any log of `logs` is the receipt's transfer. The logs are parsed by `chain.parse_transfer`
## (values floored to the token's units) or, for a null `chain`, by `parse_transfer_log` (the token
## contract's own log, whose raw value is already in the token's 6 decimals). Python's `any()` over
## a lazy generator: the logs after the first match are never parsed.
## @fallible
static func _matches(logs: Variant, chain: Variant, want: Dictionary) -> Variant:
	var seq: Variant = ScfgPy.py_iter(logs)
	if seq is ScfgErr:
		return seq
	for lg: Variant in seq:
		var t: Variant = _parse(chain, lg)
		if t is ScfgErr:
			return t
		if t != null and ScfgPy.py_eq(t["tx_hash"], want["tx_hash"]) and ScfgPy.py_eq(t["log_index"], want["log_index"]) \
				and ScfgPy.py_eq(t["value"], want["units"]) and (want["wallets"] as Array).has(t["recipient"]) \
				and ScfgPy.py_eq(t["token"], want["token"]):
			return true
	return false


## `chain.parse_transfer(lg)`, or `parse_transfer_log(lg)` for a null chain.
## @fallible
static func _parse(chain: Variant, lg: Variant) -> Variant:
	if chain == null:
		return ScfgEvm.parse_transfer_log(lg)
	return ScfgChains.parse_transfer(chain, lg)


## Judge one stored EVM receipt: "drop" if the chain contradicts it, else "keep". `opts`: rpc,
## chain, wallets. A coroutine.
## @fallible
## @coroutine
static func check_evm_receipt(r: Variant, opts: Dictionary) -> Variant:
	var rpc: Variant = opts["rpc"]
	var chain: Dictionary = opts["chain"]
	var wallets: Array = opts["wallets"]
	var tx_field: Variant = ScfgPy.py_get(r, "tx_hash")
	if tx_field is ScfgErr:
		return tx_field
	var tx_hash: Variant = _lower(tx_field)
	if tx_hash is ScfgErr:
		return tx_hash
	var raw_index: Variant = ScfgPy.py_get(r, "log_index")
	if raw_index is ScfgErr:
		return raw_index
	var log_index: Variant = ScfgPy.py_int(raw_index)
	if log_index is ScfgErr:
		return log_index
	var raw_block: Variant = ScfgPy.py_get(r, "block")
	if raw_block is ScfgErr:
		return raw_block
	var block: Variant = ScfgPy.py_int(raw_block)
	if block is ScfgErr:
		return block
	var amount: Variant = ScfgPy.py_get(r, "amount")
	if amount is ScfgErr:
		return amount
	var units: Variant = ScfgAmounts.decimal_to_units(amount)
	if units is ScfgErr:
		return units
	var token_name: Variant = ScfgPy.py_get(r, "token")
	if token_name is ScfgErr:
		return token_name
	if token_name is Array or token_name is Dictionary:
		return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(token_name))
	var token: Variant = ScfgPy.py_get(chain["tokens"], token_name)
	if token is ScfgErr:
		return token
	var wallet_topics := []
	for w: Variant in wallets:
		var topic: Variant = ScfgEvm.address_topic(w)
		if topic is ScfgErr:
			return topic
		wallet_topics.append(topic)
	var topics := [ScfgEvm.TRANSFER_TOPIC, null, wallet_topics]
	var want := {"tx_hash": tx_hash, "log_index": log_index, "units": units, "wallets": wallets,
		"token": token["address"]}

	var reported: Variant = await rpc.block_number()
	if reported is ScfgErr:
		return _keep_on_trouble(reported)
	var ahead: Variant = _ahead(block, reported)
	if ahead is ScfgErr:
		return ahead
	if ahead != 0:
		# probably a lagging endpoint: keep, unless the block is impossibly far ahead
		return "drop" if ahead > FUTURE_TOLERANCE_BLOCKS else "keep"
	var logs: Variant = await rpc.get_logs(ScfgChains.log_source(token), topics, block, block)
	if logs is ScfgErr:
		return _keep_on_trouble(logs)
	var hit: Variant = _matches(logs, chain, want)
	if hit is ScfgErr:
		return hit
	if hit:
		return "keep"
	if not ScfgPy.py_truthy(token["log_address"]):
		return "drop"
	# A receipt stored by a game that added this chain itself before it was built in
	# (add_chain("arc", ...)) points at the token contract's own log. That log is still a genuine
	# transfer, so it keeps the purchase.
	logs = await rpc.get_logs(token["address"], topics, block, block)
	if logs is ScfgErr:
		return _keep_on_trouble(logs)
	var own: Variant = _matches(logs, null, want)
	if own is ScfgErr:
		return own
	return "keep" if own else "drop"


## "keep" for the network trouble Python's `except (RpcUnavailable, RpcError)` swallows, else the
## error itself.
## @fallible
static func _keep_on_trouble(err: ScfgErr) -> Variant:
	if err.is_a("RpcUnavailable") or err.is_a("RpcError"):
		return "keep"
	return err


## How far `block` is ahead of the reported `head`: 0 when it is not ahead (`block > head` is
## false), else the (positive) distance, capped at FUTURE_TOLERANCE_BLOCKS + 1 for a distance
## that needs more than 63 bits. A block or head of 2^63 or more is an ScfgBigInt: a block that
## big is beyond any head that is an int; two of them are an OverflowError (a port limit).
## @fallible
static func _ahead(block: Variant, head: Variant) -> Variant:
	if block is ScfgBigInt:
		if head is ScfgBigInt:
			return ScfgErr.make("OverflowError", "a block and a head of 2**63 or more are not supported by the port")
		return FUTURE_TOLERANCE_BLOCKS + 1
	if head is ScfgBigInt:
		return 0
	var b: int = block
	var h: int = head
	if b <= h:
		return 0
	if h < 0 and b - h < 0:
		return FUTURE_TOLERANCE_BLOCKS + 1  # the difference wrapped
	return b - h
