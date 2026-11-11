## The interface every chain family implements. Mirrors scfg_core/backends/base.py (the Backend
## protocol); see backends/evm.gd (ScfgEvmBackend) and backends/solana.gd (ScfgSolanaBackend).
##
## Python's keyword arguments travel in one trailing `opts` Dictionary. start, advance,
## reverify, lookup, check_ready (and Solana's build_payment) are coroutines (`await` them: they
## are tagged `## @coroutine` for test_lints.gd even where this base does not suspend);
## normalize_tx and make_rpc are plain. Every method returns its value or an ScfgErr. The
## methods here are the protocol's documentation: each returns a RuntimeError ("not implemented")
## until a family overrides it.
class_name ScfgBaseBackend
extends RefCounted

## "evm" or "solana".
var family: String = ""


## @fallible
static func _missing(name: String) -> Variant:
	return ScfgErr.make("RuntimeError", "%s is not implemented by this backend" % name)


## The RPC object handed back to every other method for this chain: built on `transport`, with
## `clock` (a Callable returning seconds; an empty Callable keeps the client's monotonic default).
## @fallible
func make_rpc(_chain: Dictionary, _transport: Variant, _clock: Callable) -> Variant:
	return _missing("make_rpc")


## Validate a transaction id in this family's format. A ValueError when it is not one.
## @fallible
func normalize_tx(_text: Variant) -> Variant:
	return _missing("normalize_tx")


## Fields for a new session: must contain "start_block"; the rest is merged in. `opts`: rpc,
## chain, wallet, token, entropy (Python's `secrets`; Solana draws its reference from it).
## @fallible
## @coroutine
func start(_opts: Dictionary) -> Variant:
	return _missing("start")


## Move one pending session forward; returns a StepOutcome (see ScfgWatcher). `opts`: rpc,
## chain, wallet, items, now.
## @fallible
## @coroutine
func advance(_pending: Dictionary, _receipts: Dictionary, _sid: Variant, _opts: Dictionary) -> Variant:
	return _missing("advance")


## "keep" or "drop". Network trouble must answer "keep". `opts`: rpc, chain, wallets.
## @fallible
## @coroutine
func reverify(_receipt: Variant, _opts: Dictionary) -> Variant:
	return _missing("reverify")


## Restore candidates for tx on this chain, or [] if the chain doesn't know it (Python's None: a
## null is what a coroutine aborted by a script error returns, and ScfgRestore.lookup fails on it).
## A RestoreError ("tx_reverted" | "tx_not_a_payment"). `opts`: config, chain, rpc, receipts,
## on_error (a transfer that cannot be classified is skipped and reported: ScfgRestore.classify_or_skip).
## @fallible
## @coroutine
func lookup(_tx: Variant, _opts: Dictionary) -> Variant:
	return _missing("lookup")


## {token name: can the developer receive it on this chain}. `opts`: rpc, chain, wallet.
## @fallible
## @coroutine
func check_ready(_opts: Dictionary) -> Variant:
	return _missing("check_ready")
