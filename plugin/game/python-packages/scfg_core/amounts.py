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
# The magnitude bound of every amount the core reads: a nonzero amount's adjusted exponent (the
# power of ten of its leading digit, Decimal.adjusted()) must lie in [-40, 40]. That is far
# beyond any token amount (the smallest unit is 1E-6; a uint256 of base units read through
# Arc's 18-decimal emitter is floored to 6 decimals and never stored past 10**32 units), and it
# keeps near-Emax values (1E+999999) from overflowing or building million-digit integers.
AMOUNT_EXPONENT_LIMIT = 40
# The longest amount text the core parses (parse_amount, stored_amount): far beyond any amount
# (a stored one is at most about 40 characters), and it keeps digit parsing cheap in every core.
AMOUNT_TEXT_LIMIT = 100


def _in_range(d: Decimal) -> bool:
    return d.is_finite() and (d.is_zero() or -AMOUNT_EXPONENT_LIMIT <= d.adjusted()
                              <= AMOUNT_EXPONENT_LIMIT)


def stored_amount(value) -> Decimal:
    """An amount read back from storage (a receipt's amount, a session's price): Decimal(value)
    when it is a finite number within the amount range (AMOUNT_EXPONENT_LIMIT) and, as text, at
    most AMOUNT_TEXT_LIMIT characters, else a ValueError (a TypeError for a list or tuple, or a
    value Decimal() does not take)."""
    if isinstance(value, (list, tuple)):  # Decimal() reads a (sign, digits, exponent) tuple
        raise TypeError(f"a stored amount is a number, not a {type(value).__name__}")
    if isinstance(value, str) and len(value) > AMOUNT_TEXT_LIMIT:
        raise ValueError(f"amount text longer than {AMOUNT_TEXT_LIMIT} characters")
    try:
        d = Decimal(value)
    except InvalidOperation:
        raise ValueError(f"not a number: {value!r}") from None
    if not _in_range(d):
        raise ValueError(f"amount out of range: {value!r}")
    return d


def parse_amount(value) -> Decimal:
    """Validate a price/tip amount and return it quantized to cents.

    Accepts str, int or Decimal. Floats are refused: binary floating point and
    money do not mix. A str is a plain decimal number: Decimal's digit-group underscores
    ("1_000") are refused like any other malformed amount.
    """
    if isinstance(value, (bool, float)):
        raise TypeError("amounts must be str, int or Decimal (never float)")
    if isinstance(value, int):
        d = Decimal(value)
    elif isinstance(value, Decimal):
        d = value
    elif isinstance(value, str):
        if len(value) > AMOUNT_TEXT_LIMIT:
            raise ValueError(f"amount text longer than {AMOUNT_TEXT_LIMIT} characters")
        if "_" in value:
            raise ValueError(f"not a number: {value!r}")
        try:
            d = Decimal(value.strip())
        except InvalidOperation:
            raise ValueError(f"not a number: {value!r}") from None
    else:
        raise TypeError(f"unsupported amount type: {type(value).__name__}")
    if not d.is_finite():
        raise ValueError("amount must be finite")
    if not _in_range(d):
        raise ValueError("amount out of range")
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
    """Inverse of encode: base units -> (price, code, nonce).

    From 10**32 units the price needs more than Decimal's 28 digits and this raises
    decimal.InvalidOperation; callers that read transfers off the chain skip such a value.
    """
    if units < 0:
        raise ValueError("units must be >= 0")
    price = (Decimal(units // 10_000) / 100).quantize(_CENT)
    return price, (units // 100) % 100, units % 100


def units_to_decimal(units: int) -> Decimal:
    """Base units -> token amount, exactly (4993742 -> 4.993742), however many digits: the
    value is built from its digits instead of through Decimal's 28-digit arithmetic."""
    sign, digits, exponent = Decimal(units).as_tuple()
    return Decimal((sign, digits, exponent - TOKEN_DECIMALS))


def decimal_to_units(amount: Decimal) -> int:
    """Token amount -> base units, exactly (no rounding to Decimal's 28 digits). ValueError for
    more than 6 decimals, or for an amount that is not finite or out of range
    (AMOUNT_EXPONENT_LIMIT)."""
    amount = Decimal(amount)
    if not _in_range(amount):
        raise ValueError(f"amount out of range: {amount}")
    sign, digits, exponent = amount.as_tuple()
    q = Decimal((sign, digits, exponent + TOKEN_DECIMALS))
    if q != q.to_integral_value():
        raise ValueError("more than 6 decimals")
    return int(q)


def format_units(units: int) -> str:
    """Human display of a token amount with all 6 decimals: 4993742 -> '4.993742'."""
    return f"{units_to_decimal(units):.6f}"
