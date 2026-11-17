// Twin of scfg_core/local_server.py (CheckoutServer and its Handler), ported from godot/addons/scfg/local_server.gd's
// connection state machine. See LocalServer.h.
#include "Scfg/LocalServer.h"

#include "Scfg/BigInt.h"
#include "Scfg/Config.h"
#include "Scfg/Ints.h"
#include "Scfg/Py.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scfg::local_server {

namespace detail {

// A build the connection waits for: done (which holds only a weak reference) fills it.
struct BuildSlot {
    std::optional<Result<Json>> result;
};

std::string_view reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 413: return "Request Entity Too Large";
        case 414: return "Request-URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

// A byte of the head as Python's str(b, "iso-8859-1") holds it: its code point is its value.
bool space(char c) { return py::is_space(static_cast<unsigned char>(c)); }

// Latin-1 bytes as the UTF-8 text of the same code points (for int(), which reads a str).
std::string latin1_to_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) py::utf8_encode(static_cast<unsigned char>(c), out);
    return out;
}

// Python's str.split() with no argument (str.isspace() of each Latin-1 character).
std::vector<std::string> py_split(std::string_view s) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && space(s[i])) ++i;
        const size_t start = i;
        while (i < s.size() && !space(s[i])) ++i;
        if (i > start) words.emplace_back(s.substr(start, i - start));
    }
    return words;
}

// Python's str.split(sep) with a one-character separator (empty pieces kept).
std::vector<std::string> split_on(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t at = s.find(sep, start);
        if (at == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            return out;
        }
        out.emplace_back(s.substr(start, at - start));
        start = at + 1;
    }
}

bool ascii_digits(std::string_view s) {
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::string rstrip_line_ends(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

std::string lstrip_blanks(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return std::string(s.substr(i));
}

std::string ascii_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// A header name as email's headerRE takes it: printable ASCII but the colon (the colon is never in it here).
bool header_name(std::string_view s) {
    for (char c : s) {
        const unsigned char b = static_cast<unsigned char>(c);
        if (b < 0x21 || b > 0x7e) return false;
    }
    return true;
}

// The header lines (each read up to an LF, the LF removed) split after each \r\n, \r or \n of the block they make,
// line ends kept (feedparser's universal newlines).
std::vector<std::string> universal_lines(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    for (const std::string& line : lines) {
        size_t start = 0;
        for (;;) {
            const size_t cr = line.find('\r', start);
            if (cr == std::string::npos || cr == line.size() - 1) {
                out.push_back(line.substr(start) + "\n");
                break;
            }
            out.push_back(line.substr(start, cr + 1 - start));
            start = cr + 1;
        }
    }
    return out;
}

// Adds the header `parts` hold (its first line, then its continuation lines) unless its name is already in `out`
// (compat32's header_source_parse; Message.get returns the first).
void finish_header(std::map<std::string, std::string>& out, const std::vector<std::string>& parts) {
    if (parts.empty()) return;
    const std::string& first = parts[0];
    const size_t colon = first.find(':');
    std::string name = ascii_lower(std::string_view(first).substr(0, colon));
    if (out.count(name) != 0) return;
    std::string value = lstrip_blanks(std::string_view(first).substr(colon + 1));
    for (size_t i = 1; i < parts.size(); ++i) value += parts[i];
    out.emplace(std::move(name), rstrip_line_ends(std::move(value)));
}

// The header block as email.parser (compat32) reads it: {lowercase name: first value}. The first non-header line ends
// the block; a value loses leading blanks and trailing \r\n, not trailing spaces; a continuation line is appended line
// ends and all (so a folded value matches nothing); an empty name or an envelope `From ` line is skipped, and so are
// the continuation lines after one.
std::map<std::string, std::string> parse_headers(const std::vector<std::string>& lines) {
    std::map<std::string, std::string> out;
    std::vector<std::string> parts;
    for (const std::string& line : universal_lines(lines)) {
        if (starts_with(line, " ") || starts_with(line, "\t")) {
            if (!parts.empty()) parts.push_back(line);
            continue;
        }
        const size_t colon = line.find(':');
        const bool from = starts_with(line, "From ");
        if (!from && (colon == std::string::npos || !header_name(std::string_view(line).substr(0, colon)))) break;
        finish_header(out, parts);
        parts.clear();
        if (!from && colon > 0) parts.push_back(line);
    }
    finish_header(out, parts);
    return out;
}

bool raster(std::string_view content_type) {
    for (const auto& [ext, type] : config::LOGO_TYPES) {
        if (type == content_type) return true;
    }
    return false;
}

enum class State { head, body, building, writing, linger, closed };

}  // namespace detail

// --- the connection -------------------------------------------------------------------------------------------------

using detail::State;

struct Handler::Connection {
    State state = State::head;
    double deadline = 0;
    std::string buf;
    size_t line_start = 0;           // where the line being read starts in buf
    std::vector<std::string> lines;  // the request line, then the header lines (Latin-1 bytes, the LF removed)
    std::string method;
    std::string path;
    std::string sub;  // the path after /c/<secret>/
    size_t need = 0;  // body bytes to read
    std::shared_ptr<detail::BuildSlot> build;
    CancelBuild cancel;
    std::string out;
    size_t sent = 0;
    size_t drained = 0;
    bool complete = false;  // the whole request was read (nothing more is expected from the client)
};

// Marks a callback running: release() waits until it returns.
struct Handler::Busy {
    explicit Busy(Handler& h) : handler(h) { ++handler.busy_; }
    ~Busy() { --handler.busy_; }
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;
    Handler& handler;
};

// --- Handler --------------------------------------------------------------------------------------------------------

Result<std::unique_ptr<Handler>> Handler::make(Options options) {
    if (!options.clock) return Err::type_error("local_server: a clock is required");
    return std::unique_ptr<Handler>(new Handler(std::move(options)));
}

Handler::Handler(Options options) : options_(std::move(options)) {
    Entropy* entropy = options_.entropy;
    if (entropy == nullptr) {
        own_entropy_ = std::make_unique<SystemEntropy>();
        entropy = own_entropy_.get();
    }
    secret_ = entropy->token_urlsafe(16);
}

Handler::~Handler() = default;

std::string Handler::url() const {
    return "http://" + options_.host + ":" + py::dec(port_) + "/c/" + secret_ + "/";
}

double Handler::now() const { return options_.clock(); }

Handler::Connection* Handler::find(Conn conn) {
    auto it = conns_.find(conn);
    return it == conns_.end() ? nullptr : it->second.get();
}

const Handler::Connection* Handler::find(Conn conn) const {
    auto it = conns_.find(conn);
    return it == conns_.end() ? nullptr : it->second.get();
}

bool Handler::full() const { return open_count() >= MAX_CONNECTIONS; }

Handler::Conn Handler::open() {
    const Conn id = next_++;
    auto c = std::make_unique<Connection>();
    c->deadline = now() + DEADLINE_S;
    conns_.emplace(id, std::move(c));
    return id;
}

Handler::Io Handler::io(Conn conn) const {
    Io io;
    const Connection* c = find(conn);
    if (c == nullptr || c->state == State::closed) {
        io.close = true;
        return io;
    }
    switch (c->state) {
        case State::head: io.read = c->buf.size() <= MAX_HEAD ? MAX_HEAD + 1 - c->buf.size() : 0; break;
        case State::body: io.read = c->need > c->buf.size() ? c->need - c->buf.size() : 0; break;
        case State::writing: io.write = std::string_view(c->out).substr(c->sent); break;
        case State::linger: io.read = LINGER_MAX_BYTES - c->drained; break;
        case State::building:
        case State::closed: break;
    }
    return io;
}

void Handler::feed(Conn conn, std::string_view bytes) {
    Connection* c = find(conn);
    if (c == nullptr) return;
    const Busy busy(*this);
    switch (c->state) {
        case State::head:
        case State::body:
            if (bytes.empty()) return;
            c->buf.append(bytes);
            if (c->state == State::head) read_head(*c);
            if (c->state == State::body && c->buf.size() >= c->need) on_body(*c);
            return;
        case State::linger:
            if (bytes.empty()) {
                // The answer is out and the socket holds nothing more: a complete request closes now.
                if (c->complete && c->drained == 0) close(*c);
                return;
            }
            c->drained += bytes.size();
            if (c->drained >= LINGER_MAX_BYTES) close(*c);
            return;
        case State::building:
        case State::writing:
        case State::closed: return;
    }
}

void Handler::hang_up(Conn conn) {
    Connection* c = find(conn);
    if (c == nullptr) return;
    const Busy busy(*this);
    close(*c);
}

void Handler::sent(Conn conn, size_t n) {
    Connection* c = find(conn);
    if (c == nullptr || c->state != State::writing) return;
    c->sent = std::min(c->out.size(), c->sent + n);
    if (c->sent < c->out.size()) return;
    c->out.clear();
    c->sent = 0;
    // Read and discard what the client still sends until it closes (or LINGER_S or LINGER_MAX_BYTES pass), so
    // closing does not reset the connection under the answer.
    c->state = State::linger;
    c->deadline = std::min(c->deadline, now() + LINGER_S);
}

void Handler::pump() {
    const Busy busy(*this);
    const double t = now();
    for (Conn id : connections()) {
        Connection* c = find(id);
        if (c == nullptr) continue;
        switch (c->state) {
            case State::head:
            case State::body:
            case State::writing:
            case State::linger:
                if (t >= c->deadline) close(*c);
                break;
            case State::building: check_build(*c); break;
            case State::closed: break;
        }
    }
}

void Handler::release(Conn conn) {
    if (busy_ > 0) return;
    auto it = conns_.find(conn);
    if (it != conns_.end() && it->second->state == State::closed) conns_.erase(it);
}

void Handler::release_closed() {
    if (busy_ > 0) return;
    for (auto it = conns_.begin(); it != conns_.end();) {
        it = it->second->state == State::closed ? conns_.erase(it) : std::next(it);
    }
}

void Handler::close_all() {
    const Busy busy(*this);
    for (Conn id : connections()) {
        if (Connection* c = find(id)) close(*c);
    }
}

std::vector<Handler::Conn> Handler::connections() const {
    std::vector<Conn> out;
    out.reserve(conns_.size());
    for (const auto& [id, c] : conns_) out.push_back(id);
    return out;
}

size_t Handler::open_count() const {
    size_t n = 0;
    for (const auto& [id, c] : conns_) n += c->state != State::closed ? 1 : 0;
    return n;
}

std::string_view Handler::state(Conn conn) const {
    const Connection* c = find(conn);
    if (c == nullptr) return "closed";
    switch (c->state) {
        case State::head: return "head";
        case State::body: return "body";
        case State::building: return "building";
        case State::writing: return "writing";
        case State::linger: return "linger";
        case State::closed: return "closed";
    }
    return "closed";
}

// Splits off the complete lines received so far: the request line, then headers up to the blank line. Answers a bad
// request line at once, as http.server does before reading the headers.
void Handler::read_head(Connection& c) {
    while (c.state == State::head) {
        const size_t nl = c.buf.find('\n', c.line_start);
        const size_t line_size = (nl != std::string::npos ? nl + 1 : c.buf.size()) - c.line_start;  // with its LF
        if (line_size > MAX_LINE) {
            if (c.lines.empty()) {
                send(c, 414, "request line too long");
            } else {
                send(c, 431, "line too long");
            }
            return;
        }
        if (nl == std::string::npos) {
            if (c.buf.size() > MAX_HEAD) send(c, 431, "headers too large");
            return;
        }
        std::string raw = c.buf.substr(c.line_start, nl - c.line_start);
        c.line_start = nl + 1;
        if (c.lines.empty()) {
            request_line(c, detail::rstrip_line_ends(std::move(raw)));
        } else if (c.lines.size() > MAX_HEADERS) {
            // MAX_HEADERS header lines are in: this line, a header or the blank end, is one more
            send(c, 431, "too many headers");
        } else if (raw.empty() || raw == "\r") {
            c.buf.erase(0, c.line_start);
            c.line_start = 0;
            on_head(c);
        } else {
            c.lines.push_back(std::move(raw));
        }
    }
}

// BaseHTTPRequestHandler.parse_request's checks of the request line.
void Handler::request_line(Connection& c, std::string line) {
    const std::vector<std::string> words = detail::py_split(line);
    if (words.empty()) {
        close(c);  // http.server closes without an answer
        return;
    }
    if (words.size() >= 3) {
        const std::string& version = words.back();
        std::vector<std::string> numbers;
        if (detail::starts_with(version, "HTTP/")) numbers = detail::split_on(std::string_view(version).substr(5), '.');
        bool valid = numbers.size() == 2;
        for (const std::string& n : numbers) valid = valid && !n.empty() && n.size() <= 10 && detail::ascii_digits(n);
        if (!valid) {
            send(c, 400, "bad request version");
            return;
        }
        // At most 10 ASCII digits: the first number fits an int64, and >= 2 is HTTP/2.0 or later.
        int64_t major = 0;
        for (char d : numbers[0]) major = major * 10 + (d - '0');
        if (major >= 2) {
            send(c, 505, "invalid http version");
            return;
        }
    }
    if (words.size() != 3) {
        send(c, 400, "bad request syntax");
        return;
    }
    c.method = words[0];
    c.path = words[1];
    if (detail::starts_with(c.path, "//")) {
        // http.server's gh-87389 fix: a path starting with // is reduced to a single /
        const size_t first = c.path.find_first_not_of('/');
        c.path = "/" + (first == std::string::npos ? std::string() : c.path.substr(first));
    }
    c.lines.push_back(std::move(line));
}

void Handler::on_head(Connection& c) {
    const std::map<std::string, std::string> headers =
        detail::parse_headers(std::vector<std::string>(c.lines.begin() + 1, c.lines.end()));
    c.complete = headers.count("content-length") == 0 && headers.count("transfer-encoding") == 0;
    if (c.method != "GET" && c.method != "POST") {
        send(c, 501, "unsupported method", "text/plain; charset=utf-8", c.method == "HEAD");
        return;
    }
    const auto host = headers.find("host");
    if (host == headers.end() || host->second != options_.host + ":" + py::dec(port_)) {
        send(c, 403, "forbidden");
        return;
    }
    const std::string prefix = "/c/" + secret_ + "/";
    const std::string path = c.path.substr(0, c.path.find('?'));
    if (!detail::starts_with(path, prefix)) {
        not_found(c);
        return;
    }
    c.sub = path.substr(prefix.size());
    if (c.method == "GET") {
        serve_get(c);
        return;
    }
    if ((c.sub == "build" && !options_.build_tx) || (c.sub != "build" && c.sub != "tx")) {
        not_found(c);
        return;
    }
    // int_from(self.headers.get("Content-Length", "0")): a ValueError is -1 (400), a huge int more than MAX_BODY
    const auto declared = headers.find("content-length");
    const std::string text = declared == headers.end() ? std::string("0") : detail::latin1_to_utf8(declared->second);
    int64_t length = -1;
    const Result<BigInt> parsed = ints::int_from(Json(text));
    if (parsed.ok()) {
        const BigInt& n = parsed.value();
        if (n > BigInt(static_cast<int64_t>(MAX_BODY))) {
            length = static_cast<int64_t>(MAX_BODY) + 1;
        } else if (n > BigInt(0)) {
            const Result<int64_t> fits = n.to_int64();
            length = fits.ok() ? fits.value() : -1;
        } else {
            length = 0;
        }
    }
    if (length <= 0 || length > static_cast<int64_t>(MAX_BODY)) {
        send(c, length > static_cast<int64_t>(MAX_BODY) ? 413 : 400, "bad body");
        return;
    }
    c.need = static_cast<size_t>(length);
    c.state = State::body;
    if (c.buf.size() >= c.need) on_body(c);
}

void Handler::serve_get(Connection& c) {
    const std::string sub = c.sub.empty() ? std::string("index.html") : c.sub;
    if (std::find(ASSETS.begin(), ASSETS.end(), sub) != ASSETS.end()) {
        std::optional<std::string> body = options_.load_asset ? options_.load_asset(sub) : std::nullopt;
        if (body) {
            send(c, 200, std::move(*body), asset_content_type(sub));
        } else {
            send(c, 500, "asset missing");
        }
    } else if (sub == "session.json") {
        send_json(c, 200, options_.session_info);
    } else if (sub == "status") {
        if (options_.get_status) {
            send_json(c, 200, options_.get_status());
        } else {
            unavailable(c);
        }
    } else if (sub == "logo" && options_.load_logo) {
        std::optional<Logo> logo = options_.load_logo();
        if (logo && detail::raster(logo->content_type)) {
            const std::string content_type = logo->content_type;
            send(c, 200, std::move(logo->bytes), content_type);
        } else {
            not_found(c);
        }
    } else {
        not_found(c);
    }
}

void Handler::on_body(Connection& c) {
    c.complete = c.buf.size() == c.need;
    // json.loads(body.decode("utf-8")): strict UTF-8 (a BOM refused); anything but a dict is a 400
    const Result<Json> parsed = json::loads(std::string_view(c.buf).substr(0, c.need));
    if (!parsed.ok() || !parsed.value().is_dict()) {
        send(c, 400, "bad body");
        return;
    }
    const Json& data = parsed.value();
    if (c.sub == "build") {
        const Result<std::string> account = page_messages::parse_build_message(data);
        if (!account.ok()) {
            send(c, 400, "bad account");
            return;
        }
        auto slot = std::make_shared<detail::BuildSlot>();
        const std::weak_ptr<detail::BuildSlot> weak = slot;
        c.build = slot;
        Result<CancelBuild> started = options_.build_tx(account.value(), [weak](Result<Json> r) {
            if (const std::shared_ptr<detail::BuildSlot> s = weak.lock()) {
                if (!s->result) s->result.emplace(std::move(r));
            }
        });
        if (c.state == State::closed) return;  // build_tx stopped the server
        if (!started.ok()) {  // worker busy, RPC down: the page offers a retry
            c.build.reset();
            unavailable(c);
            return;
        }
        c.cancel = started.take();
        c.state = State::building;
        c.buf.clear();
        c.deadline = now() + options_.build_timeout_s;
        check_build(c);
        return;
    }
    const Result<page_messages::TxMessage> hint = page_messages::parse_tx_message(data);
    if (!hint.ok()) {
        send(c, 400, "bad tx hash");
        return;
    }
    if (!options_.on_tx) {
        unavailable(c);
        return;
    }
    options_.on_tx(hint.value().tx_hash, hint.value().sender);
    if (c.state != State::closed) send_json(c, 202, Json::dict({{"ok", true}}));  // (on_tx may have stopped the server)
}

// The held /build connection: answered once its build is done, 503 (and the build cancelled) after build_timeout_s. A
// failed build, or a result that is not a dict, is a 503.
void Handler::check_build(Connection& c) {
    if (c.build && c.build->result) {
        const Result<Json> r = *c.build->result;
        c.build.reset();
        c.cancel = nullptr;
        if (r.ok() && r.value().is_dict()) {
            send_json(c, 200, r.value());
        } else {
            unavailable(c);
        }
    } else if (now() >= c.deadline) {
        const CancelBuild cancel = std::move(c.cancel);
        c.cancel = nullptr;
        c.build.reset();
        if (cancel) cancel();  // don't build it later for a page that stopped waiting
        unavailable(c);
    }
}

void Handler::send(Connection& c, int code, std::string body, std::string_view content_type, bool head_only) {
    if (c.state == State::closed) return;
    std::string out = "HTTP/1.1 " + py::dec(code) + " " + std::string(detail::reason(code)) +
                      "\r\nContent-Type: " + std::string(content_type) + "\r\nContent-Length: " + py::dec(body.size()) +
                      "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: "
                      "nosniff\r\nConnection: close\r\n\r\n";
    if (!head_only) out += body;
    c.out = std::move(out);
    c.sent = 0;
    c.buf.clear();
    c.line_start = 0;
    c.state = State::writing;
    c.deadline = now() + DEADLINE_S;
}

void Handler::send_json(Connection& c, int code, const Json& value) {
    Result<std::string> text = json::dumps(value);
    if (!text.ok()) {  // an int of more than 4300 digits: Python's json.dumps raises
        unavailable(c);
        return;
    }
    send(c, code, text.take(), "application/json");
}

void Handler::not_found(Connection& c) { send(c, 404, "not found"); }

void Handler::unavailable(Connection& c) {
    send(c, 503, "{\"error\": \"unavailable\"}", "application/json");
}

void Handler::close(Connection& c) {
    if (c.state == State::closed) return;
    const bool building = c.state == State::building;
    c.state = State::closed;
    c.buf.clear();
    c.out.clear();
    c.build.reset();
    const CancelBuild cancel = std::move(c.cancel);
    c.cancel = nullptr;
    if (building && cancel) cancel();  // a page that is gone gets no build
}

// --- CheckoutServer -------------------------------------------------------------------------------------------------

Result<std::unique_ptr<CheckoutServer>> CheckoutServer::make(Options options) {
    if (!options.listen) return Err::type_error("local_server: a listen function is required");
    Options handler_options = options;
    SCFG_TRY(std::unique_ptr<Handler> handler, Handler::make(std::move(handler_options)));
    return std::unique_ptr<CheckoutServer>(new CheckoutServer(std::move(options), std::move(handler)));
}

CheckoutServer::CheckoutServer(Options options, std::unique_ptr<Handler> handler)
    : options_(std::move(options)), handler_(std::move(handler)) {}

CheckoutServer::~CheckoutServer() { stop(); }

Status CheckoutServer::start() {
    if (listener_ != nullptr) return ok();
    Result<std::unique_ptr<Listener>> made = options_.listen(options_.host);
    if (made.ok() && made.value() == nullptr) {
        made = Err::make("RuntimeError", "cannot listen on " + options_.host);
    }
    if (!made.ok()) {
        if (options_.on_error) options_.on_error(made.err());
        return made.err();
    }
    handler_->release_closed();
    listener_ = made.take();
    handler_->set_port(listener_->port());
    return ok();
}

void CheckoutServer::stop() {
    if (listener_ == nullptr) return;
    listener_.reset();       // stops listening
    handler_->close_all();   // cancels the builds the connections wait for
    sockets_.clear();        // closes every connection
    handler_->release_closed();
}

std::string CheckoutServer::url() const { return handler_->url(); }

void CheckoutServer::pump() {
    if (listener_ == nullptr) return;
    handler_->pump();  // deadlines, and the builds that finished
    while (listener_ != nullptr && !handler_->full()) {
        std::unique_ptr<Socket> socket = listener_->accept();
        if (socket == nullptr) break;
        sockets_.emplace(handler_->open(), std::move(socket));
    }
    for (Handler::Conn id : handler_->connections()) {
        if (listener_ == nullptr) break;  // a callback stopped the server: stop() closed everything
        step(id);
    }
    handler_->release_closed();  // (what a stop() inside a callback closed, too)
}

// One connection's frame: read what is there (up to what its state takes), write what the socket takes, close.
void CheckoutServer::step(Handler::Conn id) {
    for (size_t i = 0; i < READS_PER_PUMP; ++i) {
        if (listener_ == nullptr) return;
        const auto it = sockets_.find(id);
        if (it == sockets_.end()) return;
        const size_t want = handler_->io(id).read;
        if (want == 0) break;
        std::string chunk;
        const Socket::Read got = it->second->read(std::min(want, READ_CHUNK), chunk);
        if (got == Socket::Read::closed) {
            handler_->hang_up(id);
            break;
        }
        if (got == Socket::Read::nothing) chunk.clear();
        handler_->feed(id, chunk);
        if (chunk.empty()) break;
    }
    for (size_t i = 0; i < WRITE_CHUNKS; ++i) {
        if (listener_ == nullptr) return;
        const auto it = sockets_.find(id);
        if (it == sockets_.end()) return;
        const std::string_view out = handler_->io(id).write;
        if (out.empty()) break;
        const std::string_view piece = out.substr(0, WRITE_CHUNK);
        const std::optional<size_t> n = it->second->write(piece);
        if (!n) {
            handler_->hang_up(id);
            break;
        }
        handler_->sent(id, *n);
        if (*n < piece.size()) break;  // the socket is full: the rest waits for the next frame
    }
    if (listener_ != nullptr && handler_->io(id).close) {
        sockets_.erase(id);
        handler_->release(id);
    }
}

// --- the controller's factory ---------------------------------------------------------------------------------------

checkout::ServerFactory factory(Options page) {
    return [page = std::move(page)](checkout::ServerArgs args) -> Result<std::unique_ptr<checkout::Server>> {
        Options o = page;
        o.session_info = std::move(args.session_info);
        o.on_tx = std::move(args.on_tx);
        o.get_status = std::move(args.get_status);
        o.build_tx = nullptr;
        if (args.build_tx) {
            o.build_tx = [build = std::move(args.build_tx)](std::string account,
                                                           worker::OnDone<Json> done) -> Result<CancelBuild> {
                build(Json(std::move(account)), std::move(done));
                return CancelBuild();  // a submitted command cannot be withdrawn
            };
        }
        Result<std::unique_ptr<CheckoutServer>> made = CheckoutServer::make(std::move(o));
        if (!made.ok()) {
            if (page.on_error) page.on_error(made.err());
            return made.err();
        }
        return std::unique_ptr<checkout::Server>(made.take());
    };
}

}  // namespace scfg::local_server
