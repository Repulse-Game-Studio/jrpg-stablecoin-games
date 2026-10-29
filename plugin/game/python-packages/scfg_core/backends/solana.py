"""Solana: payments are found by their Solana Pay reference, by a reported signature,
or by scanning the developer's token account for the exact encoded amount. A payment
counts once the cluster marks its transaction finalized.

What a transaction paid is read from meta.pre/postTokenBalances, never from its
instructions, so it works whatever program or wallet moved the tokens.
"""

from __future__ import annotations

import base64
import secrets
from decimal import Decimal

from ..amounts import decimal_to_units, units_to_decimal
from ..receipts import add_receipt, make_receipt, receipt_key
from ..rpc import RpcClient, RpcError, RpcUnavailable
from ..sessions import clear_hint, is_past_horizon, remove_session
from ..solana.base58 import b58encode, normalize_pubkey, normalize_signature
from ..solana.pda import associated_token_address
from ..solana.rpc import SolanaRpc
from ..solana.tx import (
    compile_message,
    set_compute_unit_limit,
    set_compute_unit_price,
    transfer_checked,
    unsigned_transaction,
)
from ..transfers import Transfer
from ..watcher import StepOutcome, min_amount, result_from_match

FINAL_CONFIRMATIONS = 32  # Solana's "finalized" corresponds to 32 confirmed blocks
SCAN_TX_BUDGET = 5        # getTransaction calls per step for the token-account scan
SIG_PAGE = 100
MAX_SIG_PAGES = 10
START_SLOT_MARGIN = 150
COMPUTE_UNITS = 30_000
MIN_PRIORITY_FEE = 1_000        # micro-lamports per compute unit
MAX_PRIORITY_FEE = 1_000_000    # x 30,000 CU = at most 30,000 lamports (0.00003 SOL)
# A wallet returns the signature before the transaction lands, and a transaction whose
# blockhash expires (~60-90 s) never lands. A hint unseen for this long was dropped.
SOLANA_HINT_TIMEOUT_S = 180


def _fee_payer(tx: dict) -> str | None:
    keys = ((tx.get("transaction") or {}).get("message") or {}).get("accountKeys") or []
    if not keys:
        return None
    first = keys[0]
    return first if isinstance(first, str) else first.get("pubkey")


def transfer_to(tx: dict, *, sig: str, mint: str, owner: str) -> Transfer | None:
    """What `owner` received of `mint` in a transaction, from its token-balance changes."""
    meta = tx.get("meta") or {}
    if meta.get("err") is not None:
        return None

    def table(name: str) -> dict:
        rows = {}
        for b in meta.get(name) or []:
            if b.get("mint") == mint:
                rows[b["accountIndex"]] = (b.get("owner"), int(b["uiTokenAmount"]["amount"]))
        return rows

    pre, post = table("preTokenBalances"), table("postTokenBalances")
    gained, index, drops = 0, None, {}
    for idx in sorted(set(pre) | set(post)):
        who = (post.get(idx) or pre.get(idx))[0]
        delta = post.get(idx, (who, 0))[1] - pre.get(idx, (who, 0))[1]
        if who == owner:
            gained += delta
            if delta > 0 and index is None:
                index = idx
        elif delta < 0:
            drops[who] = drops.get(who, 0) + delta
    if gained <= 0 or index is None:
        return None
    payer = min(drops, key=drops.get) if drops else _fee_payer(tx)
    return Transfer(token=mint, sender=payer, recipient=owner, value=gained, tx_hash=sig,
                    log_index=index, block_number=int(tx["slot"]))


def _match_dict(t: Transfer) -> dict:
    return {"tx_hash": t.tx_hash, "log_index": t.log_index, "block": t.block_number,
            "value": t.value, "payer": t.sender, "recipient": t.recipient}


class SolanaBackend:
    family = "solana"

    def normalize_tx(self, text):
        return normalize_signature(text)

    def make_rpc(self, chain):
        return SolanaRpc(RpcClient(chain.rpc))

    def build_payment(self, rec, *, rpc, chain, account):
        """Unsigned transfer from the player's wallet, for the browser page to hand to
        the wallet. The wallet simulates it and shows the player what it does."""
        account = normalize_pubkey(account)
        mint = chain.tokens[rec["token"]].address
        source = associated_token_address(account, mint)
        token_balance = yield from rpc.get_token_balance(source)
        sol_balance = yield from rpc.get_balance(account)
        blockhash = yield from rpc.get_latest_blockhash()
        fee = yield from rpc.get_priority_fee([rec["dest"]])
        price = min(MAX_PRIORITY_FEE, max(MIN_PRIORITY_FEE, fee))
        instructions = [
            set_compute_unit_limit(COMPUTE_UNITS), set_compute_unit_price(price),
            transfer_checked(source=source, mint=mint, destination=rec["dest"], owner=account,
                             amount=rec["units"], decimals=6, extra=(rec["reference"],)),
        ]
        tx = unsigned_transaction(compile_message(account, instructions, blockhash))
        return {"transaction": base64.b64encode(tx).decode("ascii"),
                "token_balance": str(token_balance), "sol_balance": str(sol_balance)}

    def start(self, *, rpc, chain, wallet, token):
        dest = associated_token_address(wallet, chain.tokens[token].address)
        slot = yield from rpc.get_slot()
        newest = yield from rpc.get_signatures_for_address(dest, limit=1)
        return {"start_block": max(0, slot - START_SLOT_MARGIN),
                "reference": b58encode(secrets.token_bytes(32)), "dest": dest,
                "scan_cursor": newest[0]["signature"] if newest else None,
                "scan_before": None}

    # --- watching -----------------------------------------------------------------

    def advance(self, pending, receipts, sid, *, rpc, chain, wallet, items, now):
        rec = pending.get(sid)
        out = StepOutcome(session_id=sid, status="missing", required=FINAL_CONFIRMATIONS)
        if rec is None:
            return out
        mint = chain.tokens[rec["token"]].address
        try:
            if rec["state"] == "awaiting":
                out.status = "waiting"
                if is_past_horizon(rec, now):
                    remove_session(pending, sid)
                    out.status = "closed"
                    return out
                outcome = None
                if rec["hint_tx"]:
                    outcome = yield from self._verify_hint(rec, rpc, chain, mint, wallet, items,
                                                           receipts, now, out)
                if outcome is None:
                    # The reference also runs while a hint is pending: the hinted transaction
                    # may have been dropped and the player paid again.
                    outcome = yield from self._by_reference(rec, rpc, chain, mint, wallet, items,
                                                            receipts)
                    if outcome is not None:
                        clear_hint(rec)
                        out.error = out.hint_tx = None
                    elif not rec["hint_tx"] and (yield from self._scan(
                            rec, rpc, chain, mint, wallet, receipts)):
                        outcome = "matched"
                if outcome == "underpaid":
                    remove_session(pending, sid)
                    out.status = "underpaid"
                    out.result = result_from_match(rec, "underpaid", chain.key, rec["match"])
                    return out
                if outcome != "matched":
                    return out
                rec["state"] = "confirming"

            out.status = "confirming"
            m = rec["match"]
            status = (yield from rpc.get_signature_statuses([m["tx_hash"]]))[0]
            if status is None:
                # Unknown to this endpoint: a dropped fork or a lagging endpoint. Keep the
                # signature as a hint so it is verified again rather than lost.
                rec["state"], rec["match"] = "awaiting", None
                rec["hint_tx"], rec["hint_at"] = m["tx_hash"], now
                out.status = "reorged"
                return out
            if status.get("confirmationStatus") != "finalized":
                out.confirmations = int(status.get("confirmations") or 0)
                return out
            out.confirmations = FINAL_CONFIRMATIONS
            transfer = Transfer(mint, m["payer"], m["recipient"], m["value"], m["tx_hash"],
                                m["log_index"], m["block"])
            add_receipt(receipts, make_receipt(chain=chain.key, transfer=transfer,
                                               kind=rec["kind"], item=rec["item"],
                                               token=rec["token"], source="payment"))
            remove_session(pending, sid)
            out.status = "paid"
            out.result = result_from_match(rec, "paid", chain.key, m)
            return out
        except (RpcUnavailable, RpcError):
            # A verdict on the hint from earlier in this step wins: the hint is already
            # cleared, and later steps are plain "waiting", so it would never be reported.
            if out.error is None:
                out.error = "network_unavailable"
            return out

    def _qualifies(self, t, rec, chain, receipts) -> bool:
        return (t is not None and t.block_number >= rec["start_block"]
                and receipt_key(chain.key, t.tx_hash, t.log_index) not in receipts)

    def _verdict(self, t, rec, items) -> str:
        rec["match"] = _match_dict(t)
        return "underpaid" if units_to_decimal(t.value) < min_amount(rec, items) else "matched"

    def _verify_hint(self, rec, rpc, chain, mint, wallet, items, receipts, now, out):
        expired = now - rec["hint_at"] >= SOLANA_HINT_TIMEOUT_S
        try:
            tx = yield from rpc.get_transaction(rec["hint_tx"])
        except RpcError:
            if not expired:  # an endpoint that keeps refusing must not keep a hint forever
                raise
            tx = None
        if tx is None:  # not visible yet, or dropped: the player may pay again after a while
            if expired:
                self._drop_hint(rec, out, "tx_not_landed")
            return None
        if (tx.get("meta") or {}).get("err") is not None:
            self._drop_hint(rec, out, "tx_reverted")
            return None
        t = transfer_to(tx, sig=rec["hint_tx"], mint=mint, owner=wallet)
        if not self._qualifies(t, rec, chain, receipts):
            self._drop_hint(rec, out, "tx_not_a_payment")
            return None
        return self._verdict(t, rec, items)

    @staticmethod
    def _drop_hint(rec, out, error: str) -> None:
        out.error, out.hint_tx = error, rec["hint_tx"]
        clear_hint(rec)

    def _by_reference(self, rec, rpc, chain, mint, wallet, items, receipts):
        """The reference ties a transaction to this session, so any amount matches."""
        infos = yield from rpc.get_signatures_for_address(rec["reference"], limit=SIG_PAGE)
        for info in reversed(infos):
            if info.get("err") is not None:
                continue
            tx = yield from rpc.get_transaction(info["signature"])
            t = transfer_to(tx, sig=info["signature"], mint=mint, owner=wallet) if tx else None
            if self._qualifies(t, rec, chain, receipts):
                return self._verdict(t, rec, items)
        return None

    def _walk(self, rpc, address: str, until, before, min_slot: int):
        """Pages of signatures on `address` between `until` (exclusive, older bound) and
        `before` (exclusive, newer bound), newest first. Also says whether the walk reached
        `until`; if not, the pages cover only the newest MAX_SIG_PAGES * SIG_PAGE of them.

        A signature from before `min_slot` also ends the walk (and is dropped): with no
        cursor, or one this endpoint doesn't know, the walk would otherwise go through the
        account's whole history."""
        found = []
        for _ in range(MAX_SIG_PAGES):
            page = yield from rpc.get_signatures_for_address(address, until=until, before=before,
                                                             limit=SIG_PAGE)
            for n, info in enumerate(page):
                if info.get("slot") is not None and int(info["slot"]) < min_slot:
                    found.extend(page[:n])
                    return found, True
            found.extend(page)
            if len(page) < SIG_PAGE:
                return found, True
            before = page[-1]["signature"]
        # The last page was full too: reached `until` only if nothing older is left.
        more = yield from rpc.get_signatures_for_address(address, until=until, before=before,
                                                         limit=1)
        return found, not more

    def _scan(self, rec, rpc, chain, mint, wallet, receipts) -> bool:
        """Exchange and manual transfers carry no reference: match the exact amount.

        Signatures are only listed newest first, so a backlog larger than one walk is
        crossed in steps: each walk that cannot reach `scan_cursor` moves `scan_before` back
        and examines nothing; once a walk reaches the cursor its window is processed oldest
        first, then `scan_before` is cleared to look at the newer signatures again. A busy
        token account only delays detection, it never hides a payment. Signatures from
        before the session's start slot are never examined.
        """
        found, reached = yield from self._walk(rpc, rec["dest"], rec["scan_cursor"],
                                               rec.get("scan_before"), rec["start_block"])
        if not reached:
            rec["scan_before"] = found[-1]["signature"]
            return False
        budget = SCAN_TX_BUDGET
        for info in reversed(found):
            if info.get("err") is not None:
                rec["scan_cursor"] = info["signature"]
                continue
            if budget == 0:
                return False
            budget -= 1
            tx = yield from rpc.get_transaction(info["signature"])
            if tx is None:  # listed but not served yet: retry from here next step
                return False
            rec["scan_cursor"] = info["signature"]
            t = transfer_to(tx, sig=info["signature"], mint=mint, owner=wallet)
            if self._qualifies(t, rec, chain, receipts) and t.value == rec["units"]:
                rec["match"] = _match_dict(t)
                return True
        rec["scan_before"] = None  # window done: look at the newer signatures again
        return False

    # --- launch re-check, restore, readiness ---------------------------------------

    def reverify(self, receipt, *, rpc, chain, wallets):
        from ..reverify import FUTURE_TOLERANCE_BLOCKS

        slot = int(receipt["block"])
        owner = receipt.get("recipient")
        if owner not in wallets:
            return "drop"
        try:
            current = yield from rpc.get_slot("finalized")
            if slot > current:
                return "drop" if slot > current + FUTURE_TOLERANCE_BLOCKS else "keep"
            tx, authoritative = yield from rpc.get_transaction_any(receipt["tx_hash"],
                                                                   min_slot=slot)
        except (RpcUnavailable, RpcError):
            return "keep"
        if tx is None:
            return "drop" if authoritative else "keep"
        try:
            t = transfer_to(tx, sig=receipt["tx_hash"],
                            mint=chain.tokens[receipt["token"]].address, owner=owner)
        except (KeyError, TypeError, ValueError, AttributeError):
            return "keep"  # a payload this build can't read: can't judge the receipt
        genuine = (t is not None and t.block_number == slot
                   and t.log_index == int(receipt["log_index"])
                   and t.value == decimal_to_units(Decimal(receipt["amount"])))
        return "keep" if genuine else "drop"

    def lookup(self, tx, *, config, chain, rpc, receipts):
        from ..restore import RestoreCandidate, RestoreError, classify

        found, _ = yield from rpc.get_transaction_any(tx)
        if found is None:
            return None
        if (found.get("meta") or {}).get("err") is not None:
            raise RestoreError("tx_reverted")
        out = []
        for name, token in chain.tokens.items():
            for owner in config.accepted_wallets_for("solana"):
                t = transfer_to(found, sig=tx, mint=token.address, owner=owner)
                if t is None:
                    continue
                kind, item, choices = classify(config, t.value)
                out.append(RestoreCandidate(
                    chain=chain.key, token=name, transfer=t, kind=kind, item=item,
                    choices=choices,
                    already_restored=receipt_key(chain.key, t.tx_hash, t.log_index) in receipts))
        if not out:
            raise RestoreError("tx_not_a_payment")
        return out

    def check_ready(self, *, rpc, chain, wallet):
        """Wallets refuse to pay a wallet with no token account for the mint."""
        ready = {}
        for name, token in chain.tokens.items():
            ready[name] = yield from rpc.account_exists(
                associated_token_address(wallet, token.address))
        return ready
