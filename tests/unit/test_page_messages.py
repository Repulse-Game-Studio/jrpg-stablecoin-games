import pytest

from cryptopay_core.page_messages import parse_build_message, parse_tx_message

EVM_TX = "0x" + "ab" * 32
SOL_ACCOUNT = "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3"


def test_tx_message_with_a_checksummed_sender():
    tx, sender = parse_tx_message({"tx_hash": EVM_TX.upper().replace("0X", "0x"),
                                   "from": "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed"})
    assert tx == EVM_TX
    assert sender == "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"


def test_a_bad_sender_is_dropped_not_fatal():
    assert parse_tx_message({"tx_hash": EVM_TX, "from": "nonsense"}) == (EVM_TX, None)
    assert parse_tx_message({"tx_hash": EVM_TX}) == (EVM_TX, None)
    assert parse_tx_message({"tx_hash": EVM_TX, "from": 5}) == (EVM_TX, None)
    assert parse_tx_message({"tx_hash": EVM_TX, "from": []}) == (EVM_TX, None)


@pytest.mark.parametrize("data", [None, [], "x", {}, {"tx_hash": 5}, {"tx_hash": "0x12"}])
def test_bad_tx_messages_raise(data):
    with pytest.raises(ValueError):
        parse_tx_message(data)


def test_build_message():
    assert parse_build_message({"account": SOL_ACCOUNT}) == SOL_ACCOUNT


@pytest.mark.parametrize("data", [None, {}, {"account": 5}, {"account": "0xabc"}])
def test_bad_build_messages_raise(data):
    with pytest.raises(ValueError):
        parse_build_message(data)
