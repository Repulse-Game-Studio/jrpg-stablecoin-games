"""Restore (a player pastes a transaction hash) and re-verification of stored receipts, through
the Engine. Twins of tests/unit/test_restore.py and tests/unit/test_reverify.py.

Engine.restore_lookup asks every chain of the registry (the game's chains first), so these
scenarios serve all five mainnet EVM chains. Solana signatures are in solana/restore_lookup.
"""

from __future__ import annotations

from decimal import Decimal

from scfg_core.reverify import FUTURE_TOLERANCE_BLOCKS

from ..scenario import scenario
from .fixtures import CHAPTER2, DEV_EVM, OLD_DEV_EVM, PAYER_EVM, covers, tx_hash

OST = {"item_id": "ost", "code": 2, "price": "2.00", "token": "USDT", "title": "OST"}
EVM_CHAINS = ("arbitrum", "base", "arc", "polygon", "ethereum")


def _game(s, **config):
    """Two items, the arbitrum and base mainnet chains first, one node per EVM chain; returns
    ({chain: node}, {chain: USDC address})."""
    s.config(wallet=DEV_EVM, chains=["arbitrum", "base"], **config)
    s.item(**CHAPTER2)
    s.item(**OST)
    nodes = {key: s.evm(key) for key in EVM_CHAINS}
    return nodes, {key: s.chain(key).tokens["USDC"].address for key in EVM_CHAINS}


def _receipt_calls(s):
    """(url, method) of the calls of the last step."""
    return [(c["url"], c["method"]) for c in s.steps[-1]["calls"]]


# --- restore: what a transaction turns out to be ---------------------------------------------


@scenario("restore/unlock_found", covers=(
    covers("restore", "lookup_finds_chain_and_classifies", "classify_item_code")))
def _unlock_found(s):
    """The chain that knows the transaction yields a candidate: a payment of 4.993742 USDC is
    the Chapter 2 unlock. Arbitrum, tried first, does not have it; the search stops at Base
    (nothing is asked of the chains after it)."""
    nodes, usdc = _game(s)
    nodes["base"].add_transfer(usdc["base"], PAYER_EVM, DEV_EVM, 4_993_742, block=50,
                               tx_hash=tx_hash(1), log_index=1)
    [cand] = s.restore_lookup(tx_hash(1))
    assert (cand.chain, cand.token, cand.kind, cand.item, cand.already_restored) == (
        "base", "USDC", "unlock", "chapter2", False)
    asked = _receipt_calls(s)
    first = s.chain("arbitrum").rpc[0]
    assert asked[0] == (first, "eth_getTransactionReceipt")
    assert {m for _, m in asked} == {"eth_getTransactionReceipt"}
    assert not any(u in s.chain("polygon").rpc for u, _ in asked)


@scenario("restore/tip_to_a_previous_wallet", covers=(
    covers("restore", "lookup_accepts_previous_wallet", "classify_tip_code")))
def _tip_to_previous_wallet(s):
    """A payment to a wallet the game used to have is restorable, and an amount whose last
    four digits carry no item code is a tip."""
    nodes, usdc = _game(s, previous_wallets=[OLD_DEV_EVM])
    nodes["arbitrum"].add_transfer(usdc["arbitrum"], PAYER_EVM, OLD_DEV_EVM, 3_000_017,
                                   block=50, tx_hash=tx_hash(1))
    [cand] = s.restore_lookup(tx_hash(1))
    assert (cand.chain, cand.kind, cand.item) == ("arbitrum", "tip", None)
    assert cand.transfer.recipient == OLD_DEV_EVM


@scenario("restore/classify_amounts", covers=covers(
    "restore", "classify_item_code", "classify_tip_code", "classify_round_amount_asks_player",
    "classify_unknown_code_offers_affordable_items",
    "classify_known_code_but_below_min_price_asks"))
def _classify_amounts(s):
    """What an amount pays for: item code 37 at 4.99 is the unlock, code 17 a tip; a round
    5.00 asks the player (any item they can afford, or a tip); an unknown code offers the
    items under that amount; a known code paid far below the price offers only a tip."""
    nodes, usdc = _game(s)
    amounts = [4_993_742, 3_000_017, 5_000_000, 2_509_942, 1_003_742]
    for n, units in enumerate(amounts, 1):
        nodes["arbitrum"].add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, units, block=50,
                                       tx_hash=tx_hash(n))
    seen = []
    for n in range(1, 6):
        [cand] = s.restore_lookup(tx_hash(n))
        seen.append((cand.kind, cand.item, cand.choices))
    assert seen == [("unlock", "chapter2", ()), ("tip", None, ()),
                    (None, None, ("chapter2", "ost", "tip")), (None, None, ("ost", "tip")),
                    (None, None, ("tip",))]


@scenario("restore/ambiguous_amounts_are_applied_with_choices", covers=(
    covers("restore", "apply_auto_and_chosen_candidates")))
def _ambiguous_applied(s):
    """One transaction with two payments: 4.993742 is recognised, a whole-cent 5.00 is not
    (kind null plus choices), so applying it needs the player's choice. The chosen item is
    stored with source "restore", and applying again adds nothing."""
    nodes, usdc = _game(s)
    nodes["arbitrum"].add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, 4_993_742, block=50,
                                   tx_hash=tx_hash(1), log_index=0)
    nodes["arbitrum"].add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, 5_000_000, block=50,
                                   tx_hash=tx_hash(1), log_index=1)
    found = s.restore_lookup(tx_hash(1))
    lookup = len(s.steps) - 1
    assert [(c.kind, c.choices) for c in found] == [
        ("unlock", ()), (None, ("chapter2", "ost", "tip"))]
    added = s.restore_apply(lookup, {1: "ost"})
    assert [(r["kind"], r["item"], r["source"]) for r in added] == [
        ("unlock", "chapter2", "restore"), ("unlock", "ost", "restore")]
    assert added[1]["amount"] == str(Decimal("5.000000"))
    assert s.steps[-1]["snapshot"]["owned"] == ["chapter2", "ost"]
    assert s.restore_apply(lookup, {1: "ost"}) == []


@scenario("restore/apply_without_a_choice_or_with_a_bad_one", covers=(
    covers("restore", "apply_skips_unchosen_and_rejects_invalid_choice")))
def _apply_choices(s):
    """A 1.00 payment is ambiguous. Applying with no choice stores nothing; choosing Chapter 2
    is refused (1.00 cannot buy a 4.99 item); choosing a tip stores a tip."""
    nodes, usdc = _game(s)
    nodes["arbitrum"].add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, 1_000_000, block=50,
                                   tx_hash=tx_hash(1))
    [cand] = s.restore_lookup(tx_hash(1))
    lookup = len(s.steps) - 1
    assert (cand.kind, cand.choices) == (None, ("tip",))
    assert s.restore_apply(lookup) == []
    assert s.steps[-1]["snapshot"]["receipts"] == {}
    assert s.expect("other").restore_apply(lookup, {0: "chapter2"}) is None
    assert s.steps[-1]["snapshot"]["receipts"] == {}
    [tip] = s.restore_apply(lookup, {0: "tip"})
    assert tip["kind"] == "tip" and len(s.steps[-1]["snapshot"]["receipts"]) == 1


@scenario("restore/already_restored", covers=covers("restore", "lookup_marks_already_restored"))
def _already_restored(s):
    """A payment whose receipt is stored is marked already restored, per log: the first
    lookup finds a new payment, and after applying it the same lookup says so."""
    nodes, usdc = _game(s)
    nodes["arbitrum"].add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, 4_993_742, block=50,
                                   tx_hash=tx_hash(1), log_index=3)
    [fresh] = s.restore_lookup(tx_hash(1))
    assert fresh.already_restored is False
    s.restore_apply(len(s.steps) - 1)
    [again] = s.restore_lookup(tx_hash(1))
    assert again.already_restored is True and again.transfer.log_index == 3


# --- restore: errors --------------------------------------------------------------------------


@scenario("restore/not_found", covers=covers("restore", "lookup_not_found_anywhere"))
def _not_found(s):
    """No chain knows the transaction: every chain is asked (the game's chains first) and
    the code is tx_not_found."""
    _game(s)
    assert s.restore_lookup(tx_hash(1)) is None
    assert s.steps[-1]["raises"] == {"type": "RestoreError", "code": "tx_not_found"}
    urls = [u for u, _ in _receipt_calls(s)]
    assert urls[0] == s.chain("arbitrum").rpc[0]
    assert all(any(u in s.chain(k).rpc for u in urls) for k in EVM_CHAINS)


@scenario("restore/reverted", covers=covers("restore", "lookup_errors"))
def _reverted(s):
    """A transaction the chain reverted is tx_reverted, though it carries a payment log."""
    nodes, usdc = _game(s)
    nodes["arbitrum"].add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, 4_993_742, block=50,
                                   tx_hash=tx_hash(1))
    nodes["arbitrum"].receipts[tx_hash(1)]["status"] = "0x0"
    assert s.restore_lookup(tx_hash(1)) is None
    assert s.steps[-1]["raises"] == {"type": "RestoreError", "code": "tx_reverted"}


@scenario("restore/not_a_payment", covers=covers("restore", "lookup_errors"))
def _not_a_payment(s):
    """A successful transaction that paid nobody the game accepts is tx_not_a_payment: a
    transfer between two other wallets, a transfer to the developer of a token the game does
    not know, and a transaction with no logs."""
    nodes, usdc = _game(s)
    node = nodes["arbitrum"]
    node.add_transfer(usdc["arbitrum"], PAYER_EVM, PAYER_EVM, 5_000_000, block=50,
                      tx_hash=tx_hash(1))
    node.add_transfer("0x" + "11" * 20,
                      PAYER_EVM, DEV_EVM, 4_993_742, block=50, tx_hash=tx_hash(2))
    node.receipts[tx_hash(3)] = {"transactionHash": tx_hash(3), "blockNumber": hex(50),
                                 "blockHash": node.block_hash(50), "status": "0x1", "logs": []}
    for n in (1, 2, 3):
        assert s.restore_lookup(tx_hash(n)) is None
        assert s.steps[-1]["raises"] == {"type": "RestoreError", "code": "tx_not_a_payment"}


@scenario("restore/network_unavailable", covers=(
    covers("restore", "lookup_network_trouble_is_reported_when_not_found")))
def _network_unavailable(s):
    """A chain that cannot be asked turns "not found" into network_unavailable: Base's
    endpoints are all down and Arbitrum does not know the transaction. With every endpoint of
    every chain down it is network_unavailable too. A payment that a reachable chain does
    know is still found while another chain is down."""
    nodes, usdc = _game(s)
    for url in s.chain("base").rpc:
        nodes["base"].endpoint(url).down = True
    assert s.restore_lookup(tx_hash(1)) is None
    assert s.steps[-1]["raises"] == {"type": "RestoreError", "code": "network_unavailable"}
    nodes["arbitrum"].add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, 4_993_742, block=50,
                                   tx_hash=tx_hash(2))
    [cand] = s.restore_lookup(tx_hash(2))
    assert cand.chain == "arbitrum"
    for key in EVM_CHAINS:
        for url in s.chain(key).rpc:
            nodes[key].endpoint(url).down = True
    assert s.restore_lookup(tx_hash(3)) is None
    assert s.steps[-1]["raises"] == {"type": "RestoreError", "code": "network_unavailable"}


@scenario("restore/invalid_hash", covers=covers("restore", "lookup_invalid_hash"))
def _invalid_hash(s):
    """Text that is neither a transaction hash nor a signature is invalid_hash, and no
    endpoint is asked."""
    _game(s)
    for text in ("0x123", "not a hash"):
        assert s.restore_lookup(text) is None
        assert s.steps[-1]["raises"] == {"type": "RestoreError", "code": "invalid_hash"}
        assert s.steps[-1]["calls"] == []


# --- reverify ---------------------------------------------------------------------------------


def _receipt(n, *, recipient=DEV_EVM, units=4_993_742, block=900, log_index=2,
             chain="arbitrum", token="USDC"):
    """A stored payment receipt (the schema of conformance/schema/state.json)."""
    return {"chain": chain, "tx_hash": tx_hash(n), "log_index": log_index, "block": block,
            "kind": "unlock", "item": "chapter2",
            "amount": str(Decimal(units) / 10**6), "token": token, "payer": PAYER_EVM,
            "recipient": recipient, "source": "payment"}


def _key(n, chain="arbitrum", log_index=2):
    return f"{chain}:{tx_hash(n)}:{log_index}"


def _reverify_game(s, **config):
    s.config(wallet=DEV_EVM, **config)
    s.item(**CHAPTER2)
    node = s.evm("arbitrum")
    return node, s.chain("arbitrum").tokens["USDC"].address


@scenario("reverify/genuine_receipt_is_kept", covers=covers(
    "reverify", "genuine_receipt_is_kept", "lookup_is_scoped_to_the_receipt_block"))
def _genuine_kept(s):
    """A receipt whose log is on chain stays. The check asks one USDC Transfer query for the
    receipt's own block (from = to = 900), to the developer's wallet."""
    node, usdc = _reverify_game(s)
    s.initial(receipts={_key(1): _receipt(1)})
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 4_993_742, block=900, tx_hash=tx_hash(1),
                      log_index=2)
    assert s.reverify() == []
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [_key(1)]
    logs = [c for c in s.steps[-1]["calls"] if c["method"] == "eth_getLogs"]
    [call] = logs
    [flt] = call["params"]
    assert (flt["address"].lower(), flt["fromBlock"], flt["toBlock"]) == (
        usdc.lower(), hex(900), hex(900))
    assert flt["topics"][1] is None and len(flt["topics"][2]) == 1


@scenario("reverify/contradicted_receipts_are_revoked", covers=covers(
    "reverify", "mismatching_log_revokes", "invented_receipt_with_no_log_is_revoked"))
def _contradicted_revoked(s):
    """Next to a genuine receipt, five that the chain contradicts are removed: another amount,
    another log index, another transaction, no log at all, and a payment to someone else."""
    node, usdc = _reverify_game(s)
    receipts = {_key(1): _receipt(1, block=900)}
    for n in range(2, 7):
        receipts[_key(n)] = _receipt(n, block=900 + n)
    s.initial(receipts=receipts)
    pay = dict(token=usdc, sender=PAYER_EVM, recipient=DEV_EVM)

    def log(n, block, **kw):
        args = dict(pay, value=4_993_742, block=block, tx_hash=tx_hash(n), log_index=2)
        args.update(kw)
        node.add_transfer(args.pop("token"), args.pop("sender"), args.pop("recipient"),
                          args.pop("value"), **args)

    log(1, 900)                                # genuine
    log(2, 902, value=4_993_743)               # another amount
    log(3, 903, log_index=3)                   # another log in the same transaction
    log(4, 904, tx_hash=tx_hash(99))           # another transaction
    # receipt 5: no log at all (invented)
    log(6, 906, recipient=PAYER_EVM)           # paid to someone else
    assert s.reverify() == sorted(_key(n) for n in (2, 3, 4, 5, 6))
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [_key(1)]


@scenario("reverify/outage_keeps_everything", covers=covers("reverify", "outage_keeps_everything"))
def _outage_keeps(s):
    """Not being able to ask judges nothing: with every endpoint down the receipts stay."""
    node, _ = _reverify_game(s)
    s.initial(receipts={_key(1): _receipt(1)})        # no such log on chain
    for url in s.chain("arbitrum").rpc:
        node.endpoint(url).down = True
    assert s.reverify() == []
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [_key(1)]
    node.endpoint(s.chain("arbitrum").rpc[0]).down = False
    s.advance(600)
    assert s.reverify() == [_key(1)]                  # asked again, the same receipt is revoked


@scenario("reverify/blocks_ahead_of_the_node", covers=covers(
    "reverify", "block_slightly_ahead_of_lagging_head_is_kept",
    "impossible_future_block_is_revoked"))
def _blocks_ahead(s):
    """A receipt a few blocks past the node's head is kept (the node is probably lagging, and
    no log query is made); one more than FUTURE_TOLERANCE_BLOCKS (10 000) ahead is
    impossible and revoked."""
    _reverify_game(s)
    s.initial(receipts={_key(1): _receipt(1, block=1005),
                        _key(2): _receipt(2, block=1000 + FUTURE_TOLERANCE_BLOCKS + 1)})
    assert s.reverify() == [_key(2)]
    assert [c["method"] for c in s.steps[-1]["calls"]] == ["eth_blockNumber"] * 2
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [_key(1)]


@scenario("reverify/previous_wallet_dropped_when_unlisted", covers=covers(
    "reverify", "previous_wallet_payments_survive_wallet_change"))
def _previous_wallet_dropped(s):
    """The log of a payment to the old wallet is on chain, but the game no longer lists that
    wallet: the receipt is revoked. (With the wallet listed it survives: see
    evm/reverify_accepted_wallets.)"""
    node, usdc = _reverify_game(s)
    s.initial(receipts={_key(1): _receipt(1, recipient=OLD_DEV_EVM)})
    node.add_transfer(usdc, PAYER_EVM, OLD_DEV_EVM, 4_993_742, block=900, tx_hash=tx_hash(1),
                      log_index=2)
    assert s.reverify() == [_key(1)]


@scenario("reverify/token_or_chain_unknown", covers=(
    covers("reverify", "unknown_chain_is_left_alone", "token_not_on_chain_is_revoked")))
def _token_or_chain_unknown(s):
    """A receipt for a token its chain does not have (USDT on Base) is revoked without
    asking anyone; one for a chain this build does not know is left alone."""
    s.config(wallet=DEV_EVM)
    s.item(**CHAPTER2)
    s.initial(receipts={_key(1, "base"): _receipt(1, chain="base", token="USDT"),
                        _key(2, "tron"): _receipt(2, chain="tron")})
    assert s.reverify() == [_key(1, "base")]
    assert s.steps[-1]["calls"] == []
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [_key(2, "tron")]


@scenario("reverify/malformed_entries_are_removed", covers=covers(
    "reverify", "malformed_entries_are_removed"))
def _malformed_entries(s):
    """Stored entries that are not receipts (empty, missing fields, a non-numeric log index) are
    removed, each with its reason reported (an internal error); the genuine receipt next to
    them stays."""
    node, usdc = _reverify_game(s)
    junk = {"chain": "arbitrum", "tx_hash": tx_hash(9), "log_index": "x", "block": 1,
            "amount": "1", "token": "USDC"}
    s.initial(receipts={_key(1): _receipt(1), "junk0": {}, "junk1": {"chain": "arbitrum"},
                         "junk2": junk})
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 4_993_742, block=900, tx_hash=tx_hash(1),
                      log_index=2)
    assert s.expect("internal_error").reverify() == ["junk0", "junk1", "junk2"]
    assert s.steps[-1]["internal_errors"] == 3                    # each removed with a reason
    assert list(s.steps[-1]["snapshot"]["receipts"]) == [_key(1)]


@scenario("reverify/invalid_receipts_are_removed", covers=covers(
    "reverify", "a_receipt_with_invalid_fields_is_removed_and_reported",
    "a_forged_out_of_range_receipt_no_longer_unlocks",
    "a_receipt_whose_check_crashes_is_kept_and_reported",
    "a_type_valid_but_malformed_receipt_is_removed_before_any_rpc",
    "a_malformed_receipt_on_an_unknown_chain_is_kept"))
def _invalid_removed(s):
    """A stored receipt whose own fields are invalid is removed with its reason reported (one
    internal error each): an infinite amount, a forged Chapter 2 unlock of 1E+999999 (out of the
    amount range; it owned nothing to begin with), a token name that cannot be a key, a negative
    block, a hash too short for an EVM chain, an EVM hash on Solana, a well-formed hash with
    str.strip() whitespace after it (\\x85, \\x1c), all with no RPC asked. A receipt whose
    check fails on what the node returned (a log in its block whose logIndex is not hex) is
    kept and reported, and so is one naming a chain this build does not know (it cannot be
    checked; a chain removed from the config must not erase purchases). The receipts after them
    are still checked: the invented one is revoked, the genuine one kept."""
    node, usdc = _reverify_game(s)
    s.initial(receipts={_key(1): dict(_receipt(1), amount="Infinity"),
                        _key(2): dict(_receipt(2), amount="1E+999999"),
                        _key(3): _receipt(3), _key(4): _receipt(4),
                        _key(5): _receipt(5, block=905),
                        _key(7): dict(_receipt(7), token=["USDC"]),
                        # type-valid but malformed for their chain family: checked before any RPC
                        _key(8): dict(_receipt(8), block=-1),
                        "arbitrum:0x1234:2": dict(_receipt(9), tx_hash="0x1234"),
                        _key(10, "solana"): dict(_receipt(10), chain="solana"),
                        _key(12): dict(_receipt(12), log_index="9" * 4301),   # past int()'s digits
                        "solana:" + "2" * 89 + ":2": dict(_receipt(13), chain="solana",
                                                          tx_hash="2" * 89),  # too long to decode
                        # str.strip() whitespace around a well-formed hash (JS trim() keeps it)
                        _key(14): dict(_receipt(14), tx_hash=tx_hash(14) + "\x85"),
                        _key(15): dict(_receipt(15), tx_hash=tx_hash(15) + "\x1c"),
                        # a chain this build does not know: kept, its family fields unchecked
                        "tron:0x1234:2": dict(_receipt(11), chain="tron", tx_hash="0x1234")})
    node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 4_993_742, block=900, tx_hash=tx_hash(3),
                      log_index=2)
    bad = node.add_transfer(usdc, PAYER_EVM, DEV_EVM, 1, block=905, tx_hash=tx_hash(6))
    bad["logIndex"] = "0xzz"
    removed = s.expect("internal_error").reverify()
    assert sorted(removed) == sorted([_key(1), _key(2), _key(4), _key(7), _key(8),
                                      "arbitrum:0x1234:2", _key(10, "solana"), _key(12),
                                      "solana:" + "2" * 89 + ":2", _key(14), _key(15)])
    assert s.steps[-1]["internal_errors"] == 11
    assert sorted(s.steps[-1]["snapshot"]["receipts"]) == sorted([_key(3), _key(5),
                                                                  "tron:0x1234:2"])
    assert s.steps[-1]["snapshot"]["owned"] == ["chapter2"]


@scenario("restore/too_large_to_decode_is_skipped", covers=covers(
    "restore", "lookup_skips_a_transfer_too_large_to_decode"))
def _too_large_skipped(s):
    """A transfer of 10**32 units or more cannot be priced (decode's 28 digits): it is skipped
    and counted as an internal error, the transfer next to it is still offered. A
    transaction whose only payment is such a transfer is tx_not_a_payment."""
    nodes, usdc = _game(s)
    node = nodes["arbitrum"]
    node.add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, 10**32, block=50,
                      tx_hash=tx_hash(1), log_index=1)
    node.add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, 4_993_742, block=50,
                      tx_hash=tx_hash(1), log_index=2)
    node.add_transfer(usdc["arbitrum"], PAYER_EVM, DEV_EVM, 2**256 - 1, block=51,
                      tx_hash=tx_hash(2), log_index=0)
    [cand] = s.expect("internal_error").restore_lookup(tx_hash(1))
    assert (cand.transfer.log_index, cand.kind, cand.item) == (2, "unlock", "chapter2")
    assert s.steps[-1]["internal_errors"] == 1
    assert s.expect("internal_error").restore_lookup(tx_hash(2)) is None
    assert s.steps[-1]["raises"] == {"type": "RestoreError", "code": "tx_not_a_payment"}
