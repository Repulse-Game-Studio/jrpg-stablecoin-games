// No Python twin: the JSON-lines driver of the shared localhost checkout server suite (tests/server_security/, its
// `cpp_server` half): it runs one scfg::local_server::CheckoutServer (Plugins/Scfg/Source/ScfgCore, LocalServer.h)
// at a time on real POSIX sockets bound to 127.0.0.1, with fake callbacks, and pumps it every 2 ms as the glue pumps it
// every frame. A port of godot/tests/integration/server_driver.gd.
//
// Protocol (tests/server_security/server_handles.py's DriverServer): one JSON command per stdin line, one reply line
// per command on stdout, `SCFG-REPLY {"ok": <result>}` or `SCFG-REPLY {"error": "<why>"}`:
//   start {config}  stop the previous server, start a new one; ok = {url, port, secret}
//          config = {assets: {name: base64}, session_info, status,
//                    logo: null | "broken" | {b64, ctype},
//                    build: null | {mode: "ok", value, delay_s?} | {mode: "fail"} | {mode: "raise"} | {mode: "never"},
//                    build_timeout_s?}
//   events          ok = {calls: [[name, args]...], jobs: [{done, cancelled}...], running}: every callback the server
//                   made (on_tx, build_tx) and the state of each build job
//   stop            stop the server; ok = {running}
// End of stdin quits. Nothing else is written to stdout; stderr gets only an unexpected socket failure (the suite fails
// on any stderr line).
//
// The listener and the connections are non-blocking: the loop waits only in poll(2) on stdin, at most 2 ms, then pumps.
#include "Scfg/Entropy.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/LocalServer.h"
#include "Scfg/Py.h"
#include "Scfg/Result.h"
#include "Scfg/Submitter.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace scfg_server_driver {

using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace ls = scfg::local_server;

inline constexpr std::string_view REPLY_PREFIX = "SCFG-REPLY ";
inline constexpr int POLL_MS = 2;

double monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void report(const std::string& what) {
    const std::string line = "scfg_server_driver: " + what + "\n";
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

// --- POSIX sockets ------------------------------------------------------------------------------------------------

class PosixSocket final : public ls::Socket {
public:
    explicit PosixSocket(int fd) : fd_(fd) {}
    ~PosixSocket() override { ::close(fd_); }
    PosixSocket(const PosixSocket&) = delete;
    PosixSocket& operator=(const PosixSocket&) = delete;

    Read read(size_t max, std::string& out) override {
        char buf[ls::READ_CHUNK];
        const ssize_t n = ::recv(fd_, buf, std::min(max, sizeof buf), 0);
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            return Read::data;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return Read::nothing;
        return Read::closed;  // the peer closed (0), or reset the connection
    }

    std::optional<size_t> write(std::string_view data) override {
        const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
        if (n >= 0) return static_cast<size_t>(n);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return size_t{0};
        return std::nullopt;
    }

private:
    int fd_;
};

class PosixListener final : public ls::Listener {
public:
    PosixListener(int fd, int port) : fd_(fd), port_(port) {}
    ~PosixListener() override { ::close(fd_); }
    PosixListener(const PosixListener&) = delete;
    PosixListener& operator=(const PosixListener&) = delete;

    int port() const override { return port_; }

    std::unique_ptr<ls::Socket> accept() override {
        for (;;) {
            const int fd = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) return std::make_unique<PosixSocket>(fd);
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) report(std::string("accept failed: ") + std::strerror(errno));
            return nullptr;
        }
    }

private:
    int fd_;
    int port_;
};

Err cannot_listen(const std::string& host, const char* why) {
    return Err::make("RuntimeError", "cannot listen on " + host + ": " + why);
}

// Binds host (an IPv4 address) on a random free port, as Python's ThreadingHTTPServer((host, 0)).
Result<std::unique_ptr<ls::Listener>> listen_on(const std::string& host) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) return cannot_listen(host, "not an IPv4 address");
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return cannot_listen(host, std::strerror(errno));
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);  // as http.server's allow_reuse_address
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(fd, ls::LISTEN_BACKLOG) != 0) {
        const int e = errno;
        ::close(fd);
        return cannot_listen(host, std::strerror(e));
    }
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        const int e = errno;
        ::close(fd);
        return cannot_listen(host, std::strerror(e));
    }
    return std::unique_ptr<ls::Listener>(std::make_unique<PosixListener>(fd, ntohs(bound.sin_port)));
}

// --- the fakes ----------------------------------------------------------------------------------------------------

// base64.b64decode of what Python's base64.b64encode wrote (the standard alphabet, padded); nullopt otherwise.
std::optional<std::string> b64decode(std::string_view s) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (s.size() % 4 != 0) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < s.size(); i += 4) {
        uint32_t word = 0;
        int pad = 0;
        for (size_t k = 0; k < 4; ++k) {
            const char c = s[i + k];
            int v = 0;
            if (c == '=' && i + 4 == s.size() && k >= 2) {
                ++pad;
            } else if (pad > 0 || (v = value(c)) < 0) {
                return std::nullopt;
            }
            word = (word << 6) | static_cast<uint32_t>(v);
        }
        out.push_back(static_cast<char>((word >> 16) & 0xFF));
        if (pad < 2) out.push_back(static_cast<char>((word >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<char>(word & 0xFF));
    }
    return out;
}

// A build job: what the Python handle reports as {done, cancelled} (a concurrent.futures.Future's).
struct Job {
    bool done = false;
    bool cancelled = false;
    scfg::worker::OnDone<Json> finish;
};

struct Delayed {
    std::weak_ptr<Job> job;
    Json value;
    double due = 0;
};

class Driver {
public:
    std::string reply_to(const std::string& line) {
        const Result<Json> parsed = scfg::json::loads(line);
        if (!parsed.ok() || !parsed.value().is_dict()) return error("not a JSON object: " + line.substr(0, 200));
        const Json::Dict& cmd = parsed.value().as_dict();
        const Json op = cmd.get("op");
        if (op.is_str() && op.as_str() == "start") return start(cmd.get("config", Json::dict()));
        if (op.is_str() && op.as_str() == "events") return events();
        if (op.is_str() && op.as_str() == "stop") {
            if (server_) server_->stop();
            return ok(Json::dict({{"running", running()}}));
        }
        const Result<std::string> shown = scfg::json::dumps(op);
        return error("unknown op " + (shown.ok() ? shown.value() : std::string("?")));
    }

    // One turn of the loop: delayed builds that are due finish, then the server is pumped.
    void turn() {
        const double now = monotonic();
        std::vector<Delayed> later;
        std::vector<Delayed> due;
        for (Delayed& d : delayed_) (now >= d.due ? due : later).push_back(std::move(d));
        delayed_ = std::move(later);
        for (Delayed& d : due) {
            if (const std::shared_ptr<Job> job = d.job.lock(); job && !job->done) {
                job->done = true;
                scfg::worker::OnDone<Json> finish = std::move(job->finish);
                if (finish) finish(std::move(d.value));
            }
        }
        if (server_) server_->pump();
    }

    void finish() {
        if (server_) server_->stop();
        server_.reset();
    }

private:
    bool running() const { return server_ != nullptr && server_->running(); }

    std::string start(const Json& config) {
        if (!config.is_dict()) return error("config must be an object");
        if (server_) server_->stop();
        server_.reset();
        calls_ = Json::list();
        jobs_.clear();
        delayed_.clear();
        assets_.clear();
        const Json::Dict& c = config.as_dict();
        const Json assets = c.get("assets", Json::dict());
        if (assets.is_dict()) {
            for (const auto& [name, b64] : assets.as_dict()) {
                std::optional<std::string> bytes = b64.is_str() ? b64decode(b64.as_str()) : std::nullopt;
                if (!bytes) return error("asset " + name + " is not base64");
                assets_[name] = std::move(*bytes);
            }
        }
        status_ = c.get("status", Json::dict());
        ls::Options o;
        o.load_asset = [this](const std::string& name) -> std::optional<std::string> {
            const auto it = assets_.find(name);
            return it == assets_.end() ? std::nullopt : std::optional<std::string>(it->second);
        };
        o.session_info = c.get("session_info", Json::dict());
        o.on_tx = [this](std::string tx, std::optional<std::string> sender) {
            calls_.as_list().push_back(Json::list(
                {Json("on_tx"), Json::list({Json(std::move(tx)), sender ? Json(std::move(*sender)) : Json()})}));
        };
        o.get_status = [this] { return status_; };
        const Json logo = c.get("logo");
        if (logo.is_str() && logo.as_str() == "broken") {
            o.load_logo = []() -> std::optional<ls::Logo> { return std::nullopt; };
        } else if (logo.is_dict()) {
            const Json b64 = logo.as_dict().get("b64");
            const Json ctype = logo.as_dict().get("ctype");
            std::optional<std::string> bytes = b64.is_str() ? b64decode(b64.as_str()) : std::nullopt;
            if (!bytes || !ctype.is_str()) return error("logo must be {b64, ctype}");
            o.load_logo = [made = ls::Logo{std::move(*bytes), ctype.as_str()}]() -> std::optional<ls::Logo> {
                return made;
            };
        }
        const Json build = c.get("build");
        if (build.is_dict()) {
            o.build_tx = [this, build](std::string account, scfg::worker::OnDone<Json> done) {
                return build_tx(build, std::move(account), std::move(done));
            };
        }
        const Json timeout = c.get("build_timeout_s");
        if (timeout.is_float()) o.build_timeout_s = timeout.as_float();
        if (timeout.is_int64()) o.build_timeout_s = static_cast<double>(timeout.as_int64());
        o.clock = monotonic;
        o.listen = listen_on;  // (no on_error: start()'s Err is the reply)
        Result<std::unique_ptr<ls::CheckoutServer>> made = ls::CheckoutServer::make(std::move(o));
        if (!made.ok()) return error("start failed: " + made.err().str());
        server_ = made.take();
        const scfg::Status started = server_->start();
        if (!started.ok()) return error("start failed: " + started.err().str());
        return ok(Json::dict({{"url", server_->url()}, {"port", server_->port()}, {"secret", server_->secret()}}));
    }

    Result<ls::CancelBuild> build_tx(const Json& build, std::string account, scfg::worker::OnDone<Json> done) {
        calls_.as_list().push_back(Json::list({Json("build_tx"), Json::list({Json(std::move(account))})}));
        const Json mode = build.as_dict().get("mode");
        const std::string m = mode.is_str() ? mode.as_str() : std::string();
        if (m == "raise") return Err::make("RuntimeError", "submit failed");
        auto job = std::make_shared<Job>();
        jobs_.push_back(job);
        job->finish = std::move(done);
        if (m == "fail") {
            job->done = true;
            scfg::worker::OnDone<Json> finish = std::move(job->finish);
            finish(Err::make("RuntimeError", "timed out"));
        } else if (m == "ok") {
            const Json delay = build.as_dict().get("delay_s", Json(0));
            double seconds = 0.0;
            if (delay.is_float()) seconds = delay.as_float();
            if (delay.is_int64()) seconds = static_cast<double>(delay.as_int64());
            if (seconds > 0) {
                delayed_.push_back(Delayed{job, build.as_dict().get("value"), monotonic() + seconds});
            } else {
                job->done = true;
                scfg::worker::OnDone<Json> finish = std::move(job->finish);
                finish(build.as_dict().get("value"));
            }
        }
        // "never": the job never finishes. Python's Future.cancel(): only a job not yet done is cancelled.
        const std::weak_ptr<Job> weak = job;
        return ls::CancelBuild([weak] {
            if (const std::shared_ptr<Job> j = weak.lock(); j && !j->done) {
                j->done = true;
                j->cancelled = true;
                j->finish = nullptr;
            }
        });
    }

    std::string events() {
        Json jobs = Json::list();
        for (const std::shared_ptr<Job>& j : jobs_) {
            jobs.as_list().push_back(Json::dict({{"done", j->done}, {"cancelled", j->cancelled}}));
        }
        return ok(Json::dict({{"calls", scfg::deep_copy(calls_)}, {"jobs", jobs}, {"running", running()}}));
    }

    static std::string ok(const Json& result) {
        const Result<std::string> text = scfg::json::dumps(Json::dict({{"ok", result}}));
        return text.ok() ? text.value() : error("cannot encode the reply");
    }

    static std::string error(const std::string& why) {
        const Result<std::string> text = scfg::json::dumps(Json::dict({{"error", why}}));
        return text.ok() ? text.value() : std::string("{\"error\": \"?\"}");
    }

    std::unique_ptr<ls::CheckoutServer> server_;
    std::map<std::string, std::string> assets_;
    Json status_ = Json::dict();
    Json calls_ = Json::list();
    std::vector<std::shared_ptr<Job>> jobs_;
    std::vector<Delayed> delayed_;
};

}  // namespace scfg_server_driver

int main() {
    using namespace scfg_server_driver;
    std::signal(SIGPIPE, SIG_IGN);  // (the sockets send with MSG_NOSIGNAL; stdout's reader may still go away)
    Driver driver;
    std::string pending;
    bool eof = false;
    while (!eof) {
        pollfd in{STDIN_FILENO, POLLIN, 0};
        if (::poll(&in, 1, POLL_MS) > 0 && (in.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char buf[1 << 16];
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
            if (n > 0) {
                pending.append(buf, static_cast<size_t>(n));
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                eof = true;
            }
        }
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            const std::string reply = std::string(REPLY_PREFIX) + driver.reply_to(line) + "\n";
            std::fwrite(reply.data(), 1, reply.size(), stdout);
            std::fflush(stdout);
        }
        driver.turn();
    }
    driver.finish();
    return 0;
}
