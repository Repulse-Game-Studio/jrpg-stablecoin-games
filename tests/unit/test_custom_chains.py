"""Developer-defined chains: cryptopay.add_chain() -> config.build_chain()."""

import pytest

from cryptopay_core.chains import MAINNET
from cryptopay_core.config import add_item, build_chain, build_config, enabled_chains
from cryptopay_core.engine import Engine
from cryptopay_core.errors import ConfigError

from fakes import FakeChain, FakeClock, transfer_log

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
AVAX_USDC = "0xB97EF9Ef8734C71904D8002F8b6Bc66Dd9c48a6E"
TX = "0x" + "cd" * 32


def avalanche(**kw):
    args = dict(name="Avalanche", chain_id=43114,
                rpc=["https://my-node.example/avax", "https://api.avax.network/ext/bc/C/rpc"],
                tokens={"USDC": AVAX_USDC}, confirmations=5,
                explorer_tx="https://snowtrace.io/tx/{}", native_symbol="AVAX")
    args.update(kw)
    return build_chain(args.pop("key", "avalanche"), **args)


# --- build_chain ----------------------------------------------------------------

def test_build_chain_fields():
    c = avalanche()
    assert (c.key, c.chain_id, c.name, c.confirmations, c.native_symbol, c.fee_hint) == (
        "avalanche", 43114, "Avalanche", 5, "AVAX", "low")
    assert c.rpc == ("https://my-node.example/avax", "https://api.avax.network/ext/bc/C/rpc")
    assert c.tokens["USDC"].address == AVAX_USDC
    assert c.tokens["USDC"].decimals == 6
    assert c.testnet is False
    assert c.explorer_url(TX) == "https://snowtrace.io/tx/" + TX


def test_build_chain_accepts_single_rpc_string_and_lowercase_token():
    c = avalanche(rpc="https://api.avax.network/ext/bc/C/rpc", tokens={"USDC": AVAX_USDC.lower()})
    assert c.rpc == ("https://api.avax.network/ext/bc/C/rpc",)
    assert c.tokens["USDC"].address == AVAX_USDC


@pytest.mark.parametrize("url", ["http://127.0.0.1:8545", "http://localhost:8545/",
                                 "http://127.0.0.1"])
def test_build_chain_allows_plain_http_on_this_computer(url):
    assert avalanche(rpc=url).rpc == (url,)


@pytest.mark.parametrize("kw", [
    dict(key="Avalanche"), dict(key="a:b"), dict(key=""), dict(key="x" * 33),
    dict(name=""), dict(name="x" * 33), dict(name=None),
    dict(chain_id=0), dict(chain_id=True), dict(chain_id="43114"),
    dict(rpc=[]), dict(rpc="http://my-node.example"), dict(rpc="http://127.0.0.1.evil.com"),
    dict(rpc=["https://ok.example", 5]), dict(rpc=None),
    dict(tokens={}), dict(tokens={"DAI": AVAX_USDC}), dict(tokens={"USDC": "0x123"}),
    dict(tokens={"USDC": AVAX_USDC[:-1] + "e"}),  # bad EIP-55 checksum
    dict(tokens=[("USDC", AVAX_USDC)]),
    dict(confirmations=0), dict(confirmations=None),
    dict(explorer_tx="https://snowtrace.io/tx/"), dict(explorer_tx="http://snowtrace.io/tx/{}"),
    dict(explorer_tx="https://x.example/{}/{}"), dict(explorer_tx="https://x.example/{0}{}"),
    dict(explorer_tx="https://x.example/{tx}?{}"),
    dict(native_symbol=""), dict(native_symbol="A V"),
    dict(fee_hint="cheap"), dict(max_log_range=5),
])
def test_build_chain_rejects(kw):
    with pytest.raises(ConfigError):
        avalanche(**kw)


# --- build_config ---------------------------------------------------------------

def test_custom_chain_is_enabled_by_default_after_built_ins():
    cfg = build_config(DEV, custom_chains=[avalanche()])
    assert cfg.chains == ("arbitrum", "base", "polygon", "ethereum", "avalanche")
    assert list(enabled_chains(cfg))[-1] == "avalanche"
    assert cfg.registry["avalanche"].chain_id == 43114


def test_custom_chain_only_and_overrides():
    cfg = build_config(DEV, chains=["avalanche"], custom_chains=[avalanche()],
                       rpc_urls={"avalanche": "https://first.example"},
                       confirmations={"avalanche": 12},
                       methods={"qr": {"chains": ["avalanche"]}})
    chain = enabled_chains(cfg)["avalanche"]
    assert chain.rpc[0] == "https://first.example"
    assert chain.confirmations == 12
    assert list(enabled_chains(cfg)) == ["avalanche"]


def test_custom_chain_can_sell_items():
    cfg = build_config(DEV, chains=["avalanche"], custom_chains=[avalanche()])
    item = add_item(cfg, "ch2", code=1, price="4.99", token="USDC", title="Chapter 2")
    assert item.token == "USDC"
    with pytest.raises(ConfigError):  # the custom chain only carries USDC
        add_item(cfg, "ost", code=2, price="2.00", token="USDT", title="OST")


def test_custom_chain_belongs_to_one_network():
    test_chain = avalanche(chain_id=43113, name="Fuji", testnet=True)
    assert "avalanche" not in build_config(DEV, custom_chains=[test_chain]).registry
    cfg = build_config(DEV, testnet=True, custom_chains=[avalanche(), test_chain])
    assert cfg.registry["avalanche"].name == "Fuji"
    with pytest.raises(ConfigError, match="only added for testnet"):
        build_config(DEV, chains=["avalanche"], custom_chains=[test_chain])
    with pytest.raises(ConfigError, match="only added for mainnet"):
        build_config(DEV, testnet=True, chains=["avalanche"], custom_chains=[avalanche()])


@pytest.mark.parametrize("chains", [
    [avalanche(key="arbitrum")],                                  # built-in key
    [avalanche(key="ethereum", testnet=True, chain_id=1234)],    # built-in key, testnet
    [avalanche(), avalanche(chain_id=99)],                        # added twice
    [avalanche(chain_id=42161)],                                  # Arbitrum's chain id
    [avalanche(), avalanche(key="other")],                        # same chain id twice
    [MAINNET["arbitrum"]],                                        # a built-in chain object
    ["avalanche"],                                                # not made by add_chain
])
def test_build_config_rejects_bad_custom_chains(chains):
    with pytest.raises(ConfigError):
        build_config(DEV, custom_chains=chains)


def test_chain_ids_may_repeat_across_networks():
    cfg = build_config(DEV, testnet=True,
                       custom_chains=[avalanche(key="fork", chain_id=1, testnet=True)])
    assert cfg.registry["fork"].chain_id == 1   # Ethereum mainnet's id is fine on testnet


# --- engine: a payment on a custom chain ------------------------------------------

def test_engine_pays_restores_and_reverifies_on_custom_chain():
    cfg = build_config(DEV, chains=["avalanche"], custom_chains=[avalanche()])
    add_item(cfg, "ch2", code=37, price="4.99", token="USDC", title="Chapter 2")
    fakes = {}

    def factory(chain):
        fakes.setdefault(chain.key, FakeChain(head=1000))
        fakes[chain.key].chain_obj = chain
        return fakes[chain.key]

    clock = FakeClock(100.0)
    e = Engine(cfg, rpc_factory=factory, clock=clock)
    rec = e.start_session("unlock", "avalanche", item="ch2")
    fake = fakes["avalanche"]
    assert fake.chain_obj.rpc[0] == "https://my-node.example/avax"
    fake.logs.append(transfer_log(AVAX_USDC, PAYER, DEV, rec["units"], block=1001, tx_hash=TX))
    fake.receipts[TX] = {"status": "0x1", "logs": list(fake.logs)}
    fake.head = 1001
    e.tick()
    fake.head = 1001 + 5
    outcomes = [o for o in e.tick() if o.status == "paid"]
    assert outcomes and outcomes[0].result.chain == "avalanche"
    assert e.snapshot().owned == {"ch2"}

    assert e.reverify() == []                       # receipt checks out on its chain
    e.receipts.clear()
    found = e.restore_lookup(TX)
    assert [(c.chain, c.item) for c in found] == [("avalanche", "ch2")]


def test_receipts_from_a_removed_custom_chain_are_kept():
    """A later build without the chain can't judge the receipt, so it keeps it."""
    cfg = build_config(DEV)
    receipt = {"chain": "avalanche", "tx_hash": TX, "log_index": 0, "block": 5, "kind": "tip",
               "item": None, "amount": "1.000000", "token": "USDC", "payer": PAYER,
               "recipient": DEV, "source": "payment"}
    e = Engine(cfg, receipts={"avalanche:" + TX + ":0": receipt},
               rpc_factory=lambda chain: FakeChain())
    assert e.reverify() == []
