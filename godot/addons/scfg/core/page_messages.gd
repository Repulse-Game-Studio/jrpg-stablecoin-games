## What the checkout page may tell the game, checked the same way wherever the page runs
## (local_server.py on desktop, overlay.py in the web build). A tx hash is only a hint: the
## watcher verifies it on-chain before anything is unlocked. Mirrors scfg_core/page_messages.py.
class_name ScfgPageMessages
extends RefCounted

const BUILD_TIMEOUT_S := 15  # how long a page waits for the game to build a Solana transaction


## {"tx_hash", "from"?} -> [normalized tx hash or signature, normalized sender or null].
## A ValueError for a missing or malformed hash; a malformed sender is dropped. As in Python, a
## truthy sender whose `str()` starts with "0x" is checksummed as an EVM address and anything
## else is read as a Solana account (a non-string fails that, and is dropped).
## @fallible
static func parse_tx_message(data: Variant) -> Variant:
	if not data is Dictionary:
		return ScfgErr.make("ValueError", "not an object")
	if not (data as Dictionary).has("tx_hash"):
		return ScfgErr.make("ValueError", "bad tx hash")  # Python: KeyError, re-raised
	var tx_hash: Variant = ScfgBackends.normalize_any_tx(data["tx_hash"])
	if tx_hash is ScfgErr:
		var e: ScfgErr = tx_hash
		if e.is_a("KeyError") or e.is_a("TypeError") or e.is_a("AttributeError"):
			return ScfgErr.make("ValueError", "bad tx hash")
		return e
	var sender: Variant = (data as Dictionary).get("from")
	if ScfgPy.py_truthy(sender):
		var is_text: bool = sender is String or sender is StringName
		var normalized: Variant = (ScfgEvm.to_checksum_address(sender) if is_text and str(sender).begins_with("0x")
			else ScfgSolanaBase58.normalize_pubkey(sender))  # err-ok: checked on the next line
		if normalized is ScfgErr:
			var e: ScfgErr = normalized
			if not (e.is_a("ValueError") or e.is_a("TypeError")):
				return e
			sender = null
		else:
			sender = normalized
	else:
		sender = null
	return [tx_hash, sender]


## {"account"} -> the player's Solana account, normalized. A ValueError for a missing or
## malformed account.
## @fallible
static func parse_build_message(data: Variant) -> Variant:
	if not data is Dictionary:
		return ScfgErr.make("ValueError", "not an object")
	if not (data as Dictionary).has("account"):
		return ScfgErr.make("ValueError", "bad account")
	var account: Variant = ScfgSolanaBase58.normalize_pubkey(data["account"])
	if account is ScfgErr:
		var e: ScfgErr = account
		if e.is_a("KeyError") or e.is_a("TypeError") or e.is_a("AttributeError"):
			return ScfgErr.make("ValueError", "bad account")
		return e
	return account
