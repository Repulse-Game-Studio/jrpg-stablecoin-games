"""Drives the Engine without threads (Ren'Py's web build).

The web build has no threads, so the Engine's cooperative jobs (generators that yield
coio.Request, see coio) are advanced from the game's main loop: each pump() resumes the
current job until it waits on a request that is still in flight, then returns at once.
Requests go out through a bridge: in the game, web_page.WebPage, which uses the browser's
fetch(). Jobs never interleave: a command or a tick runs to completion before the next
starts, exactly like the desktop Worker's single thread. The interface matches
worker.Worker, so the rest of the plugin can't tell them apart.
"""

from __future__ import annotations

import collections
import inspect
import json
import time
from concurrent.futures import Future
from typing import Callable

from .coio import TransportError, response_from_envelope

MAX_STEPS_PER_PUMP = 50  # bounds the work done in one frame


class PumpWorker:
    def __init__(self, engine, bridge, *, interval: Callable[[], float] = lambda: 15.0,
                 clock: Callable[[], float] = time.monotonic,
                 on_error: Callable[[BaseException], None] | None = None):
        self.engine = engine
        self.bridge = bridge
        self.interval = interval
        self.clock = clock
        self.on_error = on_error or (lambda e: None)
        self.snapshot = engine.snapshot()
        self._commands: collections.deque = collections.deque()
        self._outcomes: collections.deque = collections.deque()
        self._job = None          # the generator being driven
        self._job_future = None   # its Future; None for a tick
        self._in_flight = None    # bridge id of the request the job waits on
        self._next_tick = clock()
        self._woken = False       # wake() since the running tick started
        self._first = True
        self._stopped = False

    # --- the Worker interface ----------------------------------------------------------

    def submit(self, fn) -> Future:
        """Run fn(engine) between jobs; a generator result is driven to completion."""
        fut: Future = Future()
        self._commands.append((fn, fut))
        self.wake()
        return fut

    def wake(self) -> None:
        """Tick as soon as the current job and queued commands are done."""
        self._next_tick = self.clock()
        self._woken = True

    def drain_outcomes(self) -> list:
        out = list(self._outcomes)
        self._outcomes.clear()
        return out

    def start(self) -> None:
        """Nothing to start: pump() does the work."""

    def stop(self, timeout: float = 0.0) -> None:
        self._stopped = True
        if self._job is not None:
            self._job.close()
            if self._job_future is not None:
                self._job_future.cancel()
        self._job = self._job_future = self._in_flight = None
        while self._commands:
            self._commands.popleft()[1].cancel()

    @property
    def running(self) -> bool:
        return not self._stopped

    # --- driving -----------------------------------------------------------------------

    def pump(self) -> None:
        """Advance jobs until one waits on the network. Called every frame; never blocks."""
        if self._stopped:
            return
        for _ in range(MAX_STEPS_PER_PUMP):
            if self._job is None:
                if not self._start_job():
                    return
                self._step()
                continue
            try:
                envelope = self.bridge.poll(self._in_flight)
                if envelope is None:
                    return
                self._in_flight = None
                response = response_from_envelope(envelope)
            except Exception as e:
                # TransportError, or anything a broken bridge or a malformed envelope
                # raises: thrown into the job, like coio.run_sync does, so the core's own
                # handlers see it and the job can't be left stuck.
                self._in_flight = None
                self._step(throw=e)
            else:
                self._step(send=response)

    def _start_job(self) -> bool:
        while self._commands:
            fn, fut = self._commands.popleft()
            if not fut.set_running_or_notify_cancel():
                continue
            try:
                result = fn(self.engine)
            except BaseException as e:  # delivered to the caller through the Future
                fut.set_exception(e)
                self.snapshot = self.engine.snapshot()
                continue
            if inspect.isgenerator(result):
                self._job, self._job_future = result, fut
                return True
            fut.set_result(result)
            self.snapshot = self.engine.snapshot()
        if self.clock() >= self._next_tick:
            self._woken = False
            self._job, self._job_future = self._tick_job(), None
            return True
        return False

    def _tick_job(self):
        if self._first:
            self._first = False
            yield from self.engine.reverify_co()
        for o in (yield from self.engine.tick_co()):
            if o.status != "waiting" or o.error:
                self._outcomes.append(o)

    def _step(self, send=None, throw=None) -> None:
        """Resume the job; it either finishes or yields its next Request."""
        try:
            request = self._job.throw(throw) if throw is not None else self._job.send(send)
        except StopIteration as stop:
            self._finish(result=stop.value)
            return
        except BaseException as e:
            self._finish(error=e)
            return
        try:
            self._in_flight = self.bridge.start(request.url, json.dumps(request.payload),
                                                request.timeout)
        except Exception as e:
            self._step(throw=TransportError(str(e)))

    def _finish(self, result=None, error=None) -> None:
        fut = self._job_future
        self._job = self._job_future = self._in_flight = None
        if fut is None:                                  # a tick
            if error is not None:
                self.on_error(error)
            # A wake() or submit() that landed mid-tick must not wait a full interval.
            self._next_tick = self.clock() + (0.0 if self._woken else self.interval())
            self._woken = False
        elif error is not None:
            fut.set_exception(error)
        else:
            fut.set_result(result)
        self.snapshot = self.engine.snapshot()
