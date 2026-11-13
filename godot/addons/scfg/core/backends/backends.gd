## Chain-family backends: everything that differs between EVM chains and Solana. Mirrors
## scfg_core/backends/__init__.py.
##
## The engine, sessions, receipts and screens are family-neutral. Anything that touches a chain's
## RPC, transaction-id format or payment matching goes through backend_for(chain). One backend
## instance per family, created on first use, in Python's registration order (EVM, then Solana).
class_name ScfgBackends
extends RefCounted

static var _BACKENDS: Dictionary = {}


static func _load() -> Dictionary:
	if _BACKENDS.is_empty():
		_BACKENDS["evm"] = ScfgEvmBackend.new()
		_BACKENDS["solana"] = ScfgSolanaBackend.new()
	return _BACKENDS


## The backend of a chain family. An unknown family is a ValueError.
## @fallible
static func backend_for_family(family: Variant) -> Variant:
	var loaded := _load()
	if (family is String or family is StringName) and loaded.has(str(family)):
		return loaded[str(family)]
	return ScfgErr.make("ValueError", "unknown chain family %s" % ScfgPy.py_repr(family))


## `backend_for_family(chain.family)`. A chain without a family is a KeyError.
## @fallible
static func backend_for(chain: Variant) -> Variant:
	var family: Variant = ScfgPy.py_get(chain, "family")
	if family is ScfgErr:
		return family
	return backend_for_family(family)


## A transaction id in any supported family's format, normalized: each family's normalize_tx in
## registration order (EVM's normalize_tx_hash, then Solana's normalize_signature); a ValueError
## from one moves on to the next, and when none accepts the text it is a ValueError.
## @fallible
static func normalize_any_tx(text: Variant) -> Variant:
	var loaded := _load()
	for family: String in loaded:
		var backend: ScfgBaseBackend = loaded[family]
		var out: Variant = backend.normalize_tx(text)
		if out is ScfgErr and (out as ScfgErr).is_a("ValueError"):
			continue
		return out
	return ScfgErr.make("ValueError", "not a transaction hash or signature: %s" % ScfgPy.py_repr(text))
