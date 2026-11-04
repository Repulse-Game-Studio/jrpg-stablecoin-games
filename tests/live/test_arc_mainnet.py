"""Read-only checks against the real Arc mainnet: two pinned historical payments, found
on 2026-11-03, read through the same code the game uses. No key, no funds.

Run with:  uv run pytest -m live      (skipped when Arc's RPCs can't be reached)

The chain is the ledger, so these transactions never change:
  - a native USDC send (a wallet's "Send"): one log, at the system emitter, 18 decimals,
    with sub-micro dust that must be floored away;
  - an ERC-20 transfer() on the 0x3600 interface: the emitter log plus the interface's
    own 6-decimal log, which must count once.
"""
import pytest

from scfg_core.chains import MAINNET
from scfg_core.coio import run_sync
from scfg_core.config import build_config, enabled_chains
from scfg_core.receipts import make_receipt
from scfg_core.restore import lookup_evm
from scfg_core.reverify import check_evm_receipt
from scfg_core.rpc import RpcClient, RpcError, RpcUnavailable, UrllibTransport
from scfg_core.sessions import create_session
from scfg_core.watcher import _scan

pytestmark = pytest.mark.live

NATIVE_SEND = {
    "tx": "0x793ffdc20ca0d12f366c9820c79fefd92e6d7cab174eade40dd28d11ba1456c7",
    "block": 29336763, "log_index": 28,
    "to": "0xBaeD383EDE0e5d9d72430661f3285DAa77E9439F",
    "units": 98_899_999_579,   # logged as 98899999579979000000000 (18 decimals)
}
TRANSFER = {
    "tx": "0xedd1945e984bf7af72763def0dde49ce79bc52cbfc310b748df30f5ec6fb623b",
    "block": 29336630, "log_index": 1,   # the 0x3600 duplicate is log 2
    "to": "0x60f0ae22dBd05BD5C1804A22bb8ED07Fc00aDB77",
    "units": 91_144,
}


class Live:
    """The real RpcClient over the real transport; cooperative calls run to completion."""

    def __init__(self):
        self.client = RpcClient(MAINNET["arc"].rpc, log_range=MAINNET["arc"].max_log_range)
        self.transport = UrllibTransport()

    def run(self, gen):
        return run_sync(gen, self.transport)


@pytest.fixture(scope="module")
def rpc():
    live = Live()
    try:
        assert int(live.run(live.client.call("eth_chainId", [])), 16) == 5042
    except (RpcUnavailable, RpcError) as e:
        pytest.skip(f"Arc mainnet unreachable: {e}")
    return live


def arc_for(wallet):
    config = build_config(wallet, chains=["arc"])
    return config, enabled_chains(config)["arc"]


def run(rpc, gen):
    return rpc.run(gen)


@pytest.mark.parametrize("pinned", [NATIVE_SEND, TRANSFER], ids=["native_send", "transfer"])
def test_live_restore_counts_each_payment_once(rpc, pinned):
    config, chain = arc_for(pinned["to"])
    out = run(rpc, lookup_evm(pinned["tx"], config=config, chain=chain, rpc=rpc.client,
                              receipts={}))
    assert len(out) == 1
    t = out[0].transfer
    assert (out[0].token, t.value, t.log_index, t.block_number, t.recipient) == (
        "USDC", pinned["units"], pinned["log_index"], pinned["block"], pinned["to"])


@pytest.mark.parametrize("pinned", [NATIVE_SEND, TRANSFER], ids=["native_send", "transfer"])
def test_live_reverify_keeps_real_receipts(rpc, pinned):
    config, chain = arc_for(pinned["to"])
    (c,) = run(rpc, lookup_evm(pinned["tx"], config=config, chain=chain, rpc=rpc.client,
                               receipts={}))
    receipt = make_receipt(chain="arc", transfer=c.transfer, kind="tip", item=None,
                           token="USDC", source="restore")
    assert run(rpc, check_evm_receipt(receipt, rpc=rpc.client, chain=chain,
                                      wallets=(pinned["to"],))) == "keep"
    forged = dict(receipt, amount="0.091145")
    assert run(rpc, check_evm_receipt(forged, rpc=rpc.client, chain=chain,
                                      wallets=(pinned["to"],))) == "drop"


def test_live_scan_finds_transfer_by_exact_amount(rpc):
    _, chain = arc_for(TRANSFER["to"])
    pending = {}
    # 0.091144 reads as price 0.09, item code 11, nonce 44.
    rec = create_session(pending, kind="unlock", item="pinned", title="Pinned", token="USDC",
                         chain="arc", price="0.09", code=11, nonce=44,
                         start_block=TRANSFER["block"], now=0.0)
    assert rec["units"] == TRANSFER["units"]
    found = run(rpc, _scan(rec, rpc.client, chain, TRANSFER["to"], {}, TRANSFER["block"]))
    assert found is True
    assert (rec["match"]["tx_hash"], rec["match"]["log_index"]) == (
        TRANSFER["tx"], TRANSFER["log_index"])
