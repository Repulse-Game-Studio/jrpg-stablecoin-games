## The Tier 2 conformance replay (tier2.gd) and its self-tests: the harness must fail a vector
## for every kind of difference the README lists, or a green run would prove nothing.
extends RefCounted

const T := preload("res://tests/t.gd")
const Vectors := preload("res://tests/vectors.gd")
const Tier2 := preload("res://tests/tier2.gd")

## Every Tier 2 vector: the count of conformance/vectors/tier2/**/*.json, pinned.
const ENABLED_COUNT := 121


func _vectors() -> Array:
	return Vectors.load_tier2(T.vectors_dir)


func _vector(id: String) -> Variant:
	for entry: Dictionary in _vectors():
		if entry["vector"]["id"] == id:
			return (entry["vector"] as Dictionary).duplicate(true)
	return null


## Every Tier 2 vector replays without a difference. None is skipped.
func test_tier2_vectors() -> bool:
	var vectors := _vectors()
	T.ok(vectors.size() > 0, "the tier2 files load")
	var ran := 0
	for entry: Dictionary in vectors:
		var path: String = entry["path"]
		T.ok(Tier2.ENABLED(entry["vector"]), "%s is enabled" % path)
		var msg: String = await Tier2.replay(entry["vector"])
		T.ok(msg == "", "%s: %s" % [path, msg])
		ran += 1
	T.eq(ran, ENABLED_COUNT, "replayed vectors")
	return T.done()


## The vector files on disk, counted without the loader: every one is enabled, and there are
## ENABLED_COUNT of them.
func test_every_vector_enabled() -> bool:
	var files: Array[String] = []
	_json_files(T.vectors_dir.path_join("tier2"), files)
	T.eq(files.size(), ENABLED_COUNT, "the tier2 vector files")
	var vectors := _vectors()
	T.eq(vectors.size(), files.size(), "the loader reads every file")
	var enabled := 0
	for entry: Dictionary in vectors:
		if Tier2.ENABLED(entry["vector"]):
			enabled += 1
		else:
			T.ok(false, "%s is not enabled" % entry["path"])
	T.eq(enabled, ENABLED_COUNT, "every vector is enabled")
	return T.done()


func _json_files(dir: String, out: Array[String]) -> void:
	for f: String in DirAccess.get_files_at(dir):
		if f.ends_with(".json"):
			out.append(dir.path_join(f))
	for d: String in DirAccess.get_directories_at(dir):
		_json_files(dir.path_join(d), out)


func test_enabled_vectors() -> bool:
	var ids: Array[String] = []
	for entry: Dictionary in _vectors():
		if Tier2.ENABLED(entry["vector"]):
			ids.append(entry["vector"]["id"])
	T.eq(ids.size(), ENABLED_COUNT, "the rule admits %d vectors" % ENABLED_COUNT)
	for id: String in ["evm/arc_large_scan", "evm/arc_large_hint", "evm/crash_in_one_session", "smoke/evm_unlock_paid",
			"restore/unlock_found", "reverify/malformed_entries_are_removed", "evm/arc_large_restore",
			"solana/build_payment", "solana/readiness_crash", "solana/scan_far_behind", "solana/restore_lookup"]:
		T.ok(id in ids, "%s is enabled" % id)
	T.ok(Tier2.ENABLED({"setup": {"config": {"solana_wallet": "x"}}, "steps": [{"action": "tick"}]}),
		"a config with a solana_wallet")
	T.ok(Tier2.ENABLED({"setup": {"config": {}}, "steps": [{"action": "reverify"}, {"action": "restore_lookup"},
		{"action": "restore_apply"}, {"action": "build_solana_payment"}]}), "every action")
	T.ok(not Tier2.ENABLED({"setup": {"config": {}}, "steps": [{"action": "fly"}]}), "an unknown action")
	T.ok(not Tier2.ENABLED({"setup": {"config": {}}, "steps": "x"}), "malformed steps")
	return T.done()


## Each mutation of a real vector must make the replay fail, naming the right kind of problem.
func test_replay_fails_on_every_kind_of_difference() -> bool:
	var base: Variant = _vector("smoke/evm_unlock_paid")
	T.ok(base is Dictionary, "the smoke vector")
	if not base is Dictionary:
		return T.done()
	T.eq(await Tier2.replay(base), "", "the vector itself replays")
	var steps: Array = base["steps"]
	var tick := -1
	for i in steps.size():
		if steps[i]["action"] == "tick" and not (steps[i]["calls"] as Array).is_empty():
			tick = i
			break
	T.ok(tick > 0, "a tick with calls")
	var cases := {
		"a recorded call not made": func(v: Dictionary) -> void:
			v["steps"][tick]["calls"].append(v["steps"][tick]["calls"][0].duplicate(true)),
		"an unexpected call": func(v: Dictionary) -> void: v["steps"][tick]["calls"].pop_back(),
		"other params": func(v: Dictionary) -> void:
			var c: Dictionary = v["steps"][tick]["calls"][-1]
			c["params"] = ["0x0"] if (c["params"] as Array).is_empty() else ([true] + (c["params"] as Array).slice(1)),
		"unused entropy": func(v: Dictionary) -> void:
			v["steps"][0]["entropy"].append({"fn": "token_hex", "n": 8, "value": "00"}),
		"other entropy": func(v: Dictionary) -> void: v["steps"][0]["entropy"][0]["n"] = 99,
		"other returns": func(v: Dictionary) -> void: v["steps"][0]["returns"]["units"] = 1,
		"an extra returned key": func(v: Dictionary) -> void: (v["steps"][0]["returns"] as Dictionary).erase("units"),
		"other raises": func(v: Dictionary) -> void: v["steps"][0]["raises"] = {"type": "other"},
		"other internal errors": func(v: Dictionary) -> void: v["steps"][tick]["internal_errors"] = 1,
		"another snapshot": func(v: Dictionary) -> void: v["steps"][0]["snapshot"]["tip_total"] = "1",
		"unsorted owned in the record": func(v: Dictionary) -> void: v["steps"][-1]["snapshot"]["owned"] = ["x"],
	}
	for name: String in cases:
		var v: Dictionary = base.duplicate(true)
		(cases[name] as Callable).call(v)
		var msg: String = await Tier2.replay(v)
		T.ok(msg != "", "%s is caught" % name)
	T.eq(await Tier2.replay({"tier": 1}), "not a Tier 2 vector", "tier")
	return T.done()


func test_replay_transport_is_a_multiset_in_recorded_order() -> bool:
	var t := Tier2.ReplayTransport.new()
	var a := {"url": "u", "method": "m", "params": [1, {"b": true, "a": "x"}], "response_text": "{\"result\": 1}"}
	var b := {"url": "u", "method": "m", "params": [1, {"a": "x", "b": true}], "response_text": "{\"result\": 2}"}
	var e := {"url": "u", "method": "n", "params": [], "transport_error": {"message": "HTTP 429", "status": 429}}
	t.load([a, b, e])
	var first: Variant = await t.post_json("u", {"method": "n", "params": [], "id": 9}, 1.0)
	T.ok(first is ScfgErr, "a transport error")
	var r1: Variant = await t.post_json("u", {"method": "m", "params": [1.0, {"a": "x", "b": true}], "id": 1}, 1.0)
	var r2: Variant = await t.post_json("u", {"method": "m", "params": [1, {"a": "x", "b": true}], "id": 2}, 1.0)
	T.json_eq([r1, r2], [{"result": 1}, {"result": 2}], "identical requests take their copies in recorded order")
	T.eq(t.leftover(), [], "all used")
	T.is_err(await t.post_json("u", {"method": "m", "params": [1, {"a": "x", "b": true}]}, 1.0), "AssertionError",
		"a third copy is unexpected")
	T.eq(t.problems.size(), 1, "recorded")
	t.load([a])
	T.is_err(await t.post_json("u", {"method": "m", "params": [true, {"a": "x", "b": true}]}, 1.0), "AssertionError",
		"true is not 1")
	T.eq(t.leftover().size(), 1, "unused call")
	t.load([{"url": "u", "method": "m", "params": [], "response_text": "not json"}])
	var bad: Variant = await t.post_json("u", {"method": "m", "params": []}, 1.0)
	T.ok(bad is ScfgErr and (bad as ScfgErr).kind == "TransportError" and (bad as ScfgErr).message == "invalid JSON response",
		"an unparsable body is the transport's failure")
	var tr: Variant = null
	t.load([e])
	tr = await t.post_json("u", {"method": "n", "params": []}, 1.0)
	T.eq([tr.kind, tr.message, tr.status], ["TransportError", "HTTP 429", 429], "the recorded transport error")
	return T.done()


func test_scripted_entropy_reports_every_difference() -> bool:
	var s := Tier2.ScriptedEntropy.new([{"fn": "randbelow", "n": 100, "value": 7}, {"fn": "token_bytes", "n": 2,
		"value": "0aff"}])
	T.eq(s.randbelow(100), 7, "a draw")
	T.eq(s.token_bytes(2), PackedByteArray([10, 255]), "token_bytes are hex")
	T.is_err(s.token_hex(8), "AssertionError", "exhausted")
	var m := Tier2.ScriptedEntropy.new([{"fn": "randbelow", "n": 100, "value": 7}])
	T.is_err(m.token_hex(8), "AssertionError", "another fn")
	T.eq(m.problems.size(), 1, "recorded")
	var n := Tier2.ScriptedEntropy.new([{"fn": "randbelow", "n": 100, "value": 7}])
	T.is_err(n.randbelow(99), "AssertionError", "another n")
	var left := Tier2.ScriptedEntropy.new([{"fn": "randbelow", "n": 100, "value": 7}])
	T.eq(left.leftover(), ["unconsumed entropy: [(randbelow, 100)]"], "unused draws")
	return T.done()


func test_encoders_follow_the_vector_rules() -> bool:
	T.eq(Tier2.to_json({"a": [1, 2.0, "x", null, true]}), {"a": [1, 2, "x", null, true]}, "plain JSON")
	T.eq(Tier2.to_json(ScfgDec.parse("4.990000")), "4.990000", "a Decimal is its str")
	T.eq(Tier2.to_json(ScfgBigInt.from_decimal("9007199254740991")), 9007199254740991, "a BigInt below 2^53")
	T.is_err(Tier2.to_json(ScfgBigInt.from_decimal("9007199254740992")), "ValueError", "a BigInt of 2^53")
	T.is_err(Tier2.to_json([9007199254740992]), "ValueError", "an int of 2^53")
	T.is_err(Tier2.to_json({"x": 1.5}), "ValueError", "a fractional float")
	T.is_err(Tier2.to_json({5: 1}), "TypeError", "a non-string key")
	T.is_err(Tier2.to_json(PackedByteArray([1])), "TypeError", "bytes")
	T.is_err(Tier2.to_json(RefCounted.new()), "TypeError", "another object")
	return T.done()


func test_run_action_buckets_raises_and_refuses_null() -> bool:
	var v: Variant = _vector("evm/start_rejections")
	T.ok(v is Dictionary, "the vector")
	T.eq(await Tier2.replay(v), "", "ConfigError codes are compared")
	var cfg: Variant = ScfgConfig.build_config({"wallet": "0x52908400098527886E0F7030069857D2E4169EE7"})
	var e := ScfgEngine.new(cfg, {"rpc_factory": func(_c: Dictionary) -> Variant: return null})
	var r: Array = await Tier2.run_action(e, "report_hint", {"sid": "ps_x", "tx": "0x"}, [])
	T.eq(r, [null, {"type": "other"}, ""], "a KeyError is other")
	r = await Tier2.run_action(e, "start_session", {"chain": "arbitrum"}, [])
	T.eq(r, [null, {"type": "other"}, ""], "a missing kind is other")
	r = await Tier2.run_action(e, "discard_session", {"sid": "ps_x"}, [])
	T.eq(r, [null, null, ""], "a void action returns None")
	r = await Tier2.run_action(e, "build_solana_payment", {"sid": "ps_x", "account": "x"}, [])
	T.eq(r, [null, {"type": "other"}, ""], "build_solana_payment of an unknown session is other")
	r = await Tier2.run_action(e, "build_solana_payment", {"sid": "ps_x"}, [])
	T.eq(r, [null, {"type": "other"}, ""], "a missing account is other")
	r = await Tier2.run_action(e, "fly", {}, [])
	T.ok(r[2] != "", "an unknown action")
	return T.done()


# --- snapshot_json sorts what Python's sets leave unordered --------------------------------------


## An Engine whose snapshot lists `owned` and `unready` in the wrong order, as a Python set may.
class UnsortedEngine:
	extends ScfgEngine

	func snapshot() -> Variant:
		return {"pending": {}, "receipts": {}, "owned": ["ost", "chapter2", "bonus"], "tip_total": ScfgDec.parse("0"),
			"unready": [["solana", "USDT"], ["base", "USDT"], ["arbitrum", "USDT"], ["base", "USDC"]]}


## Mutation test: the vectors record `owned` and `unready` sorted, so the runner must sort them
## itself. With an unsorted source its output is sorted, and a replay that compares an unsorted
## record fails.
func test_snapshot_json_sorts_owned_and_unready() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"wallet": "0x52908400098527886E0F7030069857D2E4169EE7"})
	var snap: Variant = Tier2.snapshot_json(UnsortedEngine.new(cfg))
	T.ok(snap is Dictionary, "a snapshot")
	if snap is Dictionary:
		T.eq(snap["owned"], ["bonus", "chapter2", "ost"], "owned is sorted")
		T.eq(snap["unready"], [["arbitrum", "USDT"], ["base", "USDC"], ["base", "USDT"], ["solana", "USDT"]],
			"unready is sorted by chain, then token")
		T.eq(snap["tip_total"], "0", "tip_total is the Decimal's str")
	# the replay itself: a vector with two owned items, recorded sorted, fails once the record is not
	var base: Variant = _vector("restore/ambiguous_amounts_are_applied_with_choices")
	T.ok(base is Dictionary, "the vector")
	if not base is Dictionary:
		return T.done()
	T.eq(await Tier2.replay(base), "", "the vector replays")
	var steps: Array = base["steps"]
	var at := -1
	for i in steps.size():
		if (steps[i]["snapshot"]["owned"] as Array).size() >= 2:
			at = i
			break
	T.ok(at >= 0, "a step with two owned items")
	var reversed: Dictionary = base.duplicate(true)
	(reversed["steps"][at]["snapshot"]["owned"] as Array).reverse()
	var msg: String = await Tier2.replay(reversed)
	T.ok(msg.contains("snapshot"), "an unsorted record is caught: %s" % msg)
	return T.done()


# --- restore candidates ---------------------------------------------------------------------


func _candidate_json(transfer: Dictionary) -> Dictionary:
	return {"chain": "arbitrum", "token": "USDC", "transfer": transfer, "kind": null, "item": null,
		"choices": ["tip"], "already_restored": false}


func test_candidate_from_json_rebuilds_the_dataclasses() -> bool:
	var log := {"token": "0xa", "sender": "0xb", "recipient": "0xc", "value": 5, "tx_hash": "0xd", "log_index": 1,
		"block_number": 2, "block_hash": "0xe", "class": "TransferLog"}
	var c: Variant = Tier2.candidate_from_json(_candidate_json(log))
	T.eq((c["transfer"] as Dictionary).keys(), ["token", "sender", "recipient", "value", "tx_hash", "log_index",
		"block_number", "block_hash"], "a TransferLog without its class tag")
	T.eq(c.keys(), ["chain", "token", "transfer", "kind", "item", "choices", "already_restored"], "Python's field order")
	T.eq(c["choices"], ["tip"], "choices")
	T.ok(c["transfer"]["value"] is ScfgBigInt and (c["transfer"]["value"] as ScfgBigInt).to_decimal() == "5",
		"a TransferLog's value is an ScfgBigInt (the TransferLog contract)")
	var plain := log.duplicate()
	plain.erase("block_hash")
	plain["class"] = "Transfer"
	var p: Variant = Tier2.candidate_from_json(_candidate_json(plain))
	T.ok(p is Dictionary, "a Transfer")
	T.ok(p is Dictionary and ScfgPy.py_eq(p["transfer"]["value"], 5), "its value")
	T.is_err(Tier2.candidate_from_json(_candidate_json(log.merged({"class": "Transfer"}, true))), "TypeError",
		"a TransferLog's fields for a Transfer")
	T.is_err(Tier2.candidate_from_json(_candidate_json(log.merged({"class": "Other"}, true))), "KeyError", "an unknown class")
	var untagged := log.duplicate()
	untagged.erase("class")
	T.is_err(Tier2.candidate_from_json(_candidate_json(untagged)), "KeyError", "no class")
	var short := log.duplicate()
	short.erase("value")
	T.is_err(Tier2.candidate_from_json(_candidate_json(short)), "TypeError", "a missing field")
	T.is_err(Tier2.candidate_from_json({"chain": "x"}), "KeyError", "a missing key")
	return T.done()


func test_candidate_to_json_tags_the_transfer_class() -> bool:
	var log := {"token": "0xa", "sender": "0xb", "recipient": "0xc", "value": ScfgBigInt.from_int(5), "tx_hash": "0xd",
		"log_index": 1, "block_number": 2, "block_hash": "0xe"}
	var j: Variant = Tier2.candidate_to_json(_candidate_json(log))
	T.eq(j["transfer"]["class"], "TransferLog", "an EVM log")
	T.eq(j["transfer"]["value"], 5, "the value is a JSON int")
	var plain := log.duplicate()
	plain.erase("block_hash")
	T.eq(Tier2.candidate_to_json(_candidate_json(plain))["transfer"]["class"], "Transfer", "a Solana transfer")
	T.eq(log.has("class"), false, "the candidate itself is not touched")
	return T.done()


func test_restore_apply_reads_the_candidates_of_an_earlier_step() -> bool:
	var e := ScfgEngine.new(ScfgConfig.build_config({"wallet": "0x52908400098527886E0F7030069857D2E4169EE7"}))
	var log := {"token": "0xa", "sender": "0xb", "recipient": "0xc", "value": 1000000, "tx_hash": "0xd", "log_index": 1,
		"block_number": 2, "block_hash": "0xe", "class": "TransferLog"}
	var steps := [{"returns": [_candidate_json(log)]}]
	var r: Array = await Tier2.run_action(e, "restore_apply", {"from_step": 0, "choices": {"0": "tip"}}, steps)
	T.eq([r[1], r[2]], [null, ""], "no raise, no harness problem")
	T.eq(r[0].size(), 1, "one receipt")
	T.eq(r[0][0]["kind"], "tip", "chosen through the string key '0'")
	var none: Array = await Tier2.run_action(e, "restore_apply", {"from_step": 0, "choices": null}, steps)
	T.eq(none[0], [], "choices null: nothing chosen")
	var bad: Array = await Tier2.run_action(e, "restore_apply", {"from_step": 0, "choices": {"0": "ost"}}, steps)
	T.eq(bad[1], {"type": "other"}, "a choice the candidate does not offer")
	var no_step: Array = await Tier2.run_action(e, "restore_apply", {"from_step": 3, "choices": null}, steps)
	T.eq(no_step[1], {"type": "other"}, "a step that does not exist")
	var raised: Array = await Tier2.run_action(e, "restore_apply", {"from_step": 0, "choices": null}, [{"returns": null}])
	T.eq(raised[1], {"type": "other"}, "a step that recorded no returns")
	var missing: Array = await Tier2.run_action(e, "restore_apply", {"choices": null}, steps)
	T.eq(missing[1], {"type": "other"}, "no from_step")
	return T.done()


func test_run_action_maps_a_restore_error_to_its_code() -> bool:
	var cfg: Variant = ScfgConfig.build_config({"wallet": "0x52908400098527886E0F7030069857D2E4169EE7"})
	var e := ScfgEngine.new(cfg, {"rpc_factory": func(_c: Dictionary) -> Variant: return null})
	var r: Array = await Tier2.run_action(e, "restore_lookup", {"tx": "not a hash"}, [])
	T.eq(r, [null, {"type": "RestoreError", "code": "invalid_hash"}, ""], "invalid_hash")
	return T.done()


func test_encode_returns_tags_the_candidates_of_a_restore_lookup() -> bool:
	var log := {"token": "0xa", "sender": "0xb", "recipient": "0xc", "value": 5, "tx_hash": "0xd", "log_index": 1,
		"block_number": 2, "block_hash": "0xe"}
	var out: Variant = Tier2.encode_returns("restore_lookup", [_candidate_json(log)])
	T.eq(out[0]["transfer"]["class"], "TransferLog", "tagged")
	T.eq(Tier2.encode_returns("reverify", ["a:b:1"]), ["a:b:1"], "other actions are encoded as they are")
	return T.done()
