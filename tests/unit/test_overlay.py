from concurrent.futures import Future

from cryptopay_core.overlay import OverlayCheckout

from fakes import FakeClock

TX = "0x" + "ab" * 32
SOL = "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3"


class FakeUi:
    def __init__(self):
        self.calls = []
        self.inbox = []

    def open(self, key, info, logo_url):
        self.calls.append(("open", key, info["title"], logo_url))

    def show(self, key):
        self.calls.append(("show", key))

    def close(self, key):
        self.calls.append(("close", key))

    def take(self, key):
        out, self.inbox = self.inbox, []
        return out

    def set_status(self, key, status):
        self.calls.append(("status", status["state"]))

    def reply(self, key, rid, status, body):
        self.calls.append(("reply", rid, status, body))


def make(build_tx=None, clock=None):
    ui, hints = FakeUi(), []
    status = {"state": "waiting"}
    o = OverlayCheckout(ui=ui, key="o1", session_info={"title": "Chapter 2"},
                        on_tx=lambda tx, sender: hints.append((tx, sender)),
                        get_status=lambda: dict(status), logo_url="data:image/png;base64,AA",
                        build_tx=build_tx, clock=clock or FakeClock(0.0))
    return o, ui, hints, status


def test_start_opens_the_overlay_and_pushes_the_status():
    o, ui, _, _ = make()
    assert o.start() == OverlayCheckout.url and o.running
    assert ui.calls == [("open", "o1", "Chapter 2", "data:image/png;base64,AA"),
                        ("status", "waiting")]


def test_status_is_pushed_only_when_it_changes():
    o, ui, _, status = make()
    o.start()
    o.pump()
    o.pump()
    status["state"] = "confirming"
    o.pump()
    assert [c for c in ui.calls if c[0] == "status"] == [("status", "waiting"),
                                                         ("status", "confirming")]


def test_a_tx_message_becomes_a_hint():
    o, ui, hints, _ = make()
    o.start()
    ui.inbox = [{"type": "tx", "body": {"tx_hash": TX}}]
    o.pump()
    assert hints == [(TX, None)]


def test_garbled_messages_are_ignored():
    o, ui, hints, _ = make(build_tx=lambda account: Future())
    o.start()
    ui.inbox = ["x", None, {"type": "tx"}, {"type": "tx", "body": "x"},
                {"type": "tx", "body": {"tx_hash": "0x12"}}, {"type": "nope"},
                {"type": "build", "id": "7", "body": {"account": SOL}}]
    o.pump()
    assert hints == [] and not [c for c in ui.calls if c[0] == "reply"]


def test_build_replies_when_the_future_is_done():
    fut = Future()
    o, ui, _, _ = make(build_tx=lambda account: fut)
    o.start()
    ui.inbox = [{"type": "build", "id": 1, "body": {"account": SOL}}]
    o.pump()
    assert not [c for c in ui.calls if c[0] == "reply"]      # still building
    fut.set_result({"transaction": "AAA"})
    o.pump()
    assert ("reply", 1, 200, {"transaction": "AAA"}) in ui.calls


def test_build_failures_and_bad_accounts():
    failing = Future()
    failing.set_exception(RuntimeError("rpc down"))
    o, ui, _, _ = make(build_tx=lambda account: failing)
    o.start()
    ui.inbox = [{"type": "build", "id": 1, "body": {"account": SOL}},
                {"type": "build", "id": 2, "body": {"account": "0xabc"}}]
    o.pump()
    assert ("reply", 1, 503, {"error": "unavailable"}) in ui.calls
    assert ("reply", 2, 400, {"error": "bad account"}) in ui.calls


def test_build_without_solana_is_not_found():
    o, ui, _, _ = make(build_tx=None)
    o.start()
    ui.inbox = [{"type": "build", "id": 3, "body": {"account": SOL}}]
    o.pump()
    assert ("reply", 3, 404, {"error": "not_found"}) in ui.calls


def test_a_build_that_takes_too_long_is_cancelled():
    clock, never = FakeClock(0.0), Future()
    o, ui, _, _ = make(build_tx=lambda account: never, clock=clock)
    o.start()
    ui.inbox = [{"type": "build", "id": 1, "body": {"account": SOL}}]
    o.pump()
    clock.advance(16)
    o.pump()
    assert never.cancelled() and ("reply", 1, 503, {"error": "unavailable"}) in ui.calls


def test_reopen_shows_the_same_overlay():
    o, ui, _, _ = make()
    o.start()
    o.reopen()
    assert [c[0] for c in ui.calls].count("open") == 1 and ("show", "o1") in ui.calls


def test_stop_closes_once_and_pump_does_nothing_after():
    o, ui, hints, _ = make()
    o.start()
    o.stop()
    o.stop()
    ui.inbox = [{"type": "tx", "body": {"tx_hash": TX}}]
    o.pump()
    assert [c for c in ui.calls if c[0] == "close"] == [("close", "o1")]
    assert hints == [] and not o.running
