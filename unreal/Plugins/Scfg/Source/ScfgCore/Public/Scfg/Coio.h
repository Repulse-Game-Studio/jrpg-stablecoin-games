// Copyright 2026 Repulse
// Twin of scfg_core/coio.py, its transport half (TransportError, Request, response_from_envelope), and of
// rpc.UrllibTransport's rules for reading an HTTP answer (parse_rpc_body), as unity/.../Core/Coio.cs ports them.
//
// The core never performs a network request itself. Where Python's generator yields a Request, a Co<T> awaits
// the Future<Json> an injected Transport hands out (Co.h). TransportError is an Err kind (Err::transport_error).
// Not ported: run_sync, Sync and synced (Co and Job replace them), and UrllibTransport itself: a real transport
// is platform code (the console driver's, Unreal's FHttpModule in phase 2) that reads the answer's status and
// bytes and hands them to parse_rpc_body.
#pragma once

#include "Scfg/Co.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace scfg::coio {

// The most bytes of one HTTP answer a transport reads; a longer one is TransportError("response too large")
// (Coio.cs's MaxResponseBytes, coio.ts's MAX_RESPONSE_BYTES; scfg_core's UrllibTransport reads without a cap).
// An eth_getLogs page filtered to one recipient is a few kilobytes and public providers cap their own answers
// near 10 MB, so 16 MiB only refuses an endpoint that is misbehaving or hostile.
inline constexpr size_t MAX_RESPONSE_BYTES = 16 * 1024 * 1024;
inline constexpr std::string_view RESPONSE_TOO_LARGE = "response too large";
inline constexpr std::string_view INVALID_JSON = "invalid JSON response";

// One JSON-RPC POST the core wants made (Python's frozen dataclass): what post_json takes, for a transport or a
// test that keeps requests.
struct Request {
    std::string url;
    Json payload;
    double timeout = 0;
};

// The transport contract (Python's post_json(url, payload, timeout), driven by run_sync or the PumpWorker): POST
// payload as JSON to url, waiting at most timeout seconds, and complete the Future with the parsed JSON-RPC
// response, or with Err TransportError when the endpoint failed (parse_rpc_body makes exactly that from an HTTP
// answer). The Future may complete later (from an HTTP callback or a pump) or already be complete.
//
// Any other Err kind is not an endpoint failure, as run_sync throws whatever the transport raised into the
// generator: the RPC client passes it on to its caller, where Python's handlers would see it. A transport that
// drops a request on shutdown (destroys its Promise) completes it with CancelledError, which resumes the waiting
// coroutine inside that destructor: destroy the Jobs that use a transport before the transport (Co.h, "Teardown
// order").
class Transport {
public:
    virtual ~Transport() = default;
    virtual Future<Json> post_json(std::string url, Json payload, double timeout) = 0;
};

// The JSON-RPC response for an HTTP answer, by rpc.UrllibTransport's rules, as every real transport reads one:
// - more than MAX_RESPONSE_BYTES bytes is TransportError("response too large");
// - a 2xx answer is its body read as Python's ints.json_loads(raw.decode("utf-8")) reads it (json::loads: strict
//   UTF-8, a BOM refused, an integer literal of more than 4300 digits refused), or TransportError("invalid JSON
//   response") when it cannot be read;
// - any other status is TransportError("HTTP <status>", status), unless the body is a JSON object with a truthy
//   "error" (providers send JSON-RPC errors such as "range too large" with a 4xx status): that body is returned.
SCFGCORE_API Result<Json> parse_rpc_body(int status, std::string_view body);

// Python response_from_envelope: a browser fetch() result (bridge.js: {"status", "body"} or {"error"}) as the
// JSON-RPC response, by the same rules as parse_rpc_body (no size cap: the page already read the body). The body
// is an already-decoded str, read with json::loads_text (a lone surrogate in it is kept); the status is read with
// int_from (its ValueError/TypeError is not caught, as in Python), and a body that is not a str is json.loads's
// TypeError. {"error": e} is TransportError(str(e)).
SCFGCORE_API Result<Json> response_from_envelope(const Json::Dict& envelope);

}  // namespace scfg::coio
