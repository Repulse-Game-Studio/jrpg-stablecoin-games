import json
from decimal import Decimal

import pytest

from cryptopay_core.chains import MAINNET
from cryptopay_core.coio import Request, TransportError
from cryptopay_core.config import add_item, build_config
from cryptopay_core.engine import Engine, Snapshot
from cryptopay_core.pump_worker import PumpWorker
from cryptopay_core.watcher import StepOutcome
from cryptopay_core.worker import Worker

from fakes import FakeClock, FakeTransport, transfer_log

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"
PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"
TX = "0x" + "ab" * 32


class FakeBridge:
    """pump_worker's bridge: requests finish only when the test answers them."""

    def __init__(self):
        self.sent = {}       # id -> payload
        self.answers = {}    # id -> envelope
        self.next = 1

    def start(self, url, body, timeout):
        rid = self.next
        self.next += 1
        self.sent[rid] = json.loads(body)
        return rid

    def poll(self, rid):
        return self.answers.pop(rid, None)

    def answer(self, rid, result=None, *, error=None):
        self.answers[rid] = ({"error": error} if error else
                             {"status": 200, "body": json.dumps({"result": result})})

    def last(self):
        return max(self.sent), self.sent[max(self.sent)]["method"]


class CoopEngine:
    """Each job makes one request named after it and records the answer."""

    def __init__(self):
        self.log = []

    def snapshot(self):
        return Snapshot({}, {}, frozenset(self.log), Decimal(0))

    def _one(self, name):
        r = yield Request("http://node/", {"method": name}, 5.0)
        self.log.append(f"{name}:{r['result']}")
        return r["result"]

    def reverify_co(self):
        yield from self._one("reverify")
        return []

    def tick_co(self):
        yield from self._one("tick")
        return [StepOutcome("ps_1", "paid"), StepOutcome("ps_2", "waiting")]

    def command(self, name):
        return (yield from self._one(name))


def make(clock=None):
    engine, bridge = CoopEngine(), FakeBridge()
    clock = clock or FakeClock(100.0)
    return engine, bridge, PumpWorker(engine, bridge, interval=lambda: 10.0, clock=clock), clock


def test_pump_starts_with_reverify_and_never_blocks():
    engine, bridge, w, _ = make()
    w.pump()
    assert bridge.last() == (1, "reverify")
    w.pump()
    w.pump()
    assert len(bridge.sent) == 1          # still waiting: nothing new, nothing blocked


def test_answers_resume_the_job_and_publish_outcomes_and_snapshot():
    engine, bridge, w, _ = make()
    w.pump()
    bridge.answer(1, "ok")
    w.pump()
    assert bridge.last() == (2, "tick")
    assert w.snapshot.owned == frozenset()             # replaced only when a job ends
    bridge.answer(2, 7)
    w.pump()
    assert w.snapshot.owned == frozenset({"reverify:ok", "tick:7"})
    assert [o.status for o in w.drain_outcomes()] == ["paid"]   # plain waiting is dropped
    assert w.drain_outcomes() == []


def test_a_command_submitted_mid_tick_waits_for_the_tick():
    engine, bridge, w, _ = make()
    w.pump()
    bridge.answer(1, "ok")
    w.pump()                                            # tick in flight (id 2)
    fut = w.submit(lambda e: e.command("choose"))
    w.pump()
    assert bridge.last() == (2, "tick") and not fut.done()
    bridge.answer(2, 1)
    w.pump()
    assert bridge.last() == (3, "choose")
    bridge.answer(3, "chosen")
    w.pump()
    assert fut.result(timeout=0) == "chosen"
    assert engine.log == ["reverify:ok", "tick:1", "choose:chosen"]


def test_commands_run_before_a_due_tick_and_plain_results_resolve_at_once():
    engine, bridge, w, _ = make()
    plain = w.submit(lambda e: "now")
    w.submit(lambda e: e.command("first"))
    w.pump()
    assert plain.result(timeout=0) == "now"
    assert bridge.last() == (1, "first")


def test_ticks_follow_the_interval_and_wake():
    engine, bridge, w, clock = make()
    w.pump()
    bridge.answer(1, "ok")
    w.pump()
    bridge.answer(2, 1)
    w.pump()                                            # tick done at t=100
    w.pump()
    assert len(bridge.sent) == 2                        # next tick not due before t=110
    clock.advance(10)
    w.pump()
    assert bridge.last() == (3, "tick")
    bridge.answer(3, 2)
    w.pump()
    w.wake()
    w.pump()
    assert bridge.last() == (4, "tick")


def test_a_submit_landing_mid_tick_does_not_delay_the_next_tick_by_an_interval():
    engine, bridge, w, _ = make()
    w.pump()
    bridge.answer(1, "ok")
    w.pump()                                            # tick in flight (id 2)
    fut = w.submit(lambda e: e.command("hint"))
    bridge.answer(2, 1)
    w.pump()                                            # tick ends; the command starts
    assert bridge.last() == (3, "hint")
    bridge.answer(3, "done")
    w.pump()                                            # command ends; no clock advance
    assert fut.result(timeout=0) == "done"
    assert bridge.last() == (4, "tick")                 # the tick it asked for starts at once


def test_a_tick_without_a_mid_tick_wake_still_waits_the_interval():
    engine, bridge, w, _ = make()
    w.wake()                                            # before the tick starts: consumed by it
    w.pump()
    bridge.answer(1, "ok")
    w.pump()
    bridge.answer(2, 1)
    w.pump()
    w.pump()
    assert len(bridge.sent) == 2


def test_a_transport_error_is_thrown_into_the_job():
    engine, bridge, w, _ = make()
    fut = w.submit(lambda e: e.command("x"))
    w.pump()
    bridge.answer(1, error="Failed to fetch")
    w.pump()
    with pytest.raises(TransportError, match="Failed to fetch"):
        fut.result(timeout=0)


def test_a_failing_tick_reports_and_schedules_the_next():
    errors = []
    engine, bridge, _, clock = make()
    w = PumpWorker(engine, bridge, interval=lambda: 10.0, clock=clock, on_error=errors.append)
    w.pump()
    bridge.answer(1, error="down")
    w.pump()
    assert isinstance(errors[0], TransportError)
    clock.advance(10)
    w.pump()
    assert bridge.last() == (2, "tick")                 # reverify is not retried


def test_a_bridge_that_cannot_start_a_request_is_a_transport_error():
    engine, bridge, w, _ = make()

    def broken(url, body, timeout):
        raise RuntimeError("no fetch")
    bridge.start = broken
    fut = w.submit(lambda e: e.command("x"))
    w.pump()
    with pytest.raises(TransportError, match="no fetch"):
        fut.result(timeout=0)


def test_stop_closes_the_job_and_cancels_queued_commands():
    engine, bridge, w, _ = make()
    w.pump()                                            # reverify in flight
    queued = w.submit(lambda e: e.command("later"))
    w.stop()
    assert queued.cancelled() and not w.running
    bridge.answer(1, "ok")
    w.pump()
    assert len(bridge.sent) == 1 and engine.log == []


def test_a_malformed_envelope_is_thrown_into_the_job_not_out_of_pump():
    engine, bridge, w, _ = make()
    first = w.submit(lambda e: e.command("x"))
    second = w.submit(lambda e: e.command("y"))
    w.pump()
    bridge.answers[1] = {"status": "abc"}               # int("abc") raises ValueError
    w.pump()                                            # must not raise
    with pytest.raises(ValueError):
        first.result(timeout=0)
    assert bridge.last() == (2, "y")                    # the queue moved on
    bridge.answer(2, "fine")
    w.pump()
    assert second.result(timeout=0) == "fine"


def test_a_bridge_whose_poll_raises_is_thrown_into_the_job():
    engine, bridge, w, _ = make()
    fut = w.submit(lambda e: e.command("x"))
    w.pump()

    def broken(rid):
        raise RuntimeError("poll broke")
    bridge.poll = broken
    w.pump()
    with pytest.raises(RuntimeError, match="poll broke"):
        fut.result(timeout=0)


# --- the real Engine: same results as the desktop Worker ------------------------------

def evm_node(state):
    """A JSON-RPC handler answering from state = {"head": int, "logs": [...]}."""
    def handle(method, params):
        if method == "eth_blockNumber":
            return {"result": hex(state["head"])}
        if method == "eth_getLogs":
            q = params[0]
            lo, hi = int(q["fromBlock"], 16), int(q["toBlock"], 16)
            return {"result": [lg for lg in state["logs"] if lo <= int(lg["blockNumber"], 16) <= hi
                               and lg["address"] == q["address"].lower()]}
        if method == "eth_getBlockByNumber":
            return {"result": {"hash": "0x" + format(int(params[0], 16), "064x")}}
        if method == "eth_getTransactionReceipt":
            return {"result": None}
        raise AssertionError(method)
    return handle


class InstantBridge:
    """A bridge whose requests finish at once, through a FakeTransport."""

    def __init__(self, transport):
        self.transport, self.done, self.next = transport, {}, 1

    def start(self, url, body, timeout):
        rid, self.next = self.next, self.next + 1
        try:
            resp = self.transport.post_json(url, json.loads(body), timeout)
        except TransportError as e:
            self.done[rid] = {"error": str(e)}
        else:
            self.done[rid] = {"status": 200, "body": json.dumps(resp)}
        return rid

    def poll(self, rid):
        return self.done.pop(rid, None)


def scenario(make_worker, settle):
    state = {"head": 1000, "logs": []}
    node = evm_node(state)
    transport = FakeTransport({u: node for u in MAINNET["arbitrum"].rpc})
    cfg = build_config(DEV, chains=["arbitrum"], confirmations={"arbitrum": 2})
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    engine = Engine(cfg, transport=transport)
    worker = make_worker(engine, transport)
    fut = worker.submit(lambda e: e.start_session_co("unlock", "arbitrum", item="chapter2"))
    settle(worker)
    session = fut.result(timeout=0)
    usdc = MAINNET["arbitrum"].tokens["USDC"].address
    state["logs"].append(transfer_log(usdc, PAYER, DEV, session["units"], block=1001,
                                      tx_hash=TX))
    state["head"] = 1002
    statuses = []
    for _ in range(3):
        worker.wake()
        settle(worker)
        statuses += [o.status for o in worker.drain_outcomes()]
    return statuses, worker.snapshot.owned


def test_pump_worker_matches_the_desktop_worker():
    desktop = scenario(lambda e, t: Worker(e), lambda w: w.run_once())
    web = scenario(lambda e, t: PumpWorker(e, InstantBridge(t), interval=lambda: 0.0),
                   lambda w: w.pump())
    assert desktop == web == (["paid"], frozenset({"chapter2"}))


def test_unreachable_endpoints_surface_network_unavailable():
    state = {"head": 1000, "logs": []}
    node = evm_node(state)
    transport = FakeTransport({u: node for u in MAINNET["arbitrum"].rpc})
    cfg = build_config(DEV, chains=["arbitrum"])
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    engine = Engine(cfg, transport=transport)
    w = PumpWorker(engine, InstantBridge(transport), interval=lambda: 60.0)
    fut = w.submit(lambda e: e.start_session_co("unlock", "arbitrum", item="chapter2"))
    w.pump()
    fut.result(timeout=0)

    def blocked(method, params):     # what a CORS refusal or no network looks like
        raise TransportError("Failed to fetch")
    for u in MAINNET["arbitrum"].rpc:
        transport.handlers[u] = blocked
    w.wake()
    w.pump()                          # returns: the game never waits on the network
    assert [o.error for o in w.drain_outcomes()] == ["network_unavailable"]
