"""A session start the player moved away from (another network, another tip amount,
Cancel) must not leave a session behind: it would be watched for 24 h and reported
``expired`` to on_result 30 minutes later, for a payment the player never saw."""

import inspect
from concurrent.futures import Future

from scfg_core.checkout import CheckoutController
from scfg_core.config import add_item, build_config
from scfg_core.engine import Engine
from scfg_core.payments import prepare_payment

from fakes import FakeChain, FakeClock

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"


class DeferredSubmitter:
    """Worker.submit stand-in that runs commands only when run() is called."""

    def __init__(self, engine):
        self.engine = engine
        self.queue = []

    def __call__(self, fn):
        fut = Future()
        self.queue.append((fn, fut))
        return fut

    def run(self):
        while self.queue:
            fn, fut = self.queue.pop(0)
            if not fut.set_running_or_notify_cancel():  # cancelled while queued, as Worker skips it
                continue
            try:
                result = fn(self.engine)
                if inspect.isgenerator(result):
                    result = self.engine.run(result)
                fut.set_result(result)
            except Exception as e:
                fut.set_exception(e)


def setup(kind="unlock"):
    cfg = build_config(DEV)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    fakes = {}
    clock = FakeClock(100.0)
    engine = Engine(cfg, rpc_factory=lambda c: fakes.setdefault(c.key, FakeChain(head=1000)),
                    clock=clock)
    if kind == "unlock":
        spec = prepare_payment(cfg, "unlock", item="chapter2")
    else:
        spec = prepare_payment(cfg, "tip", amount="3", token="USDC")
    submit = DeferredSubmitter(engine)
    ctl = CheckoutController(config=cfg, spec=spec, submit=submit)
    return ctl, engine, submit, clock


def test_switching_network_while_starting_discards_the_first_session():
    ctl, engine, submit, _ = setup()
    ctl.choose_network("arbitrum")
    ctl.choose_network("base")
    submit.run()
    ctl.poll()
    assert ctl.state == "waiting" and ctl.chain_key == "base"
    submit.run()  # the discard queued once the abandoned start finished
    assert [r["chain"] for r in engine.pending.values()] == ["base"]


def test_changing_the_tip_while_starting_discards_the_first_session():
    ctl, engine, submit, _ = setup(kind="tip")
    ctl.next_step()
    ctl.choose_network("arbitrum")
    assert ctl.set_tip_amount("5")
    submit.run()
    ctl.poll()
    submit.run()
    assert len(engine.pending) == 1
    assert next(iter(engine.pending.values()))["price"] == "5.00"


def test_cancel_while_starting_leaves_no_session_to_expire():
    ctl, engine, submit, clock = setup()
    ctl.choose_network("arbitrum")
    assert ctl.cancel().status == "cancelled"
    submit.run()
    submit.run()
    assert engine.pending == {}
    clock.advance(31 * 60)
    assert [o for o in engine.tick() if o.status == "expired"] == []


def test_a_failed_abandoned_start_needs_no_discard():
    ctl, engine, submit, _ = setup()
    ctl.choose_network("arbitrum")
    fn, fut = submit.queue.pop(0)
    assert fut.set_running_or_notify_cancel()  # the start is running when the player cancels
    ctl.cancel()
    fut.set_exception(RuntimeError("rpc down"))
    submit.run()
    assert engine.pending == {}


def test_a_superseded_start_that_has_not_run_is_cancelled_and_never_runs():
    ctl, engine, submit, _ = setup()
    ctl.choose_network("arbitrum")
    first = submit.queue[0][1]
    ctl.choose_network("base")
    assert first.cancelled()  # the new pick does not queue behind it
    submit.run()
    ctl.poll()
    assert ctl.state == "waiting" and ctl.chain_key == "base"
    assert submit.queue == []  # nothing ran for arbitrum, so nothing is left to discard
    assert [r["chain"] for r in engine.pending.values()] == ["base"]


def test_a_running_superseded_start_is_marked_abandoned():
    from scfg_core.worker import abandoned
    ctl, _engine, submit, _ = setup()
    ctl.choose_network("arbitrum")
    fn, fut = submit.queue.pop(0)
    assert fut.set_running_or_notify_cancel()
    ctl.choose_network("base")
    assert not fut.cancelled() and abandoned(fut)  # its next RPC request is refused (test_worker.py)
