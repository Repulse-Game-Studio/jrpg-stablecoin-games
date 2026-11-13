"""scfg_core.ints.int_from: CPython's 4300-digit limit on int(str), enforced by the core itself so
that it holds on an interpreter without it (older embedded Pythons, such as some Ren'Py builds).
The `interpreter` fixture (conftest.py) runs each test both ways."""
import time

import pytest

from scfg_core.chains import MAINNET
from scfg_core.ints import INT_MAX_STR_DIGITS, int_from
from scfg_core.reverify import reverify_receipts

from fakes import FakeChain, drive

TX = "0x" + "ab" * 32
DEV = "0x52908400098527886E0F7030069857D2E4169EE7"


def test_the_limit_is_cpythons(interpreter):
    assert INT_MAX_STR_DIGITS == 4300
    assert int_from("9" * 4300) == 10**4300 - 1
    assert int_from(" " + "1_" * 2000 + "1 ") == int("1" * 2001)
    assert int_from(" " * 10_000 + "5") == 5
    assert int_from(7) == 7 and int_from(2.5) == 2 and int_from(True) == 1


@pytest.mark.parametrize("text", ["9" * 4301, "-" + "9" * 4301, "1_" * 4300 + "1", "9" * 1_000_000,
                                  "١" * 4301],
                         ids=["4301", "negative", "underscores", "a million", "arabic-indic"])
def test_more_digits_are_refused_at_once(interpreter, text):
    start = time.perf_counter()
    with pytest.raises(ValueError, match="Exceeds the limit"):
        int_from(text)
    assert time.perf_counter() - start < 0.5


def test_reverify_removes_a_receipt_with_too_many_digits_on_any_interpreter(interpreter):
    receipt = {"chain": "arbitrum", "tx_hash": TX, "log_index": "9" * 4301, "block": 900,
               "kind": "unlock", "item": "chapter2", "amount": "4.993742", "token": "USDC",
               "payer": DEV, "recipient": DEV, "source": "payment"}
    store, errors = {"k": receipt}, []
    removed = drive(reverify_receipts(store, rpc_for=lambda key: FakeChain(head=1000),
                                      chains=MAINNET, wallets=(DEV,), on_error=errors.append))
    assert removed == ["k"] and len(errors) == 1 and "Exceeds the limit" in str(errors[0])
