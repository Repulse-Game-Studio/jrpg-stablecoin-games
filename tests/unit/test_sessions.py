import json
from decimal import Decimal

import pytest

from cryptopay_core.sessions import (
    HINT_TIMEOUT_S,
    SCAN_HORIZON_S,
    SESSION_WINDOW_S,
    clear_hint,
    create_session,
    get_session,
    is_past_horizon,
    is_past_window,
    list_sessions,
    normalize_tx_hash,
    remove_session,
    session_price,
    set_hint,
    set_ui_status,
)

TX = "0x" + "AB" * 32


def make(store, **kw):
    args = dict(kind="unlock", item="chapter2", title="Chapter 2", token="USDC",
                chain="arbitrum", price="4.99", code=37, start_block=1000, now=500.0, nonce=42)
    args.update(kw)
    return create_session(store, **args)


def test_constants():
    assert SESSION_WINDOW_S == 1800 and SCAN_HORIZON_S == 86400 and HINT_TIMEOUT_S == 1800


def test_create_session_record_shape():
    store = {}
    rec = make(store)
    assert store[rec["id"]] is rec
    assert rec["id"].startswith("ps_")
    assert rec["units"] == 4_993_742
    assert rec["price"] == "4.99"
    assert rec["cursor"] == rec["start_block"] == 1000
    assert rec["state"] == "awaiting" and rec["ui_status"] == "open"
    assert rec["hint_tx"] is None and rec["match"] is None


def test_record_contains_only_plain_types():
    # persistent is pickled by Ren'Py: plain types survive plugin upgrades, custom classes may not
    rec = make({})
    assert json.loads(json.dumps(rec)) == rec


def test_random_nonce_and_unique_ids():
    store = {}
    recs = [make(store, nonce=None) for _ in range(50)]
    assert len(store) == 50
    assert all(0 <= r["nonce"] <= 99 for r in recs)
    assert len({r["nonce"] for r in recs}) > 1


def test_create_rejects_bad_kind_and_price():
    with pytest.raises(ValueError):
        make({}, kind="refund")
    with pytest.raises(ValueError):
        make({}, price="4.999")
    with pytest.raises(TypeError):
        make({}, price=4.99)


def test_tip_session_encodes_code_zero():
    rec = make({}, kind="tip", item=None, code=0, price="3", nonce=7)
    assert rec["units"] == 3_000_007
    assert session_price(rec) == Decimal("3.00")


def test_get_remove_list():
    store = {}
    a = make(store, now=10.0)
    b = make(store, now=5.0)
    assert get_session(store, a["id"]) is a
    assert get_session(store, "nope") is None
    assert [r["id"] for r in list_sessions(store)] == [b["id"], a["id"]]
    remove_session(store, a["id"])
    remove_session(store, a["id"])  # idempotent
    assert list(store) == [b["id"]]


def test_hints():
    store = {}
    rec = make(store)
    set_hint(store, rec["id"], "  " + TX + " ", now=600.0)
    assert rec["hint_tx"] == TX.lower() and rec["hint_at"] == 600.0
    clear_hint(rec)
    assert rec["hint_tx"] is None and rec["hint_at"] is None


@pytest.mark.parametrize("bad", ["", "0x1234", "0x" + "zz" * 32, "ab" * 32, None])
def test_bad_tx_hash(bad):
    with pytest.raises(ValueError):
        normalize_tx_hash(bad)


def test_set_hint_unknown_session():
    with pytest.raises(KeyError):
        set_hint({}, "ps_missing", TX, 0.0)


def test_ui_status():
    store = {}
    rec = make(store)
    set_ui_status(store, rec["id"], "cancelled")
    assert rec["ui_status"] == "cancelled"
    with pytest.raises(ValueError):
        set_ui_status(store, rec["id"], "paid")


def test_window_and_horizon():
    rec = make({}, now=0.0)
    assert not is_past_window(rec, SESSION_WINDOW_S - 1)
    assert is_past_window(rec, SESSION_WINDOW_S)
    assert not is_past_horizon(rec, SCAN_HORIZON_S - 1)
    assert is_past_horizon(rec, SCAN_HORIZON_S)
