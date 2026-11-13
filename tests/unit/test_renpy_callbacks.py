"""The Ren'Py glue's pay() callback map (plugin/game/scfg/scfg_api.rpy, `_state.callbacks`).

The `init -10 python in scfg:` block is run with small stand-ins for `renpy` and `store`
(no Ren'Py SDK here), then its main-thread pump `_periodic` is driven with a fake Worker and
fake checkout controllers. A session's callback stays registered while results can still
reach it (a "continue playing" session that expired can still be paid) and is dropped once
the session is over: its final result (paid / underpaid) was delivered, or the session left
the Engine (closed at its scan horizon, or discarded).
"""
from __future__ import annotations

import sys
import textwrap
import types
from decimal import Decimal
from pathlib import Path

import pytest

from scfg_core.config import add_item, build_config
from scfg_core.engine import Snapshot
from scfg_core.models import PaymentResult
from scfg_core.watcher import StepOutcome

API = Path(__file__).resolve().parents[2] / "plugin" / "game" / "scfg" / "scfg_api.rpy"
DEV = "0x52908400098527886E0F7030069857D2E4169EE7"


def load_scfg(monkeypatch):
    text = API.read_text(encoding="utf-8")
    head = "init -10 python in scfg:\n"
    start = text.index(head) + len(head)
    end = text.index("\ninit -10 python:\n", start)
    store = types.ModuleType("store")
    store.persistent = types.SimpleNamespace(scfg_receipts=None, scfg_pending=None)
    store.Action = object
    notes = []
    renpy = types.SimpleNamespace(
        emscripten=False, mobile=False, variant=lambda name: False,
        config=types.SimpleNamespace(periodic_callbacks=[], quit_callbacks=[],
                                     developer=False, save_directory="test"),
        notify=notes.append, restart_interaction=lambda: None, save_persistent=lambda: None,
        loadable=lambda name: True, hide_screen=lambda *a, **k: None)
    monkeypatch.setitem(sys.modules, "store", store)
    ns = {"renpy": renpy, "python_object": object, "python_dict": dict,
          "python_list": list, "_": lambda s: s, "__name__": "store.scfg"}
    exec(compile(textwrap.dedent(text[start:end]), str(API), "exec"), ns)
    config = build_config(DEV)
    add_item(config, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    ns["_state"].config = config
    return types.SimpleNamespace(ns=ns, state=ns["_state"], notes=notes)


def snapshot(*sids):
    pending = {sid: {"id": sid, "state": "awaiting", "ui_status": "pending", "hint_tx": None}
               for sid in sids}
    return Snapshot(pending=pending, receipts={}, owned=frozenset(), tip_total=Decimal(0))


class FakeWorker:
    def __init__(self, snap):
        self.snapshot = snap
        self.outcomes = []

    def drain_outcomes(self):
        out, self.outcomes = self.outcomes, []
        return out


class FakeController:
    """What the pump uses of scfg_core.checkout.CheckoutController."""

    def __init__(self, sid, callback):
        self.session = {"id": sid, "created_at": 1e12}
        self.callback = callback
        self.callback_sid = None          # as _ScreenCheckout
        self.state = "waiting"
        self.closed = False

    @property
    def session_id(self):
        return self.session["id"] if self.session else None

    def on_outcome(self, outcome):
        if outcome.session_id == self.session_id and outcome.status in (
                "paid", "underpaid", "expired"):
            self.state = outcome.status

    def poll(self):
        pass

    def close(self):
        self.closed = True


def result(status, sid):
    return PaymentResult(status=status, kind="unlock", item="chapter2", token="USDC",
                         chain="base", session_id=sid)


@pytest.fixture
def scfg(monkeypatch):
    s = load_scfg(monkeypatch)
    s.worker = FakeWorker(snapshot())
    s.state.worker = s.worker
    s.got = []

    def pump(snap=None, *outcomes):
        if snap is not None:
            s.worker.snapshot = snap
        s.worker.outcomes = list(outcomes)
        s.ns["_periodic"]()

    def keep_playing(sid):
        ctl = FakeController(sid, s.got.append)
        s.state.background["c-" + sid] = ctl
        return ctl

    s.pump, s.keep_playing = pump, keep_playing
    return s


def test_a_delivered_paid_result_drops_the_callback(scfg):
    scfg.keep_playing("ps_1")
    scfg.pump(snapshot("ps_1"))
    assert scfg.state.callbacks == {"ps_1": scfg.got.append}
    # the Engine removed the paid session; its outcome is delivered in the same pump
    scfg.pump(snapshot(), StepOutcome("ps_1", "paid", result=result("paid", "ps_1")))
    assert [r.status for r in scfg.got] == ["paid"]
    assert scfg.state.callbacks == {}
    scfg.pump(snapshot())
    assert scfg.state.callbacks == {} and scfg.got[-1].status == "paid"


def test_an_underpaid_result_drops_the_callback_too(scfg):
    ctl = scfg.keep_playing("ps_1")
    scfg.pump(snapshot("ps_1"))
    scfg.state.checkouts["c-open"] = ctl          # also on screen: polled twice
    scfg.pump(snapshot(), StepOutcome("ps_1", "underpaid", result=result("underpaid", "ps_1")))
    assert [r.status for r in scfg.got] == ["underpaid"] and scfg.state.callbacks == {}


def test_an_expired_session_keeps_its_callback_until_it_is_paid(scfg):
    """Keep playing: the window closed (expired is delivered) but the scan goes on, and a
    payment found later still reaches the callback."""
    scfg.keep_playing("ps_1")
    scfg.pump(snapshot("ps_1"))
    scfg.pump(snapshot("ps_1"), StepOutcome("ps_1", "expired", result=result("expired", "ps_1")))
    assert scfg.state.background == {}                # its controller closed
    assert "ps_1" in scfg.state.callbacks
    scfg.pump(snapshot("ps_1"))
    scfg.pump(snapshot(), StepOutcome("ps_1", "paid", result=result("paid", "ps_1")))
    assert [r.status for r in scfg.got] == ["expired", "paid"]
    assert scfg.state.callbacks == {}


@pytest.mark.parametrize("outcomes", [
    [StepOutcome("ps_1", "closed")],                  # closed at its scan horizon
    [],                                               # discarded: it just disappears
])
def test_a_session_that_left_the_engine_drops_its_callback(scfg, outcomes):
    scfg.keep_playing("ps_1")
    scfg.pump(snapshot("ps_1"))
    scfg.pump(snapshot("ps_1"), StepOutcome("ps_1", "expired", result=result("expired", "ps_1")))
    scfg.pump(snapshot(), *outcomes)
    assert scfg.state.callbacks == {}
    assert [r.status for r in scfg.got] == ["expired"]


def test_an_open_sheet_past_the_scan_horizon_does_not_register_its_callback_again(scfg):
    """A payment sheet left open on an expired session: once the session leaves the Engine at
    its scan horizon the callback is dropped, and the sheet's controller, still polled by every
    pump, does not register it again (no snapshot would ever list the session to prune it)."""
    ctl = FakeController("ps_1", scfg.got.append)
    scfg.state.checkouts["c-open"] = ctl              # the sheet stays open
    scfg.pump(snapshot("ps_1"))
    scfg.pump(snapshot("ps_1"), StepOutcome("ps_1", "expired", result=result("expired", "ps_1")))
    assert ctl.state == "expired" and "ps_1" in scfg.state.callbacks
    scfg.pump(snapshot(), StepOutcome("ps_1", "closed"))   # closed at its scan horizon
    assert scfg.state.callbacks == {}
    scfg.pump(snapshot())
    scfg.pump(snapshot())
    assert scfg.state.callbacks == {}, "not registered again"
    assert scfg.state.checkouts == {"c-open": ctl}


def test_a_new_session_not_in_the_snapshot_yet_keeps_its_callback(scfg):
    """The session id can reach the controller before the Worker's next snapshot lists it."""
    ctl = scfg.keep_playing("ps_2")
    scfg.pump(snapshot())
    del scfg.state.background["c-ps_2"]               # e.g. the player cancelled at once
    scfg.pump(snapshot())
    assert "ps_2" in scfg.state.callbacks
    scfg.pump(snapshot("ps_2"))
    scfg.pump(snapshot(), StepOutcome("ps_2", "paid", result=result("paid", "ps_2")))
    assert [r.status for r in scfg.got] == ["paid"] and scfg.state.callbacks == {}
    assert ctl.callback == scfg.got.append


def test_a_session_paid_before_any_snapshot_listed_it_releases_its_callback(scfg):
    """The state guard of _register_callback: the paid controller is still polled in the pump
    that delivered its result, and no snapshot ever listed the session (so _prune_callbacks
    cannot drop it): the callback must not be registered again."""
    scfg.keep_playing("ps_9")
    scfg.state.checkouts["c-open"] = scfg.state.background["c-ps_9"]   # also on screen
    scfg.pump(snapshot())
    assert "ps_9" in scfg.state.callbacks
    scfg.pump(snapshot(), StepOutcome("ps_9", "paid", result=result("paid", "ps_9")))
    assert [r.status for r in scfg.got] == ["paid"]
    assert scfg.state.callbacks == {}
    scfg.pump(snapshot())
    assert scfg.state.callbacks == {}


class RacingWorker(FakeWorker):
    """The desktop Worker thread finishing a pass while the pump runs: right after a drain that
    found nothing, the paid session leaves the snapshot and its outcome is queued for the next
    drain. A pump that read the snapshot after draining would see the session gone with no
    result delivered and drop the callback."""

    def __init__(self, snap, then_snap, then_outcome):
        super().__init__(snap)
        self._then = (then_snap, then_outcome)

    def drain_outcomes(self):
        out = super().drain_outcomes()
        if self._then is not None and not out:
            self.snapshot, outcome = self._then
            self.outcomes = [outcome]
            self._then = None
        return out


def test_the_snapshot_is_read_before_the_outcomes_are_drained(scfg):
    scfg.keep_playing("ps_1")
    scfg.pump(snapshot("ps_1"))
    scfg.pump(snapshot("ps_1"))                       # listed: the callback can be pruned later
    racing = RacingWorker(snapshot("ps_1"), snapshot(),
                          StepOutcome("ps_1", "paid", result=result("paid", "ps_1")))
    scfg.state.worker = racing
    scfg.ns["_periodic"]()                            # drains nothing, then the pass lands
    scfg.ns["_periodic"]()                            # drains the paid outcome
    assert [r.status for r in scfg.got] == ["paid"], "the paid result reached the callback"
    assert scfg.state.callbacks == {}
