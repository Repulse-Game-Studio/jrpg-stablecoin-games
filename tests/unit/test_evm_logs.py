from scfg_core.evm import TRANSFER_TOPIC, address_topic, parse_transfer_log

USDC = "0xaf88d065e77c8cC2239327C5EDb3A432268e5831"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
DEV = "0x52908400098527886E0F7030069857D2E4169EE7"


def make_log(**overrides):
    log = {
        "address": USDC.lower(),
        "topics": [TRANSFER_TOPIC, address_topic(PAYER), address_topic(DEV)],
        "data": "0x" + format(4_993_742, "064x"),
        "transactionHash": "0xABCDEF" + "0" * 58,
        "logIndex": "0xc",
        "blockNumber": "0xffb2a46",
        "blockHash": "0x" + "11" * 32,
        "removed": False,
    }
    log.update(overrides)
    return log


def test_parses_erc20_transfer():
    t = parse_transfer_log(make_log())
    assert t.token == USDC
    assert t.sender == PAYER
    assert t.recipient == DEV
    assert t.value == 4_993_742
    assert t.tx_hash == "0xabcdef" + "0" * 58
    assert t.log_index == 12
    assert t.block_number == 0xFFB2A46
    assert t.block_hash == "0x" + "11" * 32


def test_topic0_comparison_is_case_insensitive():
    assert parse_transfer_log(make_log(topics=[TRANSFER_TOPIC.upper().replace("0X", "0x"),
                                               address_topic(PAYER), address_topic(DEV)])) is not None


def test_ignores_other_events():
    assert parse_transfer_log(make_log(topics=["0x" + "ab" * 32, address_topic(PAYER),
                                               address_topic(DEV)])) is None


def test_ignores_erc721_transfer_with_four_topics():
    log = make_log(topics=[TRANSFER_TOPIC, address_topic(PAYER), address_topic(DEV), "0x" + "00" * 31 + "07"])
    assert parse_transfer_log(log) is None


def test_ignores_removed_logs():
    assert parse_transfer_log(make_log(removed=True)) is None


def test_missing_removed_field_is_fine():
    log = make_log()
    del log["removed"]
    assert parse_transfer_log(log) is not None


def test_empty_data_means_zero_value():
    assert parse_transfer_log(make_log(data="0x")).value == 0
