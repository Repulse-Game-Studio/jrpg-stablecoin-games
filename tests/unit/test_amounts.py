from decimal import Decimal

import pytest
from hypothesis import given
from hypothesis import strategies as st

from cryptopay_core.amounts import (
    decimal_to_units,
    decode,
    encode,
    parse_amount,
    units_to_decimal,
)


@pytest.mark.parametrize(
    "value, expected",
    [("4.99", Decimal("4.99")), ("3", Decimal("3.00")), (" 2.5 ", Decimal("2.50")),
     ("4.990", Decimal("4.99")), (Decimal("1.10"), Decimal("1.10")), (7, Decimal("7.00"))],
)
def test_parse_amount_accepts(value, expected):
    result = parse_amount(value)
    assert result == expected
    assert result.as_tuple().exponent == -2  # always quantized to cents


@pytest.mark.parametrize("value", [4.99, True, None, [1]])
def test_parse_amount_rejects_types(value):
    with pytest.raises(TypeError):
        parse_amount(value)


@pytest.mark.parametrize("value", ["", "abc", "4.999", "0", "-1", "0.00", "NaN", "Infinity", "1000000.01"])
def test_parse_amount_rejects_values(value):
    with pytest.raises(ValueError):
        parse_amount(value)


def test_encode_spec_example():
    assert encode(Decimal("4.99"), 37, 42) == 4_993_742


def test_encode_tip_uses_code_zero():
    assert encode("3.00", 0, 5) == 3_000_005


def test_decode_spec_example():
    assert decode(4_993_742) == (Decimal("4.99"), 37, 42)


@pytest.mark.parametrize("code, nonce", [(-1, 0), (100, 0), (0, -1), (0, 100), (True, 0), ("1", 0)])
def test_encode_rejects_bad_fields(code, nonce):
    with pytest.raises(ValueError):
        encode("1.00", code, nonce)


def test_encode_validates_price():
    with pytest.raises(ValueError):
        encode("4.999", 1, 1)


@given(
    cents=st.integers(min_value=1, max_value=100_000_000),
    code=st.integers(min_value=0, max_value=99),
    nonce=st.integers(min_value=0, max_value=99),
)
def test_encode_decode_round_trip(cents, code, nonce):
    price = Decimal(cents) / 100
    assert decode(encode(price, code, nonce)) == (price, code, nonce)


@given(cents=st.integers(min_value=1, max_value=100_000_000),
       code=st.integers(0, 99), nonce=st.integers(0, 99))
def test_encoded_amount_never_exceeds_price_by_a_cent(cents, code, nonce):
    price = Decimal(cents) / 100
    paid = units_to_decimal(encode(price, code, nonce))
    assert price <= paid < price + Decimal("0.01")


def test_units_conversions():
    assert units_to_decimal(4_993_742) == Decimal("4.993742")
    assert decimal_to_units(Decimal("4.993742")) == 4_993_742
    assert decimal_to_units(Decimal("5")) == 5_000_000


def test_decimal_to_units_rejects_sub_unit_precision():
    with pytest.raises(ValueError):
        decimal_to_units(Decimal("0.0000001"))
