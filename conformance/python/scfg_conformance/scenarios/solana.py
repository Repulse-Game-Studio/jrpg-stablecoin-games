"""Solana payments through the Engine: finding a payment (by reference, by hint or by scanning
the developer's token account), confirmation, the hint timeout, the scan walk, outages and
sessions the build cannot watch. Building the payment, readiness, restore, re-verification and
the getTransaction version/history rules are in solana_rpc.py."""

from __future__ import annotations

from scfg_core.backends.solana import SOLANA_HINT_TIMEOUT_S
from scfg_core.sessions import SCAN_HORIZON_S
from scfg_core.solana.base58 import b58decode
from scfg_core.solana.pda import associated_token_address

from ..scenario import EPOCH, scenario
from .fixtures import CHAPTER2, DEV_EVM, DEV_SOL, PAYER_EVM, PAYER_SOL, covers, signature, tx_hash

WSOL = "So11111111111111111111111111111111111111112"
FAILED = "AccountNotFound"      # a failed transaction's `err`


def game(s, *, testnet=True, **config):
    """A Solana-only game with Chapter 2; returns (node, USDC mint, USDC token account of the
    developer). Devnet has one RPC URL; mainnet has two, served by the same node."""
    s.config(solana_wallet=DEV_SOL, testnet=testnet, **config)
    s.item(**CHAPTER2)
    node = s.solana()
    mint = s.chain("solana").tokens["USDC"].address
    return node, mint, associated_token_address(DEV_SOL, mint)


def pay(node, mint, n, amount, *, to=DEV_SOL, by=PAYER_SOL, **kw):
    return node.pay(signature(n), mint=mint, payer=by, owner=to, amount=amount, **kw)


def pending(s, rec):
    return s.steps[-1]["snapshot"]["pending"][rec["id"]]


def one(s):
    """The single outcome of a tick, ignoring the 30-minute window notice."""
    [out] = [o for o in s.tick() if o.status != "expired"]
    return out


def methods(s, name):
    return [c for c in s.steps[-1]["calls"] if c["method"] == name]


# --- finding a payment ------------------------------------------------------------------------


@scenario("solana/paid_by_reference", covers=(
    covers("engine_solana", "solana_payment_end_to_end")
    + covers("solana_backend", "start_fields")
    + covers("solana_watch", "waits_when_nothing_arrived",
             "reference_payment_confirms_then_finalizes_into_a_receipt")
    + covers("solana_rpc", "get_transaction_asks_for_newest_version_and_json", "small_helpers")))
def _paid_by_reference(s):
    """An unlock found through its Solana Pay reference: the session notes the slot (150
    back), a fresh reference and the developer's USDC token account; the newest signature there
    becomes the scan cursor, or none when the account has no history. Confirmed with 1
    confirmation, then finalized into a receipt, which the launch check and a restore agree
    with. Mainnet: the first of two endpoints answers everything."""
    node, mint, dest = game(s, testnet=False)
    for token in s.chain("solana").tokens.values():
        node.accounts.add(associated_token_address(DEV_SOL, token.address))
    first = s.start_session("unlock", "solana", item="chapter2")
    assert (first["start_block"], first["dest"], first["scan_cursor"]) == (850, dest, None)
    s.discard_session(first["id"])
    pay(node, mint, 9, 1_230_000, slot=990, addresses=[dest], status="finalized")
    rec = s.start_session("unlock", "solana", item="chapter2")
    assert rec["scan_cursor"] == signature(9) and rec["reference"] != first["reference"]
    assert rec["scan_before"] is None and len(b58decode(rec["reference"])) == 32
    assert one(s).status == "waiting"
    s.advance(5)
    node.slot += 10
    pay(node, mint, 1, rec["units"], addresses=[rec["reference"], rec["dest"]])
    out = one(s)
    assert (out.status, out.confirmations, out.required) == ("confirming", 1, 32)
    assert pending(s, rec)["match"]["tx_hash"] == signature(1)
    s.advance(13)
    node.slot += 32
    node.finalize(signature(1))
    out = one(s)
    assert out.status == "paid" and out.result.chain == "solana"
    assert s.steps[-1]["snapshot"]["owned"] == ["chapter2"]
    assert s.reverify() == [] and s.steps[-1]["snapshot"]["unready"] == []
    [cand] = s.restore_lookup(signature(1))
    assert cand.already_restored and cand.item == "chapter2"


@scenario("solana/paid_by_hint", covers=(
    covers("engine_solana", "hint_on_solana_session_must_be_a_signature")
    + covers("solana_watch", "registered_and_normalizes_signatures",
             "good_hint_matches_round_amount")))
def _paid_by_hint(s):
    """The page reports the signature its wallet returned. A hash is refused, a signature
    with stray whitespace is stored trimmed, and the transaction is verified without any scan.
    A tip takes any positive amount; an unlock accepts a round 5.00 (at or above the price)."""
    node, mint, dest = game(s)
    tip = s.start_session("tip", "solana", amount="1", token="USDC")
    unlock = s.start_session("unlock", "solana", item="chapter2")
    s.expect("other").report_hint(tip["id"], "0x" + "ab" * 32)
    assert pending(s, tip)["hint_tx"] is None
    s.report_hint(tip["id"], "  " + signature(7))
    assert pending(s, tip)["hint_tx"] == signature(7)
    s.report_hint(unlock["id"], signature(8))
    pay(node, mint, 7, 1_000_000)
    pay(node, mint, 8, 5_000_000)
    outs = {o.session_id: o for o in s.tick()}
    assert outs[tip["id"]].status == outs[unlock["id"]].status == "confirming"
    assert not methods(s, "getSignaturesForAddress")      # verified by hint: no lookup, no scan
    assert pending(s, unlock)["match"]["value"] == 5_000_000
    assert pending(s, tip)["hint_tx"] == signature(7)


# --- hints that do not pay ---------------------------------------------------------------------


@scenario("solana/hint_not_landed", covers=(
    covers("solana_watch", "hint_not_yet_visible_waits_then_expires",
           "hint_that_never_lands_expires_after_the_solana_timeout")))
def _hint_not_landed(s):
    """A wallet returns the signature before the transaction lands, and a dropped one never
    does. The hint is kept (and no scan runs) until SOLANA_HINT_TIMEOUT_S after it was
    reported, then dropped with tx_not_landed naming it; the page may then offer Pay again,
    and a second payment is found by the scan."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    s.report_hint(rec["id"], signature(1))
    s.advance(10)
    out = one(s)
    assert (out.status, out.error) == ("waiting", None)
    assert pending(s, rec)["hint_tx"] == signature(1)
    assert not [c for c in s.steps[-1]["calls"] if c["params"][0] == dest]      # no scan
    s.advance(SOLANA_HINT_TIMEOUT_S - 11)
    assert one(s).status == "waiting" and pending(s, rec)["hint_tx"] == signature(1)
    s.advance(1)
    out = one(s)
    assert (out.status, out.error, out.hint_tx) == ("waiting", "tx_not_landed", signature(1))
    assert pending(s, rec)["hint_tx"] is None
    pay(node, mint, 2, rec["units"], addresses=[dest])
    assert one(s).status == "confirming"


def _hint_expiry_lookup_fails(s, error):
    """The reference lookup fails on the very tick the hint expires. Later ticks are plain
    "waiting", so that tick must be the one to say tx_not_landed."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    s.report_hint(rec["id"], signature(1))
    node.endpoint(s.chain("solana").rpc[0]).errors["getSignaturesForAddress"] = error
    s.advance(SOLANA_HINT_TIMEOUT_S)
    out = one(s)
    assert (out.status, out.error, out.hint_tx) == ("waiting", "tx_not_landed", signature(1))
    assert pending(s, rec)["hint_tx"] is None
    s.advance(1)
    out = one(s)
    assert (out.status, out.error, out.hint_tx) == ("waiting", "network_unavailable", None)


@scenario("solana/hint_expiry_reference_rpc_error", covers=(
    covers("solana_watch", "hint_expiry_is_reported_even_if_the_reference_lookup_fails")))
def _hint_expiry_rpc_error(s):
    """The node answers the reference lookup with an RpcError on the hint's expiry tick."""
    _hint_expiry_lookup_fails(s, (-32005, "node is behind"))


@scenario("solana/hint_expiry_reference_unavailable", covers=(
    covers("solana_watch", "hint_expiry_is_reported_even_if_the_reference_lookup_fails")))
def _hint_expiry_unavailable(s):
    """The only endpoint is rate limited on the reference lookup (RpcUnavailable) on the
    hint's expiry tick; it stays cooled down on the next tick."""
    _hint_expiry_lookup_fails(s, (-32005, "rate limit exceeded"))


@scenario("solana/hint_lookup_keeps_failing", covers=(
    covers("solana_watch", "hint_whose_lookup_keeps_failing_still_expires")))
def _hint_lookup_keeps_failing(s):
    """An endpoint that refuses getTransaction with an RpcError (-32015 naming version 9, which
    the retry also gets) holds the hint while it is young (network_unavailable) but cannot
    keep it forever: past the timeout the hint is dropped as not landed."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    s.report_hint(rec["id"], signature(1))
    node.endpoint(s.chain("solana").rpc[0]).errors["getTransaction"] = (
        -32015, "Transaction version (9) is not supported by the requesting client.")
    s.advance(10)
    out = one(s)
    assert out.error == "network_unavailable" and pending(s, rec)["hint_tx"] == signature(1)
    assert [c["params"][1]["maxSupportedTransactionVersion"]
            for c in methods(s, "getTransaction")] == [1, 9]
    s.advance(SOLANA_HINT_TIMEOUT_S - 9)
    out = one(s)
    assert (out.status, out.error) == ("waiting", "tx_not_landed")
    assert pending(s, rec)["hint_tx"] is None


@scenario("solana/hint_superseded_by_reference", covers=(
    covers("solana_watch", "reference_payment_matches_while_a_hint_is_pending")))
def _hint_superseded(s):
    """The hinted transaction never lands and the player pays again: the reference lookup
    still runs while a hint is pending, matches the new payment and clears the hint."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    s.report_hint(rec["id"], signature(1))
    pay(node, mint, 2, rec["units"], addresses=[rec["reference"], dest])
    s.advance(10)
    out = one(s)
    assert (out.status, out.error) == ("confirming", None)
    assert pending(s, rec)["match"]["tx_hash"] == signature(2)
    assert pending(s, rec)["hint_tx"] is None


@scenario("solana/hint_verdicts", covers=(
    covers("solana_watch", "bad_hints_are_dropped_with_a_reason",
           "hint_verdicts_name_the_hint_they_are_about")))
def _hint_verdicts(s):
    """Each bad hint is dropped with its own reason and names the signature it is about: a
    failed transaction (tx_reverted), a payment to somebody else and one from before the
    session (tx_not_a_payment). Afterwards the session just waits."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, rec["units"], err=FAILED)
    pay(node, mint, 2, rec["units"], to=PAYER_SOL, by=DEV_SOL)
    pay(node, mint, 3, rec["units"], slot=100)
    for n, error in ((1, "tx_reverted"), (2, "tx_not_a_payment"), (3, "tx_not_a_payment")):
        s.advance(1)
        s.report_hint(rec["id"], signature(n))
        out = one(s)
        assert (out.status, out.error, out.hint_tx) == ("waiting", error, signature(n))
        assert pending(s, rec)["hint_tx"] is None
    s.advance(1)
    out = one(s)
    assert (out.status, out.error, out.hint_tx) == ("waiting", None, None)


# --- what counts as a payment -------------------------------------------------------------------


@scenario("solana/underpaid", covers=(
    covers("solana_watch", "reference_matches_any_amount_and_detects_underpayment")))
def _underpaid(s):
    """A transaction carrying the reference matches whatever its amount, so one below the
    price ends the session as underpaid (with its result), by reference or by hint, while 5.00
    is a normal match."""
    node, mint, dest = game(s)
    low = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, 1_000_000, addresses=[low["reference"]])
    out = one(s)
    assert out.status == "underpaid" and out.result.status == "underpaid"
    assert low["id"] not in s.steps[-1]["snapshot"]["pending"]
    high = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 2, 5_000_000, addresses=[high["reference"]])
    assert one(s).status == "confirming"
    hinted = s.start_session("unlock", "solana", item="chapter2")
    s.report_hint(hinted["id"], signature(3))
    pay(node, mint, 3, 2_000_000)
    outs = {o.session_id: o for o in s.tick()}
    assert outs[hinted["id"]].status == "underpaid"
    assert outs[high["id"]].status == "confirming"


@scenario("solana/wrong_mint", covers=(
    covers("solana_watch", "wrong_mint_on_the_reference_is_ignored")))
def _wrong_mint(s):
    """A payment of another token (wrapped SOL) that carries the reference is ignored, and as
    a hint it is not a payment."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, WSOL, 1, rec["units"], addresses=[rec["reference"]])
    assert one(s).status == "waiting"
    s.report_hint(rec["id"], signature(1))
    out = one(s)
    assert (out.status, out.error) == ("waiting", "tx_not_a_payment")


@scenario("solana/failed_tx", covers=(
    covers("solana_watch", "scan_skips_failed_signatures_without_fetching_them")))
def _failed_tx(s):
    """A failed transaction (err set) in a signature listing is skipped without being
    fetched, by the reference lookup and by the scan; the scan cursor moves past it."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, rec["units"], addresses=[rec["reference"], dest], err=FAILED)
    assert one(s).status == "waiting"
    assert not methods(s, "getTransaction")
    assert pending(s, rec)["scan_cursor"] == signature(1)
    pay(node, mint, 2, rec["units"], addresses=[dest])
    assert one(s).status == "confirming"
    assert pending(s, rec)["match"]["tx_hash"] == signature(2)


@scenario("solana/transfer_reading", covers=(
    covers("solana_watch", "transfer_to_reads_balance_changes")))
def _transfer_reading(s):
    """What a transaction paid is read from its token-balance changes: payer, amount, the
    index of the developer's token account (2) and the slot are recorded; a payment of the
    developer to someone else, another mint, and a transaction the listing calls fine but
    whose own meta says failed, are not payments."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, rec["units"], slot=950)
    s.report_hint(rec["id"], signature(1))
    assert one(s).status == "confirming"
    match = pending(s, rec)["match"]
    assert (match["value"], match["payer"], match["recipient"], match["log_index"],
            match["block"]) == (rec["units"], PAYER_SOL, DEV_SOL, 2, 950)
    other = s.start_session("unlock", "solana", item="chapter2")
    for n, kw in ((2, dict(to=PAYER_SOL, by=DEV_SOL)), (3, {})):
        pay(node, WSOL if n == 3 else mint, n, other["units"], **kw)
        s.advance(1)
        s.report_hint(other["id"], signature(n))
        outs = {o.session_id: o for o in s.tick()}
        assert outs[other["id"]].error == "tx_not_a_payment"
    failed = pay(node, mint, 4, other["units"], addresses=[other["reference"]])
    failed["meta"]["err"] = FAILED         # the listing says ok, the transaction says failed
    s.advance(1)
    outs = {o.session_id: o for o in s.tick()}
    assert outs[other["id"]].status == "waiting" and outs[other["id"]].error is None


@scenario("solana/already_receipted", covers=(
    covers("solana_watch", "already_receipted_payment_is_not_matched_again")))
def _already_receipted(s):
    """A payment that is already a receipt is not matched again, by reference or by scan."""
    node, mint, dest = game(s)
    receipt = {"chain": "solana", "tx_hash": signature(1), "log_index": 2, "block": 1000,
               "kind": "unlock", "item": "chapter2", "amount": "4.993742", "token": "USDC",
               "payer": PAYER_SOL, "recipient": DEV_SOL, "source": "payment"}
    s.initial(receipts={f"solana:{signature(1)}:2": receipt})
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, 4_993_742, addresses=[rec["reference"], dest])
    assert one(s).status == "waiting"
    assert pending(s, rec)["state"] == "awaiting"


# --- confirmation ---------------------------------------------------------------------------------


@scenario("solana/confirmed_then_finalized", covers=(
    covers("solana_watch", "reference_payment_confirms_then_finalizes_into_a_receipt")))
def _confirmed_then_finalized(s):
    """Confirmations are reported as the cluster counts them (1, then 12, then 31) while the
    payment is only confirmed; it is a receipt only once finalized, and the receipt records
    the payer, the developer, the slot and the log index. A second payment that is finalized
    on first sight is paid in one tick."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, rec["units"], slot=1001, addresses=[rec["reference"]])
    for confirmations in (1, 12, 31):
        node.statuses[signature(1)]["confirmations"] = confirmations
        out = one(s)
        assert (out.status, out.confirmations) == ("confirming", confirmations)
        assert s.steps[-1]["snapshot"]["receipts"] == {}
        s.advance(2)
    node.finalize(signature(1))
    out = one(s)
    assert (out.status, out.confirmations) == ("paid", 32)
    [receipt] = s.steps[-1]["snapshot"]["receipts"].values()
    assert (receipt["block"], receipt["recipient"], receipt["log_index"]) == (1001, DEV_SOL, 2)
    tip = s.start_session("tip", "solana", amount="2", token="USDC")
    pay(node, mint, 2, tip["units"], addresses=[tip["reference"]], status="finalized")
    assert one(s).status == "paid"


@scenario("solana/status_vanishes_then_returns", covers=(
    covers("solana_watch", "vanished_match_becomes_a_hint_and_is_found_again")))
def _status_vanishes(s):
    """A lagging endpoint no longer knows the matched transaction's status: the session goes
    back to awaiting as `reorged`, keeping the signature as a hint, and when the endpoint
    catches up the hint is verified again and the payment completes."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, rec["units"], addresses=[dest])
    assert one(s).status == "confirming"
    saved = node.statuses[signature(1)]
    node.statuses[signature(1)] = None
    s.advance(5)
    out = one(s)
    assert out.status == "reorged"
    assert pending(s, rec)["state"] == "awaiting" and pending(s, rec)["hint_tx"] == signature(1)
    node.statuses[signature(1)] = saved
    s.advance(1)
    assert one(s).status == "confirming"
    node.finalize(signature(1))
    s.advance(1)
    assert one(s).status == "paid"


@scenario("solana/outage_changes_nothing", covers=(
    covers("solana_watch", "network_outage_changes_nothing")))
def _outage(s):
    """With the endpoint down a tick reports network_unavailable and leaves the session exactly
    as it was, whether it is awaiting or confirming."""
    node, mint, dest = game(s)
    url = s.chain("solana").rpc[0]
    rec = s.start_session("unlock", "solana", item="chapter2")
    before = pending(s, rec)
    node.endpoint(url).down = True
    out = one(s)
    assert out.error == "network_unavailable" and pending(s, rec) == before
    node.endpoint(url).down = False
    s.advance(31)
    pay(node, mint, 1, rec["units"], addresses=[dest])
    assert one(s).status == "confirming"
    before = pending(s, rec)
    node.endpoint(url).down = True
    s.advance(1)
    out = one(s)
    assert out.error == "network_unavailable" and pending(s, rec) == before


@scenario("solana/horizon_closes_session", covers=(
    covers("solana_watch", "horizon_closes_the_session")))
def _horizon(s):
    """An unpaid session is closed at its scan horizon: still watched a second before."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    s.advance(SCAN_HORIZON_S - 1)
    assert one(s).status == "waiting"
    s.advance(1)
    out = one(s)
    assert out.status == "closed" and s.steps[-1]["snapshot"]["pending"] == {}
    assert rec["id"] == out.session_id


# --- scanning the developer's token account -------------------------------------------------------


@scenario("solana/scan_exact_amount", covers=(
    covers("solana_watch", "scan_matches_exact_amount_only_and_moves_cursor")))
def _scan_exact(s):
    """The scan (no reference) matches only the exact encoded amount; an inexact payment is
    passed, moving the cursor, then the exact one is matched."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, 5_000_000, addresses=[dest])
    assert one(s).status == "waiting"
    assert pending(s, rec)["scan_cursor"] == signature(1)
    pay(node, mint, 2, rec["units"], addresses=[dest])
    s.advance(1)
    assert one(s).status == "confirming"
    assert pending(s, rec)["match"]["tx_hash"] == signature(2)


@scenario("solana/scan_budget", covers=(
    covers("solana_watch", "scan_spends_at_most_five_transactions_per_step")))
def _scan_budget(s):
    """A step fetches at most 5 transactions of the scan: with seven others ahead of the exact
    payment, the first tick fetches five and waits, the next one finds it."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    for n in range(1, 8):
        pay(node, mint, n, 1_230_000, addresses=[dest])
    pay(node, mint, 99, rec["units"], addresses=[dest])
    assert one(s).status == "waiting"
    assert len(methods(s, "getTransaction")) == 5
    s.advance(1)
    assert one(s).status == "confirming"


@scenario("solana/scan_pages_past_a_full_page", covers=(
    covers("solana_watch", "scan_pages_back_past_a_full_page")))
def _scan_pages(s):
    """More than one page (100) of new signatures since the last step must not hide a
    payment: the walk pages back with `before` until the end."""
    node, mint, dest = game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 500, rec["units"], addresses=[dest])
    for n in range(1, 250):                       # newer, unrelated traffic
        node.addresses[dest].insert(0, {"signature": signature(n), "slot": 990, "err": None})
    assert one(s).status == "confirming"
    walk = [c for c in methods(s, "getSignaturesForAddress") if c["params"][0] == dest]
    assert len(walk) == 3 and "before" in walk[1]["params"][1]


def _history_before_the_session(s, known_cursor):
    node, mint, dest = game(s)
    old = [{"signature": signature(n), "slot": 500 + n, "err": None} for n in range(250, 0, -1)]
    if known_cursor:
        node.addresses[dest] = list(old)
    rec = s.start_session("unlock", "solana", item="chapter2")
    assert rec["scan_cursor"] == (signature(250) if known_cursor else None)
    node.addresses[dest] = list(old)
    pay(node, mint, 999, rec["units"], slot=950, addresses=[dest])
    if known_cursor:                              # the endpoint no longer lists the cursor
        node.addresses[dest].remove(old[0])
    out = one(s)
    assert out.status == "confirming" and pending(s, rec)["match"]["tx_hash"] == signature(999)
    assert len(methods(s, "getTransaction")) == 1
    assert len(methods(s, "getSignaturesForAddress")) <= 2


@scenario("solana/scan_stops_before_the_session_no_cursor", covers=(
    covers("solana_watch", "scan_walk_stops_at_signatures_older_than_the_session")))
def _scan_older_no_cursor(s):
    """With no cursor the walk ends at the first signature older than the session instead of
    crawling the account's history: a payment ahead of 250 old transactions costs one page."""
    _history_before_the_session(s, known_cursor=False)


@scenario("solana/scan_stops_before_the_session_unknown_cursor", covers=(
    covers("solana_watch", "scan_walk_stops_at_signatures_older_than_the_session")))
def _scan_older_unknown_cursor(s):
    """The same when the cursor is a signature this endpoint no longer lists."""
    _history_before_the_session(s, known_cursor=True)


@scenario("solana/scan_far_behind", covers=(
    covers("solana_watch", "scan_crosses_a_backlog_longer_than_one_walk",
           "scan_never_skips_signatures_when_it_falls_far_behind",
           "scan_walk_resumes_where_it_stopped_after_new_traffic",
           "outage_during_a_scan_walk_leaves_the_cursors_unchanged")))
def _scan_far_behind(s):
    """A busy token account: 1050 failed transfers have arrived since the cursor, with the
    payment among them (500th newest). One walk reads 10 pages of 100 and cannot reach the
    cursor, so it examines nothing and moves scan_before back; an outage in the middle of the
    next walk changes neither cursor; the walk then reaches the cursor and processes that window
    oldest first. 40 more signatures arrive meanwhile and are crossed the same way. The payment
    is found with no signature skipped."""
    node, mint, dest = game(s)
    url = s.chain("solana").rpc[0]
    pay(node, mint, 1, 1_230_000, slot=900, addresses=[dest], status="finalized")
    rec = s.start_session("unlock", "solana", item="chapter2")
    assert rec["scan_cursor"] == signature(1)

    def failed(n):
        return {"signature": signature(n), "slot": 990, "err": FAILED}
    pay(node, mint, 9000, rec["units"], slot=990)              # the payment, not yet listed
    backlog = [failed(n) for n in range(2000, 2500)]           # the 500 newest
    backlog.append({"signature": signature(9000), "slot": 990, "err": None})
    backlog += [failed(n) for n in range(2500, 3049)]          # 549 older, down to the cursor
    node.addresses[dest] = backlog + node.addresses[dest]
    assert len(backlog) == 1050
    node.slot += 1

    assert one(s).status == "waiting"                     # walk 1: cannot reach the cursor
    first_before = pending(s, rec)["scan_before"]
    assert first_before is not None and len(methods(s, "getTransaction")) == 0

    node.endpoint(url).fail_from("getSignaturesForAddress", 2, -32005, "rate limit exceeded")
    s.advance(1)
    out = one(s)                                          # dies on the first page of the walk
    assert out.error == "network_unavailable" and pending(s, rec)["scan_before"] == first_before
    del node.endpoint(url).errors_from["getSignaturesForAddress"]
    s.advance(31)

    assert one(s).status == "waiting"                     # walk 2 reaches the cursor: window of 50
    assert pending(s, rec)["scan_before"] is None
    cursor = pending(s, rec)["scan_cursor"]
    assert cursor != signature(1)
    for n in range(4000, 4040):                           # new traffic during the backlog
        node.addresses[dest].insert(0, failed(n))
    for _ in range(10):
        s.advance(1)
        out = one(s)
        if out.status != "waiting":
            break
    assert out.status == "confirming"
    assert pending(s, rec)["match"]["tx_hash"] == signature(9000)


# --- sessions the build cannot watch --------------------------------------------------------------


def _stale(sid, chain):
    return {"id": sid, "kind": "unlock", "item": "chapter2", "title": "Chapter 2",
            "token": "USDC", "chain": chain, "price": "4.99", "code": 37, "nonce": 42,
            "units": 4_993_742, "start_block": 995, "cursor": 995, "created_at": EPOCH,
            "state": "awaiting", "ui_status": "expired", "hint_tx": None, "hint_at": None,
            "match": None}


@scenario("solana/unwatchable_sessions", covers=(
    covers("engine_solana", "session_without_a_wallet_for_its_chain_does_not_stall_the_others",
           "session_on_an_unwatchable_chain_is_closed_after_the_horizon")))
def _unwatchable(s):
    """Sessions left by another build: one on Arbitrum (a build with no EVM wallet cannot
    watch it) and one on a chain that is no longer in the registry. They are skipped silently
    (no request, no error) while a Solana payment completes next to them, and closed at their
    horizon."""
    node, mint, dest = game(s)
    s.initial(pending={"ps_stale_arbitrum": _stale("ps_stale_arbitrum", "arbitrum"),
                       "ps_stale_removed": _stale("ps_stale_removed", "removed_custom_chain")})
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, rec["units"], addresses=[rec["reference"], dest])
    outs = s.tick()
    assert [o.status for o in outs if o.session_id == rec["id"]] == ["confirming"]
    assert [o for o in outs if o.session_id.startswith("ps_stale")] == []
    node.finalize(signature(1))
    s.advance(1)
    outs = s.tick()
    assert [o.status for o in outs if o.session_id == rec["id"]] == ["paid"]
    assert s.steps[-1]["internal_errors"] == 0
    assert sorted(s.steps[-1]["snapshot"]["pending"]) == ["ps_stale_arbitrum", "ps_stale_removed"]
    s.advance(SCAN_HORIZON_S)
    closed = [o for o in s.tick() if o.status == "closed"]
    assert sorted(o.session_id for o in closed) == ["ps_stale_arbitrum", "ps_stale_removed"]
    assert s.steps[-1]["snapshot"]["pending"] == {}


@scenario("solana/crash_in_one_session", covers=covers(
    "engine_solana", "an_exception_in_one_session_does_not_stop_the_others"))
def _crash(s):
    """An Arbitrum session whose node returns a log with a malformed logIndex (`0xzz`) crashes while
    being advanced: it is contained (waiting with error internal_error, one internal error),
    and the Solana session in the same tick still advances."""
    s.config(wallet=DEV_EVM, solana_wallet=DEV_SOL)
    s.item(**CHAPTER2)
    arb, sol = s.evm("arbitrum"), s.solana()
    usdc = s.chain("arbitrum").tokens["USDC"].address
    mint = s.chain("solana").tokens["USDC"].address
    broken = s.start_session("tip", "arbitrum", amount="1", token="USDC")
    fine = s.start_session("unlock", "solana", item="chapter2")
    log = arb.add_transfer(usdc, PAYER_EVM, DEV_EVM, broken["units"], block=998,
                           tx_hash=tx_hash(1))
    log["logIndex"] = "0xzz"
    pay(sol, mint, 1, fine["units"], addresses=[fine["reference"], fine["dest"]])
    outs = {o.session_id: o for o in s.expect("internal_error").tick()}
    assert s.steps[-1]["internal_errors"] == 1
    assert (outs[broken["id"]].status, outs[broken["id"]].error) == ("waiting", "internal_error")
    assert outs[fine["id"]].status == "confirming"
    assert broken["id"] in s.steps[-1]["snapshot"]["pending"]
