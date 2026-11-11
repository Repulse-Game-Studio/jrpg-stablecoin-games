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
## As in Python:
## - the removed keys come back in store order;
## - a stored entry whose own fields are invalid (a missing field, a chain, token or item that
##   cannot be a key, a tx hash that is not a str, a log index or block that is not an int, an
##   amount that is not a number of base units within the stored-amount range) is removed
##   whatever chain it names, with the reason reported to `on_error`: a forged receipt cannot
##   escape verification by making its check fail;
## - a check that fails on what a node returned keeps that receipt as unverifiable, reports the
##   failure, and the run goes on. Only a CancelledError (not an Exception) stops it.
class_name ScfgReverify
extends RefCounted

const FUTURE_TOLERANCE_BLOCKS := 10_000
const _NET_KINDS: Array[String] = ["KeyError", "TypeError", "ValueError", "AttributeError", "ArithmeticError"]
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


## One check of the stored field `v`: 1 `tx_hash.lower()`, 2 `int(log_index)`, 3 `int(block)`,
## 4 `decimal_to_units(stored_amount(amount))`. Returns its result or its error.
## @fallible
static func _check_field(i: int, v: Variant) -> Variant:
	if i == 1:
		return _lower(v)
	if i == 4:
		var d: Variant = ScfgAmounts.stored_amount(v)
		if d is ScfgErr:
			return d
		return ScfgAmounts.decimal_to_units(d)
	return ScfgPy.py_int(v)


## Why the stored entry `r` is not a receipt the checks can read, or null. Python's checks, in
## order: `hash(r["chain"])`, `hash(r["token"])`, `hash(r.get("item"))`, `r["tx_hash"].lower()`,
## `int(r["log_index"]) >= 0`, `int(r["block"]) >= 0`, `decimal_to_units(stored_amount(r["amount"]))`
## and, on a chain this build knows, the tx id as its family's (no surrounding whitespace, then
## the backend's normalize_tx).
## @fallible
static func _invalid_fields(r: Variant, chains: Dictionary) -> Variant:
	for name: String in ["chain", "token"]:
		var v: Variant = ScfgPy.py_get(r, name)
		if v is ScfgErr:
			return v
		if v is Array or v is Dictionary:
			return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(v))
	var item: Variant = (r as Dictionary).get("item") if r is Dictionary else null
	if item is Array or item is Dictionary:
		return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(item))
	var fields := ["tx_hash", "log_index", "block", "amount"]
	for i in fields.size():
		var v: Variant = ScfgPy.py_get(r, fields[i])
		if v is ScfgErr:
			return v
		var checked: Variant = _check_field(i + 1, v)
		if checked is ScfgErr:
			return checked
		if (i == 1 or i == 2) and _negative(checked):
			return ScfgErr.make("ValueError", "log index and block must be >= 0")
	var chain: Variant = chains.get((r as Dictionary)["chain"])
	if chain is Dictionary:
		var tx: String = str((r as Dictionary)["tx_hash"])
		if tx != ScfgPy.py_strip(tx):
			return ScfgErr.make("ValueError", "tx id with surrounding whitespace: %s" % ScfgPy.py_repr(tx))
		var backend: Variant = ScfgBackends.backend_for(chain)
		if backend is ScfgErr:
			return backend
		var typed: ScfgBaseBackend = backend
		var normalized: Variant = typed.normalize_tx(tx)
		if normalized is ScfgErr:
			return normalized
	return null


## Whether an int (or ScfgBigInt, which is never negative) is below zero.
static func _negative(n: Variant) -> bool:
	return n is int and n < 0


## Remove receipts that the chain contradicts. Returns the removed keys, in store order. `opts`:
## rpc_for (a Callable(key) returning that chain's RPC object or an ScfgErr), chains ({key:
## Chain}), wallets: an Array (the developer's wallets, for every family) or a Dictionary
## family -> Array, and on_error (optional: told why each invalid entry was removed and of each
## check that failed on node data, whose receipt is kept; by default it is printed). A family with
## no wallets keeps its receipts: they cannot be judged. A coroutine.
## @fallible: only a CancelledError
## @coroutine
static func reverify_receipts(receipts: Dictionary, opts: Dictionary) -> Variant:
	var removed := []
	for key: Variant in receipts.keys():
		var r: Variant = receipts[key]
		var problem: Variant = _invalid_fields(r, opts["chains"])
		if problem is ScfgErr:
			if not _caught(problem):
				return problem
			receipts.erase(key)
			removed.append(key)
			ScfgErr.report(opts.get("on_error"), ScfgErr.make("ValueError",
				"stored receipt %s is invalid and was removed: %s" % [ScfgPy.py_repr(key), str(problem)]))
			continue
		var verdict: Variant = await _judge(r, opts)
		if verdict is ScfgErr:
			if not (verdict as ScfgErr).is_a("Exception"):
				return verdict
			ScfgErr.report(opts.get("on_error"), verdict)  # node data: unverifiable, kept, the rest still checked
			continue
		if ScfgPy.py_eq(verdict, "drop"):
			receipts.erase(key)
			removed.append(key)
	return removed


## A well-formed receipt's verdict: "drop", "keep" (or the backend's own answer) or the ScfgErr of
## a check that failed. A coroutine.
## @fallible
## @coroutine
static func _judge(r: Dictionary, opts: Dictionary) -> Variant:
	var rpc_for: Callable = opts["rpc_for"]
	var chains: Dictionary = opts["chains"]
	var wallets: Variant = opts["wallets"]
	var chain: Variant = chains.get(r["chain"])
	if chain == null:
		return "keep"  # chain unknown to this build: cannot judge, keep
	var token_name: Variant = r["token"]
	if not (token_name is String or token_name is StringName) or not (chain["tokens"] as Dictionary).has(str(token_name)):
		return "drop"
	var family_wallets: Variant = wallets
	if wallets is Dictionary:
		family_wallets = (wallets as Dictionary).get(chain["family"], [])
	var listed: Variant = ScfgPy.py_iter(family_wallets)
	if listed is ScfgErr:
		return listed
	if (listed as Array).is_empty():
		return "keep"  # no wallet of this family configured any more: cannot judge, keep
	var backend: Variant = ScfgBackends.backend_for(chain)
	if backend is ScfgErr:
		return backend
	var rpc: Variant = rpc_for.call(chain["key"])
	if rpc is ScfgErr:
		return rpc
	return await backend.reverify(r, {"rpc": rpc, "chain": chain, "wallets": (listed as Array).duplicate()})


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
