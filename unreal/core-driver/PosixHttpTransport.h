// Copyright 2026 Repulse
// No Python twin: the integration driver's real transport (Python's rpc.UrllibTransport, which reads an HTTP answer
// the same way). Test-only, never shipped: it uses POSIX sockets, which the core may not, and it blocks.
//
// post_json makes one HTTP/1.1 POST to an http://host[:port][/path] URL (the local chains; any other scheme is a
// TransportError), with Content-Type: application/json and Connection: close, reads the status line, the headers
// and the body (Content-Length, chunked, or up to EOF; at most MAX_RESPONSE_BYTES + 1 bytes are kept), hands the
// status and body to coio::parse_rpc_body, and returns a Future that is already complete. The timeout bounds the
// whole request (connect, send and every read, through poll). A timeout, a connection error or a malformed answer
// is a TransportError with Python's message ("timed out", "<urlopen error [Errno 111] Connection refused>", ...).
#pragma once

#include "Scfg/Coio.h"
#include "Scfg/Json.h"

#include <cstddef>
#include <string>

namespace scfg_driver {

class PosixHttpTransport final : public scfg::coio::Transport {
public:
    scfg::Future<scfg::Json> post_json(std::string url, scfg::Json payload, double timeout) override;
    // Requests made so far (transport_stats).
    size_t posts() const { return posts_; }

private:
    size_t posts_ = 0;
};

}  // namespace scfg_driver
