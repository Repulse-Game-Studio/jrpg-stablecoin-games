## Restoring purchases from a transaction hash: the chain is the ledger. Mirrors
## scfg_core/restore.py.
##
## The player pastes a tx hash. We find the chain it lives on, pick out the USDC/USDT transfers to
## the developer, and work out what each one bought from the code encoded in its amount. Amounts
## that carry no code (e.g. an exchange sent a round 5.00) are ambiguous, so the player chooses
## which item it paid for, or keeps it as a tip.
##
## A RestoreError is an ScfgErr of kind "RestoreError" whose `code` is one of: invalid_hash |
## tx_not_found | tx_reverted | tx_not_a_payment | network_unavailable.
##
## A RestoreCandidate is a Dictionary with Python's dataclass fields in order: chain, token,
## transfer (a Transfer or TransferLog Dictionary), kind ("unlock" | "tip" | null: the player must
## choose), item, choices (an Array: the item ids and "tip" offered when kind is null) and
## already_restored.
##
## Errors follow Python: only RpcUnavailable and RpcError are swallowed by `lookup`; anything
## else (a malformed log, a value of 10**32 units or more that `decode` refuses with an
## InvalidOperation) fails the whole lookup as it is.
class_name ScfgRestore
extends RefCounted

const TIP_CHOICE := "tip"


## `RestoreError(code)`.
## @fallible
static func restore_error(code: String) -> Variant:
	return ScfgErr.make("RestoreError", code, code)


## Decide what a transfer of `value` base units (an int or ScfgBigInt) paid for:
## [kind, item, choices].
## @fallible
static func classify(config: Dictionary, value: Variant) -> Variant:
	var decoded: Variant = ScfgAmounts.decode(value)
	if decoded is ScfgErr:
		return decoded
	var code: Variant = decoded[1]
	var nonce: Variant = decoded[2]
	var amount: Variant = ScfgAmounts.units_to_decimal(value)
	if amount is ScfgErr:
		return amount
	var paid: ScfgDec = amount
	var affordable := []
	for item: Dictionary in (config["items"] as Dictionary).values():
		if (item["min_price"] as ScfgDec).cmp(paid) <= 0:
			affordable.append(item["item_id"])
	if code == 0 and nonce == 0:
		# A whole-cent amount carries no code at all: could be an exchange-rounded purchase or a
		# plain tip. Let the player say which.
		return [null, null, affordable + [TIP_CHOICE]]
	if code == 0:
		return ["tip", null, []]
	var found: Variant = ScfgConfig.item_by_code(config, code)
	if found != null and paid.cmp(found["min_price"]) >= 0:
		return ["unlock", found["item_id"], []]
	return [null, null, affordable + [TIP_CHOICE]]


## Whether a transfer value (an int or ScfgBigInt) is `<= 0`.
static func _not_positive(value: Variant) -> bool:
	if value is ScfgBigInt:
		return (value as ScfgBigInt).is_zero()
	return value <= 0


## Candidates for a normalized tx hash on one EVM chain; [] if the chain lacks it (Python's None:
## see `lookup`). `opts`: config, chain, rpc, receipts. A coroutine.
## @fallible
## @coroutine
static func lookup_evm(tx_hash: Variant, opts: Dictionary) -> Variant:
	var config: Dictionary = opts["config"]
	var chain: Dictionary = opts["chain"]
	var receipts: Dictionary = opts["receipts"]
	var rpc: Variant = opts["rpc"]
	var receipt: Variant = await rpc.get_transaction_receipt(tx_hash)
	if receipt is ScfgErr:
		return receipt
	if receipt == null:
		return []
	if not receipt is Dictionary:
		return ScfgErr.make("AttributeError", "'%s' object has no attribute 'get'" % ScfgPy.py_type(receipt))
	if ScfgPy.py_eq((receipt as Dictionary).get("status"), "0x0"):
		return restore_error("tx_reverted")
	var by_address := {}
	for name: Variant in chain["tokens"]:
		by_address[chain["tokens"][name]["address"]] = name
	var wallets := ScfgConfig.accepted_wallets(config)
	var logs: Variant = (receipt as Dictionary).get("logs")
	if not ScfgPy.py_truthy(logs):
		logs = []
	var seq: Variant = ScfgPy.py_iter(logs)
	if seq is ScfgErr:
		return seq
	var out := []
	for log: Variant in seq:
		var t: Variant = ScfgChains.parse_transfer(chain, log)
		if t is ScfgErr:
			return t
		if t == null or not by_address.has(t["token"]) or _not_positive(t["value"]) \
				or not wallets.has(t["recipient"]):
			continue
		var verdict: Variant = classify(config, t["value"])
		if verdict is ScfgErr:
			return verdict
		var key: Variant = ScfgReceipts.receipt_key(chain["key"], t["tx_hash"], t["log_index"])
		if key is ScfgErr:
			return key
		out.append({"chain": chain["key"], "token": by_address[t["token"]], "transfer": t,
			"kind": verdict[0], "item": verdict[1], "choices": verdict[2],
			"already_restored": receipts.has(key)})
	if out.is_empty():
		return restore_error("tx_not_a_payment")
	return out


## Whether `err` is what `lookup` swallows as network trouble: RpcUnavailable or RpcError.
static func _network_trouble(err: ScfgErr) -> bool:
	return err.is_a("RpcUnavailable") or err.is_a("RpcError")


## Find the chain that knows `tx_hash` and the candidates in it. `opts`: config, chains
## ({key: Chain}, asked in order), rpc_for (a Callable(key) returning that chain's RPC object or
## an ScfgErr), receipts. A coroutine.
##
## A backend's `lookup` answers [] where Python returns None (the chain does not know the
## transaction): a null `found` is what a coroutine aborted by a script error leaves, so it fails
## the lookup as a RuntimeError instead of reading as "not found".
## @fallible
## @coroutine
static func lookup(tx_hash: Variant, opts: Dictionary) -> Variant:
	var chains: Dictionary = opts["chains"]
	var rpc_for: Callable = opts["rpc_for"]
	var network_trouble := false
	var understood := false
	for key: Variant in chains:
		var chain: Dictionary = chains[key]
		var backend: Variant = ScfgBackends.backend_for(chain)
		if backend is ScfgErr:
			return backend
		var typed: ScfgBaseBackend = backend
		var tx: Variant = typed.normalize_tx(tx_hash)
		if tx is ScfgErr:
			if (tx as ScfgErr).is_a("ValueError"):
				continue
			return tx
		understood = true
		var rpc: Variant = rpc_for.call(key)
		var found: Variant = rpc
		if not rpc is ScfgErr:
			found = await backend.lookup(tx, {"config": opts["config"], "chain": chain, "rpc": rpc,
				"receipts": opts["receipts"]})
		if found is ScfgErr:
			if _network_trouble(found):
				network_trouble = true
				continue
			return found
		if found == null:
			return ScfgErr.make("RuntimeError", "the %s lookup returned null (aborted by a script error?)" % ScfgPy.py_repr(key))
		if found is Array and (found as Array).is_empty():
			continue  # Python's None: this chain does not know the transaction
		return found
	if not understood:
		return restore_error("invalid_hash")
	return restore_error("network_unavailable" if network_trouble else "tx_not_found")


## Store receipts for the candidates and return the ones added. `choices` maps candidate index
## (an int) -> item id or "tip" for candidates whose kind is null (candidates without a choice
## are skipped); null is {}. A choice the candidate does not offer is a ValueError.
## @fallible
static func apply(receipts: Dictionary, candidates: Variant, choices: Variant = null) -> Variant:
	# `choices or {}`: a falsy value is {}; another non-dict fails at its first `.get`
	var picked: Dictionary = choices if choices is Dictionary else {}
	var bad_choices := ScfgPy.py_truthy(choices) and not choices is Dictionary
	var seq: Variant = ScfgPy.py_iter(candidates)
	if seq is ScfgErr:
		return seq
	var added := []
	for i in (seq as Array).size():
		var c: Variant = seq[i]
		var kind: Variant = ScfgPy.py_get(c, "kind")
		if kind is ScfgErr:
			return kind
		var item: Variant = ScfgPy.py_get(c, "item")
		if item is ScfgErr:
			return item
		if kind == null:
			if bad_choices:
				return ScfgErr.make("AttributeError", "'%s' object has no attribute 'get'" % ScfgPy.py_type(choices))
			var choice: Variant = picked.get(i)
			if choice == null:
				continue
			var offered: Variant = ScfgPy.py_get(c, "choices")
			if offered is ScfgErr:
				return offered
			var allowed: Variant = ScfgPy.py_iter(offered)
			if allowed is ScfgErr:
				return allowed
			var ok := false
			for a: Variant in allowed:
				if ScfgPy.py_eq(a, choice):
					ok = true
					break
			if not ok:
				return ScfgErr.make("ValueError", "%s is not an allowed choice" % ScfgPy.py_repr(choice))
			if ScfgPy.py_eq(choice, TIP_CHOICE):
				kind = "tip"
				item = null
			else:
				kind = "unlock"
				item = choice
		var fields := {"kind": kind, "item": item, "source": "restore"}
		for f: String in ["chain", "transfer", "token"]:
			var v: Variant = ScfgPy.py_get(c, f)
			if v is ScfgErr:
				return v
			fields[f] = v
		var r: Variant = ScfgReceipts.make_receipt(fields)
		if r is ScfgErr:
			return r
		var was_added: Variant = ScfgReceipts.add_receipt(receipts, r)
		if was_added is ScfgErr:
			return was_added
		if was_added:
			added.append(r)
	return added
