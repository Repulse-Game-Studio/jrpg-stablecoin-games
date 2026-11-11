import pytest

from scfg_core.coio import Sync
from scfg_core.rpc import RpcClient, RpcError, RpcUnavailable
from scfg_core.solana.rpc import TX_VERSION, SolanaRpc

from fakes import FakeTransport, down, err, ok

A, B = "https://a.example", "https://b.example"


def client(handlers):
    t = FakeTransport(handlers)
    return Sync(SolanaRpc(RpcClient(list(handlers), clock=lambda: 0.0)), t), t


def test_get_transaction_asks_for_newest_version_and_json():
    rpc, t = client({A: lambda m, p: ok({"slot": 5})})
    assert rpc.get_transaction("SIG") == {"slot": 5}
    assert t.calls[0][1:] == ("getTransaction", ["SIG", {
        "encoding": "json", "commitment": "confirmed", "maxSupportedTransactionVersion": TX_VERSION}])


def test_get_transaction_lowers_version_when_the_node_rejects_the_parameter():
    def handler(m, p):
        if p[1]["maxSupportedTransactionVersion"] == 1:
            return err(-32602, "Invalid params: unknown maxSupportedTransactionVersion 1")
        return ok({"slot": 5})
    rpc, _ = client({A: handler})
    assert rpc.get_transaction("SIG") == {"slot": 5} and rpc.tx_version == 0


def test_try_at_each_live_url_reports_per_endpoint_and_dead_ones_cool_down():
    rpc, t = client({A: down, B: lambda m, p: err(-1, "boom")})
    c = Sync(rpc.client, t)
    out = []
    for url in c.live_urls():
        res = c.try_at(url, "getSlot", [])
        if res is not None:
            out.append((url, res))
    assert len(out) == 1 and out[0][0] == B
    kind, value = out[0][1]
    assert kind == "error" and isinstance(value, RpcError)
    assert c.live_urls() == [B]


def _history(first_a, first_b, tx_b=None):
    def make(first, tx):
        def handler(m, p):
            if m == "getFirstAvailableBlock":
                return ok(first)
            return ok(tx)
        return handler
    return {A: make(first_a, None), B: make(first_b, tx_b)}


def test_get_transaction_any_returns_first_found():
    rpc, _ = client(_history(10**9, 0, tx_b={"slot": 50}))
    assert rpc.get_transaction_any("SIG", min_slot=50) == ({"slot": 50}, False)


def test_null_from_full_history_endpoint_is_authoritative():
    rpc, _ = client(_history(10**9, 0))
    assert rpc.get_transaction_any("SIG", min_slot=50) == (None, True)


def test_null_only_from_pruned_endpoints_is_not_authoritative():
    rpc, _ = client(_history(10**9, 10**9))
    assert rpc.get_transaction_any("SIG", min_slot=50) == (None, False)


def test_get_transaction_any_without_answers_raises():
    rpc, _ = client({A: down, B: down})
    with pytest.raises(RpcUnavailable):
        rpc.get_transaction_any("SIG", min_slot=1)


def test_small_helpers():
    def handler(m, p):
        return {
            "getSlot": ok(77),
            "getSignaturesForAddress": ok([{"signature": "S2"}, {"signature": "S1"}]),
            "getSignatureStatuses": ok({"value": [None, {"confirmationStatus": "finalized"}]}),
            "getAccountInfo": ok({"value": None}),
            "getLatestBlockhash": ok({"value": {"blockhash": "BH", "lastValidBlockHeight": 9}}),
            "getBalance": ok({"value": 1234}),
            "getRecentPrioritizationFees": ok([{"slot": 1, "prioritizationFee": f}
                                               for f in (9, 1, 5)]),
        }[m]
    rpc, t = client({A: handler})
    assert rpc.get_slot() == 77
    assert [s["signature"] for s in rpc.get_signatures_for_address("X", until="S0")] == ["S2", "S1"]
    assert t.calls[-1][2] == ["X", {"limit": 100, "commitment": "confirmed", "until": "S0"}]
    assert rpc.get_signature_statuses(["a", "b"])[1]["confirmationStatus"] == "finalized"
    assert t.calls[-1][2][1] == {"searchTransactionHistory": True}
    assert rpc.account_exists("X") is False and rpc.get_token_balance("X") == 0
    assert rpc.get_latest_blockhash() == "BH"
    assert rpc.get_balance("X") == 1234
    assert rpc.get_priority_fee(["X"]) == 5


_V2 = ("Transaction version (2) is not supported by the requesting client. Please try the "
       "request again with the following configuration parameter: "
       "\"maxSupportedTransactionVersion\": 2")


def _versioned(needs):
    def handler(m, p):
        if m == "getFirstAvailableBlock":
            return ok(0)
        if p[1]["maxSupportedTransactionVersion"] < needs:
            return err(-32015, _V2.replace("(2)", f"({needs})").replace(": 2", f": {needs}"))
        return ok({"slot": 5})
    return handler


def test_get_transaction_retries_once_with_the_version_the_node_names():
    rpc, t = client({A: _versioned(2)})
    assert rpc.get_transaction("SIG") == {"slot": 5}
    assert [c[2][1]["maxSupportedTransactionVersion"] for c in t.calls] == [TX_VERSION, 2]
    assert rpc.tx_version == TX_VERSION


def test_unsupported_version_error_naming_an_older_version_is_raised():
    rpc, t = client({A: lambda m, p: err(-32015, _V2.replace("(2)", "(0)"))})
    with pytest.raises(RpcError):
        rpc.get_transaction("SIG")
    assert len(t.calls) == 1


# A version beyond 18 significant digits is no match (as GDScript: a request cannot
# carry it as an int), so the node's error is raised; no long digit text reaches int().
@pytest.mark.parametrize("message, expected", [
    ("Transaction version (" + "9" * 19 + ") is not supported", None),
    ("Transaction version (" + "9" * 4301 + ") is not supported", None),
    ("Transaction version (" + "9" * 1_000_000 + ") is not supported", None),
    ("Transaction version (" + "9" * 18 + ") is not supported", 10**18 - 1),
    ("Transaction version (" + "0" * 30 + "3) is not supported", 3),
    ("version (" + "9" * 19 + ") version (5)", None),
    ("version (12 version (5)", 5),
    ("version (\u0663)", 3),
    ("Transaction VER\u017fION (2) x", 2),
    ("vers\u0131on (4)", 4),
    ("vers\u0130on (4)", 4),
], ids=["19-digits", "4301", "a-million", "18-digits", "leading-zeros", "first-match-only",
        "unclosed-then-closed", "arabic-indic", "long-s", "dotless-i", "dotted-I"])
def test_named_version_ignores_numbers_beyond_18_digits(interpreter, int_texts, message, expected):
    import scfg_core.ints
    import scfg_core.solana.rpc
    converted = int_texts(scfg_core.solana.rpc, scfg_core.ints)
    assert SolanaRpc(None)._named_version(RpcError(-32015, message)) == expected
    assert max(converted, default=0) <= 18


def test_an_overlong_named_version_raises_the_node_error(interpreter):
    rpc, t = client({A: lambda m, p: err(-32015, _V2.replace("(2)", "(" + "9" * 4301 + ")"))})
    with pytest.raises(RpcError):
        rpc.get_transaction("SIG")
    assert len(t.calls) == 1


def test_get_transaction_any_retries_with_the_named_version():
    rpc, _ = client({A: _versioned(3)})
    assert rpc.get_transaction_any("SIG", min_slot=1) == ({"slot": 5}, False)
