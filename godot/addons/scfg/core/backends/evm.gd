## EVM chains: the watcher, re-verification and restore behind the backend interface. Mirrors scfg_core/backends/evm.py.
class_name ScfgEvmBackend
extends ScfgBaseBackend

const START_BLOCK_MARGIN := 5


func _init() -> void:
	family = "evm"


## Python's `RpcClient(chain.rpc, log_range=chain.max_log_range)`, on `transport` and `clock` (an
## empty Callable keeps the client's default clock). A chain without urls is a ValueError.
## @fallible
func make_rpc(chain: Dictionary, transport: Variant, clock: Callable) -> Variant:
	var opts := {"transport": transport, "log_range": chain["max_log_range"]}
	if clock.is_valid():
		opts["clock"] = clock
	return ScfgRpcClient.create(chain["rpc"], opts)


## @fallible
func normalize_tx(text: Variant) -> Variant:
	return ScfgSessions.normalize_tx_hash(text)


## {"start_block": max(0, head - START_BLOCK_MARGIN)}.
## @fallible
func start(opts: Dictionary) -> Variant:
	var rpc: Variant = opts["rpc"]
	var reported: Variant = await rpc.block_number()
	if reported is ScfgErr:
		return reported
	var head: Variant = ScfgWatcher._head(reported)
	if head is ScfgErr:
		return head
	return {"start_block": maxi(0, head - START_BLOCK_MARGIN)}


## @fallible
func advance(pending: Dictionary, receipts: Dictionary, sid: Variant, opts: Dictionary) -> Variant:
	return await ScfgWatcher.advance(pending, receipts, sid, opts)


## "keep" or "drop" for a stored receipt (see ScfgReverify.check_evm_receipt). `opts`: rpc, chain,
## wallets.
## @fallible
## @coroutine
func reverify(receipt: Variant, opts: Dictionary) -> Variant:
	return await ScfgReverify.check_evm_receipt(receipt, opts)


## Restore candidates for tx on this chain (see ScfgRestore.lookup_evm). `opts`: config, chain,
## rpc, receipts.
## @fallible
## @coroutine
func lookup(tx: Variant, opts: Dictionary) -> Variant:
	return await ScfgRestore.lookup_evm(tx, opts)


## Every token can be received on an EVM chain: no network needed.
## @fallible
func check_ready(opts: Dictionary) -> Variant:
	var out := {}
	for name: Variant in opts["chain"]["tokens"]:
		out[name] = true
	return out
