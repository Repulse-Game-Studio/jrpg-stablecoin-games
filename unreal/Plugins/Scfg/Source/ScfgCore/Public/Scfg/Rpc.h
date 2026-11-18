// Copyright 2026 Repulse
// Twin of scfg_core/rpc.py (and of unity/.../Core/Rpc.cs, rpgmaker/src/core/rpc.ts): the Ethereum JSON-RPC
// client with endpoint failover (RpcClient), is_range_error, suggested_range and the error parsing.
//
// Public RPC endpoints are free but unreliable: they rate-limit, go down, and cap eth_getLogs block ranges. The
// client tries endpoints in order on every call, skipping ones in cooldown (30 s doubling to 10 min, on the
// injected clock), within a total budget of CALL_BUDGET seconds per call (each attempt waits at most what is left
// of it, so a chain with four dead endpoints says so after 15 s, not 40), and remembers per endpoint the largest
// eth_getLogs range it accepts, shrinking it when a provider says a range is too large. Every network method is a
// Co<...> that awaits the injected Transport where Python's generator yields a Request. Response ids are never
// checked against request ids, as in Python.
//
// Errors are Err kinds: RpcError (a JSON-RPC error object from the node: rpc_code, message (Python's
// err.message) and url, the endpoint that answered) and RpcUnavailable (every endpoint failed or is cooling
// down). A code beyond int64 (only a hostile node sends one) is kept as no code: it equals none the core compares
// with, as in Python, but Python's str(err) "<code>: <message>" is not reproduced (nothing in the core prints it).
//
// Block numbers are BigInt, as Python's ints are unbounded: a hostile head beyond int64 is chunked, hex()ed and
// compared as Python does it, never overflowed.
//
// Not ported: UrllibTransport (a real transport is platform code; coio::parse_rpc_body holds its rules).
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Err.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scfg::rpc {

inline constexpr double COOLDOWN_START = 30.0;
inline constexpr double COOLDOWN_MAX = 600.0;
// The most seconds one call() spends across a chain's endpoints: the first attempt gets the full per-request
// timeout (10 s), each later one min(timeout, what is left); once nothing is left the endpoints not tried yet are
// skipped (not cooled down) and the call is RpcUnavailable. Measured on the RpcClient's clock, so a session start
// on a chain whose endpoints all hang fails after 15 s whatever the number of urls.
inline constexpr double CALL_BUDGET = 15.0;
// The smallest eth_getLogs block range the client narrows to, and the smallest max_log_range a custom chain may
// declare (config.build_chain).
inline constexpr int64_t MIN_LOG_RANGE = 10;

// Python is_range_error: the node's message (lowered as Python's str.lower() for an ASCII search) holds one of
// the phrases providers use for "your eth_getLogs block range is too wide".
SCFGCORE_API bool is_range_error(const Err& err);
// Python _is_rate_error: "rate limit", "too many requests" or "rate-limit".
SCFGCORE_API bool is_rate_error(const Err& err);
// Python's `except (RpcUnavailable, RpcError)`: no endpoint answered, or the node refused the request.
inline bool network_failure(const Err& e) { return e.is_a("RpcUnavailable") || e.is_a("RpcError"); }

// Python suggested_range: the next eth_getLogs range to try after err rejected a `tried`-block request. Uses
// the largest limit the provider states that is at least MIN_LOG_RANGE and below tried ("limited to 0 - 50 blocks
// range"), otherwise halves (floor), but not below MIN_LOG_RANGE. A number is a run of Python 3.9's \d (any
// Unicode 13 decimal digit); one of more than 18 significant digits is ignored (Ruling 6, ints::digit_run_value),
// so no long run is converted.
SCFGCORE_API BigInt suggested_range(const Err& err, const BigInt& tried);

// What try_at got from an endpoint that answered: Python's ("ok", result) or ("error", RpcError).
struct Attempt {
    std::optional<Err> error;  // the node's RpcError; empty for a result
    Json result;               // the call's result (None for an error)
};

// RpcClient's keyword arguments.
struct RpcClientOptions {
    BigInt log_range = 1000;
    // Seconds on a monotonic clock (Python's float time.monotonic); empty: std::chrono::steady_clock.
    std::function<double()> clock = {};
    double timeout = 10.0;
    double budget = CALL_BUDGET;
};

// Python rpc.RpcClient. Holds the transport by reference: the caller keeps it alive for the client's lifetime.
// A network method's Co refers to this client: keep the client alive, and do not move it, while one runs.
class SCFGCORE_API RpcClient {
public:
    // ValueError "at least one RPC url required" for no urls.
    static Result<RpcClient> make(std::vector<std::string> urls, coio::Transport& transport,
                                  RpcClientOptions options = {});

    // The clock cooldowns run on (settable, as Python's rpc.clock), each request's timeout and the most one
    // call() spends across the endpoints (seconds; call_at and try_at use the plain timeout).
    std::function<double()> clock;
    double timeout = 10.0;
    double budget = CALL_BUDGET;

    const std::vector<std::string>& urls() const { return urls_; }
    // Python log_range: the eth_getLogs block range accepted by the currently preferred endpoint.
    BigInt log_range() const;
    // Endpoints not cooling down right now, in preference order.
    std::vector<std::string> live_urls() const;
    // Python _ranges[url], _state[url].delay and _state[url].until, for tests (0 for a url not in the list).
    BigInt range_of(std::string_view url) const;
    double cooldown_of(std::string_view url) const;
    double cooldown_until(std::string_view url) const;

    // The call's result from the first endpoint that answers (in order, skipping cooling ones). RpcError for a
    // node error (no rotation), RpcUnavailable when every endpoint failed; any other Err the transport gave (not a
    // TransportError) passes through, and a malformed error object can be int()'s OverflowError (a code of inf).
    Co<Json> call(std::string method, Json params);
    // Ask one endpoint only (even one cooling down). RpcUnavailable if it fails, RpcError for a node error.
    Co<Json> call_at(std::string url, std::string method, Json params);
    // Ask one endpoint: an Attempt, or nullopt if it failed (and now cools down).
    Co<std::optional<Attempt>> try_at(std::string url, std::string method, Json params);

    // int(eth_blockNumber, 16): Python's int() rules, any size or sign.
    Co<BigInt> block_number();
    // The hash goes out as given (a stored hint is sent unchecked).
    Co<Json> get_transaction_receipt(Json tx_hash);
    // block["hash"].lower(), or nullopt for a falsy block: KeyError/TypeError for a block that has no "hash",
    // AttributeError for a hash that is not a str (lowered for an ASCII search, as Evm.cpp lowers hashes).
    Co<std::optional<std::string>> get_block_hash(BigInt number);
    // eth_getLogs over [from_block, to_block] in chunks of log_range(). A range error from the endpoint that
    // answered shrinks its range (suggested_range) and retries the chunk, down to MIN_LOG_RANGE. Each answer
    // extends the result as Python's out.extend(logs or []) does.
    Co<std::vector<Json>> get_logs(std::string address, Json topics, BigInt from_block, BigInt to_block);

private:
    struct EndpointState {
        double delay = 0.0;
        double until = 0.0;
    };

    RpcClient(std::vector<std::string> urls, coio::Transport& transport, RpcClientOptions options);
    Status fail(const std::string& url, double now);
    Status ok(const std::string& url);
    const std::string& preferred() const;
    // Python _attempt: an Attempt, or nullopt when the endpoint failed (and now cools down). The request waits
    // `wait` seconds (empty: timeout).
    Co<std::optional<Attempt>> attempt(std::string url, std::string method, Json params, double now,
                                       std::optional<double> wait = std::nullopt);
    // Python _call: like call(), but also returns which endpoint answered.
    Co<std::pair<Json, std::string>> call_with_url(std::string method, Json params);

    std::vector<std::string> urls_;
    coio::Transport* transport_;
    OrderedMap<BigInt> ranges_;
    OrderedMap<EndpointState> state_;
    int64_t next_id_ = 1;
};

}  // namespace scfg::rpc
