from decimal import Decimal

import pytest

from cryptopay_core.config import (
    DEFAULT_ACCENT,
    DEFAULT_METHODS,
    add_item,
    build_config,
    chains_for_token,
    enabled_chains,
    logo_content_type,
    text_on,
)
from cryptopay_core.errors import ConfigError
from cryptopay_core.models import Item

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
OLD = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"


def cfg(**kw):
    return build_config(DEV, **kw)


# --- wallet ---------------------------------------------------------------

def test_wallet_is_normalized_from_lowercase():
    assert build_config(DEV.lower()).wallet == DEV


@pytest.mark.parametrize("bad", ["", "0x123", DEV[:-1] + "e", None])
def test_bad_wallet_raises_config_error(bad):
    with pytest.raises(ConfigError):
        build_config(bad)


def test_config_error_is_a_value_error():
    assert issubclass(ConfigError, ValueError)


def test_accepted_wallets_include_previous():
    c = cfg(previous_wallets=[OLD.lower()])
    assert c.accepted_wallets == (DEV, OLD)


# --- defaults ---------------------------------------------------------------

def test_defaults():
    c = cfg()
    assert c.testnet is False
    assert c.chains == ("arbitrum", "base", "polygon", "ethereum")
    assert c.tokens == ("USDC", "USDT")
    assert c.methods == DEFAULT_METHODS
    assert c.rpc_urls == {} and c.confirmations == {} and c.items == {}


def test_default_methods_are_not_shared_between_configs():
    a = cfg()
    a.methods["qr"]["enabled"] = False
    assert cfg().methods["qr"]["enabled"] is True
    assert DEFAULT_METHODS["qr"]["enabled"] is True


# --- chains / tokens ---------------------------------------------------------

@pytest.mark.parametrize("chains", [[], ["tron"], ["base", "base"]])
def test_bad_chains(chains):
    with pytest.raises(ConfigError):
        cfg(chains=chains)


@pytest.mark.parametrize("tokens", [[], ["DAI"]])
def test_bad_tokens(tokens):
    with pytest.raises(ConfigError):
        cfg(tokens=tokens)


def test_no_chain_supports_token():
    with pytest.raises(ConfigError):
        cfg(chains=["base"], tokens=["USDT"])


def test_testnet_has_no_usdt():
    with pytest.raises(ConfigError):
        cfg(testnet=True, tokens=["USDT"])


def test_enabled_chains_respects_order_tokens_and_overrides():
    c = cfg(chains=["polygon", "base"], tokens=["USDT"],
            rpc_urls={"polygon": ["https://my.rpc/key"]}, confirmations={"polygon": 64})
    chains = enabled_chains(c)
    assert list(chains) == ["polygon"]            # base dropped: no USDT on base
    poly = chains["polygon"]
    assert set(poly.tokens) == {"USDT"}
    assert poly.rpc[0] == "https://my.rpc/key"    # developer URL tried first
    assert len(poly.rpc) > 1                       # built-ins still present after it
    assert poly.confirmations == 64


def test_rpc_url_string_is_accepted():
    c = cfg(rpc_urls={"base": "https://one.rpc"})
    assert enabled_chains(c)["base"].rpc[0] == "https://one.rpc"


@pytest.mark.parametrize("urls", [{"tron": ["https://x"]}, {"base": ["http://insecure"]}])
def test_bad_rpc_urls(urls):
    with pytest.raises(ConfigError):
        cfg(rpc_urls=urls)


@pytest.mark.parametrize("confs", [{"base": 0}, {"base": True}, {"base": "3"}, {"nope": 3}])
def test_bad_confirmations(confs):
    with pytest.raises(ConfigError):
        cfg(confirmations=confs)


def test_chains_for_token():
    assert chains_for_token(cfg(), "USDT") == ["arbitrum", "polygon", "ethereum"]


# --- methods ----------------------------------------------------------------

def test_methods_merge_partial_options():
    c = cfg(methods={"browser": {"enabled": False}})
    assert c.methods["browser"] == {"enabled": False, "timeout_min": 30}
    assert c.methods["qr"]["enabled"] is True


@pytest.mark.parametrize("methods", [
    {"carrier_pigeon": {"enabled": True}},
    {"qr": {"colour": "red"}},
    {"qr": {"enabled": "yes"}},
    {"browser": {"enabled": False}, "qr": {"enabled": False}, "manual": {"enabled": False}},
    {"qr": {"chains": ["tron"]}},
    {"browser": {"timeout_min": "30"}},
    {"browser": {"timeout_min": 0}},
    {"browser": {"timeout_min": True}},
])
def test_bad_methods(methods):
    with pytest.raises(ConfigError):
        cfg(methods=methods)


def test_browser_timeout_accepts_a_positive_number():
    assert cfg(methods={"browser": {"timeout_min": 7.5}}).methods["browser"]["timeout_min"] == 7.5


def test_qr_chains_subset_ok():
    assert cfg(methods={"qr": {"chains": ["base"]}}).methods["qr"]["chains"] == ["base"]


# --- items ------------------------------------------------------------------

def test_add_item():
    c = cfg()
    item = add_item(c, "chapter2", code=1, price="4.99", token="USDC", title="Chapter 2")
    assert item == Item("chapter2", 1, Decimal("4.99"), "USDC", "Chapter 2", Decimal("4.99"))
    assert c.items["chapter2"] is item


def test_add_item_min_price():
    item = add_item(cfg(), "ost", code=2, price="2.00", token="USDT", title="OST", min_price="1.50")
    assert item.min_price == Decimal("1.50")


@pytest.mark.parametrize("kwargs", [
    dict(item_id="Chapter 2"),
    dict(item_id=""),
    dict(code=0),
    dict(code=100),
    dict(code=True),
    dict(price=4.99),
    dict(price="4.999"),
    dict(min_price="5.00"),
    dict(token="DAI"),
    dict(title=""),
    dict(title="x" * 81),
])
def test_add_item_rejects(kwargs):
    args = dict(item_id="chapter2", code=1, price="4.99", token="USDC", title="Chapter 2")
    args.update(kwargs)
    item_id = args.pop("item_id")
    with pytest.raises(ConfigError):
        add_item(cfg(), item_id, **args)


def test_add_item_rejects_duplicates():
    c = cfg()
    add_item(c, "a", code=1, price="1", token="USDC", title="A")
    with pytest.raises(ConfigError):
        add_item(c, "a", code=2, price="1", token="USDC", title="A again")
    with pytest.raises(ConfigError):
        add_item(c, "b", code=1, price="1", token="USDC", title="Same code")


def test_add_item_token_must_be_enabled():
    c = cfg(tokens=["USDC"])
    with pytest.raises(ConfigError):
        add_item(c, "a", code=1, price="1", token="USDT", title="A")


# --- appearance -------------------------------------------------------------

def test_appearance_defaults():
    c = cfg()
    assert c.logo is None
    assert c.accent == DEFAULT_ACCENT


@pytest.mark.parametrize("logo", ["images/logo.png", "gui/Logo.JPG", "logo.jpeg", "a/b.webp"])
def test_logo_accepts_raster_images(logo):
    assert cfg(logo=logo).logo == logo


@pytest.mark.parametrize("logo", ["", "logo.svg", "logo", "logo.gif", 5, "../logo.png", "/abs/logo.png",
                                  "c:\\logo.png", "images\\logo.png"])
def test_logo_rejects_other_values(logo):
    with pytest.raises(ConfigError):
        cfg(logo=logo)


def test_logo_content_type():
    assert logo_content_type("images/logo.PNG") == "image/png"
    assert logo_content_type("x.jpg") == logo_content_type("x.jpeg") == "image/jpeg"
    assert logo_content_type("x.webp") == "image/webp"


@pytest.mark.parametrize("value, expected", [
    ("#0E7C74", "#0e7c74"), ("#09c", "#0099cc"), ("#0099ccff", "#0099cc"), (None, DEFAULT_ACCENT),
])
def test_accent_is_normalized(value, expected):
    assert cfg(accent=value).accent == expected


@pytest.mark.parametrize("value", ["0099cc", "#0099c", "#gggggg", "teal", 0x0099CC, "#0099cc80 "])
def test_bad_accent(value):
    with pytest.raises(ConfigError):
        cfg(accent=value)


@pytest.mark.parametrize("accent, text", [
    ("#0e7c74", "#ffffff"), ("#17202e", "#ffffff"), ("#ffcc00", "#17202e"), ("#ffffff", "#17202e"),
])
def test_text_on_accent_stays_readable(accent, text):
    assert text_on(accent) == text
