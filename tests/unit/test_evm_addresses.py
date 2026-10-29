import pytest

from cryptopay_core.evm import (
    TRANSFER_TOPIC,
    address_topic,
    is_checksum_address,
    normalize_address,
    to_checksum_address,
)

# Official EIP-55 test vectors
EIP55 = [
    "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed",
    "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
    "0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB",
    "0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb",
]


@pytest.mark.parametrize("address", EIP55)
def test_checksum_from_lowercase(address):
    assert to_checksum_address(address.lower()) == address


@pytest.mark.parametrize("address", EIP55)
def test_checksum_from_uppercase_body(address):
    assert to_checksum_address("0x" + address[2:].upper()) == address


@pytest.mark.parametrize(
    "bad",
    ["", "0x", "5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAe",
     "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAedd", "0xZZAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", None, 42],
)
def test_checksum_rejects_malformed(bad):
    with pytest.raises(ValueError):
        to_checksum_address(bad)


def test_is_checksum_address():
    assert is_checksum_address(EIP55[0])
    assert not is_checksum_address(EIP55[0].lower())
    assert not is_checksum_address("nonsense")


def test_normalize_accepts_single_case_input():
    assert normalize_address(EIP55[1].lower()) == EIP55[1]
    assert normalize_address("0x" + EIP55[1][2:].upper()) == EIP55[1]


def test_normalize_accepts_correct_mixed_case():
    assert normalize_address(EIP55[2]) == EIP55[2]


def test_normalize_rejects_wrong_mixed_case():
    i = next(i for i, c in enumerate(EIP55[0]) if i > 1 and c.isalpha())
    wrong = EIP55[0][:i] + EIP55[0][i].swapcase() + EIP55[0][i + 1:]  # flip one letter's case
    with pytest.raises(ValueError, match="checksum"):
        normalize_address(wrong)


def test_transfer_topic_constant():
    assert TRANSFER_TOPIC == "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef"


def test_address_topic_is_left_padded_lowercase():
    topic = address_topic(EIP55[0])
    assert topic == "0x000000000000000000000000" + EIP55[0][2:].lower()
    assert len(topic) == 66
