from scfg_core.solana.paylinks import solana_pay_uri

WALLET = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
REF = "82ZJ7nbGpixjeDCmEhUcmwXYfvurzAgGdtSMuHnUgyny"


def test_transfer_request_uri():
    uri = solana_pay_uri(recipient=WALLET, units=4_993_742, mint=MINT, reference=REF,
                         label="Ruby Dreams", message="Chapter 2: The Fall & more")
    assert uri == (f"solana:{WALLET}?amount=4.993742&spl-token={MINT}&reference={REF}"
                   "&label=Ruby%20Dreams&message=Chapter%202%3A%20The%20Fall%20%26%20more")


def test_whole_amount_keeps_six_decimals_and_empty_label_is_omitted():
    uri = solana_pay_uri(recipient=WALLET, units=5_000_000, mint=MINT, reference=REF,
                         label="", message="Tip")
    assert "amount=5.000000&" in uri and "label=" not in uri
