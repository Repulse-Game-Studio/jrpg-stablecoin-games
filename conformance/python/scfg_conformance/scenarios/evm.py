"""EVM payments through the Engine: sessions, the watcher's scan and hint paths, confirmations,
reorgs, expiry, restore and re-verification. Arc has its own module (arc.py)."""

from __future__ import annotations

from scfg_core.sessions import HINT_TIMEOUT_S, SCAN_HORIZON_S, SESSION_WINDOW_S
from scfg_core.watcher import MAX_SCAN_CALLS, SCAN_STEP_BLOCKS

from ..scenario import EPOCH, scenario
from .fixtures import CHAPTER2, DEV_EVM, DEV_SOL, PAYER_EVM, covers, tx_hash

OTHER_EVM = "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359"


def _mainnet(s, chain="arbitrum", *, items=(CHAPTER2,), **config):
    """An EVM-only mainnet game, one node for `chain`; returns (node, USDC address)."""
    s.config(wallet=DEV_EVM, **config)
    for item in items:
        s.item(**item)
    return s.evm(chain), s.chain(chain).tokens["USDC"].address


def _outcome(s):
    """The single session outcome of a tick (the window-expiry notice, if any, is ignored)."""
    [out] = [o for o in s.tick() if o.status != "expired"]
    return out


# --- happy paths ----------------------------------------------------------------------------


@scenario("evm/unlock_paid_exact", covers=(
    covers("engine", "start_unlock_session_uses_item_defaults",
           "end_to_end_tick_pays_and_updates_snapshot")
    + covers("watcher_awaiting", "scan_without_payment_advances_cursor",
             "scan_matches_exact_amount_and_starts_confirming")
    + covers("watcher_confirming", "counts_confirmations_until_required",
             "paid_writes_receipt_and_removes_session")
    + covers("sessions", "create_session_record_shape")
    + covers("arc_logs", "parse_transfer_plain_chain_unchanged")
    + covers("backends", "builtin_chains_are_evm")
    + covers("evm_logs", "missing_removed_field_is_fine")))
def _unlock_paid_exact(s):
    """An unlock paid with exactly the encoded amount: found by the scan, counted to the
    chain's required confirmations (20 on Arbitrum) and then stored as a receipt."""
    node, usdc = _mainnet(s)
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    assert (rec["start_block"], rec["units"] // 10_000) == (995, 499)
    out = _outcome(s)
    assert out.status == "waiting"
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["cursor"] == 1001
    s.advance(10)
    node.mine(1)                   # head 1001: the next block holds the payment
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=1001, tx_hash=tx_hash(1),
                      log_index=2)
    out = _outcome(s)
    assert (out.status, out.confirmations, out.required) == ("confirming", 1, 20)
    assert s.steps[-1]["snapshot"]["owned"] == []
    node.mine(9)
    out = _outcome(s)
    assert (out.status, out.confirmations) == ("confirming", 10)
    node.mine(10)                  # head 1020: exactly 20 confirmations of block 1001
    out = _outcome(s)
    assert out.status == "paid" and out.result.tx_hash == tx_hash(1), out
    snap = s.steps[-1]["snapshot"]
    assert snap["owned"] == ["chapter2"] and snap["pending"] == {}
    assert list(snap["receipts"].values())[0]["log_index"] == 2


@scenario("evm/tip_paid_at_preset", covers=(
    covers("engine", "start_tip_session")
    + covers("sessions", "tip_session_encodes_code_zero")))
def _tip_paid_at_preset(s):
    """A 3 USDC tip (a preset amount) has code 0, no item, and adds to the tip total."""
    node, usdc = _mainnet(s, "base")
    rec = s.start_session("tip", "base", amount="3", token="USDC")
    assert (rec["code"], rec["title"], rec["price"], rec["item"]) == (0, "Tip", "3.00", None)
    assert rec["units"] // 100 == 30_000
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=998, tx_hash=tx_hash(2))
    required = s.chain("base").confirmations
    node.mine(required)
    out = _outcome(s)
    assert out.status == "paid", out
    snap = s.steps[-1]["snapshot"]
    assert snap["owned"] == [] and snap["tip_total"].startswith("3.0000")


@scenario("evm/session_ids_and_nonces", covers=covers("sessions", "random_nonce_and_unique_ids"))
def _session_ids_and_nonces(s):
    """Each session draws its nonce (0..99) and id from the entropy source; ids stay distinct."""
    _mainnet(s)
    recs = [s.start_session("unlock", "arbitrum", item="chapter2") for _ in range(12)]
    assert len({r["id"] for r in recs}) == 12 and all(0 <= r["nonce"] <= 99 for r in recs)
    assert len({r["nonce"] for r in recs}) > 1


@scenario("evm/start_block_margin", covers=covers("backends", "evm_start_notes_block_with_margin"))
def _start_block_margin(s):
    """A session starts 5 blocks before the head, never below block 0."""
    _mainnet(s, "arbitrum")
    low = s.evm("base")
    low.head = 2
    assert s.start_session("unlock", "arbitrum", item="chapter2")["start_block"] == 995
    assert s.start_session("unlock", "base", item="chapter2")["start_block"] == 0


@scenario("evm/start_rejections", covers=(
    covers("engine", "start_session_rejects", "start_session_rejects_disabled_chain")))
def _start_rejections(s):
    """Starting a session is refused, with a stable code and no session left behind, for an
    unknown item, a price below min_price, a tip with no amount, no token or too small, an
    unknown kind, and a token or chain the game does not offer
    (`chain.token_unavailable` is raised by the Engine itself)."""
    _mainnet(s, chains=["base", "arbitrum"])
    cases = [
        (("unlock", "arbitrum"), {"item": "missing"}, "item.unknown"),
        (("unlock", "arbitrum"), {"item": "chapter2", "amount": "1.00"}, "payment.below_min_price"),
        (("tip", "arbitrum"), {"token": "USDC"}, "tip.needs_amount_and_token"),
        (("tip", "arbitrum"), {"amount": "1"}, "tip.needs_amount_and_token"),
        (("tip", "arbitrum"), {"amount": "0.49", "token": "USDC"}, "tip.below_minimum"),
        (("refund", "arbitrum"), {}, "payment.bad_kind"),
        (("tip", "base"), {"amount": "3", "token": "USDT"}, "chain.token_unavailable"),
        (("tip", "solana"), {"amount": "3", "token": "USDC"}, "chain.token_unavailable"),
        (("unlock", "polygon"), {"item": "chapter2"}, "chain.token_unavailable"),  # not enabled
    ]
    for args, kwargs, code in cases:
        assert s.start_session(*args, **kwargs) is None
        assert s.steps[-1]["raises"] == {"type": "ConfigError", "code": code}, (args, kwargs)
        assert s.steps[-1]["calls"] == []
    assert s.steps[-1]["snapshot"]["pending"] == {}


# --- amounts: what the scan and a hint accept --------------------------------------------


@scenario("evm/scan_ignores_inexact_amount", covers=covers(
    "watcher_awaiting", "scan_ignores_wrong_amount_token_recipient_and_old_blocks"))
def _scan_ignores_inexact_amount(s):
    """The scan matches only the exact encoded amount: one unit more or less, or a round
    price, is not found."""
    node, usdc = _mainnet(s)
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    for n, value in enumerate([rec["units"] + 1, rec["units"] - 1, 5_000_000], start=1):
        node.add_transfer(usdc, PAYER_EVM, DEV_EVM, value, block=998, tx_hash=tx_hash(n))
    out = _outcome(s)
    assert out.status == "waiting"
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["match"] is None


def _ignored_transfer(s, make):
    """A transfer that must be ignored by both the scan and a hinted-transaction check."""
    node, usdc = _mainnet(s)
    usdt = s.chain("arbitrum").tokens["USDT"].address
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    make(node, rec["units"], usdc, usdt)
    assert _outcome(s).status == "waiting"
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["match"] is None
    s.report_hint(rec["id"], tx_hash(1))
    out = _outcome(s)
    assert out.error == "tx_not_a_payment" and out.hint_tx == tx_hash(1), out
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["hint_tx"] is None


@scenario("evm/wrong_token", covers=covers(
    "watcher_awaiting", "scan_ignores_wrong_amount_token_recipient_and_old_blocks"))
def _wrong_token(s):
    """A transfer of another token (USDT for a USDC unlock) is not a payment."""
    _ignored_transfer(s, lambda node, units, usdc, usdt: node.add_transfer(
        usdt, PAYER_EVM, DEV_EVM, units, block=998, tx_hash=tx_hash(1)))


@scenario("evm/wrong_recipient", covers=covers(
    "watcher_awaiting", "scan_ignores_wrong_amount_token_recipient_and_old_blocks"))
def _wrong_recipient(s):
    """A transfer to another wallet is not a payment."""
    _ignored_transfer(s, lambda node, units, usdc, usdt: node.add_transfer(
        usdc, PAYER_EVM, OTHER_EVM, units, block=998, tx_hash=tx_hash(1)))


@scenario("evm/transfer_before_start_block", covers=(
    covers("watcher_awaiting", "scan_ignores_wrong_amount_token_recipient_and_old_blocks",
           "hint_from_before_session_is_rejected")))
def _transfer_before_start_block(s):
    """A transfer from before the session's start block (995) is not a payment, even with the
    right amount, token and recipient."""
    _ignored_transfer(s, lambda node, units, usdc, usdt: node.add_transfer(
        usdc, PAYER_EVM, DEV_EVM, units, block=990, tx_hash=tx_hash(1)))


@scenario("evm/scan_picks_earliest_and_skips_known_receipts", covers=(
    covers("watcher_awaiting", "scan_picks_earliest_match",
           "scan_skips_transfer_already_used_as_receipt")))
def _scan_picks_earliest(s):
    """Of several exact transfers the scan takes the earliest by (block, log index), and it
    skips a transfer that is already a stored receipt."""
    s.config(wallet=DEV_EVM)
    s.item(**CHAPTER2)
    node, usdc = s.evm("arbitrum"), s.chain("arbitrum").tokens["USDC"].address
    used = {"chain": "arbitrum", "tx_hash": tx_hash(9), "log_index": 2, "block": 996,
            "kind": "tip", "item": None, "amount": "1.000000", "token": "USDC",
            "payer": PAYER_EVM, "recipient": DEV_EVM, "source": "payment"}
    s.initial(receipts={f"arbitrum:{tx_hash(9)}:2": used})
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=996, tx_hash=tx_hash(9),
                      log_index=2)     # already a receipt: skipped
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=999, tx_hash=tx_hash(2))
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=997, tx_hash=tx_hash(1))
    assert _outcome(s).status == "confirming"
    match = s.steps[-1]["snapshot"]["pending"][rec["id"]]["match"]
    assert match["tx_hash"] == tx_hash(1) and match["block"] == 997


@scenario("evm/scan_is_bounded", covers=covers(
    "watcher_awaiting", "scan_does_nothing_when_cursor_ahead_of_head", "scan_is_bounded_per_step",
    "scan_step_is_bounded_by_calls_when_rpc_reports_its_range"))
def _scan_is_bounded(s):
    """A scan reads at most 10 000 blocks per tick, at most 5 range-limited calls when the
    endpoint reports its maximum range, and nothing while the endpoint lags the cursor."""
    node, _ = _mainnet(s)
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    cursor = lambda: s.steps[-1]["snapshot"]["pending"][rec["id"]]["cursor"]  # noqa: E731
    node.head = 900                    # an endpoint behind the one that set start_block
    assert _outcome(s).status == "waiting" and cursor() == 995
    assert [c["method"] for c in s.steps[-1]["calls"]] == ["eth_blockNumber"]
    node.head = 995 + 3 * SCAN_STEP_BLOCKS
    _outcome(s)
    assert cursor() == 995 + SCAN_STEP_BLOCKS
    node.head = cursor() + 100_000
    for url in s.chain("arbitrum").rpc:
        node.endpoint(url).max_log_range = 1000     # below the 2000 the client assumes
    _outcome(s)                                      # learns the range, still scans 10 000
    assert cursor() == 995 + 2 * SCAN_STEP_BLOCKS
    before = cursor()
    _outcome(s)
    assert cursor() == before + 1000 * MAX_SCAN_CALLS


# --- hints (a transaction hash from the browser page or pasted by the player) ---------------


def _hinted(s, tx, units=None, kind="unlock", **start):
    """Start a session and report `tx` as its hint; returns the record."""
    if kind == "tip":
        rec = s.start_session("tip", "arbitrum", amount="3", token="USDC")
    else:
        rec = s.start_session("unlock", "arbitrum", item="chapter2", **start)
    s.report_hint(rec["id"], tx)
    return rec


@scenario("evm/hint_verified", covers=(
    covers("engine", "report_hint_and_ui_status")
    + covers("backends", "engine_report_hint_uses_the_chain_backend")
    + covers("sessions", "hints", "bad_tx_hash")))
def _hint_verified(s):
    """A reported hash is normalised to lowercase, stamped with the clock, and a mined
    transaction with the exact payment moves the session straight to confirming without
    any log scan. A malformed hash is refused."""
    node, usdc = _mainnet(s)
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    shout = "0x" + tx_hash(1)[2:].upper()
    s.advance(7)
    s.report_hint(rec["id"], "  " + shout + " ")
    s.set_ui_status(rec["id"], "pending")
    stored = s.steps[-1]["snapshot"]["pending"][rec["id"]]
    assert (stored["hint_tx"], stored["hint_at"], stored["ui_status"]) == (
        tx_hash(1), EPOCH + 7, "pending")
    s.expect("other").report_hint(rec["id"], "not-a-hash")
    s.expect("other").report_hint(rec["id"], "0x1234")
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=998, tx_hash=tx_hash(1))
    out = _outcome(s)
    assert out.status == "confirming", out
    assert [c["method"] for c in s.steps[-1]["calls"]].count("eth_getLogs") == 0
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["match"]["tx_hash"] == tx_hash(1)


@scenario("evm/hint_reverted", covers=covers("watcher_awaiting", "hint_reverted"))
def _hint_reverted(s):
    """A hinted transaction that reverted is reported as tx_reverted and the hint is dropped."""
    node, usdc = _mainnet(s)
    rec = _hinted(s, tx_hash(1))
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=998, tx_hash=tx_hash(1))
    node.receipts[tx_hash(1)]["status"] = "0x0"
    out = _outcome(s)
    assert out.error == "tx_reverted" and out.hint_tx == tx_hash(1), out
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["hint_tx"] is None


@scenario("evm/hint_not_a_payment", covers=covers("watcher_awaiting", "hint_not_a_payment"))
def _hint_not_a_payment(s):
    """A mined hinted transaction without a qualifying transfer is tx_not_a_payment."""
    node, _ = _mainnet(s)
    usdt = s.chain("arbitrum").tokens["USDT"].address
    rec = _hinted(s, tx_hash(1))
    node.add_transfer(usdt, PAYER_EVM, DEV_EVM, rec["units"], block=998, tx_hash=tx_hash(1))
    out = _outcome(s)
    assert out.error == "tx_not_a_payment" and out.hint_tx == tx_hash(1), out
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["hint_tx"] is None


@scenario("evm/hint_not_mined_then_dropped", covers=(
    covers("watcher_awaiting", "hint_not_mined_yet_keeps_waiting_without_scanning",
           "hint_dropped_after_timeout_then_scan_resumes")
    + covers("sessions", "constants")))
def _hint_not_mined_then_dropped(s):
    """A hint whose transaction is unknown is waited for without scanning; after 30 minutes
    it is dropped and the scan resumes."""
    _mainnet(s)
    rec = _hinted(s, tx_hash(1))
    s.advance(5)
    out = _outcome(s)
    assert out.status == "waiting" and out.error is None
    assert [c["method"] for c in s.steps[-1]["calls"]] == ["eth_getTransactionReceipt"]
    s.advance(HINT_TIMEOUT_S - 6)           # 1799 s after the hint
    _outcome(s)
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["hint_tx"] == tx_hash(1)
    s.advance(1)
    _outcome(s)
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["hint_tx"] is None
    assert "eth_getLogs" in [c["method"] for c in s.steps[-1]["calls"]]   # same tick


@scenario("evm/overpaid_above_price", covers=covers(
    "watcher_awaiting", "hint_accepts_any_amount_at_or_above_min_price"))
def _overpaid_above_price(s):
    """A hinted payment above the price (an exchange sending a round 5.00) is accepted,
    and the receipt records what was actually paid."""
    node, usdc = _mainnet(s)
    rec = _hinted(s, tx_hash(1))
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 5_000_000, block=999, tx_hash=tx_hash(1))
    assert _outcome(s).status == "confirming"
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["match"]["value"] == 5_000_000
    node.mine(20)
    out = _outcome(s)
    assert out.status == "paid" and str(out.result.amount) == "5.000000", out


@scenario("evm/underpaid_hint_is_final", covers=covers(
    "watcher_awaiting", "hint_underpaid_unlock_is_final"))
def _underpaid_hint_is_final(s):
    """A hinted unlock below its minimum price ends as underpaid: the session is removed and
    no receipt is stored."""
    node, usdc = _mainnet(s)
    _hinted(s, tx_hash(1))
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 4_000_000, block=999, tx_hash=tx_hash(1))
    out = _outcome(s)
    assert out.status == "underpaid" and out.result.tx_hash == tx_hash(1), out
    assert str(out.result.amount) == "4.000000" and out.result.payer == PAYER_EVM
    snap = s.steps[-1]["snapshot"]
    assert snap["pending"] == {} and snap["receipts"] == {}


@scenario("evm/min_price_sale", covers=covers(
    "payments", "unlock_sale_price_allowed_down_to_min_price"))
def _min_price_sale(s):
    """An item with a minimum price below its price: a hinted payment at or above the minimum
    (but below the price) is accepted; below the minimum it is underpaid."""
    sale = dict(CHAPTER2, min_price="3.99")
    node, usdc = _mainnet(s, items=(sale,))
    ok = _hinted(s, tx_hash(1))
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 4_000_000, block=999, tx_hash=tx_hash(1))
    out = _outcome(s)
    assert out.status == "confirming", out
    assert s.steps[-1]["snapshot"]["pending"][ok["id"]]["match"]["value"] == 4_000_000
    short = _hinted(s, tx_hash(2))
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 3_980_000, block=999, tx_hash=tx_hash(2))
    outs = {o.session_id: o for o in s.tick()}
    assert outs[short["id"]].status == "underpaid", outs
    assert s.start_session("unlock", "arbitrum", item="chapter2", amount="3.99")["price"] == "3.99"
    assert s.start_session("unlock", "arbitrum", item="chapter2", amount="3.98") is None
    assert s.steps[-1]["raises"]["code"] == "payment.below_min_price"


@scenario("evm/hint_picks_largest_transfer", covers=covers(
    "watcher_awaiting", "hint_with_several_transfers_takes_largest_qualifying",
    "hint_tip_accepts_any_positive_amount"))
def _hint_picks_largest(s):
    """A hinted transaction with several qualifying transfers is judged by the largest. A
    tip accepts any positive amount."""
    node, usdc = _mainnet(s)
    rec = _hinted(s, tx_hash(1))
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 1_000_000, block=999, tx_hash=tx_hash(1),
                      log_index=0)
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=999, tx_hash=tx_hash(1),
                      log_index=1)
    assert _outcome(s).status == "confirming"
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["match"]["log_index"] == 1
    tip = _hinted(s, tx_hash(2), kind="tip")
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 1_000_000, block=999, tx_hash=tx_hash(2))
    outs = {o.session_id: o for o in s.tick()}
    assert outs[tip["id"]].status == "confirming", outs


# --- confirming: reorgs, lagging endpoints, outages --------------------------------------------


def _confirming(s):
    """An unlock matched at block 998 with 20 confirmations required; returns the setup."""
    node, usdc = _mainnet(s)
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=998, tx_hash=tx_hash(1),
                      log_index=2)
    assert _outcome(s).status == "confirming"
    return node, usdc, rec


@scenario("evm/reorg_during_confirming", covers=covers(
    "watcher_confirming", "reorg_returns_to_awaiting_and_rewinds_cursor",
    "reorg_then_transfer_reappears_in_new_block"))
def _reorg_during_confirming(s):
    """When the matched block's hash changes the session is `reorged`: back to awaiting with
    the cursor rewound to the match block. If the transfer reappears in a later block it is
    matched again there."""
    node, usdc, rec = _confirming(s)
    node.reorg(998)
    node.logs.clear()
    del node.receipts[tx_hash(1)]
    node.mine(5)
    assert _outcome(s).status == "reorged"
    pending = s.steps[-1]["snapshot"]["pending"][rec["id"]]
    assert (pending["state"], pending["match"], pending["cursor"]) == ("awaiting", None, 998)
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=1010, tx_hash=tx_hash(1))
    node.mine(10)
    assert _outcome(s).status == "confirming"
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["match"]["block"] == 1010
    node.reorg(1010)           # the log survives (re-included at the same height)
    assert _outcome(s).status == "reorged"


@scenario("evm/lagging_endpoint_is_not_a_reorg", covers=covers(
    "watcher_confirming", "lagging_endpoint_below_match_block_is_not_a_reorg"))
def _lagging_endpoint(s):
    """An endpoint whose head is below the match block reports 0 confirmations; the session
    stays confirming."""
    node, _, rec = _confirming(s)
    node.head = 990
    out = _outcome(s)
    assert (out.status, out.confirmations) == ("confirming", 0)
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["state"] == "confirming"


@scenario("evm/outage_keeps_session_state", covers=(
    covers("watcher_awaiting", "network_outage_is_reported_and_state_kept")
    + covers("watcher_confirming", "outage_while_confirming_keeps_match")))
def _outage_keeps_state(s):
    """With every endpoint down the tick reports network_unavailable and the session
    keeps its cursor, state and match."""
    node, usdc = _mainnet(s)
    first = s.start_session("unlock", "arbitrum", item="chapter2")
    for url in s.chain("arbitrum").rpc:
        node.endpoint(url).down = True
    out = _outcome(s)
    assert out.error == "network_unavailable"
    pending = s.steps[-1]["snapshot"]["pending"][first["id"]]
    assert (pending["cursor"], pending["state"]) == (995, "awaiting")
    for url in s.chain("arbitrum").rpc:
        node.endpoint(url).down = False
    s.advance(600)                         # past every cooldown
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, first["units"], block=998, tx_hash=tx_hash(1))
    assert _outcome(s).status == "confirming"
    for url in s.chain("arbitrum").rpc:
        node.endpoint(url).down = True
    out = _outcome(s)
    assert out.error == "network_unavailable"
    pending = s.steps[-1]["snapshot"]["pending"][first["id"]]
    assert pending["state"] == "confirming" and pending["match"] is not None


@scenario("evm/cancelled_does_not_stop_confirmation", covers=covers(
    "watcher_confirming", "cancelled_or_expired_ui_status_does_not_stop_confirmation"))
def _cancelled_still_confirms(s):
    """A session the player cancelled in the UI is still confirmed and paid if the money
    arrived."""
    node, _, rec = _confirming(s)
    s.set_ui_status(rec["id"], "cancelled")
    s.advance(4000)
    node.mine(100)
    assert [o.status for o in s.tick()] == ["paid"]


@scenario("evm/confirmations_override_respected", covers=covers(
    "watcher_confirming", "confirmations_override_is_respected"))
def _confirmations_override(s):
    """A game's confirmation override (3 instead of 20) decides when the payment is final."""
    node, usdc = _mainnet(s, confirmations={"arbitrum": 3})
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=1000, tx_hash=tx_hash(1))
    out = _outcome(s)
    assert (out.status, out.confirmations, out.required) == ("confirming", 1, 3)
    node.mine(2)
    assert _outcome(s).status == "paid"


# --- expiry, horizon, discard ----------------------------------------------------------------


@scenario("evm/window_expiry_reported_once", covers=(
    covers("engine", "window_expiry_is_reported_once_and_scanning_continues")
    + covers("sessions", "constants", "window_and_horizon")))
def _window_expiry(s):
    """The 30-minute window closing is reported exactly once (at 1800 s, not 1799 s) while the
    session keeps being scanned up to the 24-hour horizon."""
    node, usdc = _mainnet(s)
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    assert (SESSION_WINDOW_S, SCAN_HORIZON_S, HINT_TIMEOUT_S) == (1800, 86400, 1800)
    s.advance(SESSION_WINDOW_S - 1)
    assert [o.status for o in s.tick()] == ["waiting"]
    s.advance(1)
    outs = s.tick()
    assert [o.status for o in outs] == ["waiting", "expired"]
    assert outs[1].result.status == "expired" and outs[1].result.session_id == rec["id"]
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["ui_status"] == "expired"
    assert [o.status for o in s.tick()] == ["waiting"]          # not repeated
    node.mine(1)
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, rec["units"], block=node.head, tx_hash=tx_hash(1))
    assert [o.status for o in s.tick()] == ["confirming"]       # still watched after expiry


@scenario("evm/cancelled_session_no_expiry", covers=covers(
    "engine", "cancelled_sessions_do_not_report_expiry"))
def _cancelled_no_expiry(s):
    """A cancelled session never reports the window expiring."""
    _mainnet(s)
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    s.set_ui_status(rec["id"], "cancelled")
    s.advance(31 * 60)
    assert [o.status for o in s.tick()] == ["waiting"]


@scenario("evm/horizon_closes_session", covers=(
    covers("watcher_awaiting", "horizon_closes_session")
    + covers("sessions", "constants", "window_and_horizon")))
def _horizon_closes(s):
    """An untouched session is scanned until 24 hours after it started, then closed and
    removed; one second earlier it is still waiting."""
    _mainnet(s)
    rec = s.start_session("unlock", "arbitrum", item="chapter2")
    s.advance(SCAN_HORIZON_S - 1)
    assert [o.status for o in s.tick()] == ["waiting", "expired"]
    s.advance(1)
    [closed] = s.tick()
    assert closed.status == "closed" and closed.result is None
    assert rec["id"] not in s.steps[-1]["snapshot"]["pending"]


@scenario("evm/unwatchable_session_closes_at_horizon", covers=covers(
    "backends", "reverify_keeps_receipts_of_a_family_without_wallets"))
def _unwatchable_horizon(s):
    """A pending EVM session in a build with no EVM wallet cannot be watched: it is kept
    silently until its horizon, then closed. Stored EVM receipts are kept untouched, with no
    request for them."""
    s.config(solana_wallet=DEV_SOL)
    sol = s.solana()
    session = {"id": "ps_0123456789abcdef", "kind": "unlock", "item": "chapter2",
               "title": "Chapter 2", "token": "USDC", "chain": "arbitrum", "price": "4.99",
               "code": 37, "nonce": 42, "units": 4_993_742, "start_block": 995, "cursor": 995,
               "created_at": EPOCH, "state": "awaiting", "ui_status": "expired",
               "hint_tx": None, "hint_at": None, "match": None}
    receipt = {"chain": "arbitrum", "tx_hash": tx_hash(1), "log_index": 0, "block": 5,
               "kind": "tip", "item": None, "amount": "1.000000", "token": "USDC",
               "payer": PAYER_EVM, "recipient": DEV_EVM, "source": "payment"}
    s.initial(pending={session["id"]: session}, receipts={f"arbitrum:{tx_hash(1)}:0": receipt})
    assert s.reverify() == []
    assert all(c["url"] in sol.urls for c in s.steps[-1]["calls"])      # nothing for Arbitrum
    assert len(s.steps[-1]["snapshot"]["receipts"]) == 1
    s.advance(SCAN_HORIZON_S - 1)
    assert s.tick() == []
    assert s.steps[-1]["calls"] == []
    s.advance(1)
    [closed] = s.tick()
    assert closed.status == "closed" and s.steps[-1]["snapshot"]["pending"] == {}


@scenario("evm/session_actions", covers=(
    covers("engine", "set_ui_status_ignores_finished_sessions",
           "discard_session_only_drops_untouched_sessions")
    + covers("sessions", "set_hint_unknown_session", "ui_status")))
def _session_actions(s):
    """discard_session drops only an untouched awaiting session (a hinted or a confirming
    one stays, and a second discard is harmless). set_ui_status ignores unknown sessions and
    refuses unknown statuses; report_hint refuses an unknown session."""
    node, usdc = _mainnet(s)
    a = s.start_session("unlock", "arbitrum", item="chapter2")
    b = s.start_session("unlock", "arbitrum", item="chapter2")
    c = s.start_session("tip", "arbitrum", amount="3", token="USDC")
    s.report_hint(b["id"], tx_hash(7))
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, c["units"], block=998, tx_hash=tx_hash(8))
    assert any(o.session_id == c["id"] and o.status == "confirming" for o in s.tick())
    for rec in (a, b, c, a):
        s.discard_session(rec["id"])
    assert sorted(s.steps[-1]["snapshot"]["pending"]) == sorted([b["id"], c["id"]])
    s.discard_session("ps_gone")
    s.set_ui_status("ps_gone", "cancelled")              # a finished session: no error
    s.expect("other").set_ui_status(b["id"], "paid")     # not a UI status
    s.expect("other").report_hint("ps_missing", tx_hash(1))
    assert s.steps[-1]["snapshot"]["pending"][b["id"]]["ui_status"] == "open"


# --- crashes ----------------------------------------------------------------------------------


@scenario("evm/crash_in_one_session", covers=covers(
    "engine", "tick_survives_any_transport_exception_in_each_session"))
def _crash_in_one_session(s):
    """An unexpected failure while advancing one session (here a node returning a log whose
    logIndex is `0xzz`, malformed hex that parse_transfer_log raises on) is contained:
    that session reports waiting with error internal_error, is counted in internal_errors,
    and the other sessions in the same tick still advance."""
    arb, arb_usdc = _mainnet(s, "arbitrum")
    base = s.evm("base")
    base_usdc = s.chain("base").tokens["USDC"].address
    broken = s.start_session("unlock", "arbitrum", item="chapter2")
    fine = s.start_session("tip", "base", amount="3", token="USDC")
    log = arb.add_transfer(arb_usdc, PAYER_EVM, DEV_EVM, broken["units"], block=998,
                           tx_hash=tx_hash(1))
    log["logIndex"] = "0xzz"
    base.add_transfer(base_usdc, PAYER_EVM, DEV_EVM, fine["units"], block=998, tx_hash=tx_hash(2))
    outs = {o.session_id: o for o in s.expect("internal_error").tick()}
    assert s.steps[-1]["internal_errors"] == 1
    assert (outs[broken["id"]].status, outs[broken["id"]].error) == ("waiting", "internal_error")
    assert outs[fine["id"]].status in ("confirming", "paid") and outs[fine["id"]].error is None
