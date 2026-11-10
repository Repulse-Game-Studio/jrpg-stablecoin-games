## ScfgSolanaBackend (backends/solana.gd): session start, the launch re-check, restore and
## readiness. tests/unit/test_solana_backend.py, plus M1's edges (rpgmaker/src/core/backends/
## solana.test.ts): the constants, the reference drawn after the network calls, the log index
## and accepted-wallet checks of reverify, previous-wallet restores.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

## On-curve wallet addresses (config validation needs them; Python draws random Keypairs).
const DEV := "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
const OLD := "4SELZ24sTinbBYo27VvsUAHepTRXGVBghc8WTUpvx6Pn"
const PAYER := "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe"

static var CHAIN: Dictionary = ScfgChains.MAINNET["solana"]
static var MINT: String = ScfgChains.MAINNET["solana"]["tokens"]["USDC"]["address"]
static var B := ScfgSolanaBackend.new()


func _cfg() -> Dictionary:
	var c: Variant = ScfgConfig.build_config({"solana_wallet": DEV, "previous_wallets": [OLD]})
	ScfgConfig.add_item(c, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	return c


## Entropy whose token_bytes(n) are n bytes of the draw's count (1, 2, ...), recording each draw.
class CountingEntropy:
	extends RefCounted
	var draws := 0

	func token_bytes(n: int) -> Variant:
		draws += 1
		var b := PackedByteArray()
		b.resize(n)
		b.fill(draws)
		return b

	func token_hex(_n: int) -> Variant:
		return ScfgErr.make("AssertionError", "unexpected token_hex")

	func randbelow(_n: int) -> Variant:
		return ScfgErr.make("AssertionError", "unexpected randbelow")


func _fill(n: int, byte: int) -> PackedByteArray:
	var b := PackedByteArray()
	b.resize(n)
	b.fill(byte)
	return b


func test_constants() -> bool:
	T.eq([ScfgSolanaBackend.FINAL_CONFIRMATIONS, ScfgSolanaBackend.SCAN_TX_BUDGET, ScfgSolanaBackend.SIG_PAGE,
		ScfgSolanaBackend.MAX_SIG_PAGES, ScfgSolanaBackend.START_SLOT_MARGIN, ScfgSolanaBackend.COMPUTE_UNITS,
		ScfgSolanaBackend.MIN_PRIORITY_FEE, ScfgSolanaBackend.MAX_PRIORITY_FEE, ScfgSolanaBackend.SOLANA_HINT_TIMEOUT_S],
		[32, 5, 100, 10, 150, 30_000, 1_000, 1_000_000, 180], "Python's module constants")
	T.eq([B.family, B.sig_page, B.max_sig_pages], ["solana", 100, 10], "the instance's family and page constants")
	return T.done()


func test_make_rpc_wraps_the_failover_client() -> bool:
	var transport := F.FakeTransport.new({})
	var clock := func() -> float: return 3.0
	var rpc: Variant = B.make_rpc(CHAIN, transport, clock)
	T.ok(rpc is ScfgSolanaRpc, "a SolanaRpc")
	if not rpc is ScfgSolanaRpc:
		return T.done()
	var client: Variant = (rpc as ScfgSolanaRpc).client
	T.ok(client is ScfgRpcClient, "around an RpcClient")
	if client is ScfgRpcClient:
		T.eq(Array((client as ScfgRpcClient).urls), CHAIN["rpc"], "the chain's urls")
		T.ok((client as ScfgRpcClient).clock == clock, "on the given clock")
		T.ok((client as ScfgRpcClient).transport == transport, "and transport")
	var default_clock: Variant = B.make_rpc(CHAIN, transport, Callable())
	T.ok(default_clock is ScfgSolanaRpc and (default_clock.client.clock as Callable).is_valid(),
		"an empty Callable keeps the client's clock")
	var no_urls := CHAIN.duplicate()
	no_urls["rpc"] = []
	T.is_err(B.make_rpc(no_urls, transport, clock), "ValueError", "a chain without urls")
	return T.done()


func test_start_fields() -> bool:
	var chain := F.FakeSolanaChain.new(1000)
	var dest: Variant = ScfgSolanaPda.associated_token_address(DEV, MINT)
	chain.addresses[dest] = [{"signature": F.sig(9), "slot": 999, "err": null}]
	var entropy := CountingEntropy.new()
	var f: Variant = await B.start({"rpc": chain, "chain": CHAIN, "wallet": DEV, "token": "USDC", "entropy": entropy})
	T.ok(f is Dictionary, "fields")
	if not f is Dictionary:
		return T.done()
	T.eq(f["start_block"], 1000 - ScfgSolanaBackend.START_SLOT_MARGIN, "start_block")
	T.eq(f["dest"], dest, "the developer's token account")
	T.eq(f["scan_cursor"], F.sig(9), "the newest signature is the scan cursor")
	T.eq((ScfgSolanaBase58.b58decode(f["reference"]) as PackedByteArray).size(), 32, "a 32-byte reference")
	T.eq(f["reference"], ScfgSolanaBase58.b58encode(_fill(32, 1)), "drawn from token_bytes(32)")
	T.eq(f["scan_before"], null, "scan_before")
	T.eq(f.keys(), ["start_block", "reference", "dest", "scan_cursor", "scan_before"], "Python's key order")
	T.eq(chain.calls, ["get_slot", "get_signatures_for_address"], "the calls")
	var again: Variant = await B.start({"rpc": chain, "chain": CHAIN, "wallet": DEV, "token": "USDC", "entropy": entropy})
	T.ok(again is Dictionary and again["reference"] != f["reference"], "a new reference per session")
	var fresh: Variant = await B.start({"rpc": F.FakeSolanaChain.new(10), "chain": CHAIN, "wallet": OLD, "token": "USDC",
		"entropy": entropy})
	T.eq([fresh["scan_cursor"], fresh["start_block"]] if fresh is Dictionary else fresh, [null, 0],
		"no signatures yet; the start slot is floored at 0")
	return T.done()


func test_start_draws_the_reference_after_the_network_calls() -> bool:
	var chain := F.FakeSolanaChain.new(1000)
	chain.down = true
	var entropy := CountingEntropy.new()
	T.is_err(await B.start({"rpc": chain, "chain": CHAIN, "wallet": DEV, "token": "USDC", "entropy": entropy}),
		"RpcUnavailable", "the outage")
	T.eq(entropy.draws, 0, "nothing drawn")
	T.is_err(await B.start({"rpc": chain, "chain": CHAIN, "wallet": DEV, "token": "NOPE", "entropy": entropy}),
		"KeyError", "an unknown token")
	T.is_err(await B.start({"rpc": chain, "chain": CHAIN, "wallet": null, "token": "USDC", "entropy": entropy}),
		"ValueError", "no wallet")
	return T.done()


func _receipt_for(tx_sig: String, kw: Dictionary = {}) -> Dictionary:
	var t: Variant = ScfgTransfers.make_transfer({"token": MINT, "sender": PAYER, "recipient": kw.get("recipient", DEV),
		"value": kw.get("value", 4_993_742), "tx_hash": tx_sig, "log_index": kw.get("index", 2),
		"block_number": kw.get("slot", 950)})
	return ScfgReceipts.make_receipt({"chain": "solana", "transfer": t, "kind": "unlock", "item": "chapter2",
		"token": "USDC", "source": "payment"})


func _verdict(chain: Variant, r: Variant, wallets: Array = [DEV, OLD]) -> Variant:
	return await B.reverify(r, {"rpc": chain, "chain": CHAIN, "wallets": wallets})


func _pay(chain: F.FakeSolanaChain, n: int, kw: Dictionary = {}) -> Dictionary:
	var opts := {"mint": MINT, "payer": PAYER, "owner": DEV, "amount": 4_993_742, "slot": 950}
	opts.merge(kw, true)
	return chain.pay(F.sig(n), opts)


func test_reverify_keeps_genuine_and_previous_wallet_receipts() -> bool:
	var chain := F.FakeSolanaChain.new()
	_pay(chain, 1)
	_pay(chain, 2, {"owner": OLD})
	T.eq(await _verdict(chain, _receipt_for(F.sig(1))), "keep", "genuine")
	T.eq(await _verdict(chain, _receipt_for(F.sig(2), {"recipient": OLD})), "keep", "paid to a previous wallet")
	T.eq(chain.calls, ["get_slot", "get_transaction_any", "get_slot", "get_transaction_any"], "the calls")
	return T.done()


func test_reverify_drops_contradicted_receipts() -> bool:
	var cases := [
		["amount differs", {"value": 9_000_000}, {}],
		["slot differs", {"slot": 951}, {}],
		["not paid to the developer", {"recipient": PAYER}, {}],
		["failed transaction", {}, {"err": {"InstructionError": [0, "Custom"]}}],
		["log index differs", {"index": 1}, {}],
	]
	for c: Array in cases:
		var chain := F.FakeSolanaChain.new()
		_pay(chain, 1, c[2])
		T.eq(await _verdict(chain, _receipt_for(F.sig(1), c[1])), "drop", c[0])
	var chain := F.FakeSolanaChain.new()
	_pay(chain, 1)
	T.eq(await _verdict(chain, _receipt_for(F.sig(1), {"recipient": PAYER}), [DEV, PAYER]), "drop",
		"the accepted recipient only lost tokens in that transaction")
	return T.done()


func test_invented_signature_is_dropped_when_history_covers_it() -> bool:
	var chain := F.FakeSolanaChain.new()
	chain.first_available = 0
	T.eq(await _verdict(chain, _receipt_for(F.sig(1))), "drop", "no such transaction")
	return T.done()


## Review focus 5: publicnode keeps ~2 days; a null from it proves nothing.
func test_old_receipt_on_a_pruned_endpoint_is_kept() -> bool:
	var chain := F.FakeSolanaChain.new()
	chain.first_available = 1_000_000_000
	T.eq(await _verdict(chain, _receipt_for(F.sig(1))), "keep", "pruned history")
	return T.done()


func test_outage_keeps_and_far_future_slot_drops() -> bool:
	var chain := F.FakeSolanaChain.new(1000)
	chain.down = true
	T.eq(await _verdict(chain, _receipt_for(F.sig(1))), "keep", "outage")
	chain.down = false
	T.eq(await _verdict(chain, _receipt_for(F.sig(1), {"slot": 1000 + 20_000})), "drop", "impossibly far ahead")
	_pay(chain, 3, {"slot": 1005})
	T.eq(await _verdict(chain, _receipt_for(F.sig(3), {"slot": 1005})), "keep", "lagging head")
	T.eq(await _verdict(chain, _receipt_for(F.sig(1), {"slot": 1000 + 10_000})), "keep", "the tolerance edge")
	return T.done()


func test_reverify_asks_the_finalized_slot_and_errors_keep() -> bool:
	var chain := F.FakeSolanaChain.new(1000)
	var seen := []
	chain.patched["get_slot"] = func(c: String) -> Variant:
		seen.append(c)
		return ScfgErr.make("RpcError", "node is behind", -32005)
	T.eq(await _verdict(chain, _receipt_for(F.sig(1))), "keep", "an RpcError keeps")
	T.eq(seen, ["finalized"], "the finalized slot")
	chain.patched["get_slot"] = func(_c: String) -> Variant: return ScfgErr.make("ValueError", "odd")
	T.is_err(await _verdict(chain, _receipt_for(F.sig(1))), "ValueError", "another error is not network trouble")
	return T.done()


func test_reverify_drops_a_receipt_for_a_wallet_no_longer_accepted() -> bool:
	var chain := F.FakeSolanaChain.new()
	T.eq(await _verdict(chain, _receipt_for(F.sig(1)), [OLD]), "drop", "DEV is not accepted")
	T.eq(chain.calls, [], "judged without the network")
	return T.done()


func test_lookup_classifies_by_amount_code() -> bool:
	var chain := F.FakeSolanaChain.new()
	_pay(chain, 1)
	var out: Variant = await B.lookup(F.sig(1), {"config": _cfg(), "chain": CHAIN, "rpc": chain, "receipts": {}})
	T.ok(out is Array and out.size() == 1, "one candidate")
	if not (out is Array and out.size() == 1):
		return T.done()
	var c: Dictionary = out[0]
	T.eq(c.keys(), ["chain", "token", "transfer", "kind", "item", "choices", "already_restored"], "a RestoreCandidate")
	T.eq([c["chain"], c["token"], c["kind"], c["item"], c["already_restored"]], ["solana", "USDC", "unlock", "chapter2", false],
		"classified")
	T.ok(ScfgPy.py_eq(c["transfer"]["value"], 4_993_742), "the value")
	T.eq(c["transfer"]["log_index"], 2, "the developer's token account index")
	T.eq((c["transfer"] as Dictionary).keys(), ScfgTransfers.TRANSFER_FIELDS, "a plain Transfer (no block_hash)")
	var receipts := {ScfgReceipts.receipt_key("solana", F.sig(1), 2): {}}
	var again: Variant = await B.lookup(F.sig(1), {"config": _cfg(), "chain": CHAIN, "rpc": chain, "receipts": receipts})
	T.ok(again is Array and again[0]["already_restored"], "already restored")
	return T.done()


func test_lookup_offers_previous_wallet_payments_too() -> bool:
	var chain := F.FakeSolanaChain.new()
	_pay(chain, 1, {"owner": OLD})
	var out: Variant = await B.lookup(F.sig(1), {"config": _cfg(), "chain": CHAIN, "rpc": chain, "receipts": {}})
	T.eq((out as Array).map(func(c: Dictionary) -> Array: return [c["token"], c["transfer"]["recipient"]])
		if out is Array else out, [["USDC", OLD]], "the previous wallet's payment")
	return T.done()


func test_lookup_outcomes() -> bool:
	var chain := F.FakeSolanaChain.new()
	T.eq(await B.lookup(F.sig(1), {"config": _cfg(), "chain": CHAIN, "rpc": chain, "receipts": {}}), [],
		"unknown to this chain: [] (Python's None; null is an aborted coroutine)")
	_pay(chain, 2, {"amount": 1, "err": {"x": 1}})
	var reverted: Variant = await B.lookup(F.sig(2), {"config": _cfg(), "chain": CHAIN, "rpc": chain, "receipts": {}})
	T.ok(reverted is ScfgErr and reverted.kind == "RestoreError" and reverted.code == "tx_reverted", "tx_reverted")
	_pay(chain, 3, {"payer": DEV, "owner": PAYER, "amount": 1_000_000})
	var other: Variant = await B.lookup(F.sig(3), {"config": _cfg(), "chain": CHAIN, "rpc": chain, "receipts": {}})
	T.ok(other is ScfgErr and other.kind == "RestoreError" and other.code == "tx_not_a_payment", "tx_not_a_payment")
	chain.down = true
	T.is_err(await B.lookup(F.sig(3), {"config": _cfg(), "chain": CHAIN, "rpc": chain, "receipts": {}}), "RpcUnavailable",
		"network trouble is left to restore.lookup")
	return T.done()


func test_restore_routes_signatures_to_solana_and_hashes_to_evm() -> bool:
	var sol := F.FakeSolanaChain.new()
	_pay(sol, 1)
	var evm := F.FakeChain.new()
	var chains := {"arbitrum": ScfgChains.MAINNET["arbitrum"], "solana": CHAIN}
	var rpcs := {"arbitrum": evm, "solana": sol}
	var rpc_for := func(k: Variant) -> Variant: return rpcs.get(k)
	var out: Variant = await ScfgRestore.lookup(F.sig(1), {"config": _cfg(), "chains": chains, "rpc_for": rpc_for,
		"receipts": {}})
	T.ok(out is Array and out[0]["chain"] == "solana", "found on Solana")
	T.eq(evm.calls, [], "the EVM chain was not asked")
	var hashed: Variant = await ScfgRestore.lookup("0x" + "ab".repeat(32), {"config": _cfg(), "chains": chains,
		"rpc_for": rpc_for, "receipts": {}})
	T.ok(hashed is ScfgErr and hashed.code == "tx_not_found", "an EVM hash: tx_not_found")
	T.ok(not "get_transaction_any" in sol.calls.slice(1), "Solana was not asked about the hash")
	return T.done()


func test_check_ready() -> bool:
	var chain := F.FakeSolanaChain.new()
	chain.accounts[ScfgSolanaPda.associated_token_address(DEV, MINT)] = true
	T.eq(await B.check_ready({"rpc": chain, "chain": CHAIN, "wallet": DEV}), {"USDC": true, "USDT": false},
		"a token account per token")
	chain.down = true
	T.is_err(await B.check_ready({"rpc": chain, "chain": CHAIN, "wallet": DEV}), "RpcUnavailable", "an outage propagates")
	return T.done()


func test_reverify_keeps_a_receipt_whose_transaction_it_cannot_read() -> bool:
	var payloads := {
		"no owner or amount": {"meta": {"err": null, "postTokenBalances": [{"mint": MINT, "accountIndex": 2}]}},
		"amount not a number": {"meta": {"err": null, "postTokenBalances": [
			{"mint": MINT, "accountIndex": 2, "owner": DEV, "uiTokenAmount": {"amount": "x"}}]}},
		"balances not a list": {"meta": {"err": null, "postTokenBalances": "garbage"}},
		"meta not a dict": {"meta": [1]},
		"unhashable account index": {"meta": {"err": null, "postTokenBalances": [
			{"mint": MINT, "accountIndex": [2], "owner": DEV, "uiTokenAmount": {"amount": "1"}}]}},
	}
	for why: String in payloads:
		var chain := F.FakeSolanaChain.new()
		var tx: Dictionary = (payloads[why] as Dictionary).duplicate(true)
		tx["slot"] = 950
		chain.txs[F.sig(1)] = tx
		T.eq(await _verdict(chain, _receipt_for(F.sig(1))), "keep", why)
	return T.done()
