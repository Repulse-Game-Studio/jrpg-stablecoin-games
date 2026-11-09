"""The JSON-RPC client (failover, cooldown, eth_getLogs ranges), twinned through Engine actions.

RpcClient is never called directly in Tier 2: a `tick` of a session on a custom EVM chain makes
eth_blockNumber and eth_getLogs calls through the chain's client, and the recorded calls (which
URL was asked, in what order, with what span) are what prove the behaviour. A custom chain
(`avalanche`, with the URLs and max_log_range a scenario picks) gives full control of both.

Cooldowns run on the scenario clock (`s.advance`) and persist across steps, as in a real Engine.
"""

from __future__ import annotations

from scfg_core.rpc import COOLDOWN_MAX as _MAX
from scfg_core.rpc import COOLDOWN_START as _START
from scfg_core.rpc import MIN_LOG_RANGE

from ..scenario import scenario
from .evm import covers
from .fixtures import AVALANCHE, AVAX_USDC, CHAPTER2, DEV_EVM, PAYER_EVM, tx_hash

A, B, C = "https://rpc-a.example", "https://rpc-b.example", "https://rpc-c.example"
_RANGE_ERROR = "block range too large"
COOLDOWN_START, COOLDOWN_MAX = int(_START), int(_MAX)     # the scenario clock counts whole seconds


def _game(s, urls, *, max_log_range=1000):
    """A game on the custom chain `avalanche` served by one node at `urls`; one session is
    started (cursor 995, node head 1000). Returns (node, session record)."""
    s.config(wallet=DEV_EVM, chains=["avalanche"])
    s.custom_chain(**dict(AVALANCHE, rpc=list(urls), max_log_range=max_log_range))
    s.item(**CHAPTER2)
    node = s.evm("avalanche")
    rec = s.start_session("unlock", "avalanche", item="chapter2")
    return node, rec


def _tick(s):
    """Tick; return the single outcome of the session (the window notice is ignored)."""
    [out] = [o for o in s.tick() if o.status != "expired"]
    return out


def _urls(s, method=None):
    """The URLs the last step asked, in order (optionally only for one method)."""
    return [c["url"] for c in s.steps[-1]["calls"] if method in (None, c["method"])]


def _spans(s):
    """(from, to) of every eth_getLogs the last step made, with its outcome."""
    out = []
    for c in s.steps[-1]["calls"]:
        if c["method"] == "eth_getLogs":
            [flt] = c["params"]
            out.append((int(flt["fromBlock"], 16), int(flt["toBlock"], 16)))
    return out


# --- healthy requests -------------------------------------------------------------------------


@scenario("rpc/healthy_endpoint", covers=covers(
    "rpc_client", "call_sends_jsonrpc_and_returns_result", "block_number_parses_hex"))
def _healthy(s):
    """A request is a JSON-RPC eth_blockNumber with empty params, and the hex answer is
    parsed: a node at block 436 (0x1b4) gives a session starting five blocks before it."""
    s.config(wallet=DEV_EVM, chains=["avalanche"])
    s.custom_chain(**dict(AVALANCHE, rpc=[A]))
    s.item(**CHAPTER2)
    node = s.evm("avalanche")
    node.head = 436
    rec = s.start_session("unlock", "avalanche", item="chapter2")
    [call] = s.steps[-1]["calls"]
    assert (call["url"], call["method"], call["params"]) == (A, "eth_blockNumber", [])
    assert '"0x1b4"' in call["response_text"]
    assert rec["start_block"] == 431


@scenario("rpc/receipt_and_block_hash_requests", covers=covers(
    "rpc_client", "receipt_and_block_hash", "block_hash_none_for_unknown_block"))
def _block_hash(s):
    """While confirming, the block of the payment is asked by number without transactions
    (eth_getBlockByNumber [hex, false]) and its hash is compared with the one first seen. A
    node that then answers null (the block is gone) makes the session reorged."""
    node, rec = _game(s, [A])
    node.mine(1)
    node.add_transfer(AVAX_USDC, PAYER_EVM, DEV_EVM, rec["units"], block=node.head,
                      tx_hash=tx_hash(1))
    assert _tick(s).status == "confirming"
    [call] = [c for c in s.steps[-1]["calls"] if c["method"] == "eth_getBlockByNumber"]
    assert call["params"] == [hex(1001), False]
    node.endpoint(A).raw_bodies["eth_getBlockByNumber"] = {"jsonrpc": "2.0", "id": 1,
                                                          "result": None}
    s.advance(10)
    assert _tick(s).status == "reorged"
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["state"] == "awaiting"


# --- failover and cooldown ---------------------------------------------------------------------


@scenario("rpc/failover_on_transport_faults", covers=covers(
    "rpc_client", "falls_back_to_next_endpoint_on_transport_error"))
def _failover_transport(s):
    """When the first URL is down, answers HTTP 429, or answers something that is not JSON,
    the same request goes to the second URL, which answers. Once the first recovers (after
    its cooldown) it is asked first again."""
    node, _ = _game(s, [A, B])
    first = node.endpoint(A)
    for fault in ("down", "http_status", "invalid_json"):
        setattr(first, fault, 429 if fault == "http_status" else True)
        assert _tick(s).error is None
        assert _urls(s, "eth_blockNumber") == [A, B], fault
        setattr(first, fault, None if fault == "http_status" else False)
        s.advance(COOLDOWN_MAX + 1)
        assert _tick(s).error is None
        assert _urls(s, "eth_blockNumber") == [A], fault        # A is preferred again, and answers


@scenario("rpc/endpoint_cools_down_then_is_retried", covers=covers(
    "rpc_client", "failed_endpoint_cools_down_then_is_retried"))
def _cools_down(s):
    """A failed URL (HTTP 503) is not even tried for 30 s: the next requests go straight to
    the second URL. At the 30 s mark the first URL is asked again and, now healthy, answers."""
    node, _ = _game(s, [A, B])
    first = node.endpoint(A)
    first.http_status = 503
    _tick(s)
    assert _urls(s, "eth_blockNumber") == [A, B]
    first.http_status = None
    s.advance(COOLDOWN_START - 1)
    _tick(s)
    assert _urls(s, "eth_blockNumber") == [B]
    s.advance(1)
    _tick(s)
    assert _urls(s, "eth_blockNumber") == [A]


@scenario("rpc/cooldown_doubles_to_the_cap_and_recovers", covers=covers(
    "rpc_client", "cooldown_doubles_and_caps", "success_resets_cooldown"))
def _cooldown_ladder(s):
    """One URL that stays down is skipped for 30 s, then 60, 120, 240, 480 and 600 s, and
    stays at 600 (the cap): each time a request one second short of the cooldown makes no
    call, and one at the mark asks again. When it recovers, a success clears the cooldown, so
    the next failure starts again at 30 s."""
    node, _ = _game(s, [A])
    ep = node.endpoint(A)
    ep.down = True
    ladder = [30, 60, 120, 240, 480, 600, 600]
    assert ladder == [min(COOLDOWN_MAX, COOLDOWN_START * 2 ** i) for i in range(7)]
    for delay in ladder:
        assert _tick(s).error == "network_unavailable"
        assert _urls(s, "eth_blockNumber") == [A]
        s.advance(delay - 1)
        assert _tick(s).error == "network_unavailable"
        assert _urls(s, "eth_blockNumber") == []                # still cooling down: not even tried
        s.advance(1)
    ep.down = False
    assert _tick(s).error is None            # asked at the mark, answers
    assert _urls(s, "eth_blockNumber") == [A]
    ep.down = True
    assert _tick(s).error == "network_unavailable"
    s.advance(COOLDOWN_START - 1)
    assert _tick(s).error == "network_unavailable" and _urls(s, "eth_blockNumber") == []
    s.advance(1)
    _tick(s)
    assert _urls(s, "eth_blockNumber") == [A]                   # 30 s again, not 60 or 600


@scenario("rpc/all_endpoints_fail", covers=covers(
    "rpc_client", "all_endpoints_down_raises_unavailable"))
def _all_fail(s):
    """With both URLs down the tick reports network_unavailable. Both are then cooling down,
    so the next tick asks nobody (and says the same). When one has recovered and the cooldowns
    are over, the tick succeeds."""
    node, _ = _game(s, [A, B])
    node.endpoint(A).down = node.endpoint(B).down = True
    assert _tick(s).error == "network_unavailable"
    assert _urls(s) == [A, B]
    assert _tick(s).error == "network_unavailable"
    assert _urls(s) == []
    node.endpoint(B).down = False
    s.advance(COOLDOWN_START)
    assert _tick(s).error is None
    assert _urls(s) == [A, B, B]             # A fails again (now 60 s); B answers both requests


@scenario("rpc/node_errors_and_junk_answers", covers=covers(
    "rpc_client", "malformed_response_counts_as_endpoint_failure",
    "jsonrpc_error_is_raised_without_rotation", "rate_limit_jsonrpc_error_rotates"))
def _errors_and_junk(s):
    """What counts as the endpoint failing: a rate-limit message in a JSON-RPC error, an answer
    that is not an object, an object with neither result nor error. The request goes on to
    the second URL. Any other JSON-RPC error (execution reverted) is the node's answer, not a
    failure: it is not retried elsewhere and the tick reports network_unavailable."""
    node, _ = _game(s, [A, B])
    first = node.endpoint(A)
    first.errors["eth_blockNumber"] = (-32000, "execution reverted")
    assert _tick(s).error == "network_unavailable"
    assert _urls(s, "eth_blockNumber") == [A]
    del first.errors["eth_blockNumber"]
    for fault, value in (("errors", (-32005, "Too Many Requests: rate limit")),
                         ("raw_bodies", "not a dict"),
                         ("raw_bodies", {"jsonrpc": "2.0", "id": 1})):
        getattr(first, fault)["eth_blockNumber"] = value
        assert _tick(s).error is None
        assert _urls(s, "eth_blockNumber") == [A, B], value
        del getattr(first, fault)["eth_blockNumber"]
        s.advance(COOLDOWN_MAX + 1)
    assert _tick(s).error is None and _urls(s, "eth_blockNumber") == [A]


@scenario("rpc/malformed_error_objects", covers=covers(
    "rpc_client", "malformed_error_objects_become_rpc_errors"))
def _malformed_errors(s):
    """Providers send errors that are not {code, message}. They are read the same way: a bare
    string is the message (a rate-limit phrase still fails over, a range phrase still shrinks
    the block range), and a non-numeric or missing code is 0. An error without a hint phrase
    is the node's answer: one request, no failover."""
    node, _ = _game(s, [A, B], max_log_range=40)
    first = node.endpoint(A)
    first.error_objects["eth_blockNumber"] = "Too many requests, slow down"
    assert _tick(s).error is None and _urls(s, "eth_blockNumber") == [A, B]
    del first.error_objects["eth_blockNumber"]
    s.advance(COOLDOWN_START)
    for error in ("query timeout", {"code": "bad", "message": "odd"}, {"message": "no code"}):
        first.error_objects["eth_blockNumber"] = error
        assert _tick(s).error == "network_unavailable"
        assert _urls(s, "eth_blockNumber") == [A], error
    del first.error_objects["eth_blockNumber"]
    node.mine(60)
    first.error_objects["eth_getLogs"] = {"code": "bad", "message": _RANGE_ERROR}
    assert _tick(s).error == "network_unavailable"
    assert [hi - lo + 1 for lo, hi in _spans(s)] == [40, 20, MIN_LOG_RANGE]


# --- eth_getLogs ranges ------------------------------------------------------------------------


@scenario("rpc/getlogs_is_chunked_by_the_log_range", covers=covers(
    "rpc_client", "get_logs_chunks_by_log_range", "get_logs_sends_filter_shape"))
def _chunked(s):
    """A scan of 256 blocks on a chain whose range is 100 makes three eth_getLogs requests:
    995-1094, 1095-1194, 1195-1250. Each asks the token's Transfer logs to the developer's
    wallet (filter: address, topics [Transfer, any, wallet], from/to block in hex)."""
    node, _ = _game(s, [A], max_log_range=100)
    node.mine(250)
    assert _tick(s).error is None
    assert _spans(s) == [(995, 1094), (1095, 1194), (1195, 1250)]
    [flt] = [c for c in s.steps[-1]["calls"] if c["method"] == "eth_getLogs"][0]["params"]
    assert set(flt) == {"address", "topics", "fromBlock", "toBlock"}
    assert flt["address"] == AVAX_USDC and flt["fromBlock"] == hex(995)
    assert flt["topics"][1] is None and flt["topics"][2].endswith(DEV_EVM[2:].lower())
    assert _tick(s).error is None
    assert _spans(s) == []                   # the cursor is past the head: nothing to ask


@scenario("rpc/getlogs_range_shrinks_to_a_stated_limit", covers=covers(
    "rpc_client", "stated_limit_is_adopted_in_one_step",
    "suggested_range_uses_stated_limit_or_halves"))
def _stated_limit(s):
    """A node that says "limited to 0 - 50 blocks range" is believed: the range drops from
    2000 to 50 in one step, the request is repeated in 50-block chunks, and later scans use
    50 straight away."""
    node, _ = _game(s, [A], max_log_range=2000)
    node.endpoint(A).max_log_range = 50
    node.endpoint(A).max_log_range_error = (-32602, "eth_getLogs is limited to 0 - 50 blocks range")
    node.mine(100)
    assert _tick(s).error is None
    assert _spans(s) == [(995, 1100), (995, 1044), (1045, 1094), (1095, 1100)]
    node.mine(120)
    assert _tick(s).error is None
    assert _spans(s) == [(1101, 1150), (1151, 1200), (1201, 1220)]


@scenario("rpc/getlogs_range_halves_without_a_stated_limit", covers=covers(
    "rpc_client", "get_logs_halves_range_on_range_error_and_remembers",
    "suggested_range_uses_stated_limit_or_halves"))
def _halves(s):
    """A node that only says "block range too large" gets a request half the size that was
    rejected (506 blocks, then 253), and the new range is remembered for later scans. A
    stated number larger than the rejected span ("ranges over 10000 blocks...", code 35) is
    not a limit: it halves too."""
    node, _ = _game(s, [A])
    ep = node.endpoint(A)
    ep.max_log_range, ep.max_log_range_error = 300, (-32600, _RANGE_ERROR)
    node.mine(500)
    assert _tick(s).error is None
    assert _spans(s) == [(995, 1500), (995, 1247), (1248, 1500)]
    node.mine(400)
    assert _tick(s).error is None
    assert _spans(s) == [(1501, 1753), (1754, 1900)]       # remembered: 253, no error
    ep.max_log_range = 100
    ep.max_log_range_error = (35, "ranges over 10000 blocks are not supported on free plan")
    node.mine(300)
    assert _tick(s).error is None
    assert [hi - lo + 1 for lo, hi in _spans(s)][:3] == [253, 126, 63]


@scenario("rpc/getlogs_gives_up_at_the_minimum_range", covers=covers(
    "rpc_client", "get_logs_gives_up_at_minimum_range",
    "suggested_range_uses_stated_limit_or_halves"))
def _gives_up(s):
    """The range halves 40, 20, 10 and no further: at MIN_LOG_RANGE the node's error is
    final and the tick reports network_unavailable. The floor is remembered, so the next scan
    asks for 10 blocks and fails at once; the session keeps its cursor."""
    node, rec = _game(s, [A], max_log_range=40)
    ep = node.endpoint(A)
    ep.max_log_range, ep.max_log_range_error = 1, (-32005, _RANGE_ERROR)
    node.mine(100)
    assert _tick(s).error == "network_unavailable"
    assert [hi - lo + 1 for lo, hi in _spans(s)] == [40, 20, MIN_LOG_RANGE]
    assert _tick(s).error == "network_unavailable"
    assert [hi - lo + 1 for lo, hi in _spans(s)] == [MIN_LOG_RANGE]
    assert s.steps[-1]["snapshot"]["pending"][rec["id"]]["cursor"] == 995


@scenario("rpc/range_error_phrases", covers=covers(
    "rpc_client", "is_range_error", "archive_message_counts_as_range_error"))
def _range_phrases(s):
    """What providers say when the block range is too wide is recognised by phrase, whatever
    the code: "block range is too large", "more than 10000 results; limit exceeded" and
    "Archive requests require a personal token" each shrink the range; "execution reverted"
    does not: it is the end of the request (one eth_getLogs, range unchanged)."""
    node, _ = _game(s, [A])
    ep = node.endpoint(A)
    phases = [
        (400, (-32600, "eth_getLogs block range is too large"), 1000, 1000),
        (100, (-32005, "query returned more than 10000 results; limit exceeded"), 250, 300),
        (30, (-32602, "Archive requests require a personal token. Get one at x"), 62, 70),
    ]
    for limit, error, first_span, blocks in phases:
        ep.max_log_range, ep.max_log_range_error = limit, error
        node.mine(blocks)
        assert _tick(s).error is None
        spans = [hi - lo + 1 for lo, hi in _spans(s)]
        assert spans[0] == first_span and spans[1] < first_span, error
    ep.max_log_range = None
    ep.errors["eth_getLogs"] = (-32000, "execution reverted")
    node.mine(100)
    assert _tick(s).error == "network_unavailable"
    assert len(_spans(s)) == 1
    del ep.errors["eth_getLogs"]
    assert _tick(s).error is None
    assert [hi - lo + 1 for lo, hi in _spans(s)][0] == 15      # unchanged by that error


@scenario("rpc/log_range_is_remembered_per_endpoint", covers=covers(
    "rpc_client", "log_range_is_remembered_per_endpoint"))
def _per_endpoint(s):
    """A stingy fallback must not shrink the generous primary. The primary is down, so the
    fallback serves the scan and learns its 50-block limit. When the primary recovers it is
    asked first again, with its own range (200): one request for 100 blocks."""
    node, _ = _game(s, [A, B], max_log_range=200)
    node.endpoint(B).max_log_range = 50
    node.mine(150)
    assert _tick(s).error is None
    assert _spans(s) == [(995, 1150)]                      # A: one request
    node.endpoint(A).down = True
    node.mine(120)
    assert _tick(s).error is None
    assert _urls(s, "eth_getLogs")[0] == B
    assert [hi - lo + 1 for lo, hi in _spans(s)] == [120, 50, 50, 20]
    node.endpoint(A).down = False
    s.advance(COOLDOWN_START)
    node.mine(100)
    assert _tick(s).error is None
    assert _urls(s, "eth_getLogs") == [A]
    assert [hi - lo + 1 for lo, hi in _spans(s)] == [100]
