
import time
from decimal import Decimal

import pytest

from scfg_core.chains import MAINNET
from scfg_core.evm import TransferLog
from scfg_core.models import Item
from scfg_core.receipts import add_receipt, compute_ownership, make_receipt
from scfg_core.reverify import FUTURE_TOLERANCE_BLOCKS, reverify_receipts

from fakes import FakeChain, drive, transfer_log

ARB = MAINNET["arbitrum"]
USDC = ARB.tokens["USDC"].address
DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
PAYER = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"
TX = "0x" + "ab" * 32
CHAPTER2 = Item("chapter2", 37, Decimal("4.99"), "USDC", "Chapter 2", Decimal("4.99"))


def store_with(value=4_993_742, block=900, recipient=DEV, token="USDC", chain="arbitrum", log_index=2):
    store = {}
    t = TransferLog(USDC, PAYER, recipient, value, TX, log_index, block, "0x" + "11" * 32)
    add_receipt(store, make_receipt(chain=chain, transfer=t, kind="unlock", item="chapter2",
                                    token=token, source="payment"))
    return store


def run(store, chain, wallets=(DEV,), on_error=None):
    return drive(reverify_receipts(store, rpc_for=lambda key: chain, chains=MAINNET,
                                   wallets=wallets, on_error=on_error))


def test_genuine_receipt_is_kept():
    store, chain = store_with(), FakeChain(head=1000)
    chain.logs.append(transfer_log(USDC, PAYER, DEV, 4_993_742, block=900, tx_hash=TX, log_index=2))
    assert run(store, chain) == [] and len(store) == 1


def test_lookup_is_scoped_to_the_receipt_block():
    store, chain = store_with(), FakeChain(head=1000)
    run(store, chain)
    address, topics, lo, hi = chain.last_get_logs
    assert (address, lo, hi) == (USDC, 900, 900)
    assert topics[2] == ["0x" + "0" * 24 + DEV[2:].lower()]


@pytest.mark.parametrize("log_kwargs", [
    dict(value=4_993_743),                 # amount differs
    dict(log_index=3),                     # different log in same tx
    dict(tx_hash="0x" + "cd" * 32),        # different tx
])
def test_mismatching_log_revokes(log_kwargs):
    store, chain = store_with(), FakeChain(head=1000)
    args = dict(value=4_993_742, block=900, tx_hash=TX, log_index=2)
    args.update(log_kwargs)
    chain.logs.append(transfer_log(USDC, PAYER, DEV, args.pop("value"), **args))
    assert len(run(store, chain)) == 1 and store == {}


def test_invented_receipt_with_no_log_is_revoked():
    store, chain = store_with(), FakeChain(head=1000)
    assert len(run(store, chain)) == 1


def test_outage_keeps_everything():
    store, chain = store_with(), FakeChain(head=1000)
    chain.down = True
    assert run(store, chain) == [] and len(store) == 1


def test_block_slightly_ahead_of_lagging_head_is_kept():
    store, chain = store_with(block=1005), FakeChain(head=1000)
    assert run(store, chain) == [] and len(store) == 1


def test_impossible_future_block_is_revoked():
    store, chain = store_with(block=1000 + FUTURE_TOLERANCE_BLOCKS + 1), FakeChain(head=1000)
    assert len(run(store, chain)) == 1


def test_previous_wallet_payments_survive_wallet_change():
    store, chain = store_with(recipient=OLD), FakeChain(head=1000)
    chain.logs.append(transfer_log(USDC, PAYER, OLD, 4_993_742, block=900, tx_hash=TX, log_index=2))
    assert run(store, chain, wallets=(DEV, OLD)) == []
    assert run(store, chain, wallets=(DEV,)) != []


def test_unknown_chain_is_left_alone():
    store, chain = store_with(chain="tron"), FakeChain(head=1000)
    assert run(store, chain) == [] and len(store) == 1


def test_token_not_on_chain_is_revoked():
    store, chain = store_with(chain="base", token="USDT"), FakeChain(head=1000)
    assert len(run(store, chain)) == 1


@pytest.mark.parametrize("junk", [{}, {"chain": "arbitrum"},
                                   {"chain": "arbitrum", "tx_hash": TX, "log_index": "x",
                                    "block": 1, "amount": "1", "token": "USDC"}])
def test_malformed_entries_are_removed(junk):
    store, chain = store_with(), FakeChain(head=1000)
    chain.logs.append(transfer_log(USDC, PAYER, DEV, 4_993_742, block=900, tx_hash=TX, log_index=2))
    store["junk"] = junk
    errors = []
    assert run(store, chain, on_error=errors.append) == ["junk"]
    assert len(store) == 1
    assert len(errors) == 1                                       # removed with a reason


@pytest.mark.parametrize("corrupt", [
    {"amount": "Infinity"},
    {"amount": "1E+999999"},         # out of range (forged): would overflow scaleb
    {"amount": "1E+999993"},         # out of range: a million-digit int, seconds to build
    {"amount": "-1E+999995"},
    {"amount": "1E+41"},
    {"token": ["USDC"]},             # unhashable token name
    {"item": ["chapter2"]},          # unhashable item
    {"chain": ["arbitrum"]},
    {"tx_hash": 5},
    {"log_index": float("inf")},     # int(inf): OverflowError
    {"block": "0x10"},
])
def test_a_receipt_with_invalid_fields_is_removed_and_reported(corrupt):
    """A stored receipt whose own fields are invalid is removed, with the reason reported to
    on_error, whatever chain it names; the receipts after it are still checked. A forged
    receipt cannot escape verification by making its check crash."""
    store, chain = {}, FakeChain(head=1000)
    bad = dict(store_with(log_index=1)[next(iter(store_with(log_index=1)))], **corrupt)
    store["a:corrupt"] = bad
    store.update(store_with(log_index=2))                         # genuine
    store.update(store_with(log_index=3))                         # invented: no log
    chain.logs.append(transfer_log(USDC, PAYER, DEV, 4_993_742, block=900, tx_hash=TX, log_index=2))
    errors = []
    start = time.perf_counter()
    removed = run(store, chain, on_error=errors.append)
    assert time.perf_counter() - start < 2.0                     # generous: no huge int is built
    assert removed == ["a:corrupt", f"arbitrum:{TX}:3"]
    assert list(store) == [f"arbitrum:{TX}:2"]
    assert len(errors) == 1 and isinstance(errors[0], ValueError)
    assert "a:corrupt" in str(errors[0])


@pytest.mark.parametrize("chain, corrupt", [
    ("arbitrum", {"tx_hash": "0x123"}),
    ("arbitrum", {"tx_hash": "0x" + "zz" * 32}),
    ("arbitrum", {"tx_hash": "ab" * 32}),
    ("arbitrum", {"tx_hash": TX + " "}),
    ("arbitrum", {"tx_hash": TX + "\x85"}),           # str.strip() whitespace JS trim() keeps
    ("arbitrum", {"tx_hash": TX + "\x1c"}),
    ("arbitrum", {"tx_hash": "\x1f" + TX}),
    ("arbitrum", {"tx_hash": "0x" + "ab" * 33}),
    ("arbitrum", {"log_index": -1}),
    ("arbitrum", {"log_index": "-1"}),
    ("arbitrum", {"block": -5}),
    ("arbitrum", {"log_index": "9" * 4301}),   # more digits than int() converts (CPython's limit)
    ("arbitrum", {"block": "9" * 100_000}),
    ("solana", {"tx_hash": TX}),                       # an EVM hash on Solana
    ("solana", {"tx_hash": "0OIl" * 22}),              # not base58
    ("solana", {"tx_hash": "1" * 63}),                 # base58, not 64 bytes
    ("solana", {"tx_hash": "2" * 100_000}),            # refused before base58 decoding
    ("solana", {"tx_hash": "1" * 64 + "\x85"}),        # a valid signature, then whitespace
], ids=lambda v: str(v)[:40])
def test_a_type_valid_but_malformed_receipt_is_removed_before_any_rpc(chain, corrupt):
    """Each field is checked against its chain family before any RPC: an EVM hash is 0x and 64
    hex digits, a Solana signature 64 base58 bytes, log index and block are ints >= 0."""
    store, node = {}, FakeChain(head=1000)
    bad = dict(store_with(log_index=1)[next(iter(store_with(log_index=1)))], chain=chain, **corrupt)
    store["bad"] = bad
    errors = []
    assert run(store, node, on_error=errors.append) == ["bad"] and store == {}
    assert len(errors) == 1 and "'bad'" in str(errors[0])
    assert node.calls == []                                     # judged without the network


def test_a_malformed_receipt_on_an_unknown_chain_is_kept():
    """A chain this build does not know (removed from the config) cannot be checked: its receipts
    are kept, as are their family-specific fields."""
    store, node = {}, FakeChain(head=1000)
    store["old"] = dict(store_with()[next(iter(store_with()))], chain="tron", tx_hash="0x123")
    assert run(store, node, on_error=lambda e: None) == [] and len(store) == 1


def test_a_forged_out_of_range_receipt_no_longer_unlocks():
    """The forged "1E+999999" unlock owns nothing, and reverify removes it."""
    store, chain = {}, FakeChain(head=1000)
    forged = dict(store_with()[next(iter(store_with()))], amount="1E+999999")
    store["forged"] = forged
    assert compute_ownership(store, {"chapter2": CHAPTER2})[0] == set()
    assert run(store, chain, on_error=lambda e: None) == ["forged"] and store == {}


def test_a_receipt_whose_check_crashes_is_kept_and_reported():
    """The backend's own check crashing (here on a log whose logIndex is not hex) is the same:
    that receipt is kept and reported, the next one is still judged."""
    store, chain = store_with(log_index=2), FakeChain(head=1000)
    store.update(store_with(log_index=3))
    bad = transfer_log(USDC, PAYER, DEV, 1, block=900, tx_hash=TX)
    bad["logIndex"] = "0xzz"
    chain.logs.append(bad)
    errors = []
    assert run(store, chain, on_error=errors.append) == [] and len(store) == 2
    assert [type(e) for e in errors] == [ValueError, ValueError]


def test_a_report_without_on_error_is_printed(capsys):
    store, chain = store_with(), FakeChain(head=1000)
    store["x"] = dict(store[next(iter(store))], amount="Infinity")
    assert run(store, chain) == [f"arbitrum:{TX}:2", "x"]
    assert "'x'" in capsys.readouterr().err
