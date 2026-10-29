import json
from decimal import Decimal

import pytest

from scfg_core.evm import TransferLog
from scfg_core.models import Item
from scfg_core.receipts import (
    add_receipt,
    compute_ownership,
    make_receipt,
    receipt_key,
    remove_receipt,
)

USDC = "0xaf88d065e77c8cC2239327C5EDb3A432268e5831"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
ITEMS = {
    "chapter2": Item("chapter2", 37, Decimal("4.99"), "USDC", "Chapter 2", Decimal("4.99")),
    "ost": Item("ost", 2, Decimal("2.00"), "USDT", "OST", Decimal("1.50")),
}


def transfer(value=4_993_742, tx="0x" + "ab" * 32, log_index=3, block=777):
    return TransferLog(USDC, PAYER, DEV, value, tx, log_index, block, "0x" + "11" * 32)


def receipt(kind="unlock", item="chapter2", **kw):
    return make_receipt(chain="arbitrum", transfer=transfer(**kw), kind=kind, item=item,
                        token="USDC", source="payment")


def test_receipt_key():
    assert receipt_key("base", "0xABC", 4) == "base:0xabc:4"


def test_make_receipt_shape():
    r = receipt()
    assert r == {
        "chain": "arbitrum", "tx_hash": "0x" + "ab" * 32, "log_index": 3, "block": 777,
        "kind": "unlock", "item": "chapter2", "amount": "4.993742", "token": "USDC",
        "payer": PAYER, "recipient": DEV, "source": "payment",
    }
    assert json.loads(json.dumps(r)) == r


def test_make_receipt_validates():
    with pytest.raises(ValueError):
        make_receipt(chain="base", transfer=transfer(), kind="gift", item=None, token="USDC",
                     source="payment")
    with pytest.raises(ValueError):
        make_receipt(chain="base", transfer=transfer(), kind="tip", item=None, token="USDC",
                     source="magic")


def test_add_receipt_dedupes_on_key():
    store = {}
    assert add_receipt(store, receipt()) is True
    assert add_receipt(store, receipt()) is False
    assert list(store) == ["arbitrum:0x" + "ab" * 32 + ":3"]
    remove_receipt(store, list(store)[0])
    remove_receipt(store, "missing")  # idempotent
    assert store == {}


def test_ownership_unlock_and_tips():
    store = {}
    add_receipt(store, receipt())
    add_receipt(store, receipt(kind="tip", item=None, value=3_000_005, log_index=4))
    add_receipt(store, receipt(kind="tip", item="tip_jar", value=1_000_000, log_index=5))
    owned, tips = compute_ownership(store, ITEMS)
    assert owned == {"chapter2"}
    assert tips == Decimal("4.000005")


def test_ownership_requires_min_price():
    store = {}
    add_receipt(store, receipt(item="ost", value=1_490_000))
    assert compute_ownership(store, ITEMS)[0] == set()
    add_receipt(store, receipt(item="ost", value=1_500_000, log_index=9))
    assert compute_ownership(store, ITEMS)[0] == {"ost"}


def test_ownership_ignores_unknown_items():
    store = {}
    add_receipt(store, receipt(item="deleted_dlc"))
    assert compute_ownership(store, ITEMS) == (set(), Decimal(0))


@pytest.mark.parametrize("junk", [
    {},
    {"kind": "unlock"},
    {"kind": "unlock", "item": "chapter2", "amount": "lots"},
    {"kind": "unlock", "item": "chapter2", "amount": None},
    {"kind": "tip", "amount": "NaN"},
    {"kind": "weird", "amount": "1"},
])
def test_ownership_skips_malformed_entries(junk):
    store = {"junk": junk}
    add_receipt(store, receipt())
    assert compute_ownership(store, ITEMS) == ({"chapter2"}, Decimal(0))
