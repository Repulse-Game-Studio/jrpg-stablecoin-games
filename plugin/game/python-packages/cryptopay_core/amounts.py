"""Money handling: parsing developer/player amounts and the price|code|nonce codec.

On-chain USDC/USDT amounts are integers in base units (6 decimals). The last four
decimals of every payment carry an item code (2 digits) and a random nonce
(2 digits), so a transaction can be traced back to what it bought:

    4.99 | 37 | 42   ->   4.993742 USDC   (units = 4_993_742)
"""

from __future__ import annotations

from decimal import Decimal, InvalidOperation

TOKEN_DECIMALS = 6
MAX_AMOUNT = Decimal("1000000")
_CENT = Decimal("0.01")


def parse_amount(value) -> Decimal:
    """Validate a price/tip amount and return it quantized to cents.

    Accepts str, int or Decimal. Floats are refused: binary floating point and
    money do not mix.
    """
    if isinstance(value, (bool, float)):
        raise TypeError("amounts must be str, int or Decimal (never float)")
    if isinstance(value, int):
        d = Decimal(value)
    elif isinstance(value, Decimal):
        d = value
    elif isinstance(value, str):
        try:
            d = Decimal(value.strip())
        except InvalidOperation:
            raise ValueError(f"not a number: {value!r}") from None
    else:
        raise TypeError(f"unsupported amount type: {type(value).__name__}")
    if not d.is_finite():
        raise ValueError("amount must be finite")
    if d <= 0:
        raise ValueError("amount must be positive")
    if d > MAX_AMOUNT:
        raise ValueError("amount too large")
    q = d.quantize(_CENT)
    if q != d:
        raise ValueError("amount must have at most 2 decimals")
    return q


def _check_field(name: str, value) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 99:
        raise ValueError(f"{name} must be an int in 0..99")


def encode(price, code: int, nonce: int) -> int:
    """price (2 decimals) + code (0 = tip, 1..99 = item) + nonce -> base units."""
    p = parse_amount(price)
    _check_field("code", code)
    _check_field("nonce", nonce)
    return int(p * 100) * 10_000 + code * 100 + nonce


def decode(units: int) -> tuple[Decimal, int, int]:
    """Inverse of encode: base units -> (price, code, nonce)."""
    if units < 0:
        raise ValueError("units must be >= 0")
    price = (Decimal(units // 10_000) / 100).quantize(_CENT)
    return price, (units // 100) % 100, units % 100


def units_to_decimal(units: int) -> Decimal:
    return Decimal(units).scaleb(-TOKEN_DECIMALS)


def decimal_to_units(amount: Decimal) -> int:
    q = Decimal(amount).scaleb(TOKEN_DECIMALS)
    if q != q.to_integral_value():
        raise ValueError("more than 6 decimals")
    return int(q)


def format_units(units: int) -> str:
    """Human display of a token amount with all 6 decimals: 4993742 -> '4.993742'."""
    return f"{units_to_decimal(units):.6f}"
