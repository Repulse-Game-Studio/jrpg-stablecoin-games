"""EVM receipts through the Engine: restore, re-verification and the game's chain overrides."""

from __future__ import annotations

from ..scenario import scenario
from .evm import covers
from .fixtures import CHAPTER2, DEV_EVM, OLD_DEV_EVM, PAYER_EVM, tx_hash


@scenario("evm/restore_through_engine", covers=(
    covers("engine", "restore_through_engine")
    + covers("restore", "lookup_finds_chain_and_classifies")))
def _restore_through_engine(s):
    """Restoring from a transaction hash: every chain is asked, the chain that knows the
    transaction yields a candidate classified as the Chapter 2 unlock, and applying it grants
    ownership."""
    s.config(wallet=DEV_EVM)
    s.item(**CHAPTER2)
    nodes = {key: s.evm(key) for key in ("arbitrum", "base", "arc", "polygon", "ethereum")}
    usdc = s.chain("arbitrum").tokens["USDC"].address
    nodes["arbitrum"].add_transfer(usdc, PAYER_EVM, DEV_EVM, 4_993_742, block=10,
                                   tx_hash=tx_hash(1))
    [cand] = s.restore_lookup(tx_hash(1))
    assert (cand.chain, cand.item, cand.already_restored) == ("arbitrum", "chapter2", False)
    s.restore_apply(len(s.steps) - 1)
    assert s.steps[-1]["snapshot"]["owned"] == ["chapter2"]


@scenario("evm/reverify_accepted_wallets", covers=(
    covers("engine", "reverify_uses_accepted_wallets")
    + covers("backends", "reverify_accepts_wallets_per_family")
    + covers("reverify", "previous_wallet_payments_survive_wallet_change")))
def _reverify_accepted_wallets(s):
    """Re-verification accepts a receipt paid to the current or a previous wallet when its
    log is on chain, and revokes one the chain contradicts."""
    s.config(wallet=DEV_EVM, previous_wallets=[OLD_DEV_EVM])
    node, usdc = s.evm("arbitrum"), s.chain("arbitrum").tokens["USDC"].address

    def receipt(n, recipient, units=4_993_742):
        return {"chain": "arbitrum", "tx_hash": tx_hash(n), "log_index": 0, "block": 900,
                "kind": "tip", "item": None, "amount": f"{units / 1e6:.6f}", "token": "USDC",
                "payer": PAYER_EVM, "recipient": recipient, "source": "payment"}

    keep_new, keep_old, gone = receipt(1, DEV_EVM), receipt(2, OLD_DEV_EVM), receipt(3, DEV_EVM)
    s.initial(receipts={f"arbitrum:{tx_hash(n)}:0": r
                        for n, r in ((1, keep_new), (2, keep_old), (3, gone))})
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 4_993_742, block=900, tx_hash=tx_hash(1))
    node.add_transfer(usdc, PAYER_EVM, OLD_DEV_EVM, 4_993_742, block=900, tx_hash=tx_hash(2))
    assert s.reverify() == [f"arbitrum:{tx_hash(3)}:0"]
    assert set(s.steps[-1]["snapshot"]["receipts"]) == {
        f"arbitrum:{tx_hash(1)}:0", f"arbitrum:{tx_hash(2)}:0"}


# --- configuration ---------------------------------------------------------------------------


@scenario("evm/config_overrides_and_full_registry", covers=(
    covers("engine", "chain_applies_config_overrides", "rpc_clients_are_cached_per_chain")))
def _config_overrides(s):
    """A game's rpc_urls and confirmations apply to a built-in chain (its URL is tried first,
    then the defaults), and a token the game disabled still verifies old receipts. A failing
    endpoint is skipped for 30 s, which only happens if the chain's RPC client persists across
    ticks."""
    s.config(wallet=DEV_EVM, tokens=["USDC"], rpc_urls={"arbitrum": ["https://mine.example"]},
             confirmations={"arbitrum": 2})
    s.item(**CHAPTER2)
    chain = s.chain("arbitrum")
    assert chain.rpc[0] == "https://mine.example" and chain.confirmations == 2
    assert set(chain.tokens) == {"USDC", "USDT"}
    mine = s.evm("arbitrum", urls=["https://mine.example"])
    fallback = s.evm("arbitrum", urls=list(chain.rpc[1:]))
    usdt = chain.tokens["USDT"].address
    receipt = {"chain": "arbitrum", "tx_hash": tx_hash(5), "log_index": 0, "block": 900,
               "kind": "tip", "item": None, "amount": "2.000000", "token": "USDT",
               "payer": PAYER_EVM, "recipient": DEV_EVM, "source": "payment"}
    s.initial(receipts={f"arbitrum:{tx_hash(5)}:0": receipt})
    mine.endpoint("https://mine.example").down = True
    fallback.add_transfer(usdt, PAYER_EVM, DEV_EVM, 2_000_000, block=900, tx_hash=tx_hash(5))
    assert s.reverify() == []                     # USDT receipt kept though USDT is disabled
    assert s.steps[-1]["calls"][0]["url"] == "https://mine.example"
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    urls = [c["url"] for c in s.steps[-1]["calls"]]
    assert "https://mine.example" not in urls     # still cooling down: the client persisted
    s.advance(31)
    mine.endpoint("https://mine.example").down = False
    s.tick()
    assert s.steps[-1]["calls"][0]["url"] == "https://mine.example"
    assert rec["id"] in s.steps[-1]["snapshot"]["pending"]
