"""Tier 2: recording scenarios against the simulator and replaying vectors without it."""

from __future__ import annotations

import copy
import json
import secrets
from pathlib import Path

import pytest

from scfg_conformance import canonical, replay, scenario, tier2, vectors
from scfg_conformance.scenarios.fixtures import CHAPTER2, DEV_EVM, DEV_SOL, PAYER_EVM, tx_hash
from scfg_core import sessions
from scfg_core.backends import solana as solana_backend
from scfg_core.chains import TESTNET
from scfg_core.coio import TransportError
from scfg_core.evm import TRANSFER_TOPIC, TransferLog, address_topic
from scfg_core.restore import RestoreCandidate
from scfg_core.transfers import Transfer

ROOT = Path(__file__).resolve().parents[2]
TIER2_DIR = ROOT / "conformance" / "vectors" / "tier2"
SMOKE = ["smoke/evm_unlock_paid", "smoke/solana_unlock_paid"]
USDC = TESTNET["base"].tokens["USDC"].address
STEP_KEYS = {"at", "action", "args", "entropy", "calls", "returns", "raises", "internal_errors",
             "snapshot"}


def load(vid: str) -> dict:
    return json.loads((TIER2_DIR / f"{vid}.json").read_text(encoding="utf-8"))


def all_tier2():
    return [pytest.param(p, id=p.relative_to(TIER2_DIR).with_suffix("").as_posix())
            for p in sorted(TIER2_DIR.rglob("*.json"))]


def base_scenario(sid="test/base", urls=None):
    s = scenario.Scenario(sid, [])
    s.config(wallet=DEV_EVM, testnet=True, chains=["base"])
    s.item(**CHAPTER2)
    return s, s.evm("base", urls=urls)


def first_step_with_calls(vector, action="tick"):
    for i, step in enumerate(vector["steps"]):
        if step["action"] == action and step["calls"]:
            return i, step
    raise AssertionError("no such step")


# --- shape and round trip --------------------------------------------------------------------


def test_smoke_scenarios_registered_with_shape():
    vectors.load_all()
    for vid in SMOKE:
        assert f"tier2/{vid}.json" in vectors.SOURCES
        assert f"tier2/{vid}.json" in tier2.SOURCES
        v = load(vid)
        assert set(v) == {"tier", "id", "covers", "setup", "steps"}
        assert (v["tier"], v["id"]) == (2, vid)
        assert set(v["setup"]) == {"clock", "config", "custom_chains", "items", "pending",
                                   "receipts"}
        assert v["setup"]["clock"] == scenario.EPOCH == 1_700_000_000
        for step in v["steps"]:
            assert set(step) == STEP_KEYS
            for call in step["calls"]:
                assert set(call) in ({"url", "method", "params", "response_text"},
                                     {"url", "method", "params", "transport_error"})
        last = v["steps"][-1]
        assert last["action"] == "tick" and last["returns"][0]["status"] == "paid"
        assert last["snapshot"]["owned"] == ["chapter2"] and last["snapshot"]["pending"] == {}


def test_replay_round_trip():
    for vid in SMOKE:
        replay.replay(load(vid))


@pytest.mark.parametrize("path", all_tier2())
def test_tier2_vectors_replay(path):
    replay.replay(json.loads(path.read_text(encoding="utf-8")))


def test_actions_return_the_python_result():
    s, node = base_scenario()
    rec = s.start_session("unlock", "base", item="chapter2")
    assert rec["id"].startswith("ps_") and rec["start_block"] == node.head - 5
    assert s.steps[0]["returns"] == rec
    assert s.tick()[0].status == "waiting"
    assert s.start_session("unlock", "base", item="nope") is None
    assert s.steps[-1]["raises"] == {"type": "ConfigError", "code": "item.unknown"}
    with pytest.raises(RuntimeError):
        s.item(item_id="late", code=2, price="1", token="USDC", title="Late")  # setup is over


# --- the replay is strict ---------------------------------------------------------------------


def test_replay_detects_missing_call():
    v = load(SMOKE[0])
    i, step = first_step_with_calls(v)
    step["calls"].append(dict(step["calls"][0], params=["0x" + "00" * 32]))
    with pytest.raises(AssertionError, match=rf"^step {i} \(tick\): expected call not made"):
        replay.replay(v)


def test_replay_detects_extra_call():
    v = load(SMOKE[0])
    i, step = first_step_with_calls(v)
    del step["calls"][-1]
    with pytest.raises(AssertionError, match=rf"^step {i} \(tick\): unexpected call"):
        replay.replay(v)


def test_replay_detects_snapshot_change():
    v = load(SMOKE[0])
    v["steps"][-1]["snapshot"]["owned"] = []
    last = len(v["steps"]) - 1
    with pytest.raises(AssertionError, match=rf"^step {last} \(tick\): snapshot.*owned"):
        replay.replay(v)


@pytest.mark.parametrize("field, value, message", [
    ("returns", [], "returns"),
    ("raises", {"type": "other"}, "raises"),
    ("internal_errors", 1, "internal_errors"),
])
def test_replay_detects_result_changes(field, value, message):
    v = load(SMOKE[0])
    v["steps"][-1][field] = value
    with pytest.raises(AssertionError, match=rf"\(tick\): {message}"):
        replay.replay(v)


def test_replay_counts_duplicate_calls():
    s, node = base_scenario()
    s.start_session("unlock", "base", item="chapter2")
    s.start_session("tip", "base", amount="3", token="USDC")
    s.tick()
    v = json.loads(canonical.dumps(s.to_vector()))
    tick = v["steps"][2]
    head = [c for c in tick["calls"] if c["method"] == "eth_blockNumber"]
    assert len(head) == 2                             # the same request twice in one step
    assert {(c["url"], c["method"], json.dumps(c["params"])) for c in head} == {
        (head[0]["url"], "eth_blockNumber", "[]")}
    replay.replay(copy.deepcopy(v))
    tick["calls"].remove(head[0])                     # a set-based check would still pass
    with pytest.raises(AssertionError, match=r"^step 2 \(tick\): unexpected call .*eth_block"):
        replay.replay(v)


def test_replay_raises_recorded_transport_error():
    urls = list(TESTNET["base"].rpc)
    s, node = base_scenario()
    node.endpoint(urls[0]).down = True
    s.start_session("unlock", "base", item="chapter2")
    node.endpoint(urls[0]).down = False
    s.advance(60)                                      # past the 30 s cooldown
    node.endpoint(urls[0]).http_status = 429
    s.tick()
    s.advance(120)
    node.endpoint(urls[0]).http_status = None
    node.endpoint(urls[0]).invalid_json = True
    s.tick()
    v = json.loads(canonical.dumps(s.to_vector()))
    errors = [c["transport_error"] for step in v["steps"] for c in step["calls"]
              if "transport_error" in c]
    assert errors == [{"message": "connection refused", "status": None},
                      {"message": "HTTP 429", "status": 429},
                      {"message": "invalid JSON response", "status": None}]
    assert all(c["url"] == urls[1] for c in v["steps"][0]["calls"][1:])
    replay.replay(copy.deepcopy(v))

    broken = copy.deepcopy(v)
    call = broken["steps"][0]["calls"][0]
    del call["transport_error"]
    call["response_text"] = json.dumps({"jsonrpc": "2.0", "id": 1, "result": hex(5000)})
    with pytest.raises(AssertionError, match=r"^step 0 \(start_session\)"):
        replay.replay(broken)


def test_replay_transport_raises_recorded_transport_errors_with_status():
    t = replay.ReplayTransport()
    t.load([
        {"url": "u", "method": "m", "params": [1], "transport_error": {"message": "HTTP 429",
                                                                       "status": 429}},
        {"url": "u", "method": "m", "params": [2],
         "transport_error": {"message": "connection refused", "status": None}},
        {"url": "u", "method": "m", "params": [1], "response_text": '{"result": 5}'},
    ])
    payload = {"jsonrpc": "2.0", "id": 99, "method": "m", "params": [1]}
    with pytest.raises(TransportError) as e:
        t.post_json("u", payload, 10)
    assert (str(e.value), e.value.status) == ("HTTP 429", 429)
    with pytest.raises(TransportError) as e:
        t.post_json("u", dict(payload, params=[2]), 10)
    assert (str(e.value), e.value.status) == ("connection refused", None)
    assert t.post_json("u", dict(payload, id=1), 10) == {"result": 5}  # duplicates in order
    assert t.leftover() == [] and t.problems == []
    with pytest.raises(AssertionError):
        t.post_json("u", payload, 10)                 # all copies used up
    assert t.problems and t.problems[0].startswith("unexpected call u m [1]")


def test_replay_params_compare_as_json_values():
    t = replay.ReplayTransport()
    t.load([{"url": "u", "method": "m", "params": [{"b": 1, "a": True}],
             "response_text": '{"result": 1}'}])
    with pytest.raises(AssertionError):               # true is not 1
        t.post_json("u", {"method": "m", "params": [{"a": 1, "b": 1}]}, 10)
    t.load([{"url": "u", "method": "m", "params": [{"b": 1, "a": True}],
             "response_text": '{"result": 1}'}])
    assert t.post_json("u", {"method": "m", "params": [{"a": True, "b": 1}]}, 10) == {"result": 1}


def test_entropy_recorded_and_consumed():
    evm = load(SMOKE[0])
    draws = evm["steps"][0]["entropy"]
    assert [(d["fn"], d["n"]) for d in draws] == [("randbelow", 100), ("token_hex", 8)]
    assert evm["steps"][0]["returns"]["id"] == "ps_" + draws[1]["value"]
    assert evm["steps"][0]["returns"]["nonce"] == draws[0]["value"]
    assert all(step["entropy"] == [] for step in evm["steps"][1:])
    sol = load(SMOKE[1])
    first = sol["steps"][0]["entropy"][0]
    assert (first["fn"], first["n"]) == ("token_bytes", 32) and len(first["value"]) == 64

    extra = copy.deepcopy(evm)
    extra["steps"][0]["entropy"].append({"fn": "randbelow", "n": 100, "value": 1})
    with pytest.raises(AssertionError, match=r"^step 0 \(start_session\): unconsumed entropy"):
        replay.replay(extra)
    short = copy.deepcopy(evm)
    short["steps"][0]["entropy"].pop()
    with pytest.raises(AssertionError, match=r"^step 0 \(start_session\): entropy exhausted"):
        replay.replay(short)
    wrong = copy.deepcopy(evm)
    wrong["steps"][0]["entropy"][0]["n"] = 99
    with pytest.raises(AssertionError, match=r"^step 0 \(start_session\): entropy mismatch"):
        replay.replay(wrong)
    assert sessions.secrets is secrets and solana_backend.secrets is secrets


def test_entropy_is_seeded_by_scenario_id():
    def first_id(sid):
        s, _ = base_scenario(sid)
        return s.start_session("unlock", "base", item="chapter2")["id"]
    assert first_id("test/a") == first_id("test/a") != first_id("test/b")


def test_clock_is_shared_by_engine_and_rpc_clients():
    urls = list(TESTNET["base"].rpc)
    s, node = base_scenario()
    node.endpoint(urls[0]).down = True
    s.start_session("unlock", "base", item="chapter2")
    node.endpoint(urls[0]).down = False
    s.advance(29)
    s.tick()                     # urls[0] still cooling down (30 s): only urls[1] is asked
    assert {c["url"] for c in s.steps[-1]["calls"]} == {urls[1]}
    s.advance(1)
    s.tick()
    assert s.steps[-1]["calls"][0]["url"] == urls[0]
    assert [step["at"] for step in s.steps] == [scenario.EPOCH, scenario.EPOCH + 29,
                                                scenario.EPOCH + 30]


def test_clock_is_shared_by_solana_rpc_client():
    """SolanaRpc wraps its RpcClient: the scenario clock must reach .client.clock too."""
    s = scenario.Scenario("test/solana_clock", [])
    s.config(solana_wallet=DEV_SOL, testnet=True, rpc_urls={"solana": "https://my-node.example"})
    node = s.solana()
    assert node.urls == ["https://my-node.example", TESTNET["solana"].rpc[0]]
    node.endpoint("https://my-node.example").down = True
    s.start_session("tip", "solana", amount="1", token="USDC")
    node.endpoint("https://my-node.example").down = False
    s.advance(29)
    s.tick()                     # still cooling down (30 s): only devnet's own URL is asked
    assert {c["url"] for c in s.steps[-1]["calls"]} == {TESTNET["solana"].rpc[0]}
    s.advance(1)
    s.tick()
    assert s.steps[-1]["calls"][0]["url"] == "https://my-node.example"
    replay.replay(json.loads(canonical.dumps(s.to_vector())))


def _malformed_log(node):
    """A Transfer log without transactionHash: watcher.advance crashes on it (KeyError)."""
    node.logs.append({"address": USDC.lower(), "blockNumber": hex(node.head),
                      "topics": [TRANSFER_TOPIC, address_topic(PAYER_EVM),
                                 address_topic(DEV_EVM)]})


def test_declared_internal_error_is_counted_not_typed():
    s, node = base_scenario()
    s.start_session("unlock", "base", item="chapter2")
    _malformed_log(node)
    [out] = s.expect("internal_error").tick()
    assert out.error == "internal_error" and s.steps[-1]["internal_errors"] == 1
    v = json.loads(canonical.dumps(s.to_vector()))
    assert "KeyError" not in json.dumps(v) and "expect" not in json.dumps(v)
    replay.replay(v)


@pytest.fixture
def registry(monkeypatch):
    """An empty scenario registry, so test scenarios never reach the real vectors."""
    monkeypatch.setattr(scenario, "SOURCES", {})
    return scenario.SOURCES


def test_undeclared_internal_error_fails_generation(registry):
    @scenario.scenario("test/crash", [])
    def _(s):
        s.config(wallet=DEV_EVM, testnet=True, chains=["base"])
        s.item(**CHAPTER2)
        node = s.evm("base")
        s.start_session("unlock", "base", item="chapter2")
        _malformed_log(node)
        s.tick()

    with pytest.raises(scenario.ScenarioError,
                       match=r"step 1 \(tick\): 1 internal error\(s\) the scenario did not "
                             r"declare") as e:
        registry["tier2/test/crash.json"]()
    assert isinstance(e.value.__cause__, KeyError)   # the real traceback travels along


def test_undeclared_other_raise_fails_generation_even_if_caught(registry):
    @scenario.scenario("test/typo", [])
    def _(s):
        s.config(wallet=DEV_EVM, testnet=True, chains=["base"])
        s.evm("base")
        try:
            s.report_hint("ps_missing", tx_hash(1))  # a typo'd session id: KeyError
        except scenario.ScenarioError:
            pass                                     # swallowing it does not help

    with pytest.raises(scenario.ScenarioError, match=r"step 0 \(report_hint\): raised KeyError"):
        registry["tier2/test/typo.json"]()


def test_expect_opts_in_one_action_and_must_be_met(registry):
    s, node = base_scenario()
    s.start_session("unlock", "base", item="chapter2")
    assert s.expect("other").report_hint(s.steps[0]["returns"]["id"], "0x12") is None
    assert s.steps[-1]["raises"] == {"type": "other"}
    _malformed_log(node)
    s.expect("internal_error").tick()
    with pytest.raises(scenario.ScenarioError, match=r"step 3 \(tick\): 1 internal error"):
        s.tick()                                     # the opt-in covered one action only
    with pytest.raises(scenario.ScenarioError, match=r"expected an 'other' raise, got None"):
        s.expect("other", "internal_error").set_ui_status("ps_x", "open")
    with pytest.raises(ValueError):
        s.expect("crash")
    with pytest.raises(ValueError):
        s.expect()

    @scenario.scenario("test/declared", [])
    def _(s):
        s.config(wallet=DEV_EVM, testnet=True, chains=["base"])
        s.item(**CHAPTER2)
        node = s.evm("base")
        rec = s.start_session("unlock", "base", item="chapter2")
        s.expect("other").report_hint(rec["id"], "not a hash")
        _malformed_log(node)
        s.expect("internal_error").tick()

    v = registry["tier2/test/declared.json"]()       # builds, and replays clean
    assert [st["internal_errors"] for st in v["steps"]] == [0, 0, 1]
    assert v["steps"][1]["raises"] == {"type": "other"}


def test_restore_apply_replays_candidates_from_recorded_returns():
    s, node = base_scenario()
    exact = s.chain("base").tokens["USDC"].address
    node.add_transfer(exact, PAYER_EVM, DEV_EVM, 4_990_000, block=990, tx_hash=tx_hash(1))
    found = s.restore_lookup(tx_hash(1))
    assert found[0].kind is None and "chapter2" in found[0].choices   # whole cents: ambiguous
    s.restore_apply(0, {0: "chapter2"})
    assert s.steps[1]["args"] == {"from_step": 0, "choices": {"0": "chapter2"}}
    assert s.steps[1]["snapshot"]["owned"] == ["chapter2"]
    cand = s.steps[0]["returns"][0]
    assert cand["transfer"]["class"] == "TransferLog"
    with pytest.raises(ValueError):
        s.restore_apply(1)                       # step 1 is not a restore_lookup
    replay.replay(json.loads(canonical.dumps(s.to_vector())))


def test_candidate_json_round_trip():
    for transfer in (TransferLog(USDC, PAYER_EVM, DEV_EVM, 5, tx_hash(1), 2, 3, tx_hash(4)),
                     Transfer("Mint", "Payer", "Dev", 5, "Sig", 1, 9)):
        c = RestoreCandidate(chain="base", token="USDC", transfer=transfer, kind=None,
                             item=None, choices=("chapter2", "tip"), already_restored=False)
        data = json.loads(json.dumps(replay.candidate_to_json(c)))
        assert data["transfer"]["class"] == type(transfer).__name__
        assert replay.candidate_from_json(data) == c


def test_scenario_registration():
    assert scenario.SOURCES is tier2.SOURCES
    with pytest.raises(ValueError):
        scenario.scenario("smoke/evm_unlock_paid", [])(lambda s: None)
    with pytest.raises(ValueError):
        scenario.scenario("Bad Id", [])
    with pytest.raises(ValueError):
        scenario.scenario("test/x", ["not a test id"])
