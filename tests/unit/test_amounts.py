import time
from decimal import Decimal

import pytest
from hypothesis import given
from hypothesis import strategies as st

from scfg_core.amounts import (
    decimal_to_units,
    decode,
    encode,
    parse_amount,
    stored_amount,
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


@pytest.mark.parametrize("value", ["", "abc", "4.999", "0", "-1", "0.00", "NaN", "Infinity", "1000000.01",
                                   "1_000", "4_99", "1_0.00", "_5", "5_", "1E+41", "1E-41",
                                   "1E+999999999", "1E-999999999"])
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
    assert str(units_to_decimal(0)) == "0.000000"
    # exact past Decimal's default 28 significant digits: no rounding
    assert str(units_to_decimal(10**28 + 1)) == "10000000000000000000000.000001"
    assert str(units_to_decimal(2**256 - 1)) == (
        "115792089237316195423570985008687907853269984665640564039457584007913129.639935")
    assert str(units_to_decimal(-(10**30) - 7)) == "-1000000000000000000000000.000007"


def test_decimal_to_units_rejects_sub_unit_precision():
    with pytest.raises(ValueError):
        decimal_to_units(Decimal("0.0000001"))


@pytest.mark.parametrize("amount", ["1E+41", "-1E+41", "1E+999993", "1E+999999", "Infinity",
                                    "-Infinity", "NaN", "1E-41", "9.99E+41"])
def test_decimal_to_units_rejects_amounts_out_of_range(amount):
    """A nonzero amount's adjusted exponent must be within +-40 (far beyond any token amount),
    so no stored value can make the conversion slow or overflow."""
    start = time.perf_counter()
    with pytest.raises(ValueError):
        decimal_to_units(Decimal(amount))
    assert time.perf_counter() - start < 1.0          # generous: it is a constant-time check


def test_amount_text_is_bounded():
    """An amount's text is at most AMOUNT_TEXT_LIMIT (100) characters: longer is refused before
    it is parsed (GDScript parses digits in quadratic time)."""
    assert stored_amount("4.99" + "0" * 96) == Decimal("4.99")      # 100 characters
    assert parse_amount("4.99" + "0" * 96) == Decimal("4.99")
    for text in ("4.99" + "0" * 97, "9" * 100_000, "0." + "9" * 100_000, " " * 99 + "5 "):
        start = time.perf_counter()
        with pytest.raises(ValueError):
            stored_amount(text)
        with pytest.raises(ValueError):
            parse_amount(text)
        assert time.perf_counter() - start < 0.5


def test_stored_amount_refuses_lists_and_tuples():
    """Decimal() reads a (sign, digits, exponent) tuple or list; a stored amount never is one."""
    for value in ([0, [2, 5], -1], (0, (2, 5), -1), [1]):
        with pytest.raises(TypeError):
            stored_amount(value)


def test_decimal_to_units_is_exact():
    """No rounding to Decimal's 28 digits: the conversion is exact within the amount range."""
    assert decimal_to_units(Decimal("12345678901234567890123456789012")) == (
        12345678901234567890123456789012 * 10**6)
    assert decimal_to_units(Decimal("1.2345678901234567890123456789012345E+30")) == (
        1234567890123456789012345678901234500)
    assert decimal_to_units(Decimal("4.9900000000000000000000000000000000")) == 4_990_000
    with pytest.raises(ValueError):    # 7th decimal nonzero, far past 28 digits
        decimal_to_units(Decimal("0.000001000000000000000000000000001"))


def test_decimal_to_units_range_edges():
    assert decimal_to_units(Decimal("1E+40")) == 10**46
    assert decimal_to_units(Decimal("-9.999999E+40")) == -9_999_999 * 10**40
    assert decimal_to_units(Decimal("0E+999999")) == 0      # a zero has no magnitude to bound
    assert decimal_to_units(Decimal("0.000001")) == 1


@pytest.mark.parametrize("value, expected", [
    ("4.990000", "4.990000"), (" 5 ", "5"), (5, "5"), ("0", "0"), ("0E-999999", "0E-999999"),
    ("1E+40", "1E+40"), ("-1.5E-40", "-1.5E-40"), (1.5, "1.5"),
])
def test_stored_amount_reads_numbers_in_range(value, expected):
    assert str(stored_amount(value)) == expected


@pytest.mark.parametrize("value", ["1E+41", "1E-41", "1E+999999", "Infinity", "NaN", "sNaN",
                                   "abc", "", None, [1], 1e300, float("inf"),
                                   [0, [2, 5], -1], (0, (2, 5), -1), {}])
def test_stored_amount_refuses_others(value):
    start = time.perf_counter()
    with pytest.raises((ValueError, TypeError)):
        stored_amount(value)
    assert time.perf_counter() - start < 1.0
