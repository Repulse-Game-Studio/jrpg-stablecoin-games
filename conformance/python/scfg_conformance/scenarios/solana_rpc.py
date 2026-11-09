"""Solana through the Engine, part 2: building the payment, launch readiness, restore,
re-verification and the getTransaction rules (transaction versions, endpoints with different
history). Payments and the scan are in solana.py."""

from __future__ import annotations

import base64

from scfg_core.solana.base58 import b58decode
from scfg_core.solana.pda import associated_token_address

from ..scenario import scenario
from .evm import covers
from .fixtures import CHAPTER2, DEV_EVM, DEV_SOL, OLD_DEV_SOL, PAYER_SOL, signature, tx_hash
from .solana import FAILED, methods, one, pay, pending


def mainnet(s, **config):
    """A Solana-only mainnet game (two endpoints, served by one node); returns (node, mint)."""
    s.config(solana_wallet=DEV_SOL, **config)
    s.item(**CHAPTER2)
    node = s.solana()
    return node, s.chain("solana").tokens["USDC"].address


def receipt(n, *, amount="4.993742", block=950, recipient=DEV_SOL, index=2):
    return {"chain": "solana", "tx_hash": signature(n), "log_index": index, "block": block,
            "kind": "tip", "item": None, "amount": amount, "token": "USDC",
            "payer": PAYER_SOL, "recipient": recipient, "source": "payment"}


def receipts(*items):
    """Receipt store in sorted key order: re-verification reports removals in store order, and
    a vector's setup is stored with sorted keys."""
    return dict(sorted((f"solana:{r['tx_hash']}:{r['log_index']}", r) for r in items))


def raised(s):
    """The ConfigError/RestoreError code the last action raised."""
    return s.steps[-1]["raises"]["code"]


def removed_keys(*items):
    return sorted(f"solana:{r['tx_hash']}:{r['log_index']}" for r in items)


# --- building the payment ---


@scenario("solana/build_payment", covers=(
    covers("solana_build", "build_payment_transaction", "build_payment_rejects_a_bad_account",
           "engine_builds_only_for_solana_sessions")
    + covers("solana_rpc", "small_helpers")))
def _build_payment(s):
    """The unsigned transfer for the player's wallet to sign: their USDC token account is
    derived from the wallet and the mint (the balance asked for is that account's), the recent
    blockhash is embedded, the priority fee is the median of recent fees clamped to
    1 000..1 000 000 micro-lamports per compute unit, the compute-budget instructions come
    first and then a checked transfer of the session's units with the reference as an extra
    key. A wallet address that is not an address, or an EVM session, is refused."""
    s.config(wallet=DEV_EVM, solana_wallet=DEV_SOL)
    s.item(**CHAPTER2)
    node = s.solana()
    s.evm("base")
    mint = s.chain("solana").tokens["USDC"].address
    player = PAYER_SOL
    source = associated_token_address(player, mint)
    node.token_balances[source] = 7_000_000
    node.balances[player] = 2_000_000
    rec = s.start_session("unlock", "solana", item="chapter2")
    for fees, expected in (([5_000], 5_000), ([], 1_000), ([10**12], 1_000_000)):
        node.priority_fees = fees
        out = s.build_solana_payment(rec["id"], player)
        assert (out["token_balance"], out["sol_balance"]) == ("7000000", "2000000")
        raw = base64.b64decode(out["transaction"])
        assert raw[:65] == bytes([1]) + bytes(64)                  # one empty signature slot
        assert raw[69:101] == b58decode(player) and b58decode(node.blockhash) in raw
        assert b58decode(rec["reference"]) in raw and b58decode(rec["dest"]) in raw
        assert b58decode(source) in raw
        assert bytes([3]) + expected.to_bytes(8, "little") in raw  # set compute unit price
        assert bytes([12]) + rec["units"].to_bytes(8, "little") + bytes([6]) in raw
        assert [c["method"] for c in s.steps[-1]["calls"]] == [
            "getAccountInfo", "getBalance", "getLatestBlockhash", "getRecentPrioritizationFees"]
    s.expect("other").build_solana_payment(rec["id"], "0x1234")
    assert s.steps[-1]["calls"] == []
    evm = s.start_session("unlock", "base", item="chapter2")
    s.expect("other").build_solana_payment(evm["id"], player)


# --- launch readiness ---


@scenario("solana/readiness", covers=(
    covers("engine_solana", "missing_token_account_marks_token_unready",
           "readiness_check_failure_assumes_ready")
    + covers("solana_backend", "check_ready")
    + covers("solana_rpc", "small_helpers")))
def _readiness(s):
    """At launch the developer needs a token account for each enabled token (wallets refuse to
    pay one without): a missing one marks that token unready. With every account missing both
    are unready; when the check cannot be made (endpoint down) nothing is hidden."""
    node, mint = mainnet(s)
    usdt = s.chain("solana").tokens["USDT"].address
    node.accounts.add(associated_token_address(DEV_SOL, mint))
    assert s.reverify() == []
    assert s.steps[-1]["snapshot"]["unready"] == [["solana", "USDT"]]
    assert [c["method"] for c in s.steps[-1]["calls"]] == ["getAccountInfo"] * 2
    node.accounts.add(associated_token_address(DEV_SOL, usdt))
    s.reverify()
    assert s.steps[-1]["snapshot"]["unready"] == []
    node.accounts.clear()
    s.reverify()
    assert s.steps[-1]["snapshot"]["unready"] == [["solana", "USDC"], ["solana", "USDT"]]
    for url in s.chain("solana").rpc:
        node.endpoint(url).down = True
    s.reverify()
    assert s.steps[-1]["snapshot"]["unready"] == []


@scenario("solana/readiness_crash", covers=(
    covers("engine_solana", "readiness_check_crash_assumes_ready_and_is_reported")))
def _readiness_crash(s):
    """A readiness answer the core cannot read (a result with no `value`) is a crash inside the
    check: it is reported as an internal error, and nothing is marked unready."""
    node, mint = mainnet(s)
    node._getAccountInfo = lambda ep, params: {}        # the node's answer lacks `value`
    s.expect("internal_error").reverify()
    assert s.steps[-1]["internal_errors"] == 1
    assert s.steps[-1]["snapshot"]["unready"] == []


# --- restore ---


@scenario("solana/restore_lookup", covers=(
    covers("solana_backend", "lookup_classifies_by_amount_code", "lookup_outcomes",
           "restore_routes_signatures_to_solana_and_hashes_to_evm")))
def _restore_lookup(s):
    """Restoring from a signature: only Solana is asked, the amount's code classifies the
    payment (the Chapter 2 unlock), applying it stores a receipt and a second lookup says
    already restored. An unknown signature is not found, a failed transaction is reverted and one
    that paid nobody the developer accepts is not a payment. A 0x hash is asked of the EVM chains
    and never reaches Solana."""
    s.config(wallet=DEV_EVM, solana_wallet=DEV_SOL)
    s.item(**CHAPTER2)
    node = s.solana()
    for key in ("arbitrum", "base", "arc", "polygon", "ethereum"):
        s.evm(key)
    mint = s.chain("solana").tokens["USDC"].address
    pay(node, mint, 1, 4_993_742, slot=950)
    [cand] = s.restore_lookup(signature(1))
    assert (cand.chain, cand.token, cand.kind, cand.item, cand.already_restored) == (
        "solana", "USDC", "unlock", "chapter2", False)
    assert cand.transfer.value == 4_993_742 and cand.transfer.log_index == 2
    assert {c["method"] for c in s.steps[-1]["calls"]} == {"getTransaction"}
    s.restore_apply(len(s.steps) - 1)
    [again] = s.restore_lookup(signature(1))
    assert again.already_restored
    s.restore_lookup(signature(2))
    assert raised(s) == "tx_not_found"
    assert len(s.steps[-1]["calls"]) == 2                  # both endpoints were asked
    pay(node, mint, 3, 1, err=FAILED)
    s.restore_lookup(signature(3))
    assert raised(s) == "tx_reverted"
    pay(node, mint, 4, 1_000_000, to=PAYER_SOL, by=DEV_SOL)
    s.restore_lookup(signature(4))
    assert raised(s) == "tx_not_a_payment"
    s.restore_lookup(tx_hash(5))
    assert raised(s) == "tx_not_found"
    assert not [c for c in s.steps[-1]["calls"] if c["url"] in node.urls]


# --- re-verification ---


@scenario("solana/reverify_receipts", covers=(
    covers("solana_backend", "reverify_keeps_genuine_and_previous_wallet_receipts",
           "reverify_drops_contradicted_receipts")))
def _reverify_receipts(s):
    """Launch re-check: receipts to the current or a previous wallet that the chain confirms are
    kept; one with a different amount, a different slot, a recipient that is not a developer
    wallet, or whose transaction failed, is revoked."""
    node, mint = mainnet(s, previous_wallets=[OLD_DEV_SOL])
    keep = [receipt(1), receipt(2, recipient=OLD_DEV_SOL)]
    drop = [receipt(3, amount="9.000000"), receipt(4, block=951), receipt(5, recipient=PAYER_SOL),
            receipt(6)]
    s.initial(receipts=receipts(*keep, *drop))
    pay(node, mint, 1, 4_993_742, slot=950)
    pay(node, mint, 2, 4_993_742, slot=950, to=OLD_DEV_SOL)
    pay(node, mint, 3, 4_993_742, slot=950)
    pay(node, mint, 4, 4_993_742, slot=950)
    pay(node, mint, 6, 4_993_742, slot=950, err=FAILED)
    assert s.reverify() == removed_keys(*drop)
    assert sorted(s.steps[-1]["snapshot"]["receipts"]) == removed_keys(*keep)
    assert not [c for c in s.steps[-1]["calls"] if signature(5) in str(c["params"])]


@scenario("solana/reverify_unreadable_transactions", covers=(
    covers("solana_backend", "reverify_keeps_a_receipt_whose_transaction_it_cannot_read")))
def _reverify_unreadable(s):
    """A transaction the build cannot read (a balance without an amount, a non-numeric
    amount, a balances list that is not a list) proves nothing against its receipt: it is kept."""
    node, mint = mainnet(s)
    payloads = [
        {"meta": {"err": None, "postTokenBalances": [{"mint": mint, "accountIndex": 2}]}},
        {"meta": {"err": None, "postTokenBalances": [
            {"mint": mint, "accountIndex": 2, "owner": DEV_SOL,
             "uiTokenAmount": {"amount": "x"}}]}},
        {"meta": {"err": None, "postTokenBalances": "garbage"}},
    ]
    items = [receipt(n) for n in (1, 2, 3)]
    for item, payload in zip(items, payloads):
        node.txs[item["tx_hash"]] = dict(payload, slot=950)
    s.initial(receipts=receipts(*items))
    assert s.reverify() == []
    assert len(s.steps[-1]["snapshot"]["receipts"]) == 3


@scenario("solana/reverify_outage_and_future_slot", covers=(
    covers("solana_backend", "outage_keeps_and_far_future_slot_drops")))
def _reverify_outage(s):
    """With the endpoints down nothing is judged and every receipt is kept. Once they answer, a
    receipt for a slot far beyond the cluster's (20 000 ahead) is revoked, while one only a few
    slots ahead of a lagging endpoint is kept."""
    node, mint = mainnet(s)
    ok, future, lagging = receipt(1), receipt(2, block=21_000), receipt(3, block=1005)
    s.initial(receipts=receipts(ok, future, lagging))
    pay(node, mint, 1, 4_993_742, slot=950)
    pay(node, mint, 3, 4_993_742, slot=1005)
    for url in node.urls:
        node.endpoint(url).down = True
    assert s.reverify() == []
    for url in node.urls:
        node.endpoint(url).down = False
    s.advance(31)
    assert s.reverify() == removed_keys(future)
    assert sorted(s.steps[-1]["snapshot"]["receipts"]) == removed_keys(ok, lagging)


# --- endpoints with different history ---


def _history_scenario(s, first_a, first_b, *, found=False):
    """One receipt checked against two endpoints whose history starts at first_a / first_b. The
    transaction exists on the node only if `found`; the endpoint's history decides who serves it."""
    node, mint = mainnet(s)
    a, b = s.chain("solana").rpc
    node.endpoint(a).first_available = first_a
    node.endpoint(b).first_available = first_b
    r = receipt(1)
    s.initial(receipts=receipts(r))
    if found:
        pay(node, mint, 1, 4_993_742, slot=950)
    return r, s.reverify()


@scenario("solana/history_pruned_endpoints_keep", covers=(
    covers("solana_backend", "old_receipt_on_a_pruned_endpoint_is_kept")
    + covers("solana_rpc", "null_only_from_pruned_endpoints_is_not_authoritative")))
def _pruned_keep(s):
    """Both endpoints have pruned that far back, so the null they answer proves nothing: the
    receipt is kept."""
    r, removed = _history_scenario(s, 10**9, 10**9)
    assert removed == [] and len(s.steps[-1]["snapshot"]["receipts"]) == 1
    assert len(methods(s, "getFirstAvailableBlock")) == 2


@scenario("solana/history_covering_endpoint_drops", covers=(
    covers("solana_backend", "invented_signature_is_dropped_when_history_covers_it")
    + covers("solana_rpc", "null_from_full_history_endpoint_is_authoritative")))
def _covering_drops(s):
    """One endpoint is pruned and the other keeps all history: the null of the one that would
    have the transaction is proof, so an invented signature revokes its receipt."""
    r, removed = _history_scenario(s, 10**9, 0)
    assert removed == removed_keys(r) and s.steps[-1]["snapshot"]["receipts"] == {}


@scenario("solana/history_found_on_second_endpoint", covers=(
    covers("solana_rpc", "get_transaction_any_returns_first_found")))
def _found_second(s):
    """The first endpoint pruned the transaction (null), the second still has it: the first
    answer that has the transaction wins and the receipt is kept."""
    r, removed = _history_scenario(s, 10**9, 0, found=True)
    assert removed == []
    got = [c["url"] for c in methods(s, "getTransaction")]
    assert got == list(s.chain("solana").rpc)


@scenario("solana/history_endpoints_fail", covers=(
    covers("solana_rpc", "try_at_each_live_url_reports_per_endpoint_and_dead_ones_cool_down",
           "get_transaction_any_without_answers_raises")))
def _endpoints_fail(s):
    """One endpoint is down and the other answers getTransaction with an error that names no
    version: nobody answered, so the receipt is kept; the dead endpoint is cooling down and is not
    asked again (the live-endpoint list shrinks to the other). A lookup of a signature while
    both are down is network_unavailable."""
    node, mint = mainnet(s)
    a, b = s.chain("solana").rpc
    s.initial(receipts=receipts(receipt(1)))
    node.endpoint(a).down = True
    node.endpoint(b).errors["getTransaction"] = (-1, "boom")
    assert s.reverify() == []
    assert [c["url"] for c in methods(s, "getTransaction")] == [b]
    s.advance(1)
    assert s.reverify() == []
    assert all(c["url"] == b for c in s.steps[-1]["calls"])
    del node.endpoint(b).errors["getTransaction"]
    node.endpoint(b).down = True
    s.advance(100)
    s.restore_lookup(signature(2))
    assert raised(s) == "network_unavailable"


# --- transaction versions ---


def _version_game(s):
    s.config(solana_wallet=DEV_SOL, testnet=True)
    s.item(**CHAPTER2)
    node = s.solana()
    return node, s.chain("solana").tokens["USDC"].address, s.chain("solana").rpc[0]


def _versions(s):
    return [c["params"][1]["maxSupportedTransactionVersion"] for c in methods(s, "getTransaction")]


@scenario("solana/tx_version_named_by_node", covers=(
    covers("solana_rpc", "get_transaction_retries_once_with_the_version_the_node_names",
           "get_transaction_any_retries_with_the_named_version")))
def _version_named(s):
    """A transaction of version 2 answers -32015 naming that version: the request is repeated
    once with it and succeeds, and the next request starts from version 1 again (the node's
    answer does not change what this build asks for). The same for a version-3 transaction in a
    launch re-check."""
    s.initial(receipts=receipts(receipt(10)))
    node, mint, url = _version_game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, rec["units"], version=2)
    s.report_hint(rec["id"], signature(1))
    assert one(s).status == "confirming"
    assert _versions(s) == [1, 2]
    other = s.start_session("tip", "solana", amount="1", token="USDC")
    pay(node, mint, 2, other["units"], version=2)
    s.report_hint(other["id"], signature(2))
    s.tick()
    assert _versions(s) == [1, 2]
    pay(node, mint, 10, 4_993_742, slot=950, version=3)
    assert s.reverify() == []
    assert _versions(s) == [1, 3]


@scenario("solana/tx_version_lowered", covers=(
    covers("solana_rpc", "get_transaction_lowers_version_when_the_node_rejects_the_parameter")))
def _version_lowered(s):
    """An older node rejects maxSupportedTransactionVersion 1 with -32602: the request is
    repeated with 0, and the lower version is used for every later request."""
    node, mint, url = _version_game(s)
    node.endpoint(url).tx_version_max = 0
    rec = s.start_session("unlock", "solana", item="chapter2")
    pay(node, mint, 1, rec["units"])
    s.report_hint(rec["id"], signature(1))
    assert one(s).status == "confirming"
    assert _versions(s) == [1, 0]
    other = s.start_session("tip", "solana", amount="1", token="USDC")
    pay(node, mint, 2, other["units"])
    s.report_hint(other["id"], signature(2))
    s.tick()
    assert _versions(s) == [0]
    assert pending(s, other)["match"]["tx_hash"] == signature(2)


@scenario("solana/tx_version_error_naming_an_older_version", covers=(
    covers("solana_rpc", "unsupported_version_error_naming_an_older_version_is_raised")))
def _version_older(s):
    """A -32015 that names a version no newer than the one asked is not retried: the error is
    raised (the hint stays, the tick reports network_unavailable) after a single request."""
    node, mint, url = _version_game(s)
    rec = s.start_session("unlock", "solana", item="chapter2")
    s.report_hint(rec["id"], signature(1))
    node.endpoint(url).errors["getTransaction"] = (
        -32015, "Transaction version (0) is not supported by the requesting client.")
    out = one(s)
    assert out.error == "network_unavailable" and pending(s, rec)["hint_tx"] == signature(1)
    assert _versions(s) == [1]
