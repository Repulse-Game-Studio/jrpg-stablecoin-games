from decimal import Decimal

import pytest

from cryptopay_core.config import add_item, build_config
from cryptopay_core.errors import ConfigError
from cryptopay_core.payments import MIN_TIP, PaymentSpec, prepare_payment

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"


def cfg(**kw):
    c = build_config(DEV, **kw)
    add_item(c, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2", min_price="3.99")
    return c


def test_unlock_defaults_from_item():
    spec = prepare_payment(cfg(), "unlock", item="chapter2")
    assert spec == PaymentSpec("unlock", "chapter2", Decimal("4.99"), "USDC", 37, "Chapter 2",
                               ("arbitrum", "base", "polygon", "ethereum"))


def test_unlock_sale_price_allowed_down_to_min_price():
    assert prepare_payment(cfg(), "unlock", item="chapter2", amount="3.99").price == Decimal("3.99")
    with pytest.raises(ConfigError):
        prepare_payment(cfg(), "unlock", item="chapter2", amount="3.98")


def test_unlock_in_other_token_limits_chains():
    spec = prepare_payment(cfg(), "unlock", item="chapter2", token="USDT")
    assert spec.token == "USDT" and "base" not in spec.chains


def test_tip_spec():
    spec = prepare_payment(cfg(), "tip", amount="3", token="USDC", title="Coffee")
    assert (spec.kind, spec.code, spec.price, spec.title, spec.item) == ("tip", 0, Decimal("3.00"), "Coffee", None)


@pytest.mark.parametrize("kwargs", [
    dict(kind="unlock", item="nope"),
    dict(kind="tip", token="USDC"),
    dict(kind="tip", amount="1"),
    dict(kind="tip", amount=str(MIN_TIP - Decimal("0.01")), token="USDC"),
    dict(kind="gift", amount="1", token="USDC"),
])
def test_rejections(kwargs):
    kind = kwargs.pop("kind")
    with pytest.raises(ConfigError):
        prepare_payment(cfg(), kind, **kwargs)


def test_token_not_enabled():
    c = cfg(tokens=["USDC"])
    with pytest.raises(ConfigError):
        prepare_payment(c, "tip", amount="1", token="USDT")


def test_float_amount_is_type_error():
    with pytest.raises(TypeError):
        prepare_payment(cfg(), "tip", amount=1.5, token="USDC")
