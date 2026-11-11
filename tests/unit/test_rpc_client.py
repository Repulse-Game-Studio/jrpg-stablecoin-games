import pytest

from scfg_core.coio import Sync
from scfg_core.rpc import (
    COOLDOWN_MAX,
    COOLDOWN_START,
    MIN_LOG_RANGE,
    RpcClient,
    RpcError,
    RpcUnavailable,
    TransportError,
    is_range_error,
)

from fakes import FakeClock, FakeTransport, down, err, ok

A, B = "https://a.rpc", "https://b.rpc"


def client(handlers, **kw):
    clock = kw.pop("clock", FakeClock())
    transport = FakeTransport(handlers)
    return Sync(RpcClient(list(handlers), clock=clock, **kw), transport), transport, clock


def test_requires_urls():
    with pytest.raises(ValueError):
        RpcClient([])


def test_call_sends_jsonrpc_and_returns_result():
    c, t, _ = client({A: lambda m, p: ok("0x10")})
    assert c.call("eth_blockNumber", []) == "0x10"
    assert t.calls == [(A, "eth_blockNumber", [])]


def test_block_number_parses_hex():
    c, _, _ = client({A: lambda m, p: ok("0x1b4")})
    assert c.block_number() == 436


def test_falls_back_to_next_endpoint_on_transport_error():
    c, t, _ = client({A: down, B: lambda m, p: ok("0x1")})
    assert c.block_number() == 1
    assert [u for u, _, _ in t.calls] == [A, B]


def test_failed_endpoint_cools_down_then_is_retried():
    state = {"a_up": False}

    def a(m, p):
        if not state["a_up"]:
            raise TransportError("HTTP 503", status=503)
        return ok("0xa")

    c, t, clock = client({A: a, B: lambda m, p: ok("0xb")})
    assert c.block_number() == 0xB          # A fails -> B
    t.calls.clear()
    assert c.block_number() == 0xB          # A cooling down: not even tried
    assert [u for u, _, _ in t.calls] == [B]
    state["a_up"] = True
    clock.advance(COOLDOWN_START + 0.1)
    assert c.block_number() == 0xA          # A preferred again after cooldown


def test_cooldown_doubles_and_caps():
    c, _, clock = client({A: down})
    delays = []
    for _ in range(8):
        with pytest.raises(RpcUnavailable):
            c.call("eth_blockNumber", [])
        st = c._state[A]
        delays.append(st.delay)
        clock.advance(st.delay + 0.1)
    assert delays[:3] == [COOLDOWN_START, COOLDOWN_START * 2, COOLDOWN_START * 4]
    assert max(delays) == COOLDOWN_MAX


def test_success_resets_cooldown():
    state = {"fail": True}

    def a(m, p):
        if state["fail"]:
            raise TransportError("x")
        return ok("0x1")

    c, _, clock = client({A: a})
    with pytest.raises(RpcUnavailable):
        c.call("eth_blockNumber", [])
    state["fail"] = False
    clock.advance(COOLDOWN_START + 1)
    c.call("eth_blockNumber", [])
    assert c._state[A].delay == 0


def test_all_endpoints_down_raises_unavailable():
    c, _, _ = client({A: down, B: down})
    with pytest.raises(RpcUnavailable):
        c.block_number()


@pytest.mark.parametrize("bad", ["not a dict", {"jsonrpc": "2.0", "id": 1}])
def test_malformed_response_counts_as_endpoint_failure(bad):
    c, _, _ = client({A: lambda m, p: bad, B: lambda m, p: ok("0x2")})
    assert c.block_number() == 2


def test_jsonrpc_error_is_raised_without_rotation():
    c, t, _ = client({A: lambda m, p: err(-32000, "execution reverted"), B: lambda m, p: ok("0x1")})
    with pytest.raises(RpcError) as info:
        c.call("eth_call", [])
    assert info.value.code == -32000
    assert [u for u, _, _ in t.calls] == [A]


def test_rate_limit_jsonrpc_error_rotates():
    c, _, _ = client({A: lambda m, p: err(-32005, "Too Many Requests: rate limit"),
                      B: lambda m, p: ok("0x3")})
    assert c.block_number() == 3


def test_is_range_error():
    assert is_range_error(RpcError(-32600, "eth_getLogs block range is too large"))
    assert is_range_error(RpcError(-32005, "query returned more than 10000 results; limit exceeded"))
    assert not is_range_error(RpcError(-32000, "execution reverted"))


def test_receipt_and_block_hash():
    def h(m, p):
        if m == "eth_getTransactionReceipt":
            return ok(None if p[0] == "0xmissing" else {"status": "0x1"})
        if m == "eth_getBlockByNumber":
            assert p == ["0x64", False]
            return ok({"hash": "0xABC"})
    c, _, _ = client({A: h})
    assert c.get_transaction_receipt("0xmissing") is None
    assert c.get_transaction_receipt("0xok") == {"status": "0x1"}
    assert c.get_block_hash(100) == "0xabc"


def test_block_hash_none_for_unknown_block():
    c, _, _ = client({A: lambda m, p: ok(None)})
    assert c.get_block_hash(10**9) is None


# --- eth_getLogs chunking ---------------------------------------------------

def logs_handler(record, max_span=None):
    def h(m, p):
        f = p[0]
        lo, hi = int(f["fromBlock"], 16), int(f["toBlock"], 16)
        record.append((lo, hi))
        if max_span is not None and hi - lo + 1 > max_span:
            return err(-32600, "block range too large")
        return ok([{"blockNumber": hex(lo)}])
    return h


def test_get_logs_chunks_by_log_range():
    seen = []
    c, _, _ = client({A: logs_handler(seen)}, log_range=100)
    logs = c.get_logs("0xtoken", ["0xtopic"], 1000, 1250)
    assert seen == [(1000, 1099), (1100, 1199), (1200, 1250)]
    assert len(logs) == 3


def test_get_logs_empty_range_makes_no_calls():
    seen = []
    c, _, _ = client({A: logs_handler(seen)})
    assert c.get_logs("0xtoken", [], 10, 9) == []
    assert seen == []


def test_get_logs_halves_range_on_range_error_and_remembers():
    seen = []
    c, _, _ = client({A: logs_handler(seen, max_span=300)}, log_range=1000)
    c.get_logs("0xtoken", [], 0, 499)
    assert c.log_range == 250
    # The rejected request spanned 500 blocks, so the next try uses half of that: 250.
    assert seen == [(0, 499), (0, 249), (250, 499)]


def test_get_logs_gives_up_at_minimum_range():
    seen = []
    c, _, _ = client({A: logs_handler(seen, max_span=1)}, log_range=40)
    with pytest.raises(RpcError):
        c.get_logs("0xtoken", [], 0, 100)
    assert c.log_range == MIN_LOG_RANGE


def test_get_logs_sends_filter_shape():
    calls = []

    def h(m, p):
        calls.append(p)
        return ok([])
    c, _, _ = client({A: h}, log_range=1000)
    c.get_logs("0xToken", ["0xt0", None, "0xt2"], 16, 31)
    assert calls == [[{"address": "0xToken", "topics": ["0xt0", None, "0xt2"],
                       "fromBlock": "0x10", "toBlock": "0x1f"}]]


def test_log_range_is_remembered_per_endpoint():
    """A stingy fallback must not permanently shrink the generous primary endpoint."""
    state = {"a_down": False}
    seen = []

    def a(m, p):
        if state["a_down"]:
            raise TransportError("down")
        return logs_handler(seen)(m, p)

    c, _, clock = client({A: a, B: logs_handler(seen, max_span=50)}, log_range=1000)
    c.get_logs("0xtoken", [], 0, 999)
    assert c.log_range == 1000
    state["a_down"] = True
    c.get_logs("0xtoken", [], 0, 199)          # served by B, which only allows 50
    assert c._ranges[B] < 1000 and c.log_range == c._ranges[B]
    state["a_down"] = False
    clock.advance(COOLDOWN_START + 1)
    assert c.log_range == 1000                 # A is preferred again, with its own range


def test_suggested_range_uses_stated_limit_or_halves():
    from scfg_core.rpc import suggested_range
    assert suggested_range(RpcError(-32602, "eth_getLogs is limited to 0 - 50 blocks range"), 2000) == 50
    assert suggested_range(RpcError(35, "ranges over 10000 blocks are not supported on free plan"), 2000) == 1000
    assert suggested_range(RpcError(-32602, "Archive requests require a personal token"), 500) == 250
    assert suggested_range(RpcError(-32005, "block range is too large"), 15) == MIN_LOG_RANGE


# A digit run beyond 18 significant digits is no number (it is above any range a
# caller can try), so a node message never costs a quadratic int() or, on an interpreter with
# CPython's digit limit, a ValueError out of get_logs. \d is any Unicode decimal digit.
_OVERLONG_RANGE_ROWS = [
    ("limited to 50 blocks; request " + "9" * 4301, 2000, 50),
    ("block range too large: " + "9" * 4301, 5000, 2500),
    ("block range too large: " + "9" * 1_000_000, 5000, 2500),
    ("max " + "0" * 30 + "50 blocks", 2000, 50),
    ("max " + "9" * 19 + " or 50", 2000, 50),
    ("max \u0665\u0660 blocks", 2000, 50),
    ("max " + "\u0660" * 25 + "\u0665\u0660", 2000, 50),
    ("max \uff15\uff10", 2000, 50),
]


@pytest.mark.parametrize("message, tried, expected", _OVERLONG_RANGE_ROWS,
                         ids=["4301-beside-a-limit", "4301-alone", "a-million", "leading-zeros",
                              "19-digits", "arabic-indic", "arabic-indic-zeros", "fullwidth"])
def test_suggested_range_ignores_numbers_beyond_18_digits(interpreter, int_texts, message,
                                                          tried, expected):
    import scfg_core.ints
    import scfg_core.rpc
    from scfg_core.rpc import suggested_range
    converted = int_texts(scfg_core.rpc, scfg_core.ints)
    assert suggested_range(RpcError(-32005, message), tried) == expected
    assert max(converted, default=0) <= 18          # no long digit text reaches int()


def test_suggested_range_reads_18_significant_digits_not_19(interpreter):
    from scfg_core.rpc import suggested_range
    tried = 9 * 10**18
    assert suggested_range(RpcError(0, "max " + "9" * 18), tried) == 10**18 - 1
    assert suggested_range(RpcError(0, "max " + "0" * 5 + "9" * 18), tried) == 10**18 - 1
    assert suggested_range(RpcError(0, "max 1" + "0" * 18), tried) == tried // 2


def test_an_overlong_number_in_a_range_error_does_not_break_get_logs(interpreter):
    message = "block range too large: " + "9" * 4301

    def handler(m, p):
        lo, hi = int(p[0]["fromBlock"], 16), int(p[0]["toBlock"], 16)
        return err(-32005, message) if hi - lo + 1 > 100 else ok([])
    c, t, _ = client({A: handler}, log_range=400)
    assert c.get_logs("0xT", [], 1, 400) == []
    assert c.log_range == 100


def test_archive_message_counts_as_range_error():
    assert is_range_error(RpcError(-32602, "Archive requests require a personal token. Get one at x"))


def test_stated_limit_is_adopted_in_one_step():
    seen = []

    def h(m, p):
        f = p[0]
        lo, hi = int(f["fromBlock"], 16), int(f["toBlock"], 16)
        seen.append((lo, hi))
        if hi - lo + 1 > 50:
            return err(-32602, "eth_getLogs is limited to 0 - 50 blocks range")
        return ok([])
    c, _, _ = client({A: h}, log_range=2000)
    c.get_logs("0xtoken", [], 0, 99)
    assert seen == [(0, 99), (0, 49), (50, 99)]
    assert c.log_range == 50


@pytest.mark.parametrize("error, code, message", [
    ("query timeout", 0, "query timeout"),
    ({"code": "bad", "message": "odd"}, 0, "odd"),
    ({"message": "no code"}, 0, "no code"),
])
def test_malformed_error_objects_become_rpc_errors(error, code, message):
    c, _, _ = client({A: lambda m, p: {"jsonrpc": "2.0", "id": 1, "error": error}})
    with pytest.raises(RpcError) as info:
        c.call("eth_blockNumber", [])
    assert (info.value.code, info.value.message) == (code, message)
