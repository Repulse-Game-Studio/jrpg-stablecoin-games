"""The frozen receipt/pending format (conformance/schema/state.json)."""

from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from scfg_conformance import state_schema
from scfg_core.backends.solana import SolanaBackend
from scfg_core.chains import TESTNET
from scfg_core.coio import run_sync
from scfg_core.receipts import make_receipt
from scfg_core.sessions import create_session
from scfg_core.transfers import Transfer

ROOT = Path(__file__).resolve().parents[2]
TIER2_DIR = ROOT / "conformance" / "vectors" / "tier2"


def _snapshots():
    out = []
    for path in sorted(TIER2_DIR.rglob("*.json")):
        v = json.loads(path.read_text(encoding="utf-8"))
        name = path.relative_to(TIER2_DIR).with_suffix("").as_posix()
        out.append(pytest.param(v["setup"], [s["snapshot"] for s in v["steps"]], id=name))
    return out


# Scenarios whose loaded state is deliberately not valid (garbage in the store that
# re-verification must clear): only the snapshots after their steps must match the schema.
INVALID_SETUP = {"reverify/malformed_entries_are_removed"}


@pytest.mark.parametrize("setup, snapshots", _snapshots())
def test_every_snapshot_matches_state_schema(setup, snapshots, request):
    assert snapshots
    initial = {"pending": setup["pending"], "receipts": setup["receipts"], "owned": [],
               "tip_total": "0", "unready": []}
    if request.node.callspec.id in INVALID_SETUP:
        assert state_schema.check(initial) != []
        initial = {"pending": {}, "receipts": {}, "owned": [], "tip_total": "0", "unready": []}
    for i, snap in enumerate([initial, *snapshots]):
        assert state_schema.check(snap) == [], f"snapshot {i}"


def test_tier2_vectors_exercise_both_families():
    families = set()
    for path in TIER2_DIR.rglob("*.json"):
        for step in json.loads(path.read_text(encoding="utf-8"))["steps"]:
            for rec in step["snapshot"]["pending"].values():
                families.add("solana" if "reference" in rec else "evm")
            if step["snapshot"]["receipts"]:
                families.add("receipt")
    assert families >= {"evm", "solana", "receipt"}


def test_state_schema_fields_match_python_records():
    schema = state_schema.load()
    t = Transfer("Mint", "Payer", "Dev", 5, "Sig", 1, 9)
    receipt = make_receipt(chain="solana", transfer=t, kind="tip", item=None, token="USDC",
                           source="payment")
    assert set(schema["receipt"]) == set(receipt)
    rec = create_session({}, kind="tip", item=None, title="Tip", token="USDC", chain="base",
                         price="3", code=0, start_block=1, now=0)
    assert set(schema["pending"]) == set(rec)

    class Rpc:
        def get_slot(self):
            return 1000
            yield

        def get_signatures_for_address(self, address, limit):
            return []
            yield

    fields = run_sync(SolanaBackend().start(rpc=Rpc(), chain=TESTNET["solana"],
                                            wallet="7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU",
                                            token="USDC"), None)
    fields.pop("start_block")
    assert set(schema["pending_optional"]) == set(fields)
    assert set(schema["match"]) | set(schema["match_optional"]) == {
        "tx_hash", "log_index", "block", "block_hash", "value", "payer", "recipient"}


GOOD_RECEIPT = {"chain": "base", "tx_hash": "0x" + "ab" * 32, "log_index": 0, "block": 9,
                "kind": "unlock", "item": "chapter2", "amount": "4.990000", "token": "USDC",
                "payer": "0xP", "recipient": "0xD", "source": "payment"}


def _snapshot():
    rec = create_session({}, kind="unlock", item="chapter2", title="Chapter 2", token="USDC",
                         chain="base", price="4.99", code=37, start_block=1, now=1_700_000_000,
                         nonce=5)
    rec["match"] = {"tx_hash": "0x" + "ab" * 32, "log_index": 0, "block": 9,
                    "block_hash": "0x" + "cd" * 32, "value": 4993705, "payer": "0xP",
                    "recipient": "0xD"}
    return {"pending": {rec["id"]: rec},
            "receipts": {"base:" + "0x" + "ab" * 32 + ":0": dict(GOOD_RECEIPT)},
            "owned": ["chapter2"], "tip_total": "0", "unready": [["solana", "USDC"]]}


def test_state_schema_accepts_a_good_snapshot():
    assert state_schema.check(_snapshot()) == []


@pytest.mark.parametrize("mutate, problem", [
    (lambda s: s.pop("owned"), "snapshot: missing 'owned'"),
    (lambda s: s.update(extra=1), "snapshot: unknown 'extra'"),
    (lambda s: s.update(tip_total=0), "tip_total: expected str"),
    (lambda s: s.update(unready=[["solana"]]), "unready[0]"),
    (lambda s: s.update(owned=["b", "a"]), "owned: not sorted"),
    (lambda s: _pending(s).pop("nonce"), "missing 'nonce'"),
    (lambda s: _pending(s).update(nonce="5"), "nonce: expected int"),
    (lambda s: _pending(s).update(nonce=True), "nonce: expected int"),
    (lambda s: _pending(s).update(state="paid"), "state: expected one of"),
    (lambda s: _pending(s).update(bogus=1), "unknown 'bogus'"),
    (lambda s: _pending(s).update(reference=7), "reference: expected str"),
    (lambda s: _pending(s).update(created_at=1.5), None),
    (lambda s: _pending(s).update(hint_at=None), None),
    (lambda s: _pending(s)["match"].pop("payer"), "match: missing 'payer'"),
    (lambda s: _pending(s)["match"].pop("block_hash"), None),
    (lambda s: _pending(s).update(match=None), None),
    (lambda s: _pending(s).update(match="x"), "match: expected object or null"),
    (lambda s: _pending(s).update(id="ps_other"), "id does not match its key"),
    (lambda s: _receipt(s).update(kind="refund"), "kind: expected one of"),
    (lambda s: _receipt(s).update(log_index=1), "key does not match"),
    (lambda s: _receipt(s).update(item=None), None),
])
def test_state_schema_reports_problems(mutate, problem):
    snap = copy.deepcopy(_snapshot())
    mutate(snap)
    problems = state_schema.check(snap)
    if problem is None:
        assert problems == []
    else:
        assert problems and any(problem in p for p in problems), problems


def _pending(snap):
    return next(iter(snap["pending"].values()))


def _receipt(snap):
    return next(iter(snap["receipts"].values()))
