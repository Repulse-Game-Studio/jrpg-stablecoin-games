"""Cooperative network I/O for the payment core.

The core never performs a network request itself. Code that needs one is a generator
that yields a ``Request`` and receives the parsed JSON-RPC response (or has a
``TransportError`` thrown in). Whoever drives the generator performs the request:

* ``run_sync`` with a blocking transport, on the desktop Worker's thread (and in tools
  and tests);
* ``pump_worker.PumpWorker`` with the browser's fetch(), in Ren'Py's web build, which
  has no threads and must never block the game.

A function that may need the network is therefore always called as
``result = yield from fn(...)``. ``yield from`` can't be used in a comprehension or a
lambda: write a loop.
"""

from __future__ import annotations

import functools
import inspect
import json
from dataclasses import dataclass


class TransportError(Exception):
    """Network or HTTP failure talking to one endpoint."""

    def __init__(self, message: str, status: int | None = None):
        super().__init__(message)
        self.status = status


@dataclass(frozen=True)
class Request:
    """One JSON-RPC POST the core wants made."""

    url: str
    payload: dict
    timeout: float


def run_sync(gen, transport):
    """Drive a cooperative generator to completion with a blocking transport
    (``post_json(url, payload, timeout) -> dict``). Returns the generator's result.

    Any Exception the transport raises (not only TransportError) is thrown into the
    generator at its ``yield Request``, where the blocking call used to raise, so the
    core's own handlers (``except Exception`` around one session's step) still see it."""
    try:
        request = next(gen)
        while True:
            if transport is None:
                raise RuntimeError(f"no transport to reach {request.url}")
            try:
                response = transport.post_json(request.url, request.payload, request.timeout)
            except Exception as e:
                request = gen.throw(e)
            else:
                request = gen.send(response)
    except StopIteration as stop:
        return stop.value
    finally:
        gen.close()


class Sync:
    """A cooperative object (RpcClient, SolanaRpc, a backend) whose generator methods are
    called like plain methods, driven by run_sync. For tools and tests."""

    def __init__(self, target, transport):
        object.__setattr__(self, "_target", target)
        object.__setattr__(self, "_transport", transport)

    def __getattr__(self, name):
        value = getattr(self._target, name)
        if not callable(value):
            return value

        @functools.wraps(value)
        def call(*args, **kwargs):
            result = value(*args, **kwargs)
            return run_sync(result, self._transport) if inspect.isgenerator(result) else result
        return call

    def __setattr__(self, name, value):
        setattr(self._target, name, value)


def synced(fn, transport=None):
    """A generator function as a plain function, driven by run_sync."""
    @functools.wraps(fn)
    def call(*args, **kwargs):
        return run_sync(fn(*args, **kwargs), transport)
    return call


_UNREADABLE = object()


def response_from_envelope(envelope: dict):
    """A browser fetch() result (bridge.js: ``{"status", "body"}`` or ``{"error"}``) as the
    JSON-RPC response, by the same rules as rpc.UrllibTransport: a JSON-RPC error sent with
    an HTTP error status passes through; anything else that isn't a 2xx JSON body is a
    TransportError."""
    if "error" in envelope:
        raise TransportError(str(envelope["error"]))
    status = int(envelope.get("status") or 0)
    try:
        body = json.loads(envelope.get("body") or "")
    except ValueError:
        body = _UNREADABLE
    if not 200 <= status < 300:
        if isinstance(body, dict) and body.get("error"):
            return body
        raise TransportError(f"HTTP {status}", status=status)
    if body is _UNREADABLE:
        raise TransportError("invalid JSON response")
    return body
