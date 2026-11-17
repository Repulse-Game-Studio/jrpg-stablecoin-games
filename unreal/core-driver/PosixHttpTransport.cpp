// No Python twin: the integration driver's blocking HTTP/1.1 transport over a POSIX socket (PosixHttpTransport.h).
#include "PosixHttpTransport.h"

#include "Scfg/Err.h"
#include "Scfg/Result.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace scfg_driver::http {

using scfg::Err;
using scfg::Json;
using scfg::Result;

// The longest status or header line read (http.client's _MAXLINE).
inline constexpr size_t MAX_LINE = 65536;
// At most this many headers (http.client's _MAXHEADERS).
inline constexpr size_t MAX_HEADERS = 100;
// Kept of a body: one byte over the cap, so parse_rpc_body answers "response too large".
inline constexpr size_t BODY_CAP = scfg::coio::MAX_RESPONSE_BYTES + 1;

Err transport_error(std::string message) { return Err::transport_error(std::move(message), std::nullopt); }

// Python's str(OSError): "[Errno 111] Connection refused".
std::string os_error(int code) { return "[Errno " + std::to_string(code) + "] " + std::strerror(code); }

// urllib's URLError around a failure before the request went out: "<urlopen error ...>".
Err urlopen_error(const std::string& reason) { return transport_error("<urlopen error " + reason + ">"); }

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

// A decimal (base 10) or hex (base 16) count; nullopt when s is empty, not all digits, or too large.
std::optional<size_t> parse_count(std::string_view s, unsigned base) {
    if (s.empty()) return std::nullopt;
    size_t n = 0;
    for (char c : s) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (base == 16 && ascii_lower(c) >= 'a' && ascii_lower(c) <= 'f') {
            digit = static_cast<unsigned>(ascii_lower(c) - 'a' + 10);
        } else {
            return std::nullopt;
        }
        if (n > (std::numeric_limits<size_t>::max() - digit) / base) return std::nullopt;
        n = n * base + digit;
    }
    return n;
}

struct Url {
    std::string host;       // without IPv6 brackets
    std::string port;       // "80" by default
    std::string authority;  // the Host header
    std::string target;     // path and query, "/" by default
};

// http://host[:port][/path][?query][#fragment]; anything else is urllib's error for it.
Result<Url> parse_url(std::string_view url) {
    const size_t colon = url.find(':');
    const std::string_view scheme = colon == std::string_view::npos ? std::string_view() : url.substr(0, colon);
    if (!iequals(scheme, "http") || url.substr(colon + 1, 2) != "//") {
        if (scheme.empty()) return transport_error("unknown url type: '" + std::string(url) + "'");
        return urlopen_error("unknown url type: " + std::string(scheme));
    }
    std::string_view rest = url.substr(colon + 3);
    const size_t fragment = rest.find('#');
    if (fragment != std::string_view::npos) rest = rest.substr(0, fragment);
    const size_t end = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, end);
    std::string target = end == std::string_view::npos ? "/" : std::string(rest.substr(end));
    if (!target.empty() && target.front() == '?') target.insert(target.begin(), '/');
    const size_t at = authority.rfind('@');
    if (at != std::string_view::npos) authority = authority.substr(at + 1);  // credentials are not sent

    Url u;
    u.authority = std::string(authority);
    u.target = std::move(target);
    std::string_view host = authority;
    std::string_view port;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) return transport_error("Invalid IPv6 URL");
        host = authority.substr(1, close - 1);
        std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return transport_error("Invalid IPv6 URL");
            port = after.substr(1);
        }
    } else if (const size_t sep = authority.rfind(':'); sep != std::string_view::npos) {
        host = authority.substr(0, sep);
        port = authority.substr(sep + 1);
    }
    if (host.empty()) return urlopen_error("no host given");
    if (!port.empty()) {
        std::optional<size_t> n = parse_count(port, 10);
        if (!n || *n > 65535) return transport_error("nonnumeric port: '" + std::string(port) + "'");
    }
    u.host = std::string(host);
    u.port = port.empty() ? "80" : std::string(port);
    return u;
}

// The request's deadline: the time left, for poll.
class Deadline {
public:
    explicit Deadline(double seconds) {
        if (std::isnan(seconds) || seconds <= 0) {
            at_ = std::chrono::steady_clock::now();
        } else if (seconds < 1e9) {
            at_ = std::chrono::steady_clock::now() +
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        } else {
            forever_ = true;
        }
    }
    // Milliseconds poll may wait (-1: no limit), or nullopt once the deadline has passed.
    std::optional<int> remaining_ms() const {
        if (forever_) return -1;
        const auto left = at_ - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) return std::nullopt;
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
        return static_cast<int>(std::min<int64_t>(ms, std::numeric_limits<int>::max()));
    }

private:
    std::chrono::steady_clock::time_point at_{};
    bool forever_ = false;
};

// Waits until fd is ready for events: true, false at the deadline, or an OSError.
Result<bool> wait_for(int fd, short events, const Deadline& deadline) {
    for (;;) {
        std::optional<int> ms = deadline.remaining_ms();
        if (!ms) return false;
        pollfd p{};
        p.fd = fd;
        p.events = events;
        const int rc = ::poll(&p, 1, *ms);
        if (rc > 0) return true;
        if (rc == 0) continue;  // the deadline check above decides
        if (errno == EINTR) continue;
        return transport_error(os_error(errno));
    }
}

// Closes its descriptor.
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Socket() { close(); }
    int fd() const { return fd_; }

private:
    void close() {
        if (fd_ >= 0) ::close(std::exchange(fd_, -1));
    }
    int fd_ = -1;
};

struct AddrInfo {
    addrinfo* list = nullptr;
    AddrInfo() = default;
    AddrInfo(const AddrInfo&) = delete;
    AddrInfo& operator=(const AddrInfo&) = delete;
    ~AddrInfo() {
        if (list) ::freeaddrinfo(list);
    }
};

// A connected non-blocking socket to the first address of host:port that answers (socket.create_connection).
Result<Socket> connect_to(const Url& u, const Deadline& deadline) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    AddrInfo found;
    const int rc = ::getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &found.list);
    if (rc != 0) return urlopen_error("[Errno " + std::to_string(rc) + "] " + ::gai_strerror(rc));
    int last = ECONNREFUSED;
    for (addrinfo* a = found.list; a != nullptr; a = a->ai_next) {
        Socket s(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
        if (s.fd() < 0) {
            last = errno;
            continue;
        }
        const int flags = ::fcntl(s.fd(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(s.fd(), F_SETFL, flags | O_NONBLOCK) < 0) {
            last = errno;
            continue;
        }
        if (::connect(s.fd(), a->ai_addr, a->ai_addrlen) == 0) return s;
        if (errno != EINPROGRESS && errno != EINTR) {
            last = errno;
            continue;
        }
        Result<bool> ready = wait_for(s.fd(), POLLOUT, deadline);
        if (!ready.ok()) return ready.err();
        if (!ready.value()) return urlopen_error("timed out");
        int so_error = 0;
        socklen_t len = sizeof so_error;
        if (::getsockopt(s.fd(), SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) so_error = errno;
        if (so_error == 0) return s;
        last = so_error;
    }
    return urlopen_error(os_error(last));
}

#ifdef MSG_NOSIGNAL
inline constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
inline constexpr int SEND_FLAGS = 0;  // main() ignores SIGPIPE
#endif

scfg::Status send_all(int fd, std::string_view data, const Deadline& deadline) {
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), SEND_FLAGS);
        if (n >= 0) {
            data.remove_prefix(static_cast<size_t>(n));
            continue;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return transport_error(os_error(errno));
        Result<bool> ready = wait_for(fd, POLLOUT, deadline);
        if (!ready.ok()) return ready.err();
        if (!ready.value()) return transport_error("timed out");
    }
    return scfg::ok();
}

// Buffered reads of the answer, each bounded by the deadline.
class Reader {
public:
    Reader(int fd, const Deadline& deadline) : fd_(fd), deadline_(deadline) {}

    // One line without its CRLF (or LF); nullopt at EOF before any byte of it.
    Result<std::optional<std::string>> line() {
        for (;;) {
            const size_t nl = buf_.find('\n', pos_);
            if (nl != std::string::npos) {
                std::string out = buf_.substr(pos_, nl - pos_);
                pos_ = nl + 1;
                if (!out.empty() && out.back() == '\r') out.pop_back();
                if (out.size() > MAX_LINE) return line_too_long();
                return std::optional<std::string>(std::move(out));
            }
            if (buf_.size() - pos_ > MAX_LINE) return line_too_long();
            SCFG_TRY(bool more, fill());
            if (!more) {
                if (pos_ == buf_.size()) return std::optional<std::string>();
                std::string out = buf_.substr(pos_);
                pos_ = buf_.size();
                return std::optional<std::string>(std::move(out));
            }
        }
    }

    // n bytes appended to out (at most up to cap in all); false when EOF came first.
    Result<bool> exact(size_t n, std::string& out, size_t cap) {
        while (n > 0) {
            if (pos_ == buf_.size()) {
                SCFG_TRY(bool more, fill());
                if (!more) return false;
            }
            const size_t take = std::min(n, buf_.size() - pos_);
            const size_t room = cap > out.size() ? cap - out.size() : 0;
            out.append(buf_, pos_, std::min(take, room));
            pos_ += take;
            n -= take;
            if (out.size() >= cap) return true;  // the rest is never read
        }
        return true;
    }

    // Everything up to EOF appended to out (at most up to cap in all).
    scfg::Status rest(std::string& out, size_t cap) {
        for (;;) {
            const size_t room = cap > out.size() ? cap - out.size() : 0;
            out.append(buf_, pos_, std::min(buf_.size() - pos_, room));
            pos_ = buf_.size();
            if (out.size() >= cap) return scfg::ok();
            SCFG_TRY(bool more, fill());
            if (!more) return scfg::ok();
        }
    }

private:
    static Err line_too_long() { return transport_error("got more than " + std::to_string(MAX_LINE) + " bytes when reading header line"); }

    // More bytes into the buffer: false at EOF.
    Result<bool> fill() {
        if (pos_ > 0 && pos_ == buf_.size()) {
            buf_.clear();
            pos_ = 0;
        } else if (pos_ > MAX_LINE) {
            buf_.erase(0, pos_);
            pos_ = 0;
        }
        char chunk[65536];
        for (;;) {
            const ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
            if (n > 0) {
                buf_.append(chunk, static_cast<size_t>(n));
                return true;
            }
            if (n == 0) return false;
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return transport_error(os_error(errno));
            SCFG_TRY(bool ready, wait_for(fd_, POLLIN, deadline_));
            if (!ready) return transport_error("timed out");
        }
    }

    int fd_;
    const Deadline& deadline_;
    std::string buf_;
    size_t pos_ = 0;
};

struct Answer {
    int status = 0;
    std::string body;
};

// http.client's IncompleteRead, as str() prints it.
Err incomplete_read(size_t got, std::optional<size_t> expected) {
    std::string m = "IncompleteRead(" + std::to_string(got) + " bytes read";
    if (expected) m += ", " + std::to_string(*expected) + " more expected";
    return transport_error(m + ")");
}

// A chunked body (RFC 9112 7.1), its trailers skipped.
scfg::Status read_chunked(Reader& r, std::string& body) {
    for (;;) {
        SCFG_TRY(std::optional<std::string> size_line, r.line());
        if (!size_line) return incomplete_read(body.size(), std::nullopt);
        std::string_view digits = *size_line;
        digits = trim(digits.substr(0, digits.find(';')));
        std::optional<size_t> size = parse_count(digits, 16);
        if (!size) return incomplete_read(body.size(), std::nullopt);
        if (*size == 0) break;
        const size_t before = body.size();
        SCFG_TRY(bool whole, r.exact(*size, body, BODY_CAP));
        if (!whole) return incomplete_read(body.size(), *size - (body.size() - before));
        if (body.size() >= BODY_CAP) return scfg::ok();
        SCFG_TRY(std::optional<std::string> crlf, r.line());
        if (!crlf || !crlf->empty()) return incomplete_read(body.size(), std::nullopt);
    }
    for (;;) {  // trailers, up to the blank line or EOF
        SCFG_TRY(std::optional<std::string> trailer, r.line());
        if (!trailer || trailer->empty()) return scfg::ok();
    }
}

// The status line, the headers and the body.
Result<Answer> read_answer(Reader& r) {
    Answer a;
    std::optional<size_t> length;
    bool chunked = false;
    for (;;) {  // 1xx interim answers come before the real one
        SCFG_TRY(std::optional<std::string> status_line, r.line());
        if (!status_line) return transport_error("Remote end closed connection without response");
        // "HTTP/1.1 200 OK"
        const std::string& s = *status_line;
        const size_t sp = s.find(' ');
        std::optional<size_t> code;
        if (s.rfind("HTTP/", 0) == 0 && sp != std::string::npos) code = parse_count(std::string_view(s).substr(sp + 1, 3), 10);
        if (!code || *code < 100 || *code > 999 || (s.size() > sp + 4 && s[sp + 4] != ' ')) {
            return transport_error("bad status line: " + s.substr(0, 200));
        }
        a.status = static_cast<int>(*code);
        size_t headers = 0;
        for (;;) {
            SCFG_TRY(std::optional<std::string> header, r.line());
            if (!header) return transport_error("Remote end closed connection without response");
            if (header->empty()) break;
            if (++headers > MAX_HEADERS) return transport_error("got more than " + std::to_string(MAX_HEADERS) + " headers");
            const size_t c = header->find(':');
            if (c == std::string::npos) continue;
            std::string_view name = trim(std::string_view(*header).substr(0, c));
            std::string_view value = trim(std::string_view(*header).substr(c + 1));
            if (iequals(name, "content-length")) {
                length = parse_count(value, 10);
                if (!length) return transport_error("invalid Content-Length: " + std::string(value));
            } else if (iequals(name, "transfer-encoding")) {
                const size_t last = value.rfind(',');
                chunked = iequals(trim(last == std::string_view::npos ? value : value.substr(last + 1)), "chunked");
            }
        }
        if (a.status >= 200) break;
        length.reset();
        chunked = false;
    }
    if (a.status == 204 || a.status == 304) return a;  // no body
    if (chunked) {
        SCFG_TRY_VOID(read_chunked(r, a.body));
    } else if (length) {
        SCFG_TRY(bool whole, r.exact(*length, a.body, BODY_CAP));
        if (!whole) return incomplete_read(a.body.size(), *length - a.body.size());
    } else {
        SCFG_TRY_VOID(r.rest(a.body, BODY_CAP));
    }
    return a;
}

Result<Json> post(const std::string& url, const Json& payload, double timeout) {
    SCFG_TRY(Url u, parse_url(url));
    SCFG_TRY(const std::string body, scfg::json::dumps(payload));
    std::string request = "POST " + u.target + " HTTP/1.1\r\n";
    request += "Host: " + u.authority + "\r\n";
    request += "User-Agent: scfg/0.1\r\n";
    request += "Accept-Encoding: identity\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;
    const Deadline deadline(timeout);
    SCFG_TRY(Socket s, connect_to(u, deadline));
    SCFG_TRY_VOID(send_all(s.fd(), request, deadline));
    Reader reader(s.fd(), deadline);
    SCFG_TRY(Answer a, read_answer(reader));
    return scfg::coio::parse_rpc_body(a.status, a.body);
}

}  // namespace scfg_driver::http

namespace scfg_driver {

scfg::Future<scfg::Json> PosixHttpTransport::post_json(std::string url, scfg::Json payload, double timeout) {
    ++posts_;
    return scfg::Future<scfg::Json>::completed(http::post(url, payload, timeout));
}

}  // namespace scfg_driver
