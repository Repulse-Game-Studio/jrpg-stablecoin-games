import pytest
from solders.keypair import Keypair

from scfg_core.backends.solana import START_SLOT_MARGIN, SolanaBackend
from scfg_core.chains import MAINNET
from scfg_core.config import add_item, build_config
from scfg_core.receipts import make_receipt, receipt_key
from scfg_core.restore import RestoreError, lookup
from scfg_core.rpc import RpcClient
from scfg_core.solana.base58 import b58decode, b58encode
from scfg_core.solana.pda import associated_token_address
from scfg_core.solana.rpc import SolanaRpc
from scfg_core.transfers import Transfer

from fakes import FakeChain, FakeSolanaChain, drive

CHAIN = MAINNET["solana"]
MINT = CHAIN.tokens["USDC"].address
DEV = str(Keypair().pubkey())
OLD = str(Keypair().pubkey())
PAYER = str(Keypair().pubkey())
B = SolanaBackend()


def sig(n):
    return b58encode(n.to_bytes(64, "big"))


def cfg():
    c = build_config(solana_wallet=DEV, previous_wallets=[OLD])
    add_item(c, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    return c


def test_make_rpc_wraps_the_failover_client():
    rpc = B.make_rpc(CHAIN)
    assert isinstance(rpc, SolanaRpc) and isinstance(rpc.client, RpcClient)
    assert rpc.client.urls == CHAIN.rpc


def test_start_fields():
    chain = FakeSolanaChain(slot=1000)
    dest = associated_token_address(DEV, MINT)
    chain.addresses[dest] = [{"signature": sig(9), "slot": 999, "err": None}]
    f = drive(B.start(rpc=chain, chain=CHAIN, wallet=DEV, token="USDC"))
    assert f["start_block"] == 1000 - START_SLOT_MARGIN and f["dest"] == dest
    assert f["scan_cursor"] == sig(9) and len(b58decode(f["reference"])) == 32
    assert f["scan_before"] is None
    assert drive(B.start(rpc=chain, chain=CHAIN, wallet=DEV, token="USDC"))["reference"] != f["reference"]
    assert drive(B.start(rpc=FakeSolanaChain(slot=10), chain=CHAIN, wallet=OLD,
                         token="USDC"))["scan_cursor"] is None


def receipt_for(tx_sig, *, value=4_993_742, slot=950, recipient=DEV, index=2):
    return make_receipt(chain="solana", transfer=Transfer(MINT, PAYER, recipient, value, tx_sig,
                                                          index, slot),
                        kind="unlock", item="chapter2", token="USDC", source="payment")


def verdict(chain, r, wallets=(DEV, OLD)):
    return drive(B.reverify(r, rpc=chain, chain=CHAIN, wallets=wallets))


def test_reverify_keeps_genuine_and_previous_wallet_receipts():
    chain = FakeSolanaChain()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=4_993_742, slot=950)
    chain.pay(sig(2), mint=MINT, payer=PAYER, owner=OLD, amount=4_993_742, slot=950)
    assert verdict(chain, receipt_for(sig(1))) == "keep"
    assert verdict(chain, receipt_for(sig(2), recipient=OLD)) == "keep"


@pytest.mark.parametrize("receipt_kw, pay_kw", [
    (dict(value=9_000_000), {}),                          # amount differs
    (dict(slot=951), {}),                                 # slot differs
    (dict(recipient=PAYER), {}),                          # not paid to the developer
    ({}, dict(err={"InstructionError": [0, "Custom"]})),  # failed transaction
])
def test_reverify_drops_contradicted_receipts(receipt_kw, pay_kw):
    chain = FakeSolanaChain()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=4_993_742, slot=950, **pay_kw)
    assert verdict(chain, receipt_for(sig(1), **receipt_kw)) == "drop"


def test_invented_signature_is_dropped_when_history_covers_it():
    chain = FakeSolanaChain()
    chain.first_available = 0
    assert verdict(chain, receipt_for(sig(1))) == "drop"


def test_old_receipt_on_a_pruned_endpoint_is_kept():
    """Review focus 5: publicnode keeps ~2 days; a null from it proves nothing."""
    chain = FakeSolanaChain()
    chain.first_available = 10**9
    assert verdict(chain, receipt_for(sig(1))) == "keep"


def test_outage_keeps_and_far_future_slot_drops():
    chain = FakeSolanaChain(slot=1000)
    chain.down = True
    assert verdict(chain, receipt_for(sig(1))) == "keep"
    chain.down = False
    assert verdict(chain, receipt_for(sig(1), slot=1000 + 20_000)) == "drop"
    chain.pay(sig(3), mint=MINT, payer=PAYER, owner=DEV, amount=4_993_742, slot=1005)
    assert verdict(chain, receipt_for(sig(3), slot=1005)) == "keep"  # lagging head


def test_lookup_classifies_by_amount_code():
    chain = FakeSolanaChain()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=4_993_742, slot=950)
    [c] = drive(B.lookup(sig(1), config=cfg(), chain=CHAIN, rpc=chain, receipts={}))
    assert (c.chain, c.token, c.kind, c.item, c.already_restored) == (
        "solana", "USDC", "unlock", "chapter2", False)
    assert c.transfer.value == 4_993_742 and c.transfer.log_index == 2
    receipts = {receipt_key("solana", sig(1), 2): {}}
    assert drive(B.lookup(sig(1), config=cfg(), chain=CHAIN, rpc=chain,
                          receipts=receipts))[0].already_restored


def test_lookup_outcomes():
    chain = FakeSolanaChain()
    assert drive(B.lookup(sig(1), config=cfg(), chain=CHAIN, rpc=chain, receipts={})) is None
    chain.pay(sig(2), mint=MINT, payer=PAYER, owner=DEV, amount=1, err={"x": 1})
    with pytest.raises(RestoreError, match="tx_reverted"):
        drive(B.lookup(sig(2), config=cfg(), chain=CHAIN, rpc=chain, receipts={}))
    chain.pay(sig(3), mint=MINT, payer=DEV, owner=PAYER, amount=1_000_000)
    with pytest.raises(RestoreError, match="tx_not_a_payment"):
        drive(B.lookup(sig(3), config=cfg(), chain=CHAIN, rpc=chain, receipts={}))


def test_restore_routes_signatures_to_solana_and_hashes_to_evm():
    sol = FakeSolanaChain()
    sol.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=4_993_742)
    evm = FakeChain()
    chains = {"arbitrum": MAINNET["arbitrum"], "solana": CHAIN}
    rpcs = {"arbitrum": evm, "solana": sol}
    out = drive(lookup(sig(1), config=cfg(), chains=chains, rpc_for=rpcs.get, receipts={}))
    assert out[0].chain == "solana" and evm.calls == []
    with pytest.raises(RestoreError, match="tx_not_found"):
        drive(lookup("0x" + "ab" * 32, config=cfg(), chains=chains, rpc_for=rpcs.get, receipts={}))
    assert "get_transaction_any" not in sol.calls[1:]


def test_check_ready():
    chain = FakeSolanaChain()
    chain.accounts.add(associated_token_address(DEV, MINT))
    assert drive(B.check_ready(rpc=chain, chain=CHAIN, wallet=DEV)) == {"USDC": True, "USDT": False}


@pytest.mark.parametrize("payload", [
    {"meta": {"err": None, "postTokenBalances": [{"mint": MINT, "accountIndex": 2}]}},
    {"meta": {"err": None, "postTokenBalances": [
        {"mint": MINT, "accountIndex": 2, "owner": DEV, "uiTokenAmount": {"amount": "x"}}]}},
    {"meta": {"err": None, "postTokenBalances": "garbage"}},
])
def test_reverify_keeps_a_receipt_whose_transaction_it_cannot_read(payload):
    chain = FakeSolanaChain()
    chain.txs[sig(1)] = dict(payload, slot=950)
    assert verdict(chain, receipt_for(sig(1))) == "keep"
