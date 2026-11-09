"""Tier 2 replay: re-execute a recorded vector against scfg_core using only its data.

This module is also the half of the recorder that is not the simulator: scenario.Scenario
builds its Engine with `build_engine`, runs every action through `run_action` and serializes
with `encode_returns` / `snapshot_json`, so recording and replay differ only in the transport
(simulator vs `ReplayTransport`) and the entropy source (seeded RNG vs `ScriptedEntropy`).
It is the template for the JS and GDScript runners.

Vector shape: see conformance/README.md ("Tier 2").
"""

from __future__ import annotations

import contextlib
import copy
import json
from collections.abc import Iterator
from typing import Any, Callable

from scfg_core import sessions
from scfg_core.backends import backend_for
from scfg_core.backends import solana as solana_backend
from scfg_core.coio import TransportError
from scfg_core.config import add_item, build_chain, build_config
from scfg_core.engine import Engine
from scfg_core.errors import ConfigError
from scfg_core.evm import TransferLog
from scfg_core.restore import RestoreCandidate, RestoreError
from scfg_core.transfers import Transfer

from .canonical import to_jsonable

__all__ = ["ACTIONS", "ReplayTransport", "ScriptedEntropy", "build_config_from", "build_engine",
           "candidate_from_json", "candidate_to_json", "decode_value", "encode_returns",
           "encode_value", "patched_entropy", "replay", "run_action", "snapshot_json"]

ACTIONS = ("start_session", "build_solana_payment", "report_hint", "set_ui_status",
           "discard_session", "tick", "reverify", "restore_lookup", "restore_apply")
_TRANSFER_CLASSES = {"Transfer": Transfer, "TransferLog": TransferLog}


def _plain(obj: Any) -> Any:
    """to_jsonable, then through JSON text: exactly what a reader of the file sees."""
    return json.loads(json.dumps(to_jsonable(obj)))


# --- values that JSON cannot carry exactly -------------------------------------------------------


def encode_value(v: Any) -> Any:
    """A float as {"float": repr} (JSON would turn 3.0 into the int 3); anything else as is."""
    return {"float": repr(v)} if isinstance(v, float) else v


def decode_value(v: Any) -> Any:
    if isinstance(v, dict) and set(v) == {"float"}:
        return float(v["float"])
    return v


def _decoded(d: dict) -> dict:
    return {k: decode_value(v) for k, v in d.items()}


# --- engine -------------------------------------------------------------------------------------


def build_config_from(setup: dict):
    kw = _decoded(setup["config"])
    if setup["custom_chains"]:
        kw["custom_chains"] = [build_chain(**_decoded(c)) for c in setup["custom_chains"]]
    config = build_config(**kw)
    for item in setup["items"]:
        kw = _decoded(item)
        add_item(config, kw.pop("item_id"), **kw)
    return config


def build_engine(setup: dict, *, transport, clock: Callable[[], int],
                 on_error: Callable[[BaseException], None]) -> Engine:
    """The Engine of a vector's setup. One clock drives the Engine and every RpcClient."""
    def rpc_factory(chain):
        rpc = backend_for(chain).make_rpc(chain)
        getattr(rpc, "client", rpc).clock = clock  # SolanaRpc wraps its RpcClient
        return rpc

    return Engine(build_config_from(setup), transport=transport,
                  pending=copy.deepcopy(setup["pending"]),
                  receipts=copy.deepcopy(setup["receipts"]), clock=clock,
                  rpc_factory=rpc_factory, on_error=on_error)


# --- restore candidates ---------------------------------------------------------------------------


def candidate_to_json(c: RestoreCandidate) -> dict:
    out = to_jsonable(c)
    out["transfer"]["class"] = type(c.transfer).__name__
    return out


def candidate_from_json(d: dict) -> RestoreCandidate:
    fields = dict(d["transfer"])
    transfer = _TRANSFER_CLASSES[fields.pop("class")](**fields)
    return RestoreCandidate(chain=d["chain"], token=d["token"], transfer=transfer,
                            kind=d["kind"], item=d["item"], choices=tuple(d["choices"]),
                            already_restored=d["already_restored"])


# --- actions --------------------------------------------------------------------------------------


def run_action(engine: Engine, action: str, args: dict, steps: list[dict],
               on_raise: Callable[[BaseException], None] | None = None):
    """Run one action. Returns (result, raises): raises is None or the vector's `raises`
    value. `steps` are the steps recorded before this one (restore_apply reads one).
    `on_raise` sees an exception recorded as {"type": "other"} (the recorder reports it)."""
    a = _decoded(args)
    try:
        if action == "start_session":
            result = engine.start_session(a.pop("kind"), a.pop("chain"), **a)
        elif action == "build_solana_payment":
            result = engine.build_solana_payment(a["sid"], a["account"])
        elif action == "report_hint":
            result = engine.report_hint(a["sid"], a["tx"])
        elif action == "set_ui_status":
            result = engine.set_ui_status(a["sid"], a["status"])
        elif action == "discard_session":
            result = engine.discard_session(a["sid"])
        elif action == "tick":
            result = engine.tick()
        elif action == "reverify":
            result = engine.reverify()
        elif action == "restore_lookup":
            result = engine.restore_lookup(a["tx"])
        elif action == "restore_apply":
            candidates = [candidate_from_json(c) for c in steps[a["from_step"]]["returns"]]
            choices = a["choices"]
            if choices is not None:
                choices = {int(k): v for k, v in choices.items()}
            result = engine.restore_apply(candidates, choices)
        else:
            raise ValueError(f"unknown action {action!r}")
    except ConfigError as e:
        return None, {"type": "ConfigError", "code": e.code}
    except RestoreError as e:
        return None, {"type": "RestoreError", "code": e.code}
    except Exception as e:
        if on_raise is not None:
            on_raise(e)
        return None, {"type": "other"}
    return result, None


def encode_returns(action: str, result: Any) -> Any:
    if action == "restore_lookup" and result is not None:
        return _plain([candidate_to_json(c) for c in result])
    return _plain(result)


def snapshot_json(engine: Engine) -> dict:
    """pending, receipts, owned (sorted), tip_total (str), unready (sorted [chain, token])."""
    return _plain(engine.snapshot())


# --- entropy --------------------------------------------------------------------------------------


@contextlib.contextmanager
def patched_entropy(source) -> Iterator[None]:
    """Make scfg_core draw its randomness (`secrets.token_hex`, `randbelow`, `token_bytes`)
    from `source` for the duration."""
    modules = (sessions, solana_backend)
    saved = [m.secrets for m in modules]
    for m in modules:
        m.secrets = source
    try:
        yield
    finally:
        for m, original in zip(modules, saved):
            m.secrets = original


class ScriptedEntropy:
    """Hands out one step's recorded draws in order; any difference is a problem."""

    def __init__(self, draws: list[dict]):
        self._draws = list(draws)
        self.problems: list[str] = []

    def _take(self, fn: str, n: int):
        if not self._draws:
            self.problems.append(f"entropy exhausted at {fn}({n})")
            raise AssertionError(self.problems[-1])
        draw = self._draws.pop(0)
        if (draw["fn"], draw["n"]) != (fn, n):
            self.problems.append(f"entropy mismatch: drew {fn}({n}), recorded "
                                 f"{draw['fn']}({draw['n']})")
            raise AssertionError(self.problems[-1])
        return draw["value"]

    def token_hex(self, n: int) -> str:
        return self._take("token_hex", n)

    def randbelow(self, n: int) -> int:
        return self._take("randbelow", n)

    def token_bytes(self, n: int) -> bytes:
        return bytes.fromhex(self._take("token_bytes", n))

    def leftover(self) -> list[str]:
        if not self._draws:
            return []
        return [f"unconsumed entropy: {[(d['fn'], d['n']) for d in self._draws]}"]


# --- transport ------------------------------------------------------------------------------------


def call_key(url: str, method: str, params: Any) -> tuple[str, str, str]:
    """Calls match on (url, method, canonical params); the JSON-RPC id is ignored."""
    return url, method, json.dumps(params, sort_keys=True, separators=(",", ":"))


class ReplayTransport:
    """Answers each request from the current step's recorded calls, as a multiset: any order,
    each recorded call used once, a repeated request needs as many recorded copies."""

    def __init__(self):
        self.load([])

    def load(self, calls: list[dict]) -> None:
        self._calls = list(calls)
        self._used = [False] * len(self._calls)
        self.problems: list[str] = []

    def post_json(self, url: str, payload: dict, timeout: float):
        key = call_key(url, payload["method"], payload["params"])
        for i, call in enumerate(self._calls):
            if not self._used[i] and call_key(call["url"], call["method"], call["params"]) == key:
                self._used[i] = True
                if "transport_error" in call:
                    err = call["transport_error"]
                    raise TransportError(err["message"], status=err["status"])
                return json.loads(call["response_text"])
        self.problems.append(f"unexpected call {url} {payload['method']} {key[2]}")
        raise AssertionError(self.problems[-1])

    def leftover(self) -> list[str]:
        return [f"expected call not made: {c['url']} {c['method']} "
                f"{call_key(c['url'], c['method'], c['params'])[2]}"
                for c, used in zip(self._calls, self._used) if not used]


# --- replay ---------------------------------------------------------------------------------------


def _first_diff(a: Any, b: Any, path: str = "") -> str:
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                return f"{path}.{k}: {'missing' if k not in a else 'unexpected'}"
            if a[k] != b[k]:
                return _first_diff(a[k], b[k], f"{path}.{k}")
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)):
            if x != y:
                return _first_diff(x, y, f"{path}[{i}]")
    return f"{path or '<root>'}: recorded {json.dumps(a)}, got {json.dumps(b)}"


def replay(vector: dict) -> None:
    """Re-run a Tier 2 vector; AssertionError("step {i} ({action}): ...") on any difference."""
    if vector.get("tier") != 2:
        raise ValueError("not a Tier 2 vector")
    setup = vector["setup"]
    now = [setup["clock"]]
    errors = [0]

    def on_error(exc: BaseException) -> None:
        errors[0] += 1

    transport = ReplayTransport()
    engine = build_engine(setup, transport=transport, clock=lambda: now[0], on_error=on_error)
    steps = vector["steps"]
    for i, step in enumerate(steps):
        where = f"step {i} ({step['action']})"
        now[0] = step["at"]
        errors[0] = 0
        transport.load(step["calls"])
        entropy = ScriptedEntropy(step["entropy"])
        with patched_entropy(entropy):
            result, raises = run_action(engine, step["action"], step["args"], steps[:i])
        problems = (transport.problems + transport.leftover() + entropy.problems
                    + entropy.leftover())
        if problems:
            raise AssertionError(f"{where}: " + "; ".join(problems))
        if raises != step["raises"]:
            raise AssertionError(f"{where}: raises: recorded {step['raises']}, got {raises}")
        returns = None if raises else encode_returns(step["action"], result)
        if returns != step["returns"]:
            raise AssertionError(f"{where}: returns: {_first_diff(step['returns'], returns)}")
        if errors[0] != step["internal_errors"]:
            raise AssertionError(f"{where}: internal_errors: recorded {step['internal_errors']}, "
                                 f"got {errors[0]}")
        snapshot = snapshot_json(engine)
        if snapshot != step["snapshot"]:
            raise AssertionError(f"{where}: snapshot: "
                                 f"{_first_diff(step['snapshot'], snapshot)}")
