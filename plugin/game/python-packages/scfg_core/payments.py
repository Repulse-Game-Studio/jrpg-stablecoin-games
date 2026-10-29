"""Validation of a pay() request, independent of chain and network.

Used synchronously by scfg.pay() (so programmer errors raise immediately in
the developer's script) and again by Engine.start_session.
"""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal

from .amounts import parse_amount
from .config import Config, chains_for_token
from .errors import ConfigError

MIN_TIP = Decimal("0.50")
TIP_PRESETS = (Decimal("1.00"), Decimal("3.00"), Decimal("5.00"), Decimal("10.00"))


@dataclass(frozen=True)
class PaymentSpec:
    kind: str  # "unlock" | "tip"
    item: str | None
    price: Decimal
    token: str
    code: int  # 0 for tips
    title: str
    chains: tuple[str, ...]  # enabled chains that carry the token, in display order


def prepare_payment(config: Config, kind: str, *, item: str | None = None, amount=None,
                    token: str | None = None, title: str | None = None) -> PaymentSpec:
    if kind == "unlock":
        if item not in config.items:
            raise ConfigError(f"unknown item {item!r}; declare it with scfg.add_item()")
        it = config.items[item]
        price = it.price if amount is None else parse_amount(amount)
        if price < it.min_price:
            raise ConfigError(f"amount {price} is below the item's min_price {it.min_price}")
        token = token or it.token
        code, title = it.code, title or it.title
    elif kind == "tip":
        if amount is None or token is None:
            raise ConfigError("tips need an amount and a token")
        price = parse_amount(amount)
        if price < MIN_TIP:
            raise ConfigError(f"tips must be at least {MIN_TIP}")
        code, title = 0, title or "Tip"
    else:
        raise ConfigError(f"kind must be 'unlock' or 'tip', not {kind!r}")
    if token not in config.tokens:
        raise ConfigError(f"token {token!r} is not enabled")
    chains = tuple(chains_for_token(config, token))
    if not chains:
        raise ConfigError(f"{token} is not available on any enabled chain")
    return PaymentSpec(kind=kind, item=item, price=price, token=token, code=code,
                       title=title, chains=chains)
