import pytest
from solders.keypair import Keypair

from cryptopay_core.chains import DISPLAY_ORDER, MAINNET, TESTNET
from cryptopay_core.config import add_item, build_chain, build_config, enabled_chains
from cryptopay_core.errors import ConfigError
from cryptopay_core.solana.pda import associated_token_address

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
SOL = str(Keypair().pubkey())
SOL_OLD = str(Keypair().pubkey())
USDC = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"


def test_solana_only_game():
    cfg = build_config(solana_wallet=SOL)
    assert (cfg.wallet, cfg.solana_wallet, cfg.chains) == (None, SOL, ("solana",))
    assert cfg.accepted_wallets == () and cfg.accepted_wallets_for("solana") == (SOL,)
    assert cfg.wallet_for(MAINNET["solana"]) == SOL


def test_both_families_default_to_evm_then_solana():
    cfg = build_config(DEV, solana_wallet=SOL)
    assert cfg.chains == DISPLAY_ORDER + ("solana",)
    assert cfg.wallet_for(MAINNET["base"]) == DEV


def test_custom_evm_chains_come_before_solana():
    avax = build_chain("avalanche", name="Avalanche", chain_id=43114,
                       rpc="https://api.avax.network/ext/bc/C/rpc",
                       tokens={"USDC": "0xB97EF9Ef8734C71904D8002F8b6Bc66Dd9c48a6E"},
                       confirmations=3, explorer_tx="https://snowtrace.io/tx/{}")
    cfg = build_config(DEV, solana_wallet=SOL, custom_chains=[avax])
    assert cfg.chains[-2:] == ("avalanche", "solana")


def test_a_wallet_is_required():
    with pytest.raises(ConfigError, match="wallet"):
        build_config()


@pytest.mark.parametrize("bad", ["", "0x123", "not base58 0OIl", "1" * 31, 5])
def test_bad_solana_wallet(bad):
    with pytest.raises(ConfigError):
        build_config(DEV, solana_wallet=bad)


def test_token_account_instead_of_wallet_is_refused_with_a_hint():
    with pytest.raises(ConfigError, match="token account"):
        build_config(solana_wallet=associated_token_address(SOL, USDC))


@pytest.mark.parametrize("kwargs", [
    dict(wallet=DEV, chains=["solana"]),                  # no solana_wallet
    dict(solana_wallet=SOL, chains=["base"]),             # no EVM wallet
    dict(wallet=DEV, solana_wallet=SOL, confirmations={"solana": 5}),
])
def test_family_rules(kwargs):
    with pytest.raises(ConfigError):
        build_config(**kwargs)


def test_previous_wallets_are_routed_by_format():
    cfg = build_config(DEV, solana_wallet=SOL, previous_wallets=[OLD.lower(), SOL_OLD])
    assert cfg.accepted_wallets == (DEV, OLD)
    assert cfg.accepted_wallets_for("evm") == (DEV, OLD)
    assert cfg.accepted_wallets_for("solana") == (SOL, SOL_OLD)


def test_solana_rpc_override_and_testnet_tokens():
    cfg = build_config(solana_wallet=SOL, testnet=True,
                       rpc_urls={"solana": "https://my-node.example"})
    chain = enabled_chains(cfg)["solana"]
    assert chain.rpc == ("https://my-node.example",) + TESTNET["solana"].rpc
    add_item(cfg, "ch2", code=1, price="1.00", token="USDC", title="Chapter 2")
    with pytest.raises(ConfigError):                      # no USDT on devnet
        add_item(cfg, "ost", code=2, price="1.00", token="USDT", title="OST")


def test_previous_wallets_drop_the_current_wallet_and_repeats():
    cfg = build_config(DEV, solana_wallet=SOL,
                       previous_wallets=[DEV.lower(), OLD, OLD.lower(), SOL, SOL_OLD, SOL_OLD])
    assert cfg.accepted_wallets_for("evm") == (DEV, OLD)
    assert cfg.accepted_wallets_for("solana") == (SOL, SOL_OLD)
