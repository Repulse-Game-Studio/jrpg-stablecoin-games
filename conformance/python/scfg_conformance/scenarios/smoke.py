"""Smoke scenarios: one paid unlock per chain family, the shortest happy path."""

from __future__ import annotations

from ..scenario import scenario
from .fixtures import CHAPTER2, DEV_EVM, DEV_SOL, PAYER_EVM, PAYER_SOL, signature, tx_hash


@scenario("smoke/evm_unlock_paid", covers=[])
def _evm_unlock_paid(s):
    s.config(wallet=DEV_EVM, testnet=True, chains=["base"])
    s.item(**CHAPTER2)
    node = s.evm("base")
    usdc = s.chain("base").tokens["USDC"].address
    rec = s.start_session("unlock", "base", item="chapter2")
    s.advance(12)
    node.mine(1)
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=node.head, tx_hash=tx_hash(1))
    for _ in range(10):
        [out] = s.tick()
        if out.status == "paid":
            break
        assert out.status == "confirming", out
        s.advance(2)
        node.mine(1)
    assert out.status == "paid" and out.result.tx_hash == tx_hash(1)


@scenario("smoke/solana_unlock_paid", covers=[])
def _solana_unlock_paid(s):
    s.config(solana_wallet=DEV_SOL, testnet=True)
    s.item(**CHAPTER2)
    node = s.solana()
    mint = s.chain("solana").tokens["USDC"].address
    rec = s.start_session("unlock", "solana", item="chapter2")
    s.advance(5)
    node.slot += 10
    node.pay(signature(1), mint=mint, payer=PAYER_SOL, owner=DEV_SOL, amount=rec["units"],
             addresses=[rec["reference"], rec["dest"]])
    [out] = s.tick()
    assert out.status == "confirming", out
    s.advance(13)
    node.slot += 32
    node.finalize(signature(1))
    [out] = s.tick()
    assert out.status == "paid" and out.result.tx_hash == signature(1), out
