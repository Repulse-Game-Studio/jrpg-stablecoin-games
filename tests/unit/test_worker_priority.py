"""Testbed U15: a session start the player waits on comes before the background watching of
pending payments. With pending sessions whose checks hang (a DNS that never answers), the start
submitted while a tick runs makes the tick yield at once, its request in flight included, and
starts in well under the per-request timeout; the yielded tick keeps its outcomes and the next
tick steps the sessions again. A real Engine on a real Worker thread."""
import threading
import time

from scfg_core.config import add_item, build_config
from scfg_core.engine import Engine
from scfg_core.worker import Worker

DEV = "0x52908400098527886E0F7030069857D2E4169EE7"


class Node:
    """Answers eth_blockNumber and eth_getLogs, or hangs every request while `hang` is set."""

    def __init__(self):
        self.hang = False
        self.release = threading.Event()
        self.hung = threading.Event()
        self.methods = []

    def post_json(self, url, payload, timeout):
        self.methods.append(payload["method"])
        if self.hang:
            self.hung.set()
            self.release.wait(timeout)
            from scfg_core.coio import TransportError
            raise TransportError(f"timed out after {timeout:g}s")
        result = "0x3e8" if payload["method"] == "eth_blockNumber" else []
        return {"jsonrpc": "2.0", "id": payload["id"], "result": result}


def test_a_start_does_not_wait_behind_the_watching_of_pending_payments():
    cfg = build_config(DEV, chains=["base", "arbitrum"], testnet=True)
    add_item(cfg, "chapter2", code=37, price="4.99", token="USDC", title="Chapter 2")
    node = Node()
    engine = Engine(cfg, transport=node, clock=lambda: 1700000000)
    for _ in range(3):  # three pending tips, as in the testbed
        engine.start_session("tip", "base", amount="1.00", token="USDC")
    assert len(engine.pending) == 3
    errors = []
    w = Worker(engine, interval=lambda: 60.0, on_error=errors.append)
    node.hang = True
    w.start()
    try:
        assert node.hung.wait(5), "the tick asked the node"
        node.hang = False  # the network is back for the start (the tick's request still hangs)
        started = time.monotonic()
        fut = w.submit(lambda e: e.start_session_co("unlock", "arbitrum", item="chapter2"))
        rec = fut.result(timeout=5)
        took = time.monotonic() - started
        assert took < 2.0, f"the start waited {took:.1f} s behind the tick"
        assert rec["chain"] == "arbitrum"
        assert len(engine.pending) == 4, "the pending payments are untouched"
        assert errors == [], errors
    finally:
        node.release.set()
        w.stop()
