import pytest

from scfg_core.coio import (
    Request,
    Sync,
    TransportError,
    response_from_envelope,
    run_sync,
    synced,
)


class Transport:
    def __init__(self, answers):
        self.answers = list(answers)
        self.seen = []

    def post_json(self, url, payload, timeout):
        self.seen.append((url, payload, timeout))
        answer = self.answers.pop(0)
        if isinstance(answer, BaseException):
            raise answer
        return answer


def two_calls():
    a = yield Request("http://a", {"n": 1}, 5.0)
    b = yield Request("http://b", {"n": 2}, 5.0)
    return a["v"] + b["v"]


def test_run_sync_sends_responses_back_and_returns_the_result():
    t = Transport([{"v": 1}, {"v": 2}])
    assert run_sync(two_calls(), t) == 3
    assert t.seen == [("http://a", {"n": 1}, 5.0), ("http://b", {"n": 2}, 5.0)]


def test_run_sync_throws_transport_errors_into_the_generator():
    def guarded():
        try:
            yield Request("http://a", {}, 1.0)
        except TransportError as e:
            return "caught " + str(e)
    assert run_sync(guarded(), Transport([TransportError("down")])) == "caught down"


def test_run_sync_throws_any_transport_exception_into_the_generator():
    def guarded():
        try:
            yield Request("http://a", {}, 1.0)
        except RuntimeError as e:
            return "caught " + str(e)
    assert run_sync(guarded(), Transport([RuntimeError("odd failure")])) == "caught odd failure"


def test_an_uncaught_transport_error_reaches_the_caller():
    with pytest.raises(TransportError):
        run_sync(two_calls(), Transport([TransportError("down")]))


def test_run_sync_without_transport_runs_only_request_free_generators():
    def no_io():
        return 7
        yield  # a generator, like every cooperative function
    assert run_sync(no_io(), None) == 7
    with pytest.raises(RuntimeError, match="no transport"):
        run_sync(two_calls(), None)


def test_run_sync_closes_the_generator_on_unexpected_errors():
    closed = []

    def gen():
        try:
            yield Request("http://a", {}, 1.0)
        finally:
            closed.append(True)
    with pytest.raises(KeyError):
        run_sync(gen(), Transport([KeyError("bug")]))
    assert closed == [True]


class Thing:
    label = "x"

    def add(self, n):
        r = yield Request("http://a", {}, 1.0)
        return r["v"] + n

    def plain(self):
        return "plain"


def test_sync_drives_generator_methods_and_passes_the_rest_through():
    s = Sync(Thing(), Transport([{"v": 1}]))
    assert s.add(2) == 3
    assert s.plain() == "plain" and s.label == "x"
    s.label = "y"
    assert s._target.label == "y"


def test_synced_drives_a_generator_function():
    assert synced(two_calls, Transport([{"v": 4}, {"v": 5}]))() == 9


@pytest.mark.parametrize("envelope, expected", [
    ({"status": 200, "body": '{"result": 1}'}, {"result": 1}),
    ({"status": 400, "body": '{"error": {"code": -32005, "message": "range"}}'},
     {"error": {"code": -32005, "message": "range"}}),
    ({"status": 200, "body": "null"}, None),
])
def test_envelope_to_response(envelope, expected):
    assert response_from_envelope(envelope) == expected


@pytest.mark.parametrize("envelope, message", [
    ({"error": "timed out"}, "timed out"),
    ({"status": 503, "body": "busy"}, "HTTP 503"),
    ({"status": 429, "body": '{"jsonrpc": "2.0"}'}, "HTTP 429"),
    ({"status": 200, "body": "<html>"}, "invalid JSON response"),
    ({"status": 0, "body": ""}, "HTTP 0"),
])
def test_envelope_failures_are_transport_errors(envelope, message):
    with pytest.raises(TransportError, match=message):
        response_from_envelope(envelope)


def test_an_http_error_keeps_its_status():
    with pytest.raises(TransportError) as e:
        response_from_envelope({"status": 502, "body": ""})
    assert e.value.status == 502
