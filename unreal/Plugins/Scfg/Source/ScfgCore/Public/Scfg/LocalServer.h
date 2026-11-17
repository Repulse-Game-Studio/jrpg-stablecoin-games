// Twin of scfg_core/local_server.py (CheckoutServer), with godot/addons/scfg/local_server.gd as the template for
// serving HTTP without Python's http.server: the localhost checkout server for desktop browser wallets.
//
// Browser extensions (MetaMask, Rabby, Phantom, ...) only talk to web pages, never to a game. So the game serves the
// bundled checkout page on 127.0.0.1 and opens the browser on it; the page connects the wallet, sends the transfer and
// posts the tx hash back here.
//
// Security (as local_server.py):
// - bound to 127.0.0.1 (Options::host) on a random port;
// - every URL holds a 128-bit secret, /c/<secret>/..., otherwise 404 (Entropy::token_urlsafe(16), as Python's
//   secrets.token_urlsafe(16));
// - the Host header must be exactly 127.0.0.1:<port> (defeats DNS rebinding), otherwise 403;
// - no CORS headers; a request body is at most MAX_BODY bytes (413), with a valid Content-Length (400);
// - every answer has Cache-Control: no-store, Referrer-Policy: no-referrer and X-Content-Type-Options: nosniff;
// - /build returns the *unsigned* transaction only for a valid account, and gives up after build_timeout_s (503);
// - a posted tx hash is only a hint: the watcher verifies it on-chain;
// - the logo is served only as a raster image (config::LOGO_TYPES: png, jpeg, webp), never SVG.
// As local_server.gd (Python has no such limits): a connection has DEADLINE_S to deliver its request and again to take
// its answer, at most MAX_CONNECTIONS are open at once, and a request head is at most MAX_HEAD bytes in all (431).
//
// HTTP, as Python's BaseHTTPRequestHandler reads it (local_server.gd's rules, which pass the shared suite
// tests/server_security): GET and POST only (anything else, HEAD included, is 501; HEAD gets no body); an empty request
// line is closed without an answer, a bad version is 400 and HTTP/2+ is 505; a request line longer than MAX_LINE is
// 414, a header line longer is 431, and so are more than MAX_HEADERS - 1 header lines; the header block is read as
// email.parser (compat32) reads it: a bare CR inside a line ends a header, the first of a repeated header counts, a
// value keeps its trailing blanks; a leading // becomes /; the query string is ignored; Content-Length is read with
// Python's int() (ints::int_from). Every answer is HTTP/1.1 with Connection: close.
// Divergences from http.server (as local_server.gd): a two-word (HTTP/0.9) request line is a 400, a bad request line
// gets a status line (http.server sends a bare error page), the 501's body is plain text, an Expect: 100-continue is
// not answered, and a /build result that is not a dict is a 503 (Python serves any JSON value). Python's fut.cancel()
// is the build's CancelBuild, which may be empty: the CheckoutController's command cannot be withdrawn once
// submitted, so a page that stopped waiting gets nothing and the result is dropped when it comes.
//
// The core does no IO. Handler is the HTTP logic for any number of connections, socket-agnostic: a connection is an
// id, the bytes read from it are fed in, and io() says what to do next (read at most n bytes, write these bytes, or
// close it); the clock is injected for the deadlines and the build timeout. CheckoutServer is Python's class around
// it: checkout::Server (the CheckoutController's page server) over a Listener and its Sockets, which the platform
// supplies (the glue's FSocket, the console driver's POSIX sockets), pumped every frame without ever waiting on a
// socket. The page's files come in through load_asset (the glue reads the plugin's Resources/Web/), so nothing here
// touches a file either.
//
// Callbacks (load_asset, on_tx, get_status, load_logo, build_tx and its CancelBuild) run inside feed() and pump(), on
// the caller's thread. A callback may stop the server (stop()), but must not destroy it or call pump().
#pragma once

#include "Scfg/Checkout.h"
#include "Scfg/Entropy.h"
#include "Scfg/Err.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/PageMessages.h"
#include "Scfg/Result.h"
#include "Scfg/Submitter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scfg::local_server {

// The largest request body read, in bytes (Python's MAX_BODY).
inline constexpr size_t MAX_BODY = 4096;
// The page's files (Python's ASSETS): GET /c/<secret>/ serves index.html.
inline constexpr std::array<std::string_view, 4> ASSETS = {"index.html", "app.js", "evm.js", "solana.js"};
// Seconds a connection has to send its whole request, and again to take its answer.
inline constexpr double DEADLINE_S = 10.0;
// One line of the head in bytes, with its line end (http.server's readline(65537)): longer is a 414 for the request
// line, a 431 for a header line. A browser may send 127.0.0.1 a Cookie this large.
inline constexpr size_t MAX_LINE = 65536;
// The request line plus headers in all, in bytes; more is a 431 (Python has no total cap, only MAX_LINE per line and
// MAX_HEADERS lines).
inline constexpr size_t MAX_HEAD = 262144;
// http.client's _MAXHEADERS. It counts the blank line that ends the head as one of them, so a request has at most
// MAX_HEADERS - 1 header lines; one more is a 431.
inline constexpr size_t MAX_HEADERS = 100;
inline constexpr size_t MAX_CONNECTIONS = 32;
// After answering a request not read in full: how long, and how much, of the rest is drained before closing. Closing
// on unread input resets the connection, and the client could lose the answer.
inline constexpr double LINGER_S = 1.0;
inline constexpr size_t LINGER_MAX_BYTES = 65536;
// CheckoutServer::pump() reads a connection at most READS_PER_PUMP times READ_CHUNK bytes, and writes it at most
// WRITE_CHUNKS times WRITE_CHUNK bytes, per frame.
inline constexpr size_t READ_CHUNK = 65536;
inline constexpr size_t READS_PER_PUMP = 16;
inline constexpr size_t WRITE_CHUNK = 65536;
inline constexpr size_t WRITE_CHUNKS = 16;
// The listen backlog a Listener should use (connections beyond MAX_CONNECTIONS wait in it).
inline constexpr int LISTEN_BACKLOG = 16;

// The content type an asset is served with (mimetypes' guess, with the charset local_server.py adds), or "" for a name
// that is not one of ASSETS.
inline constexpr std::string_view asset_content_type(std::string_view name) {
    if (name == "index.html") return "text/html; charset=utf-8";
    if (name == "app.js" || name == "evm.js" || name == "solana.js") return "application/javascript; charset=utf-8";
    return "";
}

// What load_logo returns: the image's bytes and its content type (served only when it is one of config::LOGO_TYPES).
struct Logo {
    std::string bytes;
    std::string content_type;
};

// Python's fut.cancel() for a build the page stopped waiting for (or that the server dropped). May be empty.
using CancelBuild = std::function<void()>;
// build_tx(account, done): build the unsigned Solana transfer the (normalized) account signs. done gets the result once,
// now (inside the call) or later; it holds only weak state, so it may run after the server is gone. An Err is Python's
// raise: the page gets a 503 at once.
using BuildTx = std::function<Result<CancelBuild>(std::string account, worker::OnDone<Json> done)>;

// One accepted connection, non-blocking, as the platform's socket. Destroying it closes it.
class Socket {
public:
    enum class Read {
        data,     // bytes were appended to out
        nothing,  // no byte is there now (the call would block)
        closed,   // the peer closed its side, or the connection failed
    };
    virtual ~Socket() = default;
    // Appends at most max bytes that are there now to out, without waiting.
    virtual Read read(size_t max, std::string& out) = 0;
    // Sends what the socket takes now of data, without waiting: the number of bytes taken (0 when it is full), or
    // nullopt when the connection failed.
    virtual std::optional<size_t> write(std::string_view data) = 0;
};

// A listening socket, non-blocking. Destroying it stops listening.
class Listener {
public:
    virtual ~Listener() = default;
    // The port it is bound to.
    virtual int port() const = 0;
    // A connection that is waiting, or null when none is (never waits).
    virtual std::unique_ptr<Socket> accept() = 0;
};

// Binds host on a random free port and listens (LISTEN_BACKLOG). An Err (RuntimeError "cannot listen on <host>: ...")
// when it cannot.
using Listen = std::function<Result<std::unique_ptr<Listener>>(const std::string& host)>;

// CheckoutServer's keyword arguments (Python's), plus what the core does not do itself.
struct Options {
    // load_asset(name) -> the bytes of one of ASSETS, or nullopt (a 500, "asset missing"). Empty: every asset is a 500.
    std::function<std::optional<std::string>(const std::string& name)> load_asset;
    // Served as session.json.
    Json session_info = Json::dict();
    // on_tx(tx_hash, sender): the page's hint, normalized (page_messages::parse_tx_message). Empty: a 503.
    std::function<void(std::string tx_hash, std::optional<std::string> sender)> on_tx;
    // Served as status. Empty: a 503.
    std::function<Json()> get_status;
    // Optional: no logo route without it (404). nullopt (the file cannot be read) is a 404 too.
    std::function<std::optional<Logo>()> load_logo;
    // Optional (Solana): no /build route without it (404).
    BuildTx build_tx;
    std::string host = "127.0.0.1";
    // How long /build waits for its build (Python's module constant BUILD_TIMEOUT_S, which its tests lower).
    double build_timeout_s = static_cast<double>(page_messages::BUILD_TIMEOUT_S);
    // Seconds, monotonic. Required.
    std::function<double()> clock;
    // The secret's source; null: a SystemEntropy of the server's own. A given one must outlive the server.
    Entropy* entropy = nullptr;
    // CheckoutServer only (Handler ignores it): how start() listens. Required there.
    Listen listen;
    // CheckoutServer and factory() only: told each Err that start() or the factory returns (Checkout.h: the page
    // server reports its own Err, which the controller drops). Empty: not reported.
    std::function<void(const Err&)> on_error;
};

// The HTTP side of the server for any number of connections (Python's Handler class, without its socket).
class SCFGCORE_API Handler {
public:
    using Conn = uint64_t;
    // What to do next with a connection (Python's handler reading or writing its socket).
    struct Io {
        size_t read = 0;         // read at most this many bytes that are there now and feed() them (0: do not read)
        std::string_view write;  // send these bytes (as many as the socket takes) and tell sent() how many went
        bool close = false;      // close the socket and release() the id
    };

    // A TypeError when clock is empty.
    static Result<std::unique_ptr<Handler>> make(Options options);
    ~Handler();
    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;

    // 16 random bytes, base64url without padding (22 characters).
    const std::string& secret() const { return secret_; }
    const std::string& host() const { return options_.host; }
    // The port the Host header must name; 0 until set.
    int port() const { return port_; }
    void set_port(int port) { port_ = port; }
    // http://<host>:<port>/c/<secret>/
    std::string url() const;

    // MAX_CONNECTIONS are open: leave the next ones waiting in the listener's backlog.
    bool full() const;
    // A new connection (its DEADLINE_S starts now).
    Conn open();
    Io io(Conn conn) const;
    // Bytes read from the connection (at most io().read); empty when the socket had none now (a closed one is
    // hang_up()). May call load_asset, get_status, load_logo, on_tx and build_tx.
    void feed(Conn conn, std::string_view bytes);
    // The peer closed its side, or the socket failed: the connection is closed (a build it waits for is cancelled).
    void hang_up(Conn conn);
    // n bytes of io().write went out.
    void sent(Conn conn, size_t n);
    // The clock: drops connections past their deadline, answers builds that finished or timed out (cancelling those).
    // Call it every frame.
    void pump();
    // Forgets a closed connection (io().close); a no-op for an open one or while a callback runs.
    void release(Conn conn);
    // Forgets every closed connection (not while a callback runs).
    void release_closed();
    // Closes every connection; the builds they wait for are cancelled.
    void close_all();
    // The connections not yet released, in the order they were opened.
    std::vector<Conn> connections() const;
    // How many connections are open (not closed).
    size_t open_count() const;
    // The state of a connection, for the tests: "head", "body", "building", "writing", "linger" or "closed" (also for
    // an id that is unknown or released).
    std::string_view state(Conn conn) const;

private:
    struct Connection;
    struct Busy;

    explicit Handler(Options options);
    double now() const;
    Connection* find(Conn conn);
    const Connection* find(Conn conn) const;
    void read_head(Connection& c);
    void request_line(Connection& c, std::string line);
    void on_head(Connection& c);
    void serve_get(Connection& c);
    void on_body(Connection& c);
    void check_build(Connection& c);
    void send(Connection& c, int code, std::string body, std::string_view content_type = "text/plain; charset=utf-8",
              bool head_only = false);
    void send_json(Connection& c, int code, const Json& value);
    void not_found(Connection& c);
    void unavailable(Connection& c);
    void close(Connection& c);

    Options options_;
    std::unique_ptr<Entropy> own_entropy_;
    std::string secret_;
    int port_ = 0;
    Conn next_ = 1;
    std::map<Conn, std::unique_ptr<Connection>> conns_;
    int busy_ = 0;  // callbacks running: release waits
};

// Python's CheckoutServer: the checkout::Server a CheckoutController starts, stops and pumps.
class SCFGCORE_API CheckoutServer final : public checkout::Server {
public:
    // A TypeError when clock or listen is empty.
    static Result<std::unique_ptr<CheckoutServer>> make(Options options);
    // stop(): safe from any destructor.
    ~CheckoutServer() override;
    CheckoutServer(const CheckoutServer&) = delete;
    CheckoutServer& operator=(const CheckoutServer&) = delete;

    // Listens on host (a random free port); a second start() while running does nothing, a start() after stop() listens
    // again (a new port, the same secret, as Python's). The listener's Err is told to on_error, then returned.
    Status start() override;
    // Stops listening and closes every connection (the builds they wait for are cancelled). Idempotent.
    void stop() override;
    // http://<host>:<port>/c/<secret>/ (the port is 0 before start()).
    std::string url() const override;
    // Accepts, reads, answers, writes and closes, without waiting on any socket. Call it every frame.
    void pump() override;

    bool running() const { return listener_ != nullptr; }
    int port() const { return handler_->port(); }
    const std::string& secret() const { return handler_->secret(); }
    // For the tests.
    const Handler& handler() const { return *handler_; }

private:
    CheckoutServer(Options options, std::unique_ptr<Handler> handler);
    void step(Handler::Conn conn);

    Options options_;
    std::unique_ptr<Handler> handler_;
    std::unique_ptr<Listener> listener_;
    std::map<Handler::Conn, std::unique_ptr<Socket>> sockets_;
};

// The CheckoutController's server_factory (Python's server_factory=CheckoutServer, as the game glue builds it): a
// CheckoutServer with ServerArgs's session_info, on_tx, get_status and build_tx, and the rest of page (load_asset,
// load_logo, host, build_timeout_s, clock, entropy, listen, on_error; page's own session_info, on_tx, get_status and
// build_tx are replaced). The build's CancelBuild is empty (see the top of this file). An Err (a missing clock or
// listen) is told to on_error, then returned.
SCFGCORE_API checkout::ServerFactory factory(Options page);

}  // namespace scfg::local_server
