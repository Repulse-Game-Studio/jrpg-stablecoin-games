from decimal import Decimal

import pytest
from solders.keypair import Keypair

from cryptopay_core.backends import backend_for_family
from cryptopay_core.backends.solana import FINAL_CONFIRMATIONS, SolanaBackend, transfer_to
from cryptopay_core.chains import MAINNET, Chain, Token
from cryptopay_core.models import Item
from cryptopay_core.receipts import receipt_key
from cryptopay_core.sessions import HINT_TIMEOUT_S, SCAN_HORIZON_S, create_session, set_hint
from cryptopay_core.solana.base58 import b58encode, normalize_signature

from fakes import FakeSolanaChain, drive

MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
DEV = str(Keypair().pubkey())
PAYER = str(Keypair().pubkey())
REF = str(Keypair().pubkey())
DEST = "OWNERATA"
UNITS = 4_993_742
CHAIN = Chain(key="solana", chain_id=0, name="Solana", rpc=("https://x.example",),
              confirmations=FINAL_CONFIRMATIONS, max_log_range=0,
              explorer_tx="https://solscan.io/tx/{}", native_symbol="SOL", fee_hint="lowest",
              tokens={"USDC": Token(MINT)}, family="solana")
ITEMS = {"chapter2": Item("chapter2", 37, Decimal("4.99"), "USDC", "Chapter 2", Decimal("4.99"))}
B = SolanaBackend()


def sig(n):
    return b58encode(n.to_bytes(64, "big"))


def setup(**kw):
    pending, receipts, chain = {}, {}, FakeSolanaChain(slot=1000)
    args = dict(kind="unlock", item="chapter2", title="Chapter 2", token="USDC", chain="solana",
                price="4.99", code=37, nonce=42, start_block=900, now=0.0,
                extra={"reference": REF, "dest": DEST, "scan_cursor": None})
    args.update(kw)
    rec = create_session(pending, **args)
    return pending, receipts, chain, rec


def step(pending, receipts, chain, rec, now=1.0):
    return drive(B.advance(pending, receipts, rec["id"], rpc=chain, chain=CHAIN, wallet=DEV,
                           items=ITEMS, now=now))


def test_registered_and_normalizes_signatures():
    assert backend_for_family("solana").family == "solana"
    s = sig(5)
    assert B.normalize_tx(" " + s) == s == normalize_signature(s)
    with pytest.raises(ValueError):
        B.normalize_tx("0x" + "ab" * 32)


def test_token_accepts_base58_mints_and_keeps_evm_checksums():
    assert Token(MINT).address == MINT
    assert Token(MAINNET["arbitrum"].tokens["USDC"].address.lower()).address == \
        MAINNET["arbitrum"].tokens["USDC"].address
    with pytest.raises(ValueError):
        Token("not-an-address")


def test_transfer_to_reads_balance_changes():
    chain = FakeSolanaChain()
    tx = chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, slot=950)
    t = transfer_to(tx, sig=sig(1), mint=MINT, owner=DEV)
    assert (t.value, t.sender, t.recipient, t.log_index, t.block_number, t.token) == (
        UNITS, PAYER, DEV, 2, 950, MINT)
    assert transfer_to(tx, sig=sig(1), mint=MINT, owner=PAYER) is None
    assert transfer_to(tx, sig=sig(1), mint="OTHERMINT", owner=DEV) is None
    failed = chain.pay(sig(2), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS,
                       err={"InstructionError": [0, "Custom"]})
    assert transfer_to(failed, sig=sig(2), mint=MINT, owner=DEV) is None


def test_waits_when_nothing_arrived():
    pending, receipts, chain, rec = setup()
    assert step(pending, receipts, chain, rec).status == "waiting"


def test_reference_payment_confirms_then_finalizes_into_a_receipt():
    pending, receipts, chain, rec = setup()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, slot=950,
              addresses=[REF, DEST])
    out = step(pending, receipts, chain, rec)
    assert (out.status, out.confirmations, out.required) == ("confirming", 1, 32)
    chain.finalize(sig(1))
    out = step(pending, receipts, chain, rec)
    assert out.status == "paid" and out.result.tx_hash == sig(1) and out.result.chain == "solana"
    assert out.result.payer == PAYER and out.result.amount == Decimal("4.993742")
    key = receipt_key("solana", sig(1), 2)
    assert receipts[key]["block"] == 950 and receipts[key]["recipient"] == DEV
    assert rec["id"] not in pending


def test_reference_matches_any_amount_and_detects_underpayment():
    pending, receipts, chain, rec = setup()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=5_000_000, addresses=[REF])
    assert step(pending, receipts, chain, rec).status == "confirming"
    pending, receipts, chain, rec = setup()
    chain.pay(sig(2), mint=MINT, payer=PAYER, owner=DEV, amount=1_000_000, addresses=[REF])
    out = step(pending, receipts, chain, rec)
    assert out.status == "underpaid" and rec["id"] not in pending


def test_scan_matches_exact_amount_only_and_moves_cursor():
    pending, receipts, chain, rec = setup()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=5_000_000, addresses=[DEST])
    assert step(pending, receipts, chain, rec).status == "waiting"
    assert pending[rec["id"]]["scan_cursor"] == sig(1)
    chain.pay(sig(2), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, addresses=[DEST])
    assert step(pending, receipts, chain, rec).status == "confirming"
    assert pending[rec["id"]]["match"]["tx_hash"] == sig(2)


def test_scan_spends_at_most_five_transactions_per_step():
    pending, receipts, chain, rec = setup()
    for n in range(1, 8):
        chain.pay(sig(n), mint=MINT, payer=PAYER, owner=DEV, amount=1_230_000, addresses=[DEST])
    chain.pay(sig(99), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, addresses=[DEST])
    assert step(pending, receipts, chain, rec).status == "waiting"
    assert chain.calls.count("get_transaction") == 5
    assert step(pending, receipts, chain, rec).status == "confirming"


def test_scan_pages_back_past_a_full_page():
    """Review focus 3: >100 new signatures since the last step must not hide a payment."""
    pending, receipts, chain, rec = setup()
    chain.pay(sig(500), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, addresses=[DEST])
    for n in range(1, 250):  # newer, unrelated traffic
        chain.addresses[DEST].insert(0, {"signature": sig(n), "slot": 990, "err": None})
    assert step(pending, receipts, chain, rec).status == "confirming"
    assert chain.calls.count("get_signatures_for_address") >= 3


def test_scan_skips_failed_signatures_without_fetching_them():
    pending, receipts, chain, rec = setup()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, addresses=[DEST],
              err={"InstructionError": [0, "Custom"]})
    assert step(pending, receipts, chain, rec).status == "waiting"
    assert "get_transaction" not in chain.calls and pending[rec["id"]]["scan_cursor"] == sig(1)


def test_already_receipted_payment_is_not_matched_again():
    pending, receipts, chain, rec = setup()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, addresses=[REF])
    receipts[receipt_key("solana", sig(1), 2)] = {"chain": "solana"}
    assert step(pending, receipts, chain, rec).status == "waiting"


@pytest.mark.parametrize("kwargs, error", [
    (dict(err={"InstructionError": [0, "Custom"]}), "tx_reverted"),
    (dict(owner=PAYER), "tx_not_a_payment"),
    (dict(slot=100), "tx_not_a_payment"),            # before the session started
])
def test_bad_hints_are_dropped_with_a_reason(kwargs, error):
    pending, receipts, chain, rec = setup()
    args = dict(mint=MINT, payer=PAYER, owner=DEV, amount=UNITS)
    args.update(kwargs)
    chain.pay(sig(1), **args)
    set_hint(pending, rec["id"], sig(1), 0.0, normalize=B.normalize_tx)
    out = step(pending, receipts, chain, rec)
    assert (out.status, out.error) == ("waiting", error)
    assert pending[rec["id"]]["hint_tx"] is None


def test_hint_not_yet_visible_waits_then_expires():
    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], sig(1), 0.0, normalize=B.normalize_tx)
    assert step(pending, receipts, chain, rec, now=10.0).status == "waiting"
    assert pending[rec["id"]]["hint_tx"] == sig(1)
    step(pending, receipts, chain, rec, now=HINT_TIMEOUT_S + 1)
    assert pending[rec["id"]]["hint_tx"] is None


def test_good_hint_matches_round_amount():
    pending, receipts, chain, rec = setup()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=5_000_000)
    set_hint(pending, rec["id"], sig(1), 0.0, normalize=B.normalize_tx)
    assert step(pending, receipts, chain, rec).status == "confirming"


def test_vanished_match_becomes_a_hint_and_is_found_again():
    """Review focus 4: a lagging endpoint's null status must not lose the payment."""
    pending, receipts, chain, rec = setup()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, addresses=[DEST])
    assert step(pending, receipts, chain, rec).status == "confirming"
    saved = chain.statuses.pop(sig(1))
    out = step(pending, receipts, chain, rec, now=5.0)
    assert out.status == "reorged"
    assert pending[rec["id"]]["state"] == "awaiting" and pending[rec["id"]]["hint_tx"] == sig(1)
    chain.statuses[sig(1)] = saved                      # the endpoint caught up
    assert step(pending, receipts, chain, rec, now=6.0).status == "confirming"
    chain.finalize(sig(1))
    assert step(pending, receipts, chain, rec, now=7.0).status == "paid"


def test_network_outage_changes_nothing():
    pending, receipts, chain, rec = setup()
    chain.down = True
    before = dict(pending[rec["id"]])
    out = step(pending, receipts, chain, rec)
    assert out.error == "network_unavailable" and pending[rec["id"]] == before


def test_horizon_closes_the_session():
    pending, receipts, chain, rec = setup()
    assert step(pending, receipts, chain, rec, now=SCAN_HORIZON_S + 1).status == "closed"
    assert rec["id"] not in pending


def test_wrong_mint_on_the_reference_is_ignored():
    pending, receipts, chain, rec = setup()
    chain.pay(sig(1), mint="So11111111111111111111111111111111111111112", payer=PAYER, owner=DEV,
              amount=UNITS, addresses=[REF])
    assert step(pending, receipts, chain, rec).status == "waiting"


def _busy_account(chain, spam, payment_at):
    """sig(1) is the old cursor; `spam` unrelated transfers newer than it, with the exact
    payment placed after `payment_at` of them (0 = just after the cursor)."""
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=1_230_000, addresses=[DEST])
    for n in range(spam + 1):
        if n == payment_at:
            chain.pay(sig(9000), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS,
                      addresses=[DEST])
        if n < spam:
            chain.pay(sig(100 + n), mint=MINT, payer=PAYER, owner=DEV, amount=1_230_000,
                      addresses=[DEST])


def _steps_until_confirming(pending, receipts, chain, rec, limit=300):
    for n in range(limit):
        out = step(pending, receipts, chain, rec, now=float(n))
        if out.status != "waiting":
            return out, n
    raise AssertionError("payment never matched")


def test_scan_crosses_a_backlog_longer_than_one_walk():
    """A busy account (>SIG_PAGE * MAX_SIG_PAGES new signatures) only delays detection."""
    pending, receipts, chain, rec = setup()
    _busy_account(chain, 1050, 0)
    rec["scan_cursor"] = sig(1)
    out, _ = _steps_until_confirming(pending, receipts, chain, rec)
    assert out.status == "confirming" and pending[rec["id"]]["match"]["tx_hash"] == sig(9000)


@pytest.mark.parametrize("payment_at", [0, 40, 80, 104, 105])
def test_scan_never_skips_signatures_when_it_falls_far_behind(monkeypatch, payment_at):
    monkeypatch.setattr("cryptopay_core.backends.solana.SIG_PAGE", 10)
    monkeypatch.setattr("cryptopay_core.backends.solana.MAX_SIG_PAGES", 3)
    pending, receipts, chain, rec = setup()
    _busy_account(chain, 105, payment_at)       # 106+ signatures: several walks deep
    rec["scan_cursor"] = sig(1)
    out, _ = _steps_until_confirming(pending, receipts, chain, rec)
    assert out.status == "confirming" and pending[rec["id"]]["match"]["tx_hash"] == sig(9000)
    assert "get_transaction" in chain.calls


def test_scan_walk_resumes_where_it_stopped_after_new_traffic(monkeypatch):
    monkeypatch.setattr("cryptopay_core.backends.solana.SIG_PAGE", 10)
    monkeypatch.setattr("cryptopay_core.backends.solana.MAX_SIG_PAGES", 3)
    pending, receipts, chain, rec = setup()
    _busy_account(chain, 70, 35)
    rec["scan_cursor"] = sig(1)
    for n in range(4):                          # walked back and started on the oldest window
        assert step(pending, receipts, chain, rec, now=float(n)).status == "waiting"
    for k in range(40):                         # more traffic arrives mid-backlog
        chain.pay(sig(5000 + k), mint=MINT, payer=PAYER, owner=DEV, amount=1_230_000,
                  addresses=[DEST])
    out, _ = _steps_until_confirming(pending, receipts, chain, rec)
    assert pending[rec["id"]]["match"]["tx_hash"] == sig(9000)


def test_outage_during_a_scan_walk_leaves_the_cursors_unchanged(monkeypatch):
    import copy

    from cryptopay_core.rpc import RpcUnavailable

    monkeypatch.setattr("cryptopay_core.backends.solana.SIG_PAGE", 10)
    monkeypatch.setattr("cryptopay_core.backends.solana.MAX_SIG_PAGES", 3)
    pending, receipts, chain, rec = setup()
    _busy_account(chain, 105, 50)
    rec["scan_cursor"] = sig(1)
    assert step(pending, receipts, chain, rec).status == "waiting"   # first walk: moves scan_before
    assert pending[rec["id"]]["scan_before"] is not None
    before = copy.deepcopy(pending[rec["id"]])
    real, calls = chain.get_signatures_for_address, []

    def flaky(*args, **kwargs):
        calls.append(1)
        if len(calls) == 2:                                          # dies mid-walk
            raise RpcUnavailable("fake outage")
        return (yield from real(*args, **kwargs))

    monkeypatch.setattr(chain, "get_signatures_for_address", flaky)
    out = step(pending, receipts, chain, rec)
    assert out.error == "network_unavailable" and len(calls) == 2
    assert pending[rec["id"]] == before


def test_hint_that_never_lands_expires_after_the_solana_timeout():
    """A wallet returns the signature before the transaction lands; dropped ones are routine."""
    from cryptopay_core.backends.solana import SOLANA_HINT_TIMEOUT_S

    assert SOLANA_HINT_TIMEOUT_S == 180
    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], sig(1), 0.0, normalize=B.normalize_tx)
    out = step(pending, receipts, chain, rec, now=SOLANA_HINT_TIMEOUT_S - 1)
    assert (out.status, out.error) == ("waiting", None)
    assert pending[rec["id"]]["hint_tx"] == sig(1)
    out = step(pending, receipts, chain, rec, now=SOLANA_HINT_TIMEOUT_S)
    assert (out.status, out.error) == ("waiting", "tx_not_landed")
    assert pending[rec["id"]]["hint_tx"] is None


def test_reference_payment_matches_while_a_hint_is_pending():
    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], sig(1), 0.0, normalize=B.normalize_tx)   # never lands
    chain.pay(sig(2), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, addresses=[REF, DEST])
    out = step(pending, receipts, chain, rec, now=10.0)
    assert (out.status, out.error) == ("confirming", None)
    assert pending[rec["id"]]["match"]["tx_hash"] == sig(2)
    assert pending[rec["id"]]["hint_tx"] is None


def test_hint_whose_lookup_keeps_failing_still_expires(monkeypatch):
    from cryptopay_core.backends.solana import SOLANA_HINT_TIMEOUT_S
    from cryptopay_core.rpc import RpcError

    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], sig(1), 0.0, normalize=B.normalize_tx)

    def broken(s, commitment="confirmed"):
        raise RpcError(-32015, "Transaction version (9) is not supported")
        yield  # never reached: makes this a generator, like the real method

    monkeypatch.setattr(chain, "get_transaction", broken)
    out = step(pending, receipts, chain, rec, now=10.0)
    assert out.error == "network_unavailable" and pending[rec["id"]]["hint_tx"] == sig(1)
    out = step(pending, receipts, chain, rec, now=SOLANA_HINT_TIMEOUT_S + 1)
    assert (out.status, out.error) == ("waiting", "tx_not_landed")
    assert pending[rec["id"]]["hint_tx"] is None


@pytest.mark.parametrize("failure", ["unavailable", "rpc_error"])
def test_hint_expiry_is_reported_even_if_the_reference_lookup_fails(monkeypatch, failure):
    """Later ticks are plain "waiting" (not forwarded), so the expiry tick must say it."""
    from cryptopay_core.backends.solana import SOLANA_HINT_TIMEOUT_S
    from cryptopay_core.rpc import RpcError, RpcUnavailable

    pending, receipts, chain, rec = setup()
    set_hint(pending, rec["id"], sig(1), 0.0, normalize=B.normalize_tx)

    def broken(address, **kw):
        if failure == "unavailable":
            raise RpcUnavailable("all endpoints down")
        raise RpcError(-32005, "node is behind")
        yield  # never reached: makes this a generator, like the real method

    monkeypatch.setattr(chain, "get_signatures_for_address", broken)
    out = step(pending, receipts, chain, rec, now=SOLANA_HINT_TIMEOUT_S)
    assert (out.status, out.error) == ("waiting", "tx_not_landed")
    assert out.hint_tx == sig(1)                        # which hint didn't land
    assert pending[rec["id"]]["hint_tx"] is None
    out = step(pending, receipts, chain, rec, now=SOLANA_HINT_TIMEOUT_S + 1)
    assert (out.status, out.error) == ("waiting", "network_unavailable")


def test_hint_verdicts_name_the_hint_they_are_about():
    pending, receipts, chain, rec = setup()
    chain.pay(sig(1), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS,
              err={"InstructionError": [0, "Custom"]})
    set_hint(pending, rec["id"], sig(1), 0.0, normalize=B.normalize_tx)
    out = step(pending, receipts, chain, rec, now=1.0)
    assert (out.error, out.hint_tx) == ("tx_reverted", sig(1))
    out = step(pending, receipts, chain, rec, now=2.0)
    assert (out.error, out.hint_tx) == (None, None)


@pytest.mark.parametrize("cursor", [None, "unknown-to-this-endpoint"])
def test_scan_walk_stops_at_signatures_older_than_the_session(cursor):
    """Without a usable cursor the walk must not crawl the token account's whole history."""
    pending, receipts, chain, rec = setup()
    for n in range(1, 251):                     # long history from before the session
        chain.pay(sig(n), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, slot=500 + n,
                  addresses=[DEST])
    chain.pay(sig(999), mint=MINT, payer=PAYER, owner=DEV, amount=UNITS, slot=950,
              addresses=[DEST])
    rec["scan_cursor"] = None if cursor is None else sig(12345)
    out = step(pending, receipts, chain, rec)
    assert out.status == "confirming" and pending[rec["id"]]["match"]["tx_hash"] == sig(999)
    assert chain.calls.count("get_transaction") == 1
    assert chain.calls.count("get_signatures_for_address") <= 2   # reference + one page
