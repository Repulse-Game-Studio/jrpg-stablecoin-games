## Solana: payments are found by their Solana Pay reference, by a reported signature, or by
## scanning the developer's token account for the exact encoded amount. A payment counts once the
## cluster marks its transaction finalized. Mirrors scfg_core/backends/solana.py, statement by
## statement.
##
## What a transaction paid is read from meta.pre/postTokenBalances, never from its instructions,
## so it works whatever program or wallet moved the tokens.
##
## GDScript specifics:
## - Python's module function `transfer_to` is the static `ScfgSolanaBackend.transfer_to(tx,
##   {sig, mint, owner})`; keyword arguments travel in `opts` Dictionaries, as everywhere.
## - Python's tests monkeypatch SIG_PAGE and MAX_SIG_PAGES, so each instance carries them as
##   `sig_page` and `max_sig_pages`.
## - Node replies and stored records are read the way Python reads them: `d[k]` is ScfgPy.py_get,
##   `d.get(k)` is `_dget`, `x or {}` is Python truthiness, `==` is ScfgPy.py_eq. So a malformed
##   reply is an error the Engine contains, never a different path.
## - `lookup` answers [] where Python returns None: a null is what a coroutine aborted by a script
##   error leaves, and ScfgRestore.lookup treats it as an error (see ScfgBaseBackend.lookup).
##
## Port limits (no vector reaches them):
## - A token-balance row's accountIndex must be a JSON number (int or integral float); Python
##   also takes an all-string (or None) set of indexes, which no node sends.
## - A slot of 2^63 or more is an OverflowError (Python's int has no bound). Token amounts are
##   exact at any size (signed ScfgBigInt arithmetic).
## - As on EVM (see ScfgWatcher), a match persists `value` as a JSON number: a transfer of 2^53
##   units or more is a ValueError (contained by the Engine) instead of a rounded value.
class_name ScfgSolanaBackend
extends ScfgBaseBackend

## Solana's "finalized" corresponds to 32 confirmed blocks.
const FINAL_CONFIRMATIONS := 32
## getTransaction calls per step for the token-account scan.
const SCAN_TX_BUDGET := 5
const SIG_PAGE := 100
const MAX_SIG_PAGES := 10
const START_SLOT_MARGIN := 150
const COMPUTE_UNITS := 30_000
## micro-lamports per compute unit
const MIN_PRIORITY_FEE := 1_000
## x 30,000 CU = at most 30,000 lamports (0.00003 SOL)
const MAX_PRIORITY_FEE := 1_000_000
## A wallet returns the signature before the transaction lands, and a transaction whose blockhash
## expires (~60-90 s) never lands. A hint unseen for this long was dropped.
const SOLANA_HINT_TIMEOUT_S := 180

## SIG_PAGE for this instance (Python's tests patch the module constant).
var sig_page: int = SIG_PAGE
## MAX_SIG_PAGES for this instance (Python's tests patch the module constant).
var max_sig_pages: int = MAX_SIG_PAGES


func _init() -> void:
	family = "solana"


# --- reading node replies the way Python does ----------------------------------------------------


## Python's `d.get(key)`: the value or null for a dict; any other value has no `.get`.
## @fallible
static func _dget(d: Variant, key: String) -> Variant:
	if d is Dictionary:
		return (d as Dictionary).get(key)
	return ScfgErr.make("AttributeError", "'%s' object has no attribute 'get'" % ScfgPy.py_type(d))


## Python's `v or fallback`.
static func _or(v: Variant, fallback: Variant) -> Variant:
	return v if ScfgPy.py_truthy(v) else fallback


static func _is_nil(v: Variant) -> bool:
	return typeof(v) == TYPE_NIL


## Python's `reversed(v)` over a JSON value: a list's items (a dict's keys, a str's characters)
## last first; anything else is a TypeError.
## @fallible
static func _reversed(v: Variant) -> Variant:
	var items: Variant = ScfgPy.py_iter(v)
	if items is ScfgErr:
		return items
	var out: Array = (items as Array).duplicate()
	out.reverse()
	return out


## Python's `a, b = v`: [a, b], a TypeError for a value that is not iterable, a ValueError for one
## that does not hold exactly two.
## @fallible
static func _pair(v: Variant) -> Variant:
	var items: Variant = ScfgPy.py_iter(v)
	if items is ScfgErr:
		return items
	if (items as Array).size() != 2:
		return ScfgErr.make("ValueError", "expected 2 values to unpack, got %d" % (items as Array).size())
	return [items[0], items[1]]


## Python's `a < b` for a slot (an int, or an ScfgBigInt of 2^63 or more) and a stored number.
## @fallible
static func _lt(a: Variant, b: Variant) -> Variant:
	if b is bool:
		b = 1 if b else 0
	if b is ScfgBigInt:
		if a is ScfgBigInt:
			return (a as ScfgBigInt).cmp(b) < 0
		return true
	if not (b is int or b is float):
		return ScfgErr.make("TypeError", "'<' not supported between instances of 'int' and '%s'" % ScfgPy.py_type(b))
	if a is ScfgBigInt:
		return b is float and (a as ScfgBigInt).to_decimal().to_float() < b
	return a < b


# --- signed amounts: [negative, ScfgBigInt magnitude] (Python's unbounded int) -----------------


static func _signed(n: Variant) -> Array:
	if n is ScfgBigInt:
		return [false, n]
	var v: int = n
	if v >= 0:
		return [false, ScfgBigInt.from_int(v)]  # err-ok: v >= 0
	if v == -9223372036854775807 - 1:
		return [true, ScfgBigInt.from_hex("8000000000000000")]  # err-ok: a hex literal
	return [true, ScfgBigInt.from_int(-v)]  # err-ok: -v > 0


static func _norm(negative: bool, mag: ScfgBigInt) -> Array:
	return [negative and not mag.is_zero(), mag]


static func _add(a: Array, b: Array) -> Array:
	var am: ScfgBigInt = a[1]
	var bm: ScfgBigInt = b[1]
	if a[0] == b[0]:
		return _norm(a[0], am.add(bm))
	if am.cmp(bm) >= 0:
		return _norm(a[0], am._sub(bm))
	return _norm(b[0], bm._sub(am))


static func _minus(a: Array, b: Array) -> Array:
	return _add(a, [not b[0], b[1]])


static func _cmp(a: Array, b: Array) -> int:
	if a[0] != b[0]:
		return -1 if a[0] else 1
	var c := (a[1] as ScfgBigInt).cmp(b[1])
	return -c if a[0] else c


static func _positive(a: Array) -> bool:
	return not a[0] and not (a[1] as ScfgBigInt).is_zero()


# --- transfer_to ---------------------------------------------------------------------------------


## The fee payer's pubkey (`accountKeys[0]`, a string or a `{pubkey}` entry), or null if there is
## none. Python indexes a str (its first character) and fails on a dict (KeyError 0).
## @fallible
static func _fee_payer(tx: Variant) -> Variant:
	var t: Variant = _dget(tx, "transaction")
	if t is ScfgErr:
		return t
	var message: Variant = _dget(_or(t, {}), "message")
	if message is ScfgErr:
		return message
	var keys: Variant = _dget(_or(message, {}), "accountKeys")
	if keys is ScfgErr:
		return keys
	keys = _or(keys, [])
	if not ScfgPy.py_truthy(keys):
		return null
	var first: Variant
	if keys is Array:
		first = keys[0]
	elif keys is String or keys is StringName:
		first = str(keys)[0]
	elif keys is Dictionary:
		return ScfgErr.make("KeyError", "0")
	else:
		return ScfgErr.make("TypeError", "'%s' object is not subscriptable" % ScfgPy.py_type(keys))
	if first is String or first is StringName:
		return first
	return _dget(first, "pubkey")


## A token-balance row's accountIndex as a dict key (see the header's port limits).
## @fallible
static func _account_index(idx: Variant) -> Variant:
	if idx is int:
		return idx
	if idx is float:
		var f: float = idx
		if not is_nan(f) and not is_inf(f) and f == floorf(f) and absf(f) < float(ScfgPy.TWO_53):
			return int(f)
	if idx is Array or idx is Dictionary:
		return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(idx))
	return ScfgErr.make("TypeError", "unsupported accountIndex %s (a JSON number is expected)" % ScfgPy.py_repr(idx))


## Python's `table(name)`: {accountIndex: [owner, signed amount]} of the rows for `mint`.
## @fallible
static func _table(meta: Variant, name: String, mint: Variant) -> Variant:
	var rows := {}
	var listed: Variant = _dget(meta, name)
	if listed is ScfgErr:
		return listed
	if not ScfgPy.py_truthy(listed):
		return rows
	var items: Variant = ScfgPy.py_iter(listed)
	if items is ScfgErr:
		return items
	for b: Variant in items:
		var row_mint: Variant = _dget(b, "mint")
		if row_mint is ScfgErr:
			return row_mint
		if not ScfgPy.py_eq(row_mint, mint):
			continue
		# Python evaluates the right-hand side (owner, int(amount)) before the subscript key
		var who: Variant = (b as Dictionary).get("owner")
		var ui: Variant = ScfgPy.py_get(b, "uiTokenAmount")
		if ui is ScfgErr:
			return ui
		var raw: Variant = ScfgPy.py_get(ui, "amount")
		if raw is ScfgErr:
			return raw
		var amount: Variant = ScfgPy.py_int(raw)
		if amount is ScfgErr:
			return amount
		var idx: Variant = ScfgPy.py_get(b, "accountIndex")
		if idx is ScfgErr:
			return idx
		var key: Variant = _account_index(idx)
		if key is ScfgErr:
			return key
		rows[key] = [who, _signed(amount)]
	return rows


## What `owner` received of `mint` in a transaction, from its token-balance changes: a Transfer
## (ScfgTransfers; `value` an ScfgBigInt, `log_index` the first of the owner's token accounts that
## gained, `sender` the account owner that lost the most, else the fee payer) or null. `opts`: sig,
## mint, owner.
## @fallible
static func transfer_to(tx: Variant, opts: Dictionary) -> Variant:
	var sig: Variant = opts["sig"]
	var mint: Variant = opts["mint"]
	var owner: Variant = opts["owner"]
	var meta: Variant = _dget(tx, "meta")
	if meta is ScfgErr:
		return meta
	meta = _or(meta, {})
	var failed: Variant = _dget(meta, "err")
	if failed is ScfgErr:
		return failed
	if not _is_nil(failed):
		return null
	var pre: Variant = _table(meta, "preTokenBalances", mint)
	if pre is ScfgErr:
		return pre
	var post: Variant = _table(meta, "postTokenBalances", mint)
	if post is ScfgErr:
		return post
	var seen := {}
	for k: Variant in pre:
		seen[k] = true
	for k: Variant in post:
		seen[k] = true
	var indexes: Array = seen.keys()
	indexes.sort()
	var zero := _signed(0)
	var gained := zero
	var index: Variant = null
	var drops := []  # [[who, signed total]] in first-drop order: Python's dict
	for idx: int in indexes:
		var row: Array = post[idx] if (post as Dictionary).has(idx) else pre[idx]
		var who: Variant = row[0]
		var after: Array = post[idx][1] if (post as Dictionary).has(idx) else zero
		var before: Array = pre[idx][1] if (pre as Dictionary).has(idx) else zero
		var delta := _minus(after, before)
		if ScfgPy.py_eq(who, owner):
			gained = _add(gained, delta)
			if _positive(delta) and index == null:
				index = idx
		elif delta[0]:
			if who is Array or who is Dictionary:
				return ScfgErr.make("TypeError", "unhashable type: '%s'" % ScfgPy.py_type(who))
			var known := false
			for d: Array in drops:
				if ScfgPy.py_eq(d[0], who):
					d[1] = _add(d[1], delta)
					known = true
					break
			if not known:
				drops.append([who, delta])
	if not _positive(gained) or index == null:
		return null
	var payer: Variant
	if not drops.is_empty():
		var best: Array = drops[0]  # Python's min(drops, key=drops.get): the first of the largest drops
		for d: Array in drops:
			if _cmp(d[1], best[1]) < 0:
				best = d
		payer = best[0]
	else:
		payer = _fee_payer(tx)
		if payer is ScfgErr:
			return payer
	var raw_slot: Variant = ScfgPy.py_get(tx, "slot")
	if raw_slot is ScfgErr:
		return raw_slot
	var slot: Variant = ScfgPy.py_int(raw_slot)
	if slot is ScfgErr:
		return slot
	if slot is ScfgBigInt:
		return ScfgErr.make("OverflowError", "slot %s is beyond the port's 64-bit ints" % (slot as ScfgBigInt).to_decimal())
	return ScfgTransfers.make_transfer({"token": mint, "sender": payer, "recipient": owner, "value": gained[1],
		"tx_hash": sig, "log_index": index, "block_number": slot})


## A Transfer's match record: its value must fit a JSON number exactly (see the header).
## @fallible
static func _match_dict(t: Dictionary) -> Variant:
	var value: Variant = ScfgWatcher._stored_int(t["value"], "transfer value")
	if value is ScfgErr:
		return value
	return {"tx_hash": t["tx_hash"], "log_index": t["log_index"], "block": t["block_number"], "value": value,
		"payer": t["sender"], "recipient": t["recipient"]}


# --- the backend ---------------------------------------------------------------------------------


## @fallible
func normalize_tx(text: Variant) -> Variant:
	return ScfgSolanaBase58.normalize_signature(text)


## Python's `SolanaRpc(RpcClient(chain.rpc))`, on `transport` and `clock` (an empty Callable keeps
## the client's default clock). A chain without urls is a ValueError.
## @fallible
func make_rpc(chain: Dictionary, transport: Variant, clock: Callable) -> Variant:
	var opts := {"transport": transport}
	if clock.is_valid():
		opts["clock"] = clock
	var client: Variant = ScfgRpcClient.create(chain["rpc"], opts)
	if client is ScfgErr:
		return client
	return ScfgSolanaRpc.new(client)


## Python's `min(MAX_PRIORITY_FEE, max(MIN_PRIORITY_FEE, fee))`.
## @fallible
static func _clamp_fee(fee: Variant) -> Variant:
	if fee is ScfgBigInt:
		return MAX_PRIORITY_FEE
	if fee is bool:
		fee = 1 if fee else 0
	if not (fee is int or fee is float):
		return ScfgErr.make("TypeError", "'>' not supported between instances of '%s' and 'int'" % ScfgPy.py_type(fee))
	var at_least: Variant = fee if fee > MIN_PRIORITY_FEE else MIN_PRIORITY_FEE
	return at_least if at_least < MAX_PRIORITY_FEE else MAX_PRIORITY_FEE


## Unsigned transfer from the player's wallet, for the browser page to hand to the wallet.
## `opts`: rpc, chain, account. Returns {transaction (padded base64), token_balance, sol_balance
## (str)}. An account that is not a Solana address is a ValueError, before any request.
## @fallible
## @coroutine
func build_payment(rec: Variant, opts: Dictionary) -> Variant:
	var rpc: Variant = opts["rpc"]
	var chain: Dictionary = opts["chain"]
	var account: Variant = ScfgSolanaBase58.normalize_pubkey(opts["account"])
	if account is ScfgErr:
		return account
	var token: Variant = ScfgWatcher._token(rec, chain)
	if token is ScfgErr:
		return token
	var mint: Variant = token["address"]
	var source: Variant = ScfgSolanaPda.associated_token_address(account, mint)
	if source is ScfgErr:
		return source
	var token_balance: Variant = await rpc.get_token_balance(source)
	if token_balance is ScfgErr:
		return token_balance
	var sol_balance: Variant = await rpc.get_balance(account)
	if sol_balance is ScfgErr:
		return sol_balance
	var blockhash: Variant = await rpc.get_latest_blockhash()
	if blockhash is ScfgErr:
		return blockhash
	var dest: Variant = ScfgPy.py_get(rec, "dest")
	if dest is ScfgErr:
		return dest
	var fee: Variant = await rpc.get_priority_fee([dest])
	if fee is ScfgErr:
		return fee
	var price: Variant = _clamp_fee(fee)
	if price is ScfgErr:
		return price
	var limit_ix: Variant = ScfgSolanaTx.set_compute_unit_limit(COMPUTE_UNITS)
	if limit_ix is ScfgErr:
		return limit_ix
	var price_ix: Variant = ScfgSolanaTx.set_compute_unit_price(price)
	if price_ix is ScfgErr:
		return price_ix
	var fields := {}
	for f: String in ["dest", "units", "reference"]:
		var v: Variant = ScfgPy.py_get(rec, f)
		if v is ScfgErr:
			return v
		fields[f] = v
	var transfer_ix: Variant = ScfgSolanaTx.transfer_checked({"source": source, "mint": mint, "destination": fields["dest"],
		"owner": account, "amount": fields["units"], "decimals": 6, "extra": [fields["reference"]]})
	if transfer_ix is ScfgErr:
		return transfer_ix
	var message: Variant = ScfgSolanaTx.compile_message(account, [limit_ix, price_ix, transfer_ix], blockhash)
	if message is ScfgErr:
		return message
	var tx: Variant = ScfgSolanaTx.unsigned_transaction(message)
	if tx is ScfgErr:
		return tx
	return {"transaction": ScfgBytes.base64(tx), "token_balance": ScfgPy.py_str(token_balance), "sol_balance": ScfgPy.py_str(sol_balance)}


## Fields for a new session: start_block (START_SLOT_MARGIN before the slot), reference (32 bytes
## drawn from `entropy.token_bytes` after the network calls, base58), dest (the developer's token
## account), scan_cursor (its newest signature, or null) and scan_before (null). `opts`: rpc,
## chain, wallet, token, entropy (null: OS randomness).
## @fallible
func start(opts: Dictionary) -> Variant:
	var rpc: Variant = opts["rpc"]
	var chain: Dictionary = opts["chain"]
	var token: Variant = ScfgWatcher._token({"token": opts["token"]}, chain)  # chain.tokens[token]
	if token is ScfgErr:
		return token
	var dest: Variant = ScfgSolanaPda.associated_token_address(opts["wallet"], token["address"])
	if dest is ScfgErr:
		return dest
	var reported: Variant = await rpc.get_slot()
	if reported is ScfgErr:
		return reported
	var slot: Variant = ScfgWatcher._head(reported)
	if slot is ScfgErr:
		return slot
	var newest: Variant = await rpc.get_signatures_for_address(dest, {"limit": 1})
	if newest is ScfgErr:
		return newest
	var start_block: int = maxi(0, slot - START_SLOT_MARGIN)
	var entropy: Variant = opts.get("entropy")
	if entropy == null:
		entropy = ScfgSessions.OsEntropy.new()
	var drawn: Variant = entropy.token_bytes(32)
	if drawn is ScfgErr:
		return drawn
	if not drawn is PackedByteArray:
		return ScfgErr.make("TypeError", "token_bytes returned %s, not bytes" % ScfgPy.py_type(drawn))
	var reference := ScfgSolanaBase58.b58encode(drawn)
	var cursor: Variant = null
	if ScfgPy.py_truthy(newest):
		var first: Variant = ScfgPy.py_get(newest, 0)
		if first is ScfgErr:
			return first
		cursor = ScfgPy.py_get(first, "signature")
		if cursor is ScfgErr:
			return cursor
	return {"start_block": start_block, "reference": reference, "dest": dest, "scan_cursor": cursor, "scan_before": null}


# --- watching ------------------------------------------------------------------------------------


## Move one pending session forward; returns a StepOutcome (see ScfgWatcher). `opts`: rpc, chain,
## wallet, items, now. Python's `except (RpcUnavailable, RpcError)` around the step: network
## trouble keeps a verdict on the hint from earlier in this step, else sets network_unavailable;
## any other error is returned for the Engine to contain.
## @fallible
func advance(pending: Dictionary, receipts: Dictionary, sid: Variant, opts: Dictionary) -> Variant:
	var rec: Variant = pending.get(sid)
	var made: Variant = ScfgWatcher.step_outcome(sid, "missing", {"required": FINAL_CONFIRMATIONS})
	if made is ScfgErr:
		return made
	var out: Dictionary = made
	if rec == null:
		return out
	var token: Variant = ScfgWatcher._token(rec, opts["chain"])
	if token is ScfgErr:
		return token
	var result: Variant = await _step(pending, receipts, sid, rec, token["address"], opts, out)
	if result is ScfgErr and ((result as ScfgErr).is_a("RpcUnavailable") or (result as ScfgErr).is_a("RpcError")):
		# A verdict on the hint from earlier in this step wins: the hint is already cleared, and
		# later steps are plain "waiting", so it would never be reported.
		if out["error"] == null:
			out["error"] = "network_unavailable"
		return out
	return result


## The body of Python's try in `advance`: the outcome, or the first error.
## @fallible
func _step(pending: Dictionary, receipts: Dictionary, sid: Variant, rec: Dictionary, mint: Variant, opts: Dictionary,
		out: Dictionary) -> Variant:
	var rpc: Variant = opts["rpc"]
	var chain: Dictionary = opts["chain"]
	var wallet: Variant = opts["wallet"]
	var items: Dictionary = opts["items"]
	var now: Variant = opts["now"]
	var state: Variant = ScfgPy.py_get(rec, "state")
	if state is ScfgErr:
		return state
	if ScfgPy.py_eq(state, "awaiting"):
		out["status"] = "waiting"
		var past: Variant = ScfgSessions.is_past_horizon(rec, now)
		if past is ScfgErr:
			return past
		if past:
			ScfgSessions.remove_session(pending, sid)
			out["status"] = "closed"
			return out
		var outcome: Variant = null
		var hint: Variant = ScfgPy.py_get(rec, "hint_tx")
		if hint is ScfgErr:
			return hint
		if ScfgPy.py_truthy(hint):
			outcome = await _verify_hint(rec, rpc, chain, mint, wallet, items, receipts, now, out)
			if outcome is ScfgErr:
				return outcome
		if outcome == null:
			# The reference also runs while a hint is pending: the hinted transaction may have been
			# dropped and the player paid again.
			outcome = await _by_reference(rec, rpc, chain, mint, wallet, items, receipts)
			if outcome is ScfgErr:
				return outcome
			if outcome != null:
				ScfgSessions.clear_hint(rec)
				out["error"] = null
				out["hint_tx"] = null
			elif not ScfgPy.py_truthy(rec["hint_tx"]):
				var found: Variant = await _scan(rec, rpc, chain, mint, wallet, receipts)
				if found is ScfgErr:
					return found
				if found:
					outcome = "matched"
		if ScfgPy.py_eq(outcome, "underpaid"):
			ScfgSessions.remove_session(pending, sid)
			out["status"] = "underpaid"
			var underpaid: Variant = ScfgWatcher.result_from_match(rec, "underpaid", chain["key"], rec["match"])
			if underpaid is ScfgErr:
				return underpaid
			out["result"] = underpaid
			return out
		if not ScfgPy.py_eq(outcome, "matched"):
			return out
		rec["state"] = "confirming"

	out["status"] = "confirming"
	var m: Variant = ScfgPy.py_get(rec, "match")
	if m is ScfgErr:
		return m
	var tx_hash: Variant = ScfgPy.py_get(m, "tx_hash")
	if tx_hash is ScfgErr:
		return tx_hash
	var statuses: Variant = await rpc.get_signature_statuses([tx_hash])
	if statuses is ScfgErr:
		return statuses
	var status: Variant = ScfgPy.py_get(statuses, 0)
	if status is ScfgErr:
		return status
	if _is_nil(status):
		# Unknown to this endpoint: a dropped fork or a lagging endpoint. Keep the signature as a
		# hint so it is verified again rather than lost.
		rec["state"] = "awaiting"
		rec["match"] = null
		rec["hint_tx"] = tx_hash
		rec["hint_at"] = now
		out["status"] = "reorged"
		return out
	var finality: Variant = _dget(status, "confirmationStatus")
	if finality is ScfgErr:
		return finality
	if not ScfgPy.py_eq(finality, "finalized"):
		var count: Variant = (status as Dictionary).get("confirmations")
		var n: Variant = ScfgPy.py_int(count if ScfgPy.py_truthy(count) else 0)
		if n is ScfgErr:
			return n
		out["confirmations"] = n
		return out
	out["confirmations"] = FINAL_CONFIRMATIONS
	var fields := {"token": mint}
	for pair: Array in [["sender", "payer"], ["recipient", "recipient"], ["value", "value"], ["tx_hash", "tx_hash"],
			["log_index", "log_index"], ["block_number", "block"]]:
		var v: Variant = ScfgPy.py_get(m, pair[1])
		if v is ScfgErr:
			return v
		fields[pair[0]] = v
	var transfer: Variant = ScfgTransfers.make_transfer(fields)
	if transfer is ScfgErr:
		return transfer
	var receipt_opts := {"chain": chain["key"], "transfer": transfer, "source": "payment"}
	for f: String in ["kind", "item", "token"]:
		var v: Variant = ScfgPy.py_get(rec, f)
		if v is ScfgErr:
			return v
		receipt_opts[f] = v
	var receipt: Variant = ScfgReceipts.make_receipt(receipt_opts)
	if receipt is ScfgErr:
		return receipt
	var added: Variant = ScfgReceipts.add_receipt(receipts, receipt)
	if added is ScfgErr:
		return added
	ScfgSessions.remove_session(pending, sid)
	out["status"] = "paid"
	var paid: Variant = ScfgWatcher.result_from_match(rec, "paid", chain["key"], m)
	if paid is ScfgErr:
		return paid
	out["result"] = paid
	return out


## Python's `_qualifies`: a transfer at or after the session's start slot that is not already a
## receipt.
## @fallible
static func _qualifies(t: Variant, rec: Dictionary, chain: Dictionary, receipts: Dictionary) -> Variant:
	if t == null:
		return false
	var start_slot: Variant = ScfgSessions.number_field(rec, "start_block")
	if start_slot is ScfgErr:
		return start_slot
	if not t["block_number"] >= start_slot:
		return false
	var key: Variant = ScfgReceipts.receipt_key(chain["key"], t["tx_hash"], t["log_index"])
	if key is ScfgErr:
		return key
	return not receipts.has(key)


## Python's `_verdict`: records the match, then "underpaid" or "matched".
## @fallible
static func _verdict(t: Dictionary, rec: Dictionary, items: Dictionary) -> Variant:
	var m: Variant = _match_dict(t)
	if m is ScfgErr:
		return m
	rec["match"] = m
	var paid: Variant = ScfgAmounts.units_to_decimal(t["value"])
	if paid is ScfgErr:
		return paid
	var least: Variant = ScfgWatcher.min_amount(rec, items)
	if least is ScfgErr:
		return least
	return "underpaid" if (paid as ScfgDec).cmp(least) < 0 else "matched"


## Check the signature the browser page reported (or the player pasted): "matched", "underpaid",
## or null (keep waiting / hint dropped, with `out` naming the verdict and the hint).
## @fallible
func _verify_hint(rec: Dictionary, rpc: Variant, chain: Dictionary, mint: Variant, wallet: Variant, items: Dictionary,
		receipts: Dictionary, now: Variant, out: Dictionary) -> Variant:
	var at: Variant = ScfgSessions.number_field(rec, "hint_at")
	if at is ScfgErr:
		return at
	var expired: bool = now - at >= SOLANA_HINT_TIMEOUT_S
	var tx: Variant = await rpc.get_transaction(rec["hint_tx"])
	if tx is ScfgErr:
		if not (tx as ScfgErr).is_a("RpcError") or not expired:
			return tx  # an endpoint that keeps refusing must not keep a hint forever
		tx = null
	if _is_nil(tx):  # not visible yet, or dropped: the player may pay again after a while
		if expired:
			_drop_hint(rec, out, "tx_not_landed")
		return null
	var meta: Variant = _dget(tx, "meta")
	if meta is ScfgErr:
		return meta
	var failed: Variant = _dget(_or(meta, {}), "err")
	if failed is ScfgErr:
		return failed
	if not _is_nil(failed):
		_drop_hint(rec, out, "tx_reverted")
		return null
	var t: Variant = transfer_to(tx, {"sig": rec["hint_tx"], "mint": mint, "owner": wallet})
	if t is ScfgErr:
		return t
	var q: Variant = _qualifies(t, rec, chain, receipts)
	if q is ScfgErr:
		return q
	if not q:
		_drop_hint(rec, out, "tx_not_a_payment")
		return null
	return _verdict(t, rec, items)


static func _drop_hint(rec: Dictionary, out: Dictionary, error: String) -> void:
	out["error"] = error
	out["hint_tx"] = rec["hint_tx"]
	ScfgSessions.clear_hint(rec)


## The reference ties a transaction to this session, so any amount matches: "matched",
## "underpaid" or null.
## @fallible
func _by_reference(rec: Dictionary, rpc: Variant, chain: Dictionary, mint: Variant, wallet: Variant, items: Dictionary,
		receipts: Dictionary) -> Variant:
	var reference: Variant = ScfgPy.py_get(rec, "reference")
	if reference is ScfgErr:
		return reference
	var infos: Variant = await rpc.get_signatures_for_address(reference, {"limit": sig_page})
	if infos is ScfgErr:
		return infos
	var newest_last: Variant = _reversed(infos)
	if newest_last is ScfgErr:
		return newest_last
	for info: Variant in newest_last:
		var failed: Variant = _dget(info, "err")
		if failed is ScfgErr:
			return failed
		if not _is_nil(failed):
			continue
		var s: Variant = ScfgPy.py_get(info, "signature")
		if s is ScfgErr:
			return s
		var tx: Variant = await rpc.get_transaction(s)
		if tx is ScfgErr:
			return tx
		var t: Variant = null
		if ScfgPy.py_truthy(tx):
			t = transfer_to(tx, {"sig": s, "mint": mint, "owner": wallet})
			if t is ScfgErr:
				return t
		var q: Variant = _qualifies(t, rec, chain, receipts)
		if q is ScfgErr:
			return q
		if q:
			return _verdict(t, rec, items)
	return null


## Pages of signatures on `address` between `until` (exclusive, older bound) and `before`
## (exclusive, newer bound), newest first: [found, reached], where `reached` says whether the walk
## reached `until`; if not, the pages cover only the newest max_sig_pages * sig_page of them.
##
## A signature from before `min_slot` also ends the walk (and is dropped): with no cursor, or one
## this endpoint doesn't know, the walk would otherwise go through the account's whole history.
## @fallible
func _walk(rpc: Variant, address: Variant, until: Variant, before: Variant, min_slot: Variant) -> Variant:
	var found := []
	for _p in max_sig_pages:
		var listed: Variant = await rpc.get_signatures_for_address(address, {"until": until, "before": before,
			"limit": sig_page})
		if listed is ScfgErr:
			return listed
		var page: Variant = ScfgPy.py_iter(listed)
		if page is ScfgErr:
			return page
		for n in (page as Array).size():
			var info: Variant = page[n]
			var at: Variant = _dget(info, "slot")
			if at is ScfgErr:
				return at
			if _is_nil(at):
				continue
			var slot: Variant = ScfgPy.py_int(at)
			if slot is ScfgErr:
				return slot
			var older: Variant = _lt(slot, min_slot)
			if older is ScfgErr:
				return older
			if older:
				found.append_array((page as Array).slice(0, n))
				return [found, true]
		found.append_array(page)
		if (page as Array).size() < sig_page:
			return [found, true]
		var last: Variant = ScfgPy.py_get(page, -1)
		if last is ScfgErr:
			return last
		before = ScfgPy.py_get(last, "signature")
		if before is ScfgErr:
			return before
	# The last page was full too: reached `until` only if nothing older is left.
	var more: Variant = await rpc.get_signatures_for_address(address, {"until": until, "before": before, "limit": 1})
	if more is ScfgErr:
		return more
	return [found, not ScfgPy.py_truthy(more)]


## Exchange and manual transfers carry no reference: match the exact amount. Returns whether one
## was found (rec["match"] is then set).
##
## Signatures are only listed newest first, so a backlog larger than one walk is crossed in steps:
## each walk that cannot reach `scan_cursor` moves `scan_before` back and examines nothing; once a
## walk reaches the cursor its window is processed oldest first, then `scan_before` is cleared to
## look at the newer signatures again. A busy token account only delays detection, it never hides
## a payment. Signatures from before the session's start slot are never examined.
## @fallible
func _scan(rec: Dictionary, rpc: Variant, chain: Dictionary, mint: Variant, wallet: Variant, receipts: Dictionary) -> Variant:
	var args := []
	for f: String in ["dest", "scan_cursor"]:
		var v: Variant = ScfgPy.py_get(rec, f)
		if v is ScfgErr:
			return v
		args.append(v)
	var start_slot: Variant = ScfgPy.py_get(rec, "start_block")
	if start_slot is ScfgErr:
		return start_slot
	var walked: Variant = await _walk(rpc, args[0], args[1], rec.get("scan_before"), start_slot)
	if walked is ScfgErr:
		return walked
	var found: Array = walked[0]
	if not walked[1]:
		var last: Variant = ScfgPy.py_get(found, -1)
		if last is ScfgErr:
			return last
		var oldest: Variant = ScfgPy.py_get(last, "signature")
		if oldest is ScfgErr:
			return oldest
		rec["scan_before"] = oldest
		return false
	var budget := SCAN_TX_BUDGET
	for i in range(found.size() - 1, -1, -1):
		var info: Variant = found[i]
		var failed: Variant = _dget(info, "err")
		if failed is ScfgErr:
			return failed
		if not _is_nil(failed):
			var passed: Variant = ScfgPy.py_get(info, "signature")
			if passed is ScfgErr:
				return passed
			rec["scan_cursor"] = passed
			continue
		if budget == 0:
			return false
		budget -= 1
		var s: Variant = ScfgPy.py_get(info, "signature")
		if s is ScfgErr:
			return s
		var tx: Variant = await rpc.get_transaction(s)
		if tx is ScfgErr:
			return tx
		if _is_nil(tx):  # listed but not served yet: retry from here next step
			return false
		rec["scan_cursor"] = s
		var t: Variant = transfer_to(tx, {"sig": s, "mint": mint, "owner": wallet})
		if t is ScfgErr:
			return t
		var q: Variant = _qualifies(t, rec, chain, receipts)
		if q is ScfgErr:
			return q
		if not q:
			continue
		var units: Variant = ScfgPy.py_get(rec, "units")  # read only for a transfer that qualifies
		if units is ScfgErr:
			return units
		if ScfgPy.py_eq(t["value"], units):
			var m: Variant = _match_dict(t)
			if m is ScfgErr:
				return m
			rec["match"] = m
			return true
	rec["scan_before"] = null  # window done: look at the newer signatures again
	return false


# --- launch re-check, restore, readiness ---------------------------------------------------------


## Judge one stored Solana receipt: "drop" if the chain contradicts it, else "keep". `opts`: rpc,
## chain, wallets. Network trouble keeps; so does a transaction this build cannot read
## (Python's `except (KeyError, TypeError, ValueError, AttributeError)`).
## @fallible
func reverify(receipt: Variant, opts: Dictionary) -> Variant:
	var rpc: Variant = opts["rpc"]
	var chain: Dictionary = opts["chain"]
	var raw_block: Variant = ScfgPy.py_get(receipt, "block")
	if raw_block is ScfgErr:
		return raw_block
	var slot: Variant = ScfgPy.py_int(raw_block)
	if slot is ScfgErr:
		return slot
	var owner: Variant = _dget(receipt, "recipient")
	if owner is ScfgErr:
		return owner
	var wallets: Variant = ScfgPy.py_iter(opts["wallets"])
	if wallets is ScfgErr:
		return wallets
	if not (wallets as Array).any(func(w: Variant) -> bool: return ScfgPy.py_eq(w, owner)):
		return "drop"
	var current: Variant = await rpc.get_slot("finalized")
	if current is ScfgErr:
		return ScfgReverify._keep_on_trouble(current)
	var ahead: Variant = ScfgReverify._ahead(slot, current)
	if ahead is ScfgErr:
		return ahead
	if ahead != 0:
		return "drop" if ahead > ScfgReverify.FUTURE_TOLERANCE_BLOCKS else "keep"
	var tx_hash: Variant = ScfgPy.py_get(receipt, "tx_hash")
	if tx_hash is ScfgErr:
		return tx_hash
	var got: Variant = await rpc.get_transaction_any(tx_hash, {"min_slot": slot})
	if got is ScfgErr:
		return ScfgReverify._keep_on_trouble(got)
	var pair: Variant = _pair(got)
	if pair is ScfgErr:
		return pair
	var tx: Variant = pair[0]
	if _is_nil(tx):
		return "drop" if ScfgPy.py_truthy(pair[1]) else "keep"
	var t: Variant = _read_receipt_transfer(tx, receipt, chain, owner)
	if t is ScfgErr:
		var e: ScfgErr = t
		if e.is_a("KeyError") or e.is_a("TypeError") or e.is_a("ValueError") or e.is_a("AttributeError"):
			return "keep"  # a payload this build can't read: can't judge the receipt
		return e
	if t == null or not ScfgPy.py_eq(t["block_number"], slot):
		return "drop"
	var raw_index: Variant = ScfgPy.py_get(receipt, "log_index")
	if raw_index is ScfgErr:
		return raw_index
	var log_index: Variant = ScfgPy.py_int(raw_index)
	if log_index is ScfgErr:
		return log_index
	if not ScfgPy.py_eq(t["log_index"], log_index):
		return "drop"
	var amount: Variant = ScfgPy.py_get(receipt, "amount")
	if amount is ScfgErr:
		return amount
	var units: Variant = ScfgAmounts.decimal_to_units(amount)
	if units is ScfgErr:
		return units
	return "keep" if ScfgPy.py_eq(t["value"], units) else "drop"


## The body of reverify's inner try: `transfer_to(tx, sig=receipt["tx_hash"],
## mint=chain.tokens[receipt["token"]].address, owner=owner)`.
## @fallible
static func _read_receipt_transfer(tx: Variant, receipt: Variant, chain: Dictionary, owner: Variant) -> Variant:
	var sig: Variant = ScfgPy.py_get(receipt, "tx_hash")
	if sig is ScfgErr:
		return sig
	var token: Variant = ScfgWatcher._token(receipt, chain)
	if token is ScfgErr:
		return token
	return transfer_to(tx, {"sig": sig, "mint": token["address"], "owner": owner})


## Restore candidates for a signature on this chain (see ScfgRestore): one per token and accepted
## Solana wallet that received something, or [] when this chain does not know the transaction
## (Python's None). A RestoreError tx_reverted or tx_not_a_payment. `opts`: config, chain, rpc,
## receipts.
## @fallible
func lookup(tx: Variant, opts: Dictionary) -> Variant:
	var config: Dictionary = opts["config"]
	var chain: Dictionary = opts["chain"]
	var receipts: Dictionary = opts["receipts"]
	var rpc: Variant = opts["rpc"]
	var got: Variant = await rpc.get_transaction_any(tx)
	if got is ScfgErr:
		return got
	var pair: Variant = _pair(got)
	if pair is ScfgErr:
		return pair
	var found: Variant = pair[0]
	if _is_nil(found):
		return []
	var meta: Variant = _dget(found, "meta")
	if meta is ScfgErr:
		return meta
	var failed: Variant = _dget(_or(meta, {}), "err")
	if failed is ScfgErr:
		return failed
	if not _is_nil(failed):
		return ScfgRestore.restore_error("tx_reverted")
	var out := []
	for name: Variant in chain["tokens"]:
		var token: Dictionary = chain["tokens"][name]
		for owner: Variant in ScfgConfig.accepted_wallets_for(config, "solana"):
			var t: Variant = transfer_to(found, {"sig": tx, "mint": token["address"], "owner": owner})
			if t is ScfgErr:
				return t
			if t == null:
				continue
			var verdict: Variant = ScfgRestore.classify_or_skip(config, t["value"], opts.get("on_error"))
			if verdict is ScfgErr:
				return verdict
			if verdict == null:
				continue
			var key: Variant = ScfgReceipts.receipt_key(chain["key"], t["tx_hash"], t["log_index"])
			if key is ScfgErr:
				return key
			out.append({"chain": chain["key"], "token": name, "transfer": t, "kind": verdict[0], "item": verdict[1],
				"choices": verdict[2], "already_restored": receipts.has(key)})
	if out.is_empty():
		return ScfgRestore.restore_error("tx_not_a_payment")
	return out


## Wallets refuse to pay a wallet with no token account for the mint: {token name: whether the
## developer's token account exists}. `opts`: rpc, chain, wallet.
## @fallible
func check_ready(opts: Dictionary) -> Variant:
	var rpc: Variant = opts["rpc"]
	var chain: Dictionary = opts["chain"]
	var ready := {}
	for name: Variant in chain["tokens"]:
		var address: Variant = ScfgSolanaPda.associated_token_address(opts["wallet"], chain["tokens"][name]["address"])
		if address is ScfgErr:
			return address
		var exists: Variant = await rpc.account_exists(address)
		if exists is ScfgErr:
			return exists
		ready[name] = exists
	return ready
