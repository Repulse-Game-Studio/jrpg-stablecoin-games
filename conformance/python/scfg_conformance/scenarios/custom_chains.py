"""Developer-added EVM chains (scfg.add_chain) through the Engine."""

from __future__ import annotations

from ..scenario import scenario
from .fixtures import AVALANCHE, AVAX_USDC, DEV_EVM, PAYER_EVM, tx_hash

_UNIT = "tests/unit/test_custom_chains.py"


@scenario("custom_chains/pay_restore_reverify",
          covers=[f"{_UNIT}::test_engine_pays_restores_and_reverifies_on_custom_chain",
                  "tests/unit/test_restore.py::test_lookup_marks_already_restored"])
def _pay_restore_reverify(s):
    s.config(wallet=DEV_EVM, chains=["avalanche"])
    s.custom_chain(**AVALANCHE)
    s.item(item_id="ch2", code=37, price="4.99", token="USDC", title="Chapter 2")
    node = s.evm("avalanche")
    rec = s.start_session("unlock", "avalanche", item="ch2")
    assert s.steps[-1]["calls"][0]["url"] == "https://my-node.example/avax"
    node.mine(1)
    node.add_transfer(AVAX_USDC, PAYER_EVM, DEV_EVM, rec["units"], block=node.head,
                      tx_hash=tx_hash(1))
    s.advance(10)
    assert [o.status for o in s.tick()] == ["confirming"]
    node.mine(5)
    s.advance(10)
    [paid] = s.tick()
    assert paid.status == "paid" and paid.result.chain == "avalanche"
    assert s.steps[-1]["snapshot"]["owned"] == ["ch2"]
    assert s.reverify() == []                 # the receipt checks out on its chain
    found = s.restore_lookup(tx_hash(1))
    assert [(c.chain, c.item, c.already_restored) for c in found] == [("avalanche", "ch2", True)]


@scenario("custom_chains/removed_chain_receipt_kept",
          covers=[f"{_UNIT}::test_receipts_from_a_removed_custom_chain_are_kept",
                  "tests/unit/test_reverify.py::test_unknown_chain_is_left_alone"])
def _removed_chain_receipt_kept(s):
    """A later build without the chain can't judge the receipt, so it keeps it."""
    s.config(wallet=DEV_EVM)
    receipt = {"chain": "avalanche", "tx_hash": tx_hash(1), "log_index": 0, "block": 5,
               "kind": "tip", "item": None, "amount": "1.000000", "token": "USDC",
               "payer": PAYER_EVM, "recipient": DEV_EVM, "source": "payment"}
    s.initial(receipts={f"avalanche:{tx_hash(1)}:0": receipt})
    assert s.reverify() == []
    assert s.steps[-1]["calls"] == [] and len(s.steps[-1]["snapshot"]["receipts"]) == 1
