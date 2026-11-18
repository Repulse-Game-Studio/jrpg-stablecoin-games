import threading
from decimal import Decimal

import pytest

from scfg_core.coio import Request, run_sync
from scfg_core.engine import Snapshot
from scfg_core.watcher import StepOutcome
from scfg_core.worker import Abandoned, Worker, abandon


class FakeEngine:
    def __init__(self):
        self.ticks = 0
        self.reverified = 0
        self.statuses = ["waiting"]
        self.ticked = threading.Event()

    def tick(self):
        self.ticks += 1
        self.ticked.set()
        return [StepOutcome("ps_1", s) for s in self.statuses]

    def reverify(self):
        self.reverified += 1
        return []

    def snapshot(self):
        return Snapshot({}, {}, frozenset({f"tick{self.ticks}"}), Decimal(0))


def test_initial_snapshot_taken_at_construction():
    w = Worker(FakeEngine())
    assert w.snapshot.owned == frozenset({"tick0"})


def test_run_once_reverifies_only_first_time_and_publishes_snapshot():
    e = FakeEngine()
    w = Worker(e)
    w.run_once()
    w.run_once()
    assert e.reverified == 1 and e.ticks == 2
    assert w.snapshot.owned == frozenset({"tick2"})


def test_waiting_outcomes_without_error_are_not_published():
    e = FakeEngine()
    w = Worker(e)
    e.statuses = ["waiting", "confirming", "paid"]
    w.run_once()
    assert [o.status for o in w.drain_outcomes()] == ["confirming", "paid"]
    assert w.drain_outcomes() == []


def test_waiting_outcome_with_error_is_published():
    e = FakeEngine()

    def tick():
        return [StepOutcome("ps_1", "waiting", error="network_unavailable")]
    e.tick = tick
    w = Worker(e)
    w.run_once()
    assert w.drain_outcomes()[0].error == "network_unavailable"


def test_submitted_commands_run_before_tick_and_resolve_futures():
    e = FakeEngine()
    w = Worker(e)
    ok = w.submit(lambda eng: eng.ticks)
    bad = w.submit(lambda eng: 1 / 0)
    assert not ok.done()
    w.run_once()
    assert ok.result(timeout=0) == 0          # ran before the tick
    with pytest.raises(ZeroDivisionError):
        bad.result(timeout=0)


def test_a_command_submitted_by_a_command_runs_before_the_same_tick():
    e = FakeEngine()
    w = Worker(e)
    inner = []
    w.submit(lambda eng: inner.append(w.submit(lambda eng2: eng2.ticks)))
    w.run_once()
    assert inner[0].result(timeout=0) == 0


def test_cancelled_future_is_skipped():
    w = Worker(FakeEngine())
    ran = []
    fut = w.submit(lambda eng: ran.append(1))
    fut.cancel()
    w.run_once()
    assert ran == []


def test_thread_loop_wake_and_stop():
    e = FakeEngine()
    w = Worker(e, interval=lambda: 60.0)      # long interval: only wake() can trigger tick 2
    w.start()
    try:
        assert e.ticked.wait(2)
        e.ticked.clear()
        fut = w.submit(lambda eng: "done")    # submit wakes the loop
        assert fut.result(timeout=2) == "done"
        assert e.ticked.wait(2)
        assert w.running
    finally:
        w.stop()
    assert not w.running


def test_loop_survives_exceptions():
    e = FakeEngine()
    errors = []
    calls = {"n": 0}
    done = threading.Event()

    def flaky():
        calls["n"] += 1
        if calls["n"] == 1:
            raise RuntimeError("boom")
        done.set()
        return []
    e.tick = flaky
    w = Worker(e, interval=lambda: 0.01, on_error=errors.append)
    w.start()
    try:
        assert done.wait(2)
    finally:
        w.stop()
    assert isinstance(errors[0], RuntimeError)


def test_generator_commands_are_driven_by_the_engine():
    e = FakeEngine()
    driven = []

    def run(gen, transport=None):
        driven.append(True)
        return run_sync(gen, transport)
    e.run = run

    def job(engine):
        return 41 + 1
        yield  # never reached: makes job(engine) a generator
    w = Worker(e)
    fut = w.submit(job)
    w.run_once()
    assert fut.result(timeout=0) == 42 and driven == [True]


class RecordingTransport:
    def __init__(self, on_post=None):
        self.urls = []
        self.on_post = on_post

    def post_json(self, url, payload, timeout):
        self.urls.append(url)
        if self.on_post:
            self.on_post()
        return {"result": 1}


def test_an_abandoned_running_job_is_refused_its_next_request():
    """The player picks another network while a start waits on its first endpoint: the start is
    refused the next one with Abandoned, so the start that replaced it does not wait for it."""
    e = FakeEngine()
    holder = {}
    e.transport = RecordingTransport(on_post=lambda: abandon(holder["fut"]))
    e.run = lambda gen, transport=None: run_sync(gen, transport)

    def start(engine):
        yield Request("http://first/", {}, 10.0)
        yield Request("http://second/", {}, 10.0)
        return "started"
    w = Worker(e)
    holder["fut"] = w.submit(start)
    w.run_once()
    with pytest.raises(Abandoned):
        holder["fut"].result(timeout=0)
    assert e.transport.urls == ["http://first/"]


def test_abandoning_a_queued_job_cancels_it():
    w = Worker(FakeEngine())
    ran = []
    fut = w.submit(lambda engine: ran.append(1))
    abandon(fut)
    w.run_once()
    assert fut.cancelled() and ran == []


def test_an_abandoned_job_stops_waiting_for_its_request_in_flight():
    """Testbed U15: a superseded start whose request is in flight (a host that never answers)
    ends at once when abandoned, not at the request's 10 s timeout."""
    import time
    release = threading.Event()

    class Hanging:
        def post_json(self, url, payload, timeout):
            release.wait(10)
            return {"result": 1}
    e = FakeEngine()
    e.transport = Hanging()
    e.run = lambda gen, transport=None: run_sync(gen, transport)

    def start(engine):
        yield Request("http://first/", {}, 10.0)
        return "started"
    w = Worker(e)
    fut = w.submit(start)
    threading.Timer(0.2, lambda: abandon(fut)).start()
    t = time.monotonic()
    try:
        w.run_once()
        assert time.monotonic() - t < 2.0
        with pytest.raises(Abandoned, match="in flight"):
            fut.result(timeout=0)
    finally:
        release.set()
