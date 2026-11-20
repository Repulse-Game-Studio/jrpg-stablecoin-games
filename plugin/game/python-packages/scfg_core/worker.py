"""Background thread that owns the Engine.

The Ren'Py main thread never touches the Engine directly. It:
  * submits commands with ``submit(fn)`` and polls the returned Future,
  * reads the latest immutable Snapshot from ``.snapshot`` (replaced atomically),
  * collects StepOutcomes with ``drain_outcomes()``.
So no locks are needed, and slow RPC calls never freeze the game.
A command may return a cooperative generator (Engine.*_co); the Worker drives it on its
thread.

Commands run one at a time, so a session start the player moved away from (another network,
another amount, Cancel) would hold up the start that replaced it until its RPC requests ran out.
``abandon(fut)`` prevents that: a job that has not started is cancelled, and a running one ends
with ``Abandoned`` (not a TransportError: no endpoint cools down) at once, its request in flight
included (that request's thread is left to finish on its own and its answer is dropped).

Commands come first. A tick is background work (watching pending payments), and every command is
the player's (a start they are waiting on, a hint, a restore): a command submitted while a tick
runs makes the tick yield the same way, its request in flight ending with Abandoned. The tick keeps
the outcomes it already has (Engine.tick_co stops between or inside a session's step, as a network
failure would; the interrupted session is stepped again by the next tick), and the command runs
next.
"""

from __future__ import annotations

import inspect
import queue
import threading
from concurrent.futures import Future
from typing import Callable

_ABANDONED = "_scfg_abandoned"


class Abandoned(Exception):
    """Raised into a job the caller abandoned, at its next RPC request."""


def abandon(fut: Future) -> None:
    """Stop a job the caller no longer wants: cancelled if it has not started, else refused its
    next RPC request (see the module docstring). Its done callbacks still run when it ends."""
    if not fut.cancel():
        setattr(fut, _ABANDONED, True)


def abandoned(fut) -> bool:
    """Whether abandon() was called on a job that was already running (False for None)."""
    return fut is not None and getattr(fut, _ABANDONED, False)


class _Refusing:
    """The engine's transport for one job: once ``refuse()`` is true (the job was abandoned, or a
    tick must yield to a command), a request is refused, and one in flight ends at once."""

    POLL = 0.05  # seconds between two looks at refuse() while a request is in flight

    def __init__(self, transport, refuse: Callable[[], bool]):
        self._transport = transport
        self._refuse = refuse

    def post_json(self, url: str, payload: dict, timeout: float):
        if self._refuse():
            raise Abandoned(f"abandoned before its request to {url}")
        if self._transport is None:
            raise RuntimeError(f"no transport to reach {url}")
        box: dict = {}

        def call() -> None:
            try:
                box["value"] = self._transport.post_json(url, payload, timeout)
            except BaseException as e:  # handed to the job below
                box["error"] = e

        thread = threading.Thread(target=call, name="scfg-request", daemon=True)
        thread.start()
        while True:
            thread.join(self.POLL)
            if not thread.is_alive():
                break
            if self._refuse():  # the blocking call finishes on its own; its answer is dropped
                raise Abandoned(f"abandoned while its request to {url} was in flight")
        if "error" in box:
            raise box["error"]
        return box["value"]


class Worker:
    def __init__(self, engine, *, interval: Callable[[], float] = lambda: 15.0,
                 on_error: Callable[[BaseException], None] | None = None):
        self.engine = engine
        self.interval = interval
        self.on_error = on_error or (lambda e: None)
        self.snapshot = engine.snapshot()
        self._commands: queue.Queue = queue.Queue()
        self._outcomes: queue.Queue = queue.Queue()
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._first = True

    def submit(self, fn) -> Future:
        """Run fn(engine) on the worker thread; returns a Future for the result."""
        fut: Future = Future()
        self._commands.put((fn, fut))
        self.wake()
        return fut

    def wake(self) -> None:
        self._wake.set()

    def drain_outcomes(self) -> list:
        out = []
        while True:
            try:
                out.append(self._outcomes.get_nowait())
            except queue.Empty:
                return out

    def run_once(self) -> None:
        while True:
            try:
                fn, fut = self._commands.get_nowait()
            except queue.Empty:
                break
            if not fut.set_running_or_notify_cancel():
                continue
            try:
                result = fn(self.engine)
                if inspect.isgenerator(result):
                    result = self.engine.run(result, _Refusing(
                        getattr(self.engine, "transport", None), lambda fut=fut: abandoned(fut)))
                fut.set_result(result)
            except BaseException as e:  # delivered to the caller through the Future
                fut.set_exception(e)
        if self._first:
            self._first = False
            self.engine.reverify()
        for o in self._tick():
            if o.status != "waiting" or o.error:
                self._outcomes.put(o)
        self.snapshot = self.engine.snapshot()

    def _tick(self) -> list:
        """Engine.tick, yielding to a command submitted meanwhile (see the module docstring)."""
        tick_co = getattr(self.engine, "tick_co", None)
        if tick_co is None:  # an engine without cooperative jobs (the tests' fakes)
            return self.engine.tick()
        waiting = self._commands
        try:
            return self.engine.run(tick_co(), _Refusing(getattr(self.engine, "transport", None),
                                                        lambda: not waiting.empty()))
        except Abandoned:
            return []  # an engine whose tick does not keep its outcomes when it yields

    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                self.run_once()
            except Exception as e:
                self.on_error(e)
            self._wake.wait(self.interval())
            self._wake.clear()

    def start(self) -> None:
        if self._thread is None:
            self._thread = threading.Thread(target=self._loop, name="scfg-worker",
                                            daemon=True)
            self._thread.start()

    def stop(self, timeout: float = 5.0) -> None:
        self._stop.set()
        self._wake.set()
        if self._thread is not None:
            self._thread.join(timeout)
            self._thread = None

    @property
    def running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()
