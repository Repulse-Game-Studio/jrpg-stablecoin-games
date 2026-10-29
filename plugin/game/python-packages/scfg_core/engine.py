"""The Engine composes config, sessions, watcher, receipts and restore.

It owns all plugin state (deep copies of what Ren'Py keeps in ``persistent``) and is
NOT thread-safe: only the Worker thread calls it. The Ren'Py main thread reads
immutable Snapshots and submits commands through the Worker.

Methods that need the network are cooperative (see coio): start_session_co,
build_solana_payment_co, tick_co, reverify_co, restore_lookup_co. The same names without
_co run them to completion with the Engine's transport (desktop Worker thread, tools,
tests).
"""

from __future__ import annotations

import copy
import sys
import time
import traceback
from dataclasses import dataclass, replace
from decimal import Decimal
from typing import Callable

from . import restore as restore_mod
from .backends import backend_for
from .backends.evm import START_BLOCK_MARGIN
from .chains import Chain
from .coio import run_sync
from .config import Config, enabled_chains
from .errors import ConfigError
from .models import PaymentResult
from .payments import MIN_TIP, prepare_payment
from .receipts import compute_ownership
from .reverify import reverify_receipts
from .rpc import RpcError, RpcUnavailable
from .sessions import (
    create_session,
    is_past_horizon,
    is_past_window,
    remove_session,
    set_hint,
    set_ui_status,
)
from .watcher import StepOutcome

__all__ = ["MIN_TIP", "START_BLOCK_MARGIN", "Engine", "Snapshot"]


@dataclass(frozen=True)
class Snapshot:
    pending: dict
    receipts: dict
    owned: frozenset
    tip_total: Decimal
    unready: frozenset = frozenset()


def _print_error(exc: BaseException) -> None:
    traceback.print_exception(type(exc), exc, exc.__traceback__, file=sys.stderr)


class _LazyRpc:
    """Defers building a chain's RPC client until a backend actually calls it, so chains
    whose readiness needs no network (EVM) never construct a client."""

    def __init__(self, engine: Engine, key: str):
        self._engine, self._key = engine, key

    def __getattr__(self, name):
        return getattr(self._engine.rpc(self._key), name)


class Engine:
    def __init__(self, config: Config, *, transport=None, pending: dict | None = None,
                 receipts: dict | None = None, clock: Callable[[], float] = time.time,
                 rpc_factory: Callable[[Chain], object] | None = None,
                 on_error: Callable[[BaseException], None] | None = None):
        self.config = config
        self.on_error = on_error or _print_error
        self.pending = copy.deepcopy(dict(pending or {}))
        self.receipts = copy.deepcopy(dict(receipts or {}))
        self.clock = clock
        self.transport = transport
        self._rpc_factory = rpc_factory or (lambda chain: backend_for(chain).make_rpc(chain))
        self._rpcs: dict[str, object] = {}
        self.unready: set = set()

    # --- chains & RPC ---------------------------------------------------------

    def chain(self, key: str) -> Chain:
        """Full registry chain (all tokens, so old receipts stay verifiable) with overrides."""
        base = self.config.registry[key]
        return replace(base, rpc=self.config.rpc_urls.get(key, ()) + base.rpc,
                       confirmations=self.config.confirmations.get(key, base.confirmations))

    def rpc(self, key: str):
        if key not in self._rpcs:
            self._rpcs[key] = self._rpc_factory(self.chain(key))
        return self._rpcs[key]

    # --- cooperative jobs -------------------------------------------------------

    def run(self, gen):
        """Drive a cooperative job to completion with this Engine's transport."""
        return run_sync(gen, self.transport)

    # --- payments ---------------------------------------------------------------

    def start_session(self, *args, **kwargs) -> dict:
        return self.run(self.start_session_co(*args, **kwargs))

    def start_session_co(self, kind: str, chain: str, *, item: str | None = None, amount=None,
                         token: str | None = None, title: str | None = None):
        spec = prepare_payment(self.config, kind, item=item, amount=amount, token=token,
                               title=title)
        if chain not in spec.chains:
            raise ConfigError(f"{spec.token} is not available on chain {chain!r}")
        chain_obj = self.chain(chain)
        fields = yield from backend_for(chain_obj).start(
            rpc=self.rpc(chain), chain=chain_obj, wallet=self.config.wallet_for(chain_obj),
            token=spec.token)
        start = fields.pop("start_block")
        rec = create_session(self.pending, kind=spec.kind, item=spec.item, title=spec.title,
                             token=spec.token, chain=chain, price=spec.price, code=spec.code,
                             start_block=start, now=self.clock(), extra=fields)
        return copy.deepcopy(rec)

    def build_solana_payment(self, sid: str, account: str) -> dict:
        return self.run(self.build_solana_payment_co(sid, account))

    def build_solana_payment_co(self, sid: str, account: str):
        rec = self.pending[sid]
        chain = self.chain(rec["chain"])
        if chain.family != "solana":
            raise ValueError("not a Solana session")
        return (yield from backend_for(chain).build_payment(rec, rpc=self.rpc(chain.key),
                                                            chain=chain, account=account))

    def report_hint(self, sid: str, tx_hash: str) -> None:
        rec = self.pending[sid]
        normalize = backend_for(self.chain(rec["chain"])).normalize_tx
        set_hint(self.pending, sid, tx_hash, self.clock(), normalize=normalize)

    def set_ui_status(self, sid: str, status: str) -> None:
        if sid in self.pending:
            set_ui_status(self.pending, sid, status)

    def discard_session(self, sid: str) -> None:
        """Drop a session the player abandoned before paying (e.g. switched network)."""
        rec = self.pending.get(sid)
        if rec is not None and rec["state"] == "awaiting" and not rec["hint_tx"]:
            del self.pending[sid]

    def tick(self) -> list[StepOutcome]:
        return self.run(self.tick_co())

    def tick_co(self):
        now = self.clock()
        outcomes = []
        for sid, rec in list(self.pending.items()):
            outcome = yield from self._advance(sid, rec, now)
            if outcome is not None:
                outcomes.append(outcome)
        outcomes.extend(self._expire(now))
        return outcomes

    def _advance(self, sid: str, rec: dict, now: float):
        """One session's step. A session this build can no longer watch (its chain left the
        registry, or the game stopped accepting that family) is kept until its scan horizon,
        then closed. A crash in one session never stops the others."""
        try:
            key = rec["chain"]
            if key not in self.config.registry or self.config.wallet_for(self.chain(key)) is None:
                if not is_past_horizon(rec, now):
                    return None
                remove_session(self.pending, sid)
                return StepOutcome(sid, "closed")
            chain = self.chain(key)
            return (yield from backend_for(chain).advance(
                self.pending, self.receipts, sid, rpc=self.rpc(key), chain=chain,
                wallet=self.config.wallet_for(chain), items=self.config.items, now=now))
        except Exception as e:
            self.on_error(e)
            return StepOutcome(sid, "waiting", error="internal_error")

    def _expire(self, now: float) -> list[StepOutcome]:
        """Report the 30-minute payment window closing (scanning continues up to 24 h)."""
        out = []
        for sid, rec in self.pending.items():
            if rec["ui_status"] in ("open", "pending") and is_past_window(rec, now):
                rec["ui_status"] = "expired"
                out.append(StepOutcome(sid, "expired", result=PaymentResult(
                    status="expired", kind=rec["kind"], item=rec["item"], token=rec["token"],
                    chain=rec["chain"], session_id=sid)))
        return out

    # --- receipts ---------------------------------------------------------------

    def reverify(self) -> list[str]:
        return self.run(self.reverify_co())

    def reverify_co(self):
        """Launch checks: re-verify stored receipts, then check that the developer can
        receive each enabled token (Solana needs a token account)."""
        chains = {k: self.chain(k) for k in self.config.registry}
        wallets = {f: self.config.accepted_wallets_for(f) for f in ("evm", "solana")}
        removed = yield from reverify_receipts(self.receipts, rpc_for=self.rpc, chains=chains,
                                               wallets=wallets)
        yield from self._check_ready()
        return removed

    def _check_ready(self):
        unready = set()
        for key, chain in enabled_chains(self.config).items():
            wallet = self.config.wallet_for(chain)
            try:
                ready = yield from backend_for(chain).check_ready(
                    rpc=_LazyRpc(self, key), chain=chain, wallet=wallet)
            except (RpcUnavailable, RpcError):
                continue  # never hide a payment option on doubt
            except Exception as e:
                self.on_error(e)
                continue
            unready.update((key, token) for token, ok in ready.items() if not ok)
        self.unready = unready

    def restore_lookup(self, tx_hash: str) -> list[restore_mod.RestoreCandidate]:
        return self.run(self.restore_lookup_co(tx_hash))

    def restore_lookup_co(self, tx_hash: str):
        registry = self.config.registry
        ordered = list(self.config.chains) + [k for k in registry if k not in self.config.chains]
        chains = {k: self.chain(k) for k in ordered}
        return (yield from restore_mod.lookup(tx_hash, config=self.config, chains=chains,
                                              rpc_for=self.rpc, receipts=self.receipts))

    def restore_apply(self, candidates, choices: dict | None = None) -> list[dict]:
        return restore_mod.apply(self.receipts, candidates, choices)

    def snapshot(self) -> Snapshot:
        owned, tips = compute_ownership(self.receipts, self.config.items)
        return Snapshot(pending=copy.deepcopy(self.pending), receipts=copy.deepcopy(self.receipts),
                        owned=frozenset(owned), tip_total=tips,
                        unready=frozenset(self.unready))
