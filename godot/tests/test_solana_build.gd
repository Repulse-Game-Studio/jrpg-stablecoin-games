## ScfgSolanaBackend.build_payment and ScfgEngine.build_solana_payment: the backend and Engine half
## of tests/unit/test_solana_build.py, plus the JS core's checks (rpgmaker/src/core/backends/
## solana.test.ts): the exact layout a wallet signs, padded base64, the account normalized and
## refused before any request. (The checkout controller test of that file is here too; the
## local-server tests are in test_local_server.gd.) Python signs the transaction with solders to prove
## a wallet can; here a small legacy-transaction parser checks every byte a wallet signs instead.
extends RefCounted

const T := preload("res://tests/t.gd")
const F := preload("res://tests/fakes.gd")

const DEV := "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
const DEV_EVM := "0x52908400098527886E0F7030069857D2E4169EE7"
const PLAYER := "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe"
const REFERENCE := "3RnSVDFx2gbsEz5MGXRb8GaEiug7tvzAQzMN18SHVJpw"

static var MAIN: Dictionary = ScfgChains.MAINNET["solana"]
static var MINT: String = ScfgChains.MAINNET["solana"]["tokens"]["USDC"]["address"]


func _rpc(fee: Variant = 5_000) -> F.BuildRpc:
	var rpc := F.BuildRpc.new()
	rpc.fee = fee
	rpc.token_account = ScfgSolanaPda.associated_token_address(PLAYER, MINT)
	return rpc


func _session_rec() -> Dictionary:
	return {"token": "USDC", "units": 4_993_742, "reference": REFERENCE,
		"dest": ScfgSolanaPda.associated_token_address(DEV, MINT)}


## Enough of a legacy transaction parser to check what a wallet will be asked to sign:
## {nsig, sigs, header, keys, blockhash, instructions: [{program, accounts, data}], rest}.
func _parse_tx(raw: PackedByteArray) -> Dictionary:
	var at := [0]
	var cu16 := func() -> int:
		var n := 0
		var shift := 0
		while true:
			var b: int = raw[at[0]]
			at[0] += 1
			n |= (b & 0x7f) << shift
			if b & 0x80 == 0:
				break
			shift += 7
		return n
	var take := func(n: int) -> PackedByteArray:
		var s := raw.slice(at[0], at[0] + n)
		at[0] += n
		return s
	var nsig: int = cu16.call()
	var sigs: PackedByteArray = take.call(64 * nsig)
	var header := Array(take.call(3) as PackedByteArray)
	var keys := []
	for i in cu16.call():
		keys.append(ScfgSolanaBase58.b58encode(take.call(32)))
	var blockhash := ScfgSolanaBase58.b58encode(take.call(32))
	var instructions := []
	for i in cu16.call():
		var program: String = keys[raw[at[0]]]
		at[0] += 1
		var accounts := []
		for k: int in (take.call(cu16.call()) as PackedByteArray):
			accounts.append(keys[k])
		instructions.append({"program": program, "accounts": accounts, "data": take.call(cu16.call())})
	return {"nsig": nsig, "sigs": sigs, "header": header, "keys": keys, "blockhash": blockhash,
		"instructions": instructions, "rest": raw.size() - at[0]}


func _le(v: int, size: int) -> PackedByteArray:
	var out := PackedByteArray()
	for i in size:
		out.append((v >> (8 * i)) & 0xff)
	return out


func test_build_payment_transaction() -> bool:
	for c: Array in [[5_000, 5_000], [0, ScfgSolanaBackend.MIN_PRIORITY_FEE], [1_000_000_000_000,
			ScfgSolanaBackend.MAX_PRIORITY_FEE], [ScfgBigInt.from_decimal("99999999999999999999"),
			ScfgSolanaBackend.MAX_PRIORITY_FEE], [1_000, 1_000], [1_000_000, 1_000_000]]:
		var fee: Variant = c[0]
		var expected: int = c[1]
		var label := "[fee %s]" % (fee.to_decimal() if fee is ScfgBigInt else str(fee))
		var rpc := _rpc(fee)
		var rec := _session_rec()
		var out: Variant = await ScfgSolanaBackend.new().build_payment(rec, {"rpc": rpc, "chain": MAIN, "account": PLAYER})
		T.ok(out is Dictionary, "%s a result" % label)
		if not out is Dictionary:
			continue
		T.eq(out.keys(), ["transaction", "token_balance", "sol_balance"], "%s Python's keys" % label)
		T.eq([out["token_balance"], out["sol_balance"]], ["7000000", "2000000"], "%s balances as str()" % label)
		T.eq(rpc.calls, ["get_token_balance", "get_balance", "get_latest_blockhash", "get_priority_fee"],
			"%s the calls, in order" % label)
		var raw := Marshalls.base64_to_raw(out["transaction"])
		T.eq(Marshalls.raw_to_base64(raw), out["transaction"], "%s padded base64" % label)
		T.eq(int(out["transaction"].length()) % 4, 0, "%s padded to a multiple of 4" % label)
		var tx := _parse_tx(raw)
		T.eq(tx["rest"], 0, "%s nothing after the message" % label)
		T.eq(tx["nsig"], 1, "%s one signature slot" % label)
		T.ok(Array(tx["sigs"]).all(func(b: int) -> bool: return b == 0), "%s unsigned: the wallet fills the slot" % label)
		T.eq(tx["header"], [1, 0, 4], "%s one signer; mint, reference and the 2 programs read-only" % label)
		T.eq(tx["keys"][0], PLAYER, "%s the player pays the fee" % label)
		T.ok(rec["reference"] in tx["keys"] and rec["dest"] in tx["keys"], "%s reference and destination" % label)
		T.eq(tx["blockhash"], F.BuildRpc.BLOCKHASH, "%s the recent blockhash" % label)
		var ixs: Array = tx["instructions"]
		T.eq(ixs.map(func(ix: Dictionary) -> String: return ix["program"]), [ScfgSolanaTx.COMPUTE_BUDGET_PROGRAM_ID,
			ScfgSolanaTx.COMPUTE_BUDGET_PROGRAM_ID, ScfgSolanaPda.TOKEN_PROGRAM_ID], "%s the programs" % label)
		T.eq(ixs[0]["data"], PackedByteArray([2]) + _le(ScfgSolanaBackend.COMPUTE_UNITS, 4), "%s the unit limit" % label)
		T.eq(ixs[1]["data"], PackedByteArray([3]) + _le(expected, 8), "%s the unit price" % label)
		T.eq(ixs[2]["data"], PackedByteArray([12]) + _le(4_993_742, 8) + PackedByteArray([6]), "%s TransferChecked" % label)
		T.eq(ixs[2]["accounts"], [ScfgSolanaPda.associated_token_address(PLAYER, MINT), MINT, rec["dest"], PLAYER,
			rec["reference"]], "%s source, mint, destination, owner, reference" % label)
	return T.done()


func test_build_payment_rejects_a_bad_account() -> bool:
	var rpc := _rpc()
	T.is_err(await ScfgSolanaBackend.new().build_payment(_session_rec(), {"rpc": rpc, "chain": MAIN, "account": "0x1234"}),
		"ValueError", "not a Solana address")
	T.eq(rpc.calls, [], "refused before any request")
	return T.done()


func test_build_payment_normalizes_the_account() -> bool:
	var rpc := _rpc()
	var out: Variant = await ScfgSolanaBackend.new().build_payment(_session_rec(), {"rpc": rpc, "chain": MAIN,
		"account": " %s\n" % PLAYER})
	T.eq(out["token_balance"] if out is Dictionary else out, "7000000", "the stripped account's token account")
	return T.done()


func test_build_payment_reads_the_session_like_python() -> bool:
	var b := ScfgSolanaBackend.new()
	for key: String in ["dest", "units", "reference"]:
		var rec := _session_rec()
		rec.erase(key)
		T.is_err(await b.build_payment(rec, {"rpc": _rpc(), "chain": MAIN, "account": PLAYER}), "KeyError", "no %s" % key)
	var rec := _session_rec()
	rec["token"] = "DAI"
	var rpc := _rpc()
	T.is_err(await b.build_payment(rec, {"rpc": rpc, "chain": MAIN, "account": PLAYER}), "KeyError", "an unknown token")
	T.eq(rpc.calls, [], "before any request")
	rec = _session_rec()
	rec["units"] = -1
	T.is_err(await b.build_payment(rec, {"rpc": _rpc(), "chain": MAIN, "account": PLAYER}), "OverflowError",
		"a negative amount (Python's to_bytes)")
	var big := _rpc()
	big.token_account = ""
	var out: Variant = await b.build_payment(_session_rec(), {"rpc": big, "chain": MAIN, "account": PLAYER})
	T.eq(out["token_balance"] if out is Dictionary else out, "0", "no token account: 0")
	return T.done()


func test_build_payment_propagates_network_trouble() -> bool:
	var rpc := _rpc()
	rpc.fee = ScfgErr.make("RpcUnavailable", "down")
	T.is_err(await ScfgSolanaBackend.new().build_payment(_session_rec(), {"rpc": rpc, "chain": MAIN, "account": PLAYER}),
		"RpcUnavailable", "the error as it is")
	return T.done()


# --- through the Engine ------------------------------------------------------------------------


func _engine(sol: F.BuildRpc) -> ScfgEngine:
	var cfg: Variant = ScfgConfig.build_config({"wallet": DEV_EVM, "solana_wallet": DEV})
	ScfgConfig.add_item(cfg, "chapter2", {"code": 37, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
	return ScfgEngine.new(cfg, {"rpc_factory": func(c: Dictionary) -> Variant:
		return sol if c["family"] == "solana" else F.FakeChain.new(), "clock": func() -> float: return 0.0})


func test_engine_builds_only_for_solana_sessions() -> bool:
	var sol := _rpc()
	var e := _engine(sol)
	var sol_rec: Variant = await e.start_session("unlock", "solana", {"item": "chapter2"})
	T.ok(sol_rec is Dictionary, "a Solana session")
	if not sol_rec is Dictionary:
		return T.done()
	var built: Variant = await e.build_solana_payment(sol_rec["id"], PLAYER)
	T.ok(built is Dictionary and built.has("transaction"), "built")
	T.eq(built["token_balance"] if built is Dictionary else built, "7000000", "the player's balance")
	var tx := _parse_tx(Marshalls.base64_to_raw(built["transaction"]))
	T.ok(sol_rec["reference"] in tx["keys"] and sol_rec["dest"] in tx["keys"], "the session's reference and destination")
	T.eq(tx["instructions"][2]["data"], PackedByteArray([12]) + _le(sol_rec["units"], 8) + PackedByteArray([6]),
		"the session's units")
	var evm_rec: Variant = await e.start_session("unlock", "base", {"item": "chapter2"})
	T.is_err(await e.build_solana_payment(evm_rec["id"], PLAYER), "ValueError", "not a Solana session")
	T.is_err(await e.build_solana_payment("ps_missing", PLAYER), "KeyError", "an unknown session")
	T.is_err(await e.build_solana_payment(["x"], PLAYER), "TypeError", "an unhashable sid")
	T.is_err(await e.build_solana_payment(sol_rec["id"], "0x1234"), "ValueError", "a bad account")
	var bad := ScfgEngine.new(e.config, {"nope": 1})
	T.is_err(await bad.build_solana_payment(sol_rec["id"], PLAYER), "TypeError", "the init error")
	return T.done()


## test_controller_passes_family_info_and_a_build_callback: the controller hands a Solana page its
## family fields and a build_tx that returns the Worker's Job, and an EVM page neither.
func test_controller_passes_family_info_and_a_build_callback() -> bool:
	var sol := _rpc()
	var e := _engine(sol)
	var made := []
	var sub := F.ImmediateSubmitter.new(e)
	var ctl := ScfgCheckoutController.new({"config": e.config,
		"spec": ScfgPayments.prepare_payment(e.config, "unlock", {"item": "chapter2"}), "submit": sub.submit,
		"server_factory": func(kw: Dictionary) -> Variant:
			made.append(F.FakeServer.new(kw))
			return made[-1]})
	ctl.select_chain("solana")
	ctl.poll()
	ctl.start_browser()
	T.eq(made.size(), 1, "one page")
	if made.size() != 1:
		return T.done()
	var info: Dictionary = made[0].kw["session_info"]
	T.eq(made[0].kw.keys(), ["session_info", "on_tx", "get_status", "build_tx"], "a Solana page gets build_tx")
	T.eq([info["family"], info["wallet_chain"], info["mint"]], ["solana", "solana:mainnet", MINT], "family fields")
	T.eq(info["reference"], ctl.session["reference"], "the reference")
	T.ok(not info.has("chain_id"), "no EVM fields")
	var job: ScfgJob = made[0].build_tx.call(PLAYER)
	T.ok(job.done(), "the Job is done (immediate submitter)")
	var built: Variant = job.result()
	T.ok(built is Dictionary and built.has("transaction"), "the unsigned transaction")
	ctl.close()
	ctl.select_chain("base")
	ctl.poll()
	ctl.start_browser()
	T.eq(made.size(), 2, "a second page")
	if made.size() == 2:
		T.eq(made[1].kw["session_info"]["family"], "evm", "an EVM page")
		T.ok(not made[1].kw.has("build_tx"), "no build_tx off Solana")
	return T.done()
