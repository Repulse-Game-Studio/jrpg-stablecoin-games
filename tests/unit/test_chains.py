import pytest

from scfg_core.chains import DISPLAY_ORDER, MAINNET, TESTNET, get_chains
from scfg_core.evm import is_checksum_address

# Pinned from Circle's and Tether's official contract pages. If one of these
# changes, the registry must change deliberately, never by accident.
PINNED = {
    ("ethereum", "USDC"): "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48",
    ("ethereum", "USDT"): "0xdAC17F958D2ee523a2206206994597C13D831ec7",
    ("arbitrum", "USDC"): "0xaf88d065e77c8cC2239327C5EDb3A432268e5831",
    ("arbitrum", "USDT"): "0xFd086bC7CD5C481DCC9C85ebE478A1C0b69FCbb9",
    ("polygon", "USDC"): "0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359",
    ("polygon", "USDT"): "0xc2132D05D31c914a87C6611C10748AEb04B58e8F",
    ("base", "USDC"): "0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913",
}


# The EVM-only invariants below must not see Solana entries (chain_id 0, base58 mints).
EVM = {k: c for k, c in MAINNET.items() if c.family == "evm"}
EVM_TEST = {k: c for k, c in TESTNET.items() if c.family == "evm"}


def test_mainnet_chain_ids():
    assert {k: c.chain_id for k, c in EVM.items()} == {
        "ethereum": 1, "arbitrum": 42161, "polygon": 137, "base": 8453}


def test_testnet_chain_ids_use_same_keys():
    assert {k: c.chain_id for k, c in EVM_TEST.items()} == {
        "ethereum": 11155111, "arbitrum": 421614, "polygon": 80002, "base": 84532}


@pytest.mark.parametrize("key_token, address", PINNED.items())
def test_pinned_mainnet_tokens(key_token, address):
    key, token = key_token
    assert MAINNET[key].tokens[token].address == address


def test_base_has_no_usdt():
    assert set(MAINNET["base"].tokens) == {"USDC"}


def test_mainnet_token_set_is_exactly_pinned():
    actual = {(k, t) for k, c in EVM.items() for t in c.tokens}
    assert actual == set(PINNED)


@pytest.mark.parametrize("chain", list(EVM.values()) + list(EVM_TEST.values()), ids=lambda c: c.name)
def test_chain_invariants(chain):
    assert chain.rpc and all(u.startswith("https://") for u in chain.rpc)
    assert chain.confirmations >= 1
    assert chain.max_log_range >= 10
    assert "{}" in chain.explorer_tx
    for token in chain.tokens.values():
        assert is_checksum_address(token.address)
        assert token.decimals == 6


def test_display_order_covers_all_chains():
    assert sorted(DISPLAY_ORDER) == sorted(EVM)


def test_get_chains():
    assert get_chains(False) is MAINNET
    assert get_chains(True) is TESTNET


def test_token_address_is_normalized_to_checksum():
    from scfg_core.chains import Token
    assert Token("0xaf88d065e77c8cc2239327c5edb3a432268e5831").address == PINNED[("arbitrum", "USDC")]
    with pytest.raises(ValueError):
        Token("not an address")


SOLANA_PINNED = {
    (False, "USDC"): "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
    (False, "USDT"): "Es9vMFrzaCERmJfrF4H2FYD4KCoNkY11McCe8BenwNYB",
    (True, "USDC"): "4zMMC9srt5Ri5X14GAgXhaHii3GnPAEERYPJgZJDncDU",
}


def test_solana_mints_are_pinned():
    actual = {(net, t): c.tokens[t].address
              for net, c in ((False, MAINNET["solana"]), (True, TESTNET["solana"]))
              for t in c.tokens}
    assert actual == SOLANA_PINNED


@pytest.mark.parametrize("testnet", [False, True])
def test_solana_entries(testnet):
    c = get_chains(testnet)["solana"]
    assert (c.family, c.testnet, c.native_symbol, c.fee_hint, c.confirmations) == (
        "solana", testnet, "SOL", "lowest", 32)
    assert c.wallet_chain == ("solana:devnet" if testnet else "solana:mainnet")
    assert c.rpc == (("https://api.devnet.solana.com",) if testnet else
                     ("https://solana-rpc.publicnode.com", "https://api.mainnet-beta.solana.com"))
    assert c.explorer_url("SIG") == ("https://solscan.io/tx/SIG?cluster=devnet" if testnet
                                     else "https://solscan.io/tx/SIG")
    assert "solana" not in DISPLAY_ORDER
