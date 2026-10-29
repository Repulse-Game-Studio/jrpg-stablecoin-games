import pytest

from scfg_core.backends import backend_for, backend_for_family, normalize_any_tx
from scfg_core.backends.evm import START_BLOCK_MARGIN
from scfg_core.chains import MAINNET
from scfg_core.config import add_item, build_config
from scfg_core.engine import Engine
from scfg_core.evm import TransferLog
from scfg_core.receipts import add_receipt, make_receipt
from scfg_core.reverify import reverify_receipts
from scfg_core.sessions import create_session

from fakes import FakeChain, drive, transfer_log

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
CHAIN = MAINNET["arbitrum"]
USDC = CHAIN.tokens["USDC"].address
TX = "0x" + "ab" * 32


def test_builtin_chains_are_evm():
    assert {c.family for c in MAINNET.values() if c.key != "solana"} == {"evm"}
    assert backend_for(CHAIN).family == "evm"


def test_unknown_family_raises():
    with pytest.raises(ValueError):
        backend_for_family("tron")


def test_normalize_any_tx_accepts_evm_and_rejects_garbage():
    assert normalize_any_tx(" 0x" + "AB" * 32 + " ") == "0x" + "ab" * 32
    with pytest.raises(ValueError):
        normalize_any_tx("hello")


@pytest.mark.parametrize("head, expected", [(1000, 1000 - START_BLOCK_MARGIN), (2, 0)])
def test_evm_start_notes_block_with_margin(head, expected):
    fields = drive(backend_for(CHAIN).start(rpc=FakeChain(head=head), chain=CHAIN, wallet=DEV,
                                            token="USDC"))
    assert fields == {"start_block": expected}


def test_create_session_merges_extra_fields_and_refuses_collisions():
    store = {}
    rec = create_session(store, kind="tip", item=None, title="Tip", token="USDC", chain="solana",
                         price="1.00", code=0, start_block=10, now=0.0, extra={"reference": "R"})
    assert rec["reference"] == "R" and rec["cursor"] == 10
    with pytest.raises(ValueError):
        create_session(store, kind="tip", item=None, title="Tip", token="USDC", chain="x",
                       price="1.00", code=0, start_block=10, now=0.0, extra={"units": 1})


def _receipt_store():
    store = {}
    add_receipt(store, make_receipt(
        chain="arbitrum", transfer=TransferLog(USDC, PAYER, DEV, 4_993_742, TX, 2, 900, "0xbh"),
        kind="tip", item=None, token="USDC", source="payment"))
    return store


def test_reverify_accepts_wallets_per_family():
    store = _receipt_store()
    fake = FakeChain(head=1000)
    fake.logs.append(transfer_log(USDC, PAYER, DEV, 4_993_742, block=900, tx_hash=TX, log_index=2))
    removed = drive(reverify_receipts(store, rpc_for=lambda k: fake, chains={"arbitrum": CHAIN},
                                      wallets={"evm": (DEV,)}))
    assert removed == [] and len(store) == 1


def test_reverify_keeps_receipts_of_a_family_without_wallets():
    """A game that dropped EVM (Solana-only now) can't judge old EVM receipts: keep them."""
    store = _receipt_store()
    fake = FakeChain(head=1000)               # no logs at all: would revoke if checked
    removed = drive(reverify_receipts(store, rpc_for=lambda k: fake, chains={"arbitrum": CHAIN},
                                      wallets={"evm": ()}))
    assert removed == [] and len(store) == 1
    assert "get_logs" not in fake.calls


def test_engine_report_hint_uses_the_chain_backend():
    cfg = build_config(DEV)
    add_item(cfg, "ch2", code=37, price="4.99", token="USDC", title="Chapter 2")
    e = Engine(cfg, rpc_factory=lambda chain: FakeChain(head=1000))
    rec = e.start_session("unlock", "arbitrum", item="ch2")
    e.report_hint(rec["id"], "0x" + "CD" * 32)
    assert e.pending[rec["id"]]["hint_tx"] == "0x" + "cd" * 32
    with pytest.raises(ValueError):
        e.report_hint(rec["id"], "not-a-hash")
