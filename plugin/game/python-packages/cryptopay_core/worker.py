"""Background thread that owns the Engine.

The Ren'Py main thread never touches the Engine directly. It:
  * submits commands with ``submit(fn)`` and polls the returned Future,
  * reads the latest immutable Snapshot from ``.snapshot`` (replaced atomically),
  * collects StepOutcomes with ``drain_outcomes()``.
So no locks are needed, and slow RPC calls never freeze the game.
A command may return a cooperative generator (Engine.*_co); the Worker drives it on its
thread.
"""

from __future__ import annotations

import inspect
import queue
import threading
from concurrent.futures import Future
from typing import Callable


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
                    result = self.engine.run(result)
                fut.set_result(result)
            except BaseException as e:  # delivered to the caller through the Future
                fut.set_exception(e)
        if self._first:
            self._first = False
            self.engine.reverify()
        for o in self.engine.tick():
            if o.status != "waiting" or o.error:
                self._outcomes.put(o)
        self.snapshot = self.engine.snapshot()

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
            self._thread = threading.Thread(target=self._loop, name="cryptopay-worker",
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
