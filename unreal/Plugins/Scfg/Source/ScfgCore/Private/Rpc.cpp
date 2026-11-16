// Twin of scfg_core/rpc.py: RpcClient, is_range_error, suggested_range, _parse_error, _is_rate_error.
// The one file waived for <chrono>/steady_clock (ruling R3): the default monotonic clock, Python's time.monotonic.
#include "Scfg/Rpc.h"

#include "Scfg/Ints.h"
#include "Scfg/Py.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>

namespace scfg::rpc::detail {

constexpr std::array<std::string_view, 3> kRateHints = {"rate limit", "too many requests", "rate-limit"};
// Phrases providers use for "your eth_getLogs block range is too wide". Some say "archive" (publicnode answers
// wide ranges with "Archive requests require a personal token").
constexpr std::array<std::string_view, 8> kRangeHints = {"range",      "too many",      "limit",     "exceed",
                                                         "max results", "response size", "too large", "archive"};

template <size_t N>
bool lowered_contains_any(std::string_view message, const std::array<std::string_view, N>& hints) {
    std::string m = py::lower_for_ascii(message);
    return std::any_of(hints.begin(), hints.end(), [&m](std::string_view h) { return m.find(h) != std::string::npos; });
}

// time.monotonic(): seconds on std::chrono::steady_clock.
double monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// re.findall(r"\d+", s): every maximal run of Python 3.9 decimal digits, in order.
std::vector<std::string_view> digit_runs(std::string_view s) {
    std::vector<std::string_view> runs;
    size_t start = std::string_view::npos;
    for (size_t i = 0; i < s.size();) {
        size_t at = i;
        bool digit = py::is_decimal_digit(py::utf8_decode(s, i));
        if (digit && start == std::string_view::npos) start = at;
        if (!digit && start != std::string_view::npos) {
            runs.push_back(s.substr(start, at - start));
            start = std::string_view::npos;
        }
    }
    if (start != std::string_view::npos) runs.push_back(s.substr(start));
    return runs;
}

// The RpcError _parse_error makes (a wrapper: Result<Err> is not a thing).
struct NodeError {
    Err err;
};

// Python _parse_error: providers are inconsistent, the error may be an object, a bare string, or carry a
// non-numeric code. A code int_from cannot read (TypeError, ValueError) is 0; an infinite one is int()'s
// OverflowError, which Python does not catch either. The message is str(message).
Result<NodeError> parse_error(const Json& e) {
    Json code = 0, message = e;
    if (e.is_dict()) {
        code = e.as_dict().get("code", 0);
        message = e.as_dict().get("message", "");
    }
    Result<BigInt> n = ints::int_from(code);
    BigInt value;
    if (n.ok()) {
        value = n.take();
    } else if (!n.err().is_a("TypeError") && !n.err().is_a("ValueError")) {
        return std::move(n).err();
    }
    std::string text = py::str(message);
    if (!value.fits_int64()) return NodeError{Err::make("RpcError", std::move(text))};  // no code (see Rpc.h)
    return NodeError{Err::rpc_error(value.to_int64().value(), std::move(text))};
}

std::optional<Attempt> failed() { return std::nullopt; }

}  // namespace scfg::rpc::detail

namespace scfg::rpc {

bool is_range_error(const Err& err) { return detail::lowered_contains_any(err.message, detail::kRangeHints); }

bool is_rate_error(const Err& err) { return detail::lowered_contains_any(err.message, detail::kRateHints); }

BigInt suggested_range(const Err& err, const BigInt& tried) {
    std::optional<int64_t> best;
    for (std::string_view run : detail::digit_runs(err.message)) {
        // A run of \d holds decimal digits only, so digit_run_value has nothing to refuse.
        Result<std::optional<int64_t>> v = ints::digit_run_value(run);
        if (!v.ok() || !v.value()) continue;
        int64_t n = *v.value();
        if (MIN_LOG_RANGE <= n && BigInt(n) < tried && (!best || n > *best)) best = n;
    }
    if (best) return *best;
    return std::max(BigInt(MIN_LOG_RANGE), tried.floordiv(2).value());
}

Result<RpcClient> RpcClient::make(std::vector<std::string> urls, coio::Transport& transport, RpcClientOptions options) {
    if (urls.empty()) return Err::value_error("at least one RPC url required");
    return RpcClient(std::move(urls), transport, std::move(options));
}

RpcClient::RpcClient(std::vector<std::string> urls, coio::Transport& transport, RpcClientOptions options)
    : clock(options.clock ? std::move(options.clock) : std::function<double()>(detail::monotonic)),
      timeout(options.timeout),
      urls_(std::move(urls)),
      transport_(&transport) {
    for (const std::string& u : urls_) {
        ranges_.set(u, options.log_range);
        state_.set(u, EndpointState{});
    }
}

Status RpcClient::fail(const std::string& url, double now) {
    EndpointState* st = state_.find(url);
    if (st == nullptr) return py::key_error(url);
    st->delay = st->delay == 0 ? COOLDOWN_START : std::min(COOLDOWN_MAX, st->delay * 2);
    st->until = now + st->delay;
    return scfg::ok();
}

Status RpcClient::ok(const std::string& url) {
    EndpointState* st = state_.find(url);
    if (st == nullptr) return py::key_error(url);
    st->delay = 0.0;
    st->until = 0.0;
    return scfg::ok();
}

const std::string& RpcClient::preferred() const {
    double now = clock();
    for (const std::string& url : urls_) {
        if (state_.find(url)->until <= now) return url;
    }
    return urls_[0];
}

BigInt RpcClient::log_range() const { return *ranges_.find(preferred()); }

std::vector<std::string> RpcClient::live_urls() const {
    double now = clock();
    std::vector<std::string> out;
    for (const std::string& url : urls_) {
        if (state_.find(url)->until <= now) out.push_back(url);
    }
    return out;
}

BigInt RpcClient::range_of(std::string_view url) const {
    const BigInt* r = ranges_.find(url);
    return r != nullptr ? *r : BigInt();
}

double RpcClient::cooldown_of(std::string_view url) const {
    const EndpointState* st = state_.find(url);
    return st != nullptr ? st->delay : 0.0;
}

double RpcClient::cooldown_until(std::string_view url) const {
    const EndpointState* st = state_.find(url);
    return st != nullptr ? st->until : 0.0;
}

Co<std::optional<Attempt>> RpcClient::attempt(std::string url, std::string method, Json params, double now) {
    Json payload = Json::dict({{"jsonrpc", "2.0"}, {"id", next_id_++}, {"method", method}, {"params", params}});
    Result<Json> got = co_await transport_->post_json(url, payload, timeout);
    if (!got.ok()) {
        // Python's `except TransportError`: anything else reaches the caller.
        if (!got.err().is_a("TransportError")) co_return std::move(got).err();
        SCFG_CO_TRY_VOID(fail(url, now));
        co_return detail::failed();
    }
    Json resp = got.take();
    if (!resp.is_dict() || (!resp.as_dict().contains("result") && !resp.as_dict().contains("error"))) {
        SCFG_CO_TRY_VOID(fail(url, now));
        co_return detail::failed();
    }
    Json error = resp.as_dict().get("error");
    if (!error.is_null()) {
        SCFG_CO_TRY(detail::NodeError node, detail::parse_error(error));
        if (is_rate_error(node.err)) {
            SCFG_CO_TRY_VOID(fail(url, now));
            co_return detail::failed();
        }
        SCFG_CO_TRY_VOID(ok(url));
        node.err.url = url;
        co_return std::optional<Attempt>(Attempt{std::move(node.err), Json()});
    }
    SCFG_CO_TRY_VOID(ok(url));
    // resp["result"]: {"error": null} without a result is a KeyError (not an endpoint failure).
    SCFG_CO_TRY(Json result, py::get(resp, "result"));
    co_return std::optional<Attempt>(Attempt{std::nullopt, std::move(result)});
}

Co<std::pair<Json, std::string>> RpcClient::call_with_url(std::string method, Json params) {
    double now = clock();
    for (size_t i = 0; i < urls_.size(); ++i) {
        std::string url = urls_[i];
        if (state_.find(url)->until > now) continue;
        SCFG_CO_TRY(std::optional<Attempt> res, co_await attempt(url, method, params, now));
        if (!res) continue;
        if (res->error) co_return std::move(*res->error);
        co_return std::pair<Json, std::string>(std::move(res->result), std::move(url));
    }
    co_return Err::make("RpcUnavailable", "all endpoints failed for " + method);
}

Co<Json> RpcClient::call(std::string method, Json params) {
    SCFG_CO_TRY(auto answered, co_await call_with_url(std::move(method), std::move(params)));
    co_return std::move(answered.first);
}

Co<Json> RpcClient::call_at(std::string url, std::string method, Json params) {
    SCFG_CO_TRY(std::optional<Attempt> res, co_await attempt(url, method, std::move(params), clock()));
    if (!res) co_return Err::make("RpcUnavailable", url + " failed for " + method);
    if (res->error) co_return std::move(*res->error);
    co_return std::move(res->result);
}

Co<std::optional<Attempt>> RpcClient::try_at(std::string url, std::string method, Json params) {
    co_return co_await attempt(std::move(url), std::move(method), std::move(params), clock());
}

Co<BigInt> RpcClient::block_number() {
    SCFG_CO_TRY(Json result, co_await call("eth_blockNumber", Json::list()));
    co_return py::py_int_hex(result);
}

Co<Json> RpcClient::get_transaction_receipt(Json tx_hash) {
    Json params = Json::list({std::move(tx_hash)});
    co_return co_await call("eth_getTransactionReceipt", std::move(params));
}

Co<std::optional<std::string>> RpcClient::get_block_hash(BigInt number) {
    Json params = Json::list({py::hex(number), false});
    SCFG_CO_TRY(Json block, co_await call("eth_getBlockByNumber", std::move(params)));
    if (!py::truthy(block)) co_return std::optional<std::string>();
    SCFG_CO_TRY(Json hash, py::get(block, "hash"));
    if (!hash.is_str()) {
        co_return Err::make("AttributeError", "'" + std::string(hash.type_name()) + "' object has no attribute 'lower'");
    }
    co_return std::optional<std::string>(py::lower_for_ascii(hash.as_str()));
}

Co<std::vector<Json>> RpcClient::get_logs(std::string address, Json topics, BigInt from_block, BigInt to_block) {
    std::vector<Json> out;
    BigInt start = from_block;
    while (start <= to_block) {
        BigInt end = std::min(to_block, start + log_range() - 1);
        Json filter = Json::dict(
            {{"address", address}, {"topics", topics}, {"fromBlock", py::hex(start)}, {"toBlock", py::hex(end)}});
        Json params = Json::list({filter});
        Result<std::pair<Json, std::string>> got = co_await call_with_url("eth_getLogs", std::move(params));
        if (!got.ok()) {
            const Err& e = got.err();
            if (!e.is_a("RpcError")) co_return std::move(got).err();
            // getattr(e, "url", self._preferred()): the default is evaluated (the clock read) either way.
            std::string fallback = preferred();
            BigInt* range = ranges_.find(e.url.empty() ? fallback : e.url);
            if (range == nullptr) co_return py::key_error(e.url);
            BigInt tried = std::min(*range, end - start + 1);
            if (is_range_error(e) && tried > MIN_LOG_RANGE) {
                *range = suggested_range(e, tried);
                continue;
            }
            co_return std::move(got).err();
        }
        Json logs = std::move(got.value().first);
        if (py::truthy(logs)) {
            SCFG_CO_TRY(std::vector<Json> items, py::iterate(logs));
            out.insert(out.end(), items.begin(), items.end());
        }
        start = end + 1;
    }
    co_return out;
}

}  // namespace scfg::rpc
