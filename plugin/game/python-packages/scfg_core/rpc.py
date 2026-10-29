"""Minimal Ethereum JSON-RPC client with endpoint failover.

Public RPC endpoints are free but unreliable: they rate-limit, go down, and cap
eth_getLogs block ranges. The client therefore:
  * tries endpoints in order on every call, skipping ones in cooldown,
  * puts a failing endpoint in cooldown (30 s, doubling to 10 min),
  * remembers, per endpoint, the largest eth_getLogs range it accepts (public
    providers allow anything from 50 to 2000+ blocks), shrinking it when a
    provider says a range is too large.
Every call is cooperative (see coio): RpcClient yields Requests and never touches the
network itself.
"""

from __future__ import annotations

import itertools
import json
import re
import time
import urllib.error
import urllib.request
from collections.abc import Sequence
from typing import TYPE_CHECKING, Callable

from .coio import Request, TransportError  # noqa: F401  (re-exported)

if TYPE_CHECKING:
    import ssl

COOLDOWN_START = 30.0
COOLDOWN_MAX = 600.0
MIN_LOG_RANGE = 10
_RATE_HINTS = ("rate limit", "too many requests", "rate-limit")
# Phrases providers use for "your eth_getLogs block range is too wide". Some say "archive"
# (publicnode answers wide ranges with "Archive requests require a personal token").
_RANGE_HINTS = ("range", "too many", "limit", "exceed", "max results", "response size", "too large",
                "archive")
_NUMBER = re.compile(r"\d+")


class RpcError(Exception):
    """A JSON-RPC error object returned by the node."""

    def __init__(self, code: int, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class RpcUnavailable(Exception):
    """Every endpoint failed or is cooling down."""


def is_range_error(err: RpcError) -> bool:
    m = err.message.lower()
    return any(h in m for h in _RANGE_HINTS)


def suggested_range(err: RpcError, tried: int) -> int:
    """Next eth_getLogs range to try after `err` rejected a `tried`-block request.

    Uses the limit when the provider states one ("limited to 0 - 50 blocks range"),
    otherwise halves.
    """
    stated = [int(n) for n in _NUMBER.findall(err.message)]
    candidates = [n for n in stated if MIN_LOG_RANGE <= n < tried]
    return max(candidates) if candidates else max(MIN_LOG_RANGE, tried // 2)


def _parse_error(e) -> RpcError:
    """Providers are inconsistent: the error may be an object, a bare string, or carry a
    non-numeric code. Never let a malformed error crash the game."""
    if isinstance(e, dict):
        code, message = e.get("code", 0), e.get("message", "")
    else:
        code, message = 0, e
    try:
        code = int(code)
    except (TypeError, ValueError):
        code = 0
    return RpcError(code, str(message))


def _is_rate_error(err: RpcError) -> bool:
    m = err.message.lower()
    return any(h in m for h in _RATE_HINTS)


def _ssl_context() -> ssl.SSLContext:
    # Imported here, not at the top: Ren'Py's web build has no ssl module, and the
    # web transport (fetch through bridge.js) never calls this.
    import ssl

    # Ren'Py ships certifi; using its CA bundle makes HTTPS work on platforms
    # (notably Android) where Python cannot find the system certificates.
    try:
        import certifi

        return ssl.create_default_context(cafile=certifi.where())
    except Exception:
        return ssl.create_default_context()


class UrllibTransport:
    def __init__(self, user_agent: str = "scfg/0.1"):
        self._ctx = _ssl_context()
        self._ua = user_agent

    def post_json(self, url: str, payload: dict, timeout: float):
        body = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(url, data=body, method="POST", headers={
            "Content-Type": "application/json", "User-Agent": self._ua})
        try:
            with urllib.request.urlopen(req, timeout=timeout, context=self._ctx) as resp:
                raw = resp.read()
        except urllib.error.HTTPError as e:
            # Some providers send JSON-RPC errors (e.g. "range too large") with a 4xx
            # status. Pass those through so the client can react to the message.
            try:
                body = json.loads(e.read().decode("utf-8"))
            except Exception:
                body = None
            if isinstance(body, dict) and body.get("error"):
                return body
            raise TransportError(f"HTTP {e.code}", status=e.code) from None
        except (urllib.error.URLError, OSError) as e:
            raise TransportError(str(e)) from None
        try:
            return json.loads(raw.decode("utf-8"))
        except ValueError:
            raise TransportError("invalid JSON response") from None


class _EndpointState:
    __slots__ = ("delay", "until")

    def __init__(self):
        self.delay = 0.0
        self.until = 0.0


class RpcClient:
    def __init__(self, urls: Sequence[str], *, log_range: int = 1000,
                 clock: Callable[[], float] = time.monotonic, timeout: float = 10.0):
        if not urls:
            raise ValueError("at least one RPC url required")
        self.urls = tuple(urls)
        self._ranges: dict[str, int] = {u: log_range for u in self.urls}
        self.clock = clock
        self.timeout = timeout
        self._ids = itertools.count(1)
        self._state: dict[str, _EndpointState] = {u: _EndpointState() for u in self.urls}

    def _fail(self, url: str, now: float) -> None:
        st = self._state[url]
        st.delay = COOLDOWN_START if st.delay == 0 else min(COOLDOWN_MAX, st.delay * 2)
        st.until = now + st.delay

    def _ok(self, url: str) -> None:
        st = self._state[url]
        st.delay, st.until = 0.0, 0.0

    def _preferred(self) -> str:
        """The endpoint the next call will try first."""
        now = self.clock()
        for url in self.urls:
            if self._state[url].until <= now:
                return url
        return self.urls[0]

    @property
    def log_range(self) -> int:
        """eth_getLogs block range accepted by the currently preferred endpoint."""
        return self._ranges[self._preferred()]

    def live_urls(self) -> list[str]:
        """Endpoints not cooling down right now, in preference order."""
        now = self.clock()
        return [u for u in self.urls if self._state[u].until <= now]

    def call(self, method: str, params: list):
        return (yield from self._call(method, params))[0]

    def _attempt(self, url: str, method: str, params: list, now: float):
        """("ok", result) | ("error", RpcError) | None when the endpoint failed (cooled down)."""
        payload = {"jsonrpc": "2.0", "id": next(self._ids), "method": method, "params": params}
        try:
            resp = yield Request(url, payload, self.timeout)
        except TransportError:
            self._fail(url, now)
            return None
        if not isinstance(resp, dict) or ("result" not in resp and "error" not in resp):
            self._fail(url, now)
            return None
        if resp.get("error") is not None:
            err = _parse_error(resp["error"])
            if _is_rate_error(err):
                self._fail(url, now)
                return None
            self._ok(url)
            err.url = url
            return "error", err
        self._ok(url)
        return "ok", resp["result"]

    def _call(self, method: str, params: list):
        """Like call(), but also returns which endpoint answered."""
        now = self.clock()
        for url in self.urls:
            if self._state[url].until > now:
                continue
            res = yield from self._attempt(url, method, params, now)
            if res is None:
                continue
            kind, value = res
            if kind == "error":
                raise value
            return value, url
        raise RpcUnavailable(f"all endpoints failed for {method}")

    def call_at(self, url: str, method: str, params: list):
        """Ask one endpoint only. RpcUnavailable if it fails, RpcError for a node error."""
        res = yield from self._attempt(url, method, params, self.clock())
        if res is None:
            raise RpcUnavailable(f"{url} failed for {method}")
        kind, value = res
        if kind == "error":
            raise value
        return value

    def try_at(self, url: str, method: str, params: list):
        """Ask one endpoint: ("ok", result) | ("error", RpcError) | None if it failed."""
        return (yield from self._attempt(url, method, params, self.clock()))

    def block_number(self):
        return int((yield from self.call("eth_blockNumber", [])), 16)

    def get_transaction_receipt(self, tx_hash: str):
        return (yield from self.call("eth_getTransactionReceipt", [tx_hash]))

    def get_block_hash(self, number: int):
        block = yield from self.call("eth_getBlockByNumber", [hex(number), False])
        return block["hash"].lower() if block else None

    def get_logs(self, address: str, topics: list, from_block: int, to_block: int):
        out: list[dict] = []
        start = from_block
        while start <= to_block:
            end = min(to_block, start + self.log_range - 1)
            try:
                logs, _ = yield from self._call("eth_getLogs", [{
                    "address": address, "topics": topics,
                    "fromBlock": hex(start), "toBlock": hex(end)}])
            except RpcError as e:
                url = getattr(e, "url", self._preferred())
                tried = min(self._ranges[url], end - start + 1)
                if is_range_error(e) and tried > MIN_LOG_RANGE:
                    self._ranges[url] = suggested_range(e, tried)
                    continue
                raise
            out.extend(logs or [])
            start = end + 1
        return out
