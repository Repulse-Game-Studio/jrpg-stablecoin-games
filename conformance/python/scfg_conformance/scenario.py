"""Tier 2 scenarios: drive the real Engine against simulated nodes and record every step.

    from ..scenario import scenario

    @scenario("smoke/evm_unlock_paid", covers=["tests/unit/test_engine.py::test_x"])
    def _(s):
        s.config(wallet=DEV_EVM, testnet=True, chains=["base"])
        s.item(**CHAPTER2)
        node = s.evm("base")
        rec = s.start_session("unlock", "base", item="chapter2")
        ...

A scenario function registers `tier2/<id>.json`. At generation time it runs against a fresh
`Scenario`; every action records one step (see conformance/README.md, "Tier 2"), and the
finished vector is replayed once (replay.replay) before it is written, so a vector that is
generated at all is known to replay.

Recording and replay share replay.build_engine / run_action / encode_returns / snapshot_json;
only the transport (`RecordingTransport` over a simnode.World) and the entropy
(`RecordingEntropy`, random.Random(<id>)) differ.
"""

from __future__ import annotations

import copy
import json
import random
import re
from typing import Any, Callable

from scfg_core.coio import TransportError
from scfg_core.engine import Engine

from . import canonical
from .replay import (
    build_config_from,
    build_engine,
    encode_returns,
    encode_value,
    patched_entropy,
    replay,
    run_action,
    snapshot_json,
)
from .simnode import EvmNode, SolanaNode, World

__all__ = ["EPOCH", "EXPECTABLE", "SOURCES", "RecordingEntropy", "RecordingTransport",
           "Scenario", "ScenarioError", "scenario"]

EPOCH = 1_700_000_000
SOURCES: dict[str, Callable[[], dict]] = {}
_ID = re.compile(r"^[a-z0-9_]+(/[a-z0-9_]+)*$")
_TEST_ID = re.compile(r"^tests/unit/test_\w+\.py::(Test\w+::)?test_\w+$")
# Outcomes a step may only have when the scenario says so beforehand (Scenario.expect): an
# Engine crash it swallowed, or an exception that is not a ConfigError/RestoreError. Unasked,
# either is far more likely a simulator or scenario bug than reference behaviour.
EXPECTABLE = ("internal_error", "other")


class ScenarioError(AssertionError):
    """A step did something the scenario did not declare (see Scenario.expect)."""


class RecordingTransport:
    """post_json over a World, recording each exchange into `calls` (reset per step).
    Parses the response text like rpc.UrllibTransport: unparseable text is
    TransportError("invalid JSON response")."""

    def __init__(self, world: World):
        self.world = world
        self.calls: list[dict] = []

    def post_json(self, url: str, payload: dict, timeout: float):
        entry = {"url": url, "method": payload["method"],
                 "params": json.loads(json.dumps(payload["params"]))}
        try:
            text = self.world.handle(url, payload)
        except TransportError as e:
            entry["transport_error"] = {"message": str(e), "status": e.status}
            self.calls.append(entry)
            raise TransportError(str(e), status=e.status) from None
        try:
            response = json.loads(text)
        except ValueError:
            entry["transport_error"] = {"message": "invalid JSON response", "status": None}
            self.calls.append(entry)
            raise TransportError("invalid JSON response") from None
        entry["response_text"] = text
        self.calls.append(entry)
        return response


class RecordingEntropy:
    """secrets stand-in drawing from the scenario's random.Random(id); logs every draw."""

    def __init__(self, rng: random.Random):
        self._rng = rng
        self.draws: list[dict] = []

    def token_hex(self, n: int) -> str:
        value = self._rng.randbytes(n).hex()
        self.draws.append({"fn": "token_hex", "n": n, "value": value})
        return value

    def randbelow(self, n: int) -> int:
        value = self._rng.randrange(n)
        self.draws.append({"fn": "randbelow", "n": n, "value": value})
        return value

    def token_bytes(self, n: int) -> bytes:
        value = self._rng.randbytes(n)
        self.draws.append({"fn": "token_bytes", "n": n, "value": value.hex()})
        return value


def _jsonable(kwargs: dict) -> dict:
    out = {k: encode_value(v) for k, v in kwargs.items()}
    canonical.to_jsonable(out)  # fail at the call site, not at write time
    return json.loads(json.dumps(out))


class Scenario:
    """One recorded run. Setup first (config, custom_chain, item, initial), then nodes and
    actions in any order; `advance` moves the shared clock between steps."""

    def __init__(self, id: str, covers=()):
        self.id = id
        self.covers = list(covers)
        self.world = World()
        self.now = EPOCH
        self.steps: list[dict] = []
        self._config: dict | None = None
        self._custom_chains: list[dict] = []
        self._items: list[dict] = []
        self._pending: dict = {}
        self._receipts: dict = {}
        self._rng = random.Random(id)
        self._transport = RecordingTransport(self.world)
        self._engine: Engine | None = None
        self._errors = 0
        self._expect: frozenset = frozenset()
        self._crashes: list[BaseException] = []
        self.problems: list[str] = []   # undeclared outcomes; build() refuses to write them

    # --- setup ----------------------------------------------------------------------------

    def _setup_open(self) -> None:
        if self._engine is not None:
            raise RuntimeError("setup must come before the first action")

    def config(self, **build_config_kwargs) -> None:
        """build_config kwargs (custom chains go through custom_chain())."""
        self._setup_open()
        if "custom_chains" in build_config_kwargs:
            raise ValueError("add custom chains with custom_chain(**build_chain_kwargs)")
        self._config = _jsonable(build_config_kwargs)

    def custom_chain(self, **build_chain_kwargs) -> None:
        self._setup_open()
        self._custom_chains.append(_jsonable(build_chain_kwargs))

    def item(self, **add_item_kwargs) -> None:
        """add_item kwargs, including item_id."""
        self._setup_open()
        self._items.append(_jsonable(add_item_kwargs))

    def initial(self, pending: dict | None = None, receipts: dict | None = None) -> None:
        """State loaded from persistent before the first step."""
        self._setup_open()
        # Sorted by key: vector files serialise sorted keys, so recording must run in the
        # same order replay and the ports will.
        if pending is not None:
            self._pending = dict(sorted(
                json.loads(json.dumps(canonical.to_jsonable(pending))).items()))
        if receipts is not None:
            self._receipts = dict(sorted(
                json.loads(json.dumps(canonical.to_jsonable(receipts))).items()))

    @property
    def setup(self) -> dict:
        if self._config is None:
            raise RuntimeError("call config() first")
        return {"clock": EPOCH, "config": self._config, "custom_chains": self._custom_chains,
                "items": self._items, "pending": self._pending, "receipts": self._receipts}

    def chain(self, key: str):
        """The chain as the Engine sees it (registry entry with config overrides)."""
        return Engine(build_config_from(self.setup)).chain(key)

    # --- nodes ----------------------------------------------------------------------------

    def evm(self, key: str, urls=None) -> EvmNode:
        """A simulated node for EVM chain `key`, serving `urls` (default: every URL the
        Engine will try for it). Call again with other URLs for a second, diverging node."""
        return self.world.add(EvmNode(list(urls) if urls else list(self.chain(key).rpc)))

    def solana(self, key: str = "solana", urls=None) -> SolanaNode:
        return self.world.add(SolanaNode(list(urls) if urls else list(self.chain(key).rpc)))

    # --- time -----------------------------------------------------------------------------

    def advance(self, seconds: int) -> None:
        if isinstance(seconds, bool) or not isinstance(seconds, int) or seconds < 0:
            raise ValueError("advance() takes whole seconds >= 0")
        self.now += seconds

    # --- declared outcomes ------------------------------------------------------------------

    def expect(self, *outcomes: str) -> Scenario:
        """Declare that the NEXT action has these outcomes: "internal_error" (the Engine
        swallows at least one crash: internal_errors > 0) and/or "other" (the action raises
        something other than ConfigError/RestoreError). Without it, either outcome fails the
        scenario; with it, the outcome must happen. Authoring metadata only: it is not
        written to the vector. Chain it: s.expect("internal_error").tick()."""
        unknown = [o for o in outcomes if o not in EXPECTABLE]
        if unknown or not outcomes:
            raise ValueError(f"expect() takes one or more of {EXPECTABLE}, got {outcomes}")
        self._expect = frozenset(outcomes)
        return self

    # --- actions (each records one step and returns the Python result) --------------------

    def start_session(self, kind: str, chain: str, **kw):
        return self._step("start_session", {"kind": kind, "chain": chain, **kw})

    def build_solana_payment(self, sid: str, account: str):
        return self._step("build_solana_payment", {"sid": sid, "account": account})

    def report_hint(self, sid: str, tx: str):
        return self._step("report_hint", {"sid": sid, "tx": tx})

    def set_ui_status(self, sid: str, status: str):
        return self._step("set_ui_status", {"sid": sid, "status": status})

    def discard_session(self, sid: str):
        return self._step("discard_session", {"sid": sid})

    def tick(self):
        return self._step("tick", {})

    def reverify(self):
        return self._step("reverify", {})

    def restore_lookup(self, tx: str):
        return self._step("restore_lookup", {"tx": tx})

    def restore_apply(self, from_step: int, choices: dict | None = None):
        """Apply the candidates that step `from_step` (a restore_lookup) returned. `choices`
        maps candidate index -> item id or "tip" (stored with string keys)."""
        if not (0 <= from_step < len(self.steps)
                and self.steps[from_step]["action"] == "restore_lookup"
                and self.steps[from_step]["returns"] is not None):
            raise ValueError(f"step {from_step} is not a restore_lookup that returned candidates")
        encoded = None if choices is None else {str(k): v for k, v in choices.items()}
        return self._step("restore_apply", {"from_step": from_step, "choices": encoded})

    def _on_error(self, exc: BaseException) -> None:
        self._errors += 1
        self._crashes.append(exc)  # an undeclared one becomes the ScenarioError's cause

    def _check_declared(self, index: int, action: str, raises, raised) -> None:
        where = f"scenario {self.id} step {index} ({action})"
        found = []
        if self._errors and "internal_error" not in self._expect:
            found.append(f"{where}: {self._errors} internal error(s) the scenario did not "
                         "declare (a simulator or scenario bug? if intended, call "
                         "s.expect('internal_error') before the action)")
        if raises == {"type": "other"} and "other" not in self._expect:
            found.append(f"{where}: raised {raised!r}, which the scenario did not declare "
                         "(if intended, call s.expect('other') before the action)")
        if "internal_error" in self._expect and not self._errors:
            found.append(f"{where}: expected an internal error, none happened")
        if "other" in self._expect and raises != {"type": "other"}:
            found.append(f"{where}: expected an 'other' raise, got {raises}")
        if found:
            self.problems.extend(found)
            cause = raised if raised is not None else (self._crashes[0] if self._crashes else None)
            raise ScenarioError("; ".join(found)) from cause

    def _step(self, action: str, args: dict) -> Any:
        try:  # an expect() covers this action only, whatever happens
            return self._record(action, args)
        finally:
            self._expect = frozenset()

    def _record(self, action: str, args: dict) -> Any:
        args = _jsonable(args)
        if self._engine is None:
            self._engine = build_engine(self.setup, transport=self._transport,
                                        clock=lambda: self.now, on_error=self._on_error)
        self._transport.calls = []
        self._errors = 0
        self._crashes = []
        raised: list[BaseException] = []
        entropy = RecordingEntropy(self._rng)
        with patched_entropy(entropy):
            result, raises = run_action(self._engine, action, args, self.steps,
                                        on_raise=raised.append)
        step = {
            "at": self.now, "action": action, "args": args, "entropy": entropy.draws,
            "calls": self._transport.calls,
            "returns": None if raises else encode_returns(action, result),
            "raises": raises, "internal_errors": self._errors,
            "snapshot": snapshot_json(self._engine),
        }
        self.steps.append(json.loads(json.dumps(canonical.to_jsonable(step))))
        self._check_declared(len(self.steps) - 1, action, raises, raised[0] if raised else None)
        return result

    # --- output ---------------------------------------------------------------------------

    def to_vector(self) -> dict:
        if not self.steps:
            raise RuntimeError(f"scenario {self.id} has no steps")
        return copy.deepcopy({"tier": 2, "id": self.id, "covers": self.covers,
                              "setup": self.setup, "steps": self.steps})


def scenario(id: str, covers: list[str]):
    """Register the decorated `fn(s: Scenario)` as tier2/<id>.json. `covers` lists the
    pytest ids (tests/unit/test_x.py::test_y) this scenario twins; the coverage manifest
    must map each of them to "tier2/<id>"."""
    if not isinstance(id, str) or not _ID.match(id):
        raise ValueError(f"bad scenario id {id!r} (lowercase words joined by /)")
    for test in covers:
        if not isinstance(test, str) or not _TEST_ID.match(test):
            raise ValueError(f"bad pytest id {test!r} in covers of {id}")
    rel = f"tier2/{id}.json"

    def deco(fn: Callable[[Scenario], None]):
        if rel in SOURCES:
            raise ValueError(f"duplicate scenario id {id!r}")

        def build() -> dict:
            s = Scenario(id, covers)
            fn(s)
            if s.problems:  # also when the scenario caught the ScenarioError itself
                raise ScenarioError("; ".join(s.problems))
            vector = s.to_vector()
            replay(json.loads(canonical.dumps(vector)))
            return vector

        SOURCES[rel] = build
        return fn
    return deco
