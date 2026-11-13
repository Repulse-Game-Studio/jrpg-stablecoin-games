## ScfgEvmBackend and the backend registry (backends/evm.gd, backends/backends.gd): the twin of
## rpgmaker/src/core/backends/evm.test.ts (the Solana backend: test_solana_*.gd).
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "0x52908400098527886E0F7030069857D2E4169EE7"
const OLD := "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
const TX := "0xABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB"


func test_start_block_margin() -> bool:
	T.eq(ScfgEvmBackend.START_BLOCK_MARGIN, 5, "margin")
	return T.done()


func test_backend_for_family() -> bool:
	var evm: Variant = ScfgBackends.backend_for_family("evm")
	T.ok(evm is ScfgEvmBackend, "an ScfgEvmBackend")
	T.ok(evm is ScfgBaseBackend, "a backend")
	T.eq(evm.family, "evm", "family")
	T.ok(is_same(ScfgBackends.backend_for_family("evm"), evm), "one instance per family")
	T.ok(is_same(ScfgBackends.backend_for(ScfgChains.MAINNET["base"]), evm), "backend_for(chain)")
	T.is_err(ScfgBackends.backend_for_family("cosmos"), "ValueError", "an unknown family")
	T.is_err(ScfgBackends.backend_for_family(5), "ValueError", "a non-str family")
	T.is_err(ScfgBackends.backend_for({"key": "x"}), "KeyError", "a chain without a family")
	return T.done()


func test_evm_start_backs_off_the_head_and_floors_at_zero() -> bool:
	var b := ScfgEvmBackend.new()
	var opts := {"chain": ScfgChains.MAINNET["arbitrum"], "wallet": DEV, "token": "USDC",
		"entropy": ScfgSessions.OsEntropy.new()}
	opts["rpc"] = F.FakeChain.new(1000)
	T.eq(await b.start(opts), {"start_block": 995}, "head - 5")
	opts["rpc"] = F.FakeChain.new(3)
	T.eq(await b.start(opts), {"start_block": 0}, "floored at 0")
	var down := F.FakeChain.new(1000)
	down.down = true
	opts["rpc"] = down
	T.is_err(await b.start(opts), "RpcUnavailable", "an outage propagates")
	return T.done()


func test_evm_normalize_tx() -> bool:
	var b := ScfgEvmBackend.new()
	T.eq(b.normalize_tx(" " + TX), TX.to_lower(), "normalized")
	T.is_err(b.normalize_tx("not-a-hash"), "ValueError", "rejected")
	T.eq(ScfgBackends.normalize_any_tx(TX), TX.to_lower(), "normalize_any_tx")
	return T.done()


func test_evm_make_rpc_uses_the_chain_urls_log_range_transport_and_clock() -> bool:
	var transport := F.FakeTransport.new({})
	var clock := func() -> float: return 7.0
	var chain: Dictionary = ScfgChains.MAINNET["polygon"]
	var rpc: Variant = ScfgEvmBackend.new().make_rpc(chain, transport, clock)
	T.ok(rpc is ScfgRpcClient, "an ScfgRpcClient")
	if rpc is ScfgRpcClient:
		var c: ScfgRpcClient = rpc
		T.eq(Array(c.urls), chain["rpc"], "urls")
		T.eq(c.log_range, chain["max_log_range"], "log range")
		T.ok(c.clock == clock, "clock")
		T.ok(is_same(c.transport, transport), "transport")
	var no_clock: Variant = ScfgEvmBackend.new().make_rpc(chain, transport, Callable())
	T.ok(no_clock is ScfgRpcClient, "an empty Callable keeps the default clock")
	var bad := chain.duplicate()
	bad["rpc"] = []
	T.is_err(ScfgEvmBackend.new().make_rpc(bad, transport, clock), "ValueError", "no urls")
	return T.done()


func test_evm_check_ready_says_every_token_is_ready_without_the_network() -> bool:
	var b := ScfgEvmBackend.new()
	for chain: Dictionary in [ScfgChains.MAINNET["base"], ScfgChains.MAINNET["arc"]]:
		var rpc := F.FakeChain.new()
		var ready: Variant = await b.check_ready({"rpc": rpc, "chain": chain, "wallet": DEV})
		var want := {}
		for n: String in chain["tokens"]:
			want[n] = true
		T.eq(ready, want, "%s: every token" % chain["key"])
		T.eq(rpc.calls, [], "no request")
	return T.done()


func test_evm_advance_delegates_to_the_watcher() -> bool:
	var pending := {}
	var rec: Variant = ScfgSessions.create_session(pending, {"kind": "tip", "item": null, "title": "Tip", "token": "USDC",
		"chain": "base", "price": "3", "code": 0, "nonce": 7, "start_block": 995, "now": 0.0})
	var out: Variant = await ScfgEvmBackend.new().advance(pending, {}, rec["id"], {"rpc": F.FakeChain.new(1000),
		"chain": ScfgChains.MAINNET["base"], "wallet": DEV, "items": {}, "now": 1.0})
	T.eq([out["status"], rec["cursor"]], ["waiting", 1001], "a watcher step")
	return T.done()


func test_base_backend_methods_are_not_implemented() -> bool:
	var b := ScfgBaseBackend.new()
	T.is_err(b.make_rpc({}, null, Callable()), "RuntimeError", "make_rpc")
	T.is_err(b.normalize_tx(TX), "RuntimeError", "normalize_tx")
	T.is_err(await b.start({}), "RuntimeError", "start")
	T.is_err(await b.advance({}, {}, "ps_x", {}), "RuntimeError", "advance")
	T.is_err(await b.reverify({}, {}), "RuntimeError", "reverify")
	T.is_err(await b.lookup(TX, {}), "RuntimeError", "lookup")
	T.is_err(await b.check_ready({}), "RuntimeError", "check_ready")
	return T.done()


func test_evm_reverify_and_lookup_delegate_to_the_modules() -> bool:
	var b := ScfgEvmBackend.new()
	var chain: Dictionary = ScfgChains.MAINNET["base"]
	var usdc: String = chain["tokens"]["USDC"]["address"]
	var rpc := F.FakeChain.new(1000)
	var receipt := {"chain": "base", "tx_hash": TX.to_lower(), "log_index": 0, "block": 5, "amount": "1.000000", "token": "USDC"}
	T.eq(await b.reverify(receipt, {"rpc": rpc, "chain": chain, "wallets": [DEV]}), "drop", "no log: dropped")
	rpc.logs.append(F.transfer_log(usdc, DEV, DEV, 1_000_000, {"block": 5, "tx_hash": TX.to_lower()}))
	T.eq(await b.reverify(receipt, {"rpc": rpc, "chain": chain, "wallets": [DEV]}), "keep", "the log is there: kept")
	var config: Variant = ScfgConfig.build_config({"wallet": DEV})
	T.eq(await b.lookup(TX.to_lower(), {"config": config, "chain": chain, "rpc": rpc, "receipts": {}}), [],
		"the chain does not know the tx: [] (Python's None; null is an aborted coroutine)")
	rpc.receipts[TX.to_lower()] = {"status": "0x1", "logs": [F.transfer_log(usdc, OLD, DEV, 3_000_017, {"block": 5, "tx_hash": TX.to_lower()})]}
	var found: Variant = await b.lookup(TX.to_lower(), {"config": config, "chain": chain, "rpc": rpc, "receipts": {}})
	T.eq(found[0]["kind"] if found is Array else found, "tip", "a candidate")
	return T.done()
