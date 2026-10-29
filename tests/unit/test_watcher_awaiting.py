from decimal import Decimal

from scfg_core.chains import MAINNET
from scfg_core.evm import TRANSFER_TOPIC, address_topic
from scfg_core.models import Item
from scfg_core.receipts import receipt_key
from scfg_core.sessions import HINT_TIMEOUT_S, SCAN_HORIZON_S, create_session, set_hint
from scfg_core.watcher import SCAN_STEP_BLOCKS, advance

from fakes import FakeChain, drive, transfer_log

CHAIN = MAINNET["arbitrum"]          # 20 confirmations
USDC = CHAIN.tokens["USDC"].address
USDT = CHAIN.tokens["USDT"].address
DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
ITEMS = {"chapter2": Item("chapter2", 37, Decimal("4.99"), "USDC", "Chapter 2", Decimal("4.99"))}
TX = "0x" + "ab" * 32
UNITS = 4_993_742


def setup(head=1000, **kw):
    pending, receipts, chain = {}, {}, FakeChain(head=head)
    args = dict(kind="unlock", item="chapter2", title="Chapter 2", token="USDC", chain="arbitrum",
                price="4.99", code=37, nonce=42, start_block=995, now=0.0)
    args.update(kw)
    rec = create_session(pending, **args)
    return pending, receipts, chain, rec


def step(pending, receipts, chain, rec, now=10.0):
    return drive(advance(pending, receipts, rec["id"], rpc=chain, chain=CHAIN, wallet=DEV,
                         items=ITEMS, now=now))


def test_missing_session():
    out = drive(advance({}, {}, "ps_nope", rpc=FakeChain(), chain=CHAIN, wallet=DEV,
                        items=ITEMS, now=0))
    assert out.status == "missing"


# --- amount scan --------------------------------------------------------------

def test_scan_without_payment_advances_cursor():
    pending, receipts, chain, rec = setup()
    out = step(pending, receipts, chain, rec)
    assert out.status == "waiting" and out.error is None
    assert rec["cursor"] == 1001
    address, topics, lo, hi = chain.last_get_logs
    assert address == USDC
    assert topics == [TRANSFER_TOPIC, None, address_topic(DEV)]
    assert (lo, hi) == (995, 1000)


def test_scan_does_nothing_when_cursor_ahead_of_head():
    pending, receipts, chain, rec = setup(head=900)   # lagging endpoint
    out = step(pending, receipts, chain, rec)
    assert out.status == "waiting"
    assert rec["cursor"] == 995
    assert "get_logs" not in chain.calls


def test_scan_is_bounded_per_step():
    pending, receipts, chain, rec = setup(head=995 + 3 * SCAN_STEP_BLOCKS)
    step(pending, receipts, chain, rec)
    assert rec["cursor"] == 995 + SCAN_STEP_BLOCKS


def test_scan_matches_exact_amount_and_starts_confirming():
    pending, receipts, chain, rec = setup(head=1000)
    chain.logs.append(transfer_log(USDC, PAYER, DEV, UNITS, block=998, tx_hash=TX, log_index=2))
    out = step(pending, receipts, chain, rec)
    assert out.status == "confirming"
    assert out.confirmations == 3 and out.required == 20
    assert rec["state"] == "confirming"
    assert rec["match"] == {"tx_hash": TX, "log_index": 2, "block": 998,
                            "block_hash": "0x" + format(998, "064x"), "value": UNITS,
                            "payer": PAYER, "recipient": DEV}


def test_scan_ignores_wrong_amount_token_recipient_and_old_blocks():
    pending, receipts, chain, rec = setup(head=1000)
    other = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"
    chain.logs += [
        transfer_log(USDC, PAYER, DEV, UNITS + 1, block=998, tx_hash=TX),          # amount off by 1
        transfer_log(USDT, PAYER, DEV, UNITS, block=998, tx_hash=TX),              # other token
        transfer_log(USDC, PAYER, other, UNITS, block=998, tx_hash=TX),            # other recipient
        transfer_log(USDC, PAYER, DEV, UNITS, block=990, tx_hash=TX),              # before session
    ]
    assert step(pending, receipts, chain, rec).status == "waiting"
    assert rec["match"] is None


def test_scan_picks_earliest_match():
    pending, receipts, chain, rec = setup(head=1000)
    chain.logs += [
        transfer_log(USDC, PAYER, DEV, UNITS, block=999, tx_hash="0x" + "02" * 32),
        transfer_log(USDC, PAYER, DEV, UNITS, block=997, tx_hash="0x" + "01" * 32),
    ]
    step(pending, receipts, chain, rec)
    assert rec["match"]["tx_hash"] == "0x" + "01" * 32


def test_scan_skips_transfer_already_used_as_receipt():
    pending, receipts, chain, rec = setup(head=1000)
    chain.logs.append(transfer_log(USDC, PAYER, DEV, UNITS, block=998, tx_hash=TX, log_index=2))
    receipts[receipt_key("arbitrum", TX, 2)] = {"kind": "unlock"}
    assert step(pending, receipts, chain, rec).status == "waiting"


def test_horizon_closes_session():
    pending, receipts, chain, rec = setup()
    out = step(pending, receipts, chain, rec, now=SCAN_HORIZON_S)
    assert out.status == "closed" and out.result is None
    assert pending == {}


def test_network_outage_is_reported_and_state_kept():
    pending, receipts, chain, rec = setup()
    chain.down = True
    out = step(pending, receipts, chain, rec)
    assert out.error == "network_unavailable"
    assert rec["cursor"] == 995 and rec["state"] == "awaiting"


# --- hints (browser page / pasted hash) --------------------------------------

def receipt_with(*logs, status="0x1"):
    return {"status": status, "logs": list(logs)}


def test_hint_not_mined_yet_keeps_waiting_without_scanning():
    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], TX, now=5.0)
    out = step(pending, receipts, chain, rec, now=10.0)
    assert out.status == "waiting" and out.error is None
    assert rec["hint_tx"] == TX
    assert "get_logs" not in chain.calls


def test_hint_dropped_after_timeout_then_scan_resumes():
    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], TX, now=0.0)
    step(pending, receipts, chain, rec, now=HINT_TIMEOUT_S)
    assert rec["hint_tx"] is None
    assert "get_logs" in chain.calls


def test_hint_reverted():
    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], TX, now=0.0)
    chain.receipts[TX] = receipt_with(status="0x0")
    out = step(pending, receipts, chain, rec)
    assert out.error == "tx_reverted" and rec["hint_tx"] is None
    assert out.hint_tx == TX  # names the hint judged, as on Solana


def test_hint_not_a_payment():
    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], TX, now=0.0)
    chain.receipts[TX] = receipt_with(transfer_log(USDT, PAYER, DEV, UNITS, block=998, tx_hash=TX))
    out = step(pending, receipts, chain, rec)
    assert out.error == "tx_not_a_payment" and rec["hint_tx"] is None
    assert out.hint_tx == TX  # names the hint judged, as on Solana


def test_hint_from_before_session_is_rejected():
    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], TX, now=0.0)
    chain.receipts[TX] = receipt_with(transfer_log(USDC, PAYER, DEV, UNITS, block=900, tx_hash=TX))
    assert step(pending, receipts, chain, rec).error == "tx_not_a_payment"


def test_hint_accepts_any_amount_at_or_above_min_price():
    # exchange payer: could not send the encoded decimals, sent a round 5.00
    pending, receipts, chain, rec = setup(head=1000)
    set_hint(pending, rec["id"], TX, now=0.0)
    chain.receipts[TX] = receipt_with(transfer_log(USDC, PAYER, DEV, 5_000_000, block=999, tx_hash=TX))
    out = step(pending, receipts, chain, rec)
    assert out.status == "confirming"
    assert rec["match"]["value"] == 5_000_000


def test_hint_underpaid_unlock_is_final():
    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], TX, now=0.0)
    chain.receipts[TX] = receipt_with(transfer_log(USDC, PAYER, DEV, 4_000_000, block=999, tx_hash=TX))
    out = step(pending, receipts, chain, rec)
    assert out.status == "underpaid"
    assert out.result.status == "underpaid"
    assert out.result.amount == Decimal("4.000000")
    assert out.result.tx_hash == TX and out.result.payer == PAYER
    assert pending == {} and receipts == {}


def test_hint_tip_accepts_any_positive_amount():
    pending, receipts, chain, rec = setup(kind="tip", item=None, code=0, price="3.00", head=1000)
    set_hint(pending, rec["id"], TX, now=0.0)
    chain.receipts[TX] = receipt_with(transfer_log(USDC, PAYER, DEV, 1_000_000, block=999, tx_hash=TX))
    assert step(pending, receipts, chain, rec).status == "confirming"


def test_hint_with_several_transfers_takes_largest_qualifying():
    pending, receipts, chain, rec = setup(head=1000)
    set_hint(pending, rec["id"], TX, now=0.0)
    chain.receipts[TX] = receipt_with(
        transfer_log(USDC, PAYER, DEV, 1_000_000, block=999, tx_hash=TX, log_index=0),
        transfer_log(USDC, PAYER, DEV, UNITS, block=999, tx_hash=TX, log_index=1),
    )
    step(pending, receipts, chain, rec)
    assert rec["match"]["log_index"] == 1


def test_scan_step_is_bounded_by_calls_when_rpc_reports_its_range():
    from scfg_core.watcher import MAX_SCAN_CALLS
    pending, receipts, chain, rec = setup(head=995 + 100_000)
    chain.log_range = 50                      # a stingy public endpoint
    step(pending, receipts, chain, rec)
    assert rec["cursor"] == 995 + 50 * MAX_SCAN_CALLS
