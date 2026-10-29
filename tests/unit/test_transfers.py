from scfg_core.evm import TransferLog
from scfg_core.receipts import make_receipt, receipt_key
from scfg_core.transfers import Transfer

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
USDC = "0xaf88d065e77c8cC2239327C5EDb3A432268e5831"
SIG = "5VERv8NMvzbJMEkV8xnrLkEaWRtSz9CosKDYjCJjBRnbJLgp8uirBgmQpjKhoR4tjF3ZpRzrFmBV6UjKdiSZkQUW"


def test_evm_hash_is_lowercased():
    assert receipt_key("arbitrum", "0xABcd", 3) == "arbitrum:0xabcd:3"


def test_solana_signature_keeps_its_case():
    assert receipt_key("solana", SIG, 0) == f"solana:{SIG}:0"


def test_transfer_log_converts_to_transfer():
    log = TransferLog(USDC, PAYER, DEV, 5, "0xab", 2, 900, "0xbh")
    assert log.to_transfer() == Transfer(USDC, PAYER, DEV, 5, "0xab", 2, 900)


def test_make_receipt_accepts_a_solana_transfer():
    t = Transfer("MINT", "PAYER", "DEVWALLET", 4_993_742, SIG, 7, 123)
    r = make_receipt(chain="solana", transfer=t, kind="tip", item=None, token="USDC",
                     source="payment")
    assert r == {"chain": "solana", "tx_hash": SIG, "log_index": 7, "block": 123, "kind": "tip",
                 "item": None, "amount": "4.993742", "token": "USDC", "payer": "PAYER",
                 "recipient": "DEVWALLET", "source": "payment"}
