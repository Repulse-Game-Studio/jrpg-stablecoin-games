
import pytest

from scfg_core.chains import MAINNET
from scfg_core.evm import TransferLog
from scfg_core.receipts import add_receipt, make_receipt
from scfg_core.reverify import FUTURE_TOLERANCE_BLOCKS, reverify_receipts

from fakes import FakeChain, drive, transfer_log

ARB = MAINNET["arbitrum"]
USDC = ARB.tokens["USDC"].address
DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
PAYER = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"
TX = "0x" + "ab" * 32


def store_with(value=4_993_742, block=900, recipient=DEV, token="USDC", chain="arbitrum", log_index=2):
    store = {}
    t = TransferLog(USDC, PAYER, recipient, value, TX, log_index, block, "0x" + "11" * 32)
    add_receipt(store, make_receipt(chain=chain, transfer=t, kind="unlock", item="chapter2",
                                    token=token, source="payment"))
    return store


def run(store, chain, wallets=(DEV,)):
    return drive(reverify_receipts(store, rpc_for=lambda key: chain, chains=MAINNET,
                                   wallets=wallets))


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
    assert run(store, chain) == ["junk"]
    assert len(store) == 1
