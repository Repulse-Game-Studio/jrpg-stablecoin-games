"""Configuration overrides as the Engine applies them (developer RPC URLs first,
confirmation overrides)."""

from __future__ import annotations

from ..scenario import scenario
from .fixtures import CHAPTER2, DEV_EVM, DEV_SOL, PAYER_EVM, PAYER_SOL, signature, tx_hash


@scenario("config/rpc_and_confirmation_overrides", covers=[
    "tests/unit/test_config.py::test_enabled_chains_respects_order_tokens_and_overrides",
    "tests/unit/test_config_solana.py::test_solana_rpc_override_and_testnet_tokens",
    "tests/unit/test_custom_chains.py::test_custom_chain_only_and_overrides",
])
def _rpc_and_confirmation_overrides(s):
    s.config(wallet=DEV_EVM, solana_wallet=DEV_SOL, testnet=True, chains=["base", "solana"],
             rpc_urls={"base": ["https://my-base.example"], "solana": "https://my-node.example"},
             confirmations={"base": 1})
    s.item(**CHAPTER2)
    assert s.chain("base").rpc[0] == "https://my-base.example"
    evm = s.evm("base")
    sol = s.solana()
    rec = s.start_session("unlock", "base", item="chapter2")
    assert {c["url"] for c in s.steps[-1]["calls"]} == {"https://my-base.example"}
    evm.mine(1)
    evm.add_transfer(s.chain("base").tokens["USDC"].address, PAYER_EVM, DEV_EVM, rec["units"],
                     block=evm.head, tx_hash=tx_hash(1))
    s.advance(5)
    [paid] = s.tick()                         # one confirmation is enough here
    assert (paid.status, paid.required, paid.confirmations) == ("paid", 1, 1)

    tip = s.start_session("tip", "solana", amount="1", token="USDC")
    assert {c["url"] for c in s.steps[-1]["calls"]} == {"https://my-node.example"}
    sol.pay(signature(1), mint=s.chain("solana").tokens["USDC"].address, payer=PAYER_SOL,
            owner=DEV_SOL, amount=tip["units"], addresses=[tip["reference"], tip["dest"]],
            status="finalized")
    s.advance(5)
    [paid] = s.tick()
    assert paid.status == "paid" and paid.required == 32
    s.start_session("unlock", "solana", item="chapter2")
    s.start_session("tip", "solana", amount="1", token="USDT")   # no USDT on devnet
    assert s.steps[-1]["raises"] == {"type": "ConfigError", "code": "token.unavailable"}, \
        s.steps[-1]["raises"]
