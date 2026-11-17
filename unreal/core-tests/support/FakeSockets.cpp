// No Python twin: see FakeSockets.h.
#include "support/FakeSockets.h"

#include "Scfg/Py.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace scfg_test::sockets {

namespace detail {

class FakeSocket final : public scfg::local_server::Socket {
public:
    explicit FakeSocket(std::shared_ptr<Pipe> pipe) : pipe_(std::move(pipe)) {}
    ~FakeSocket() override { pipe_->server_closed = true; }

    Read read(size_t max, std::string& out) override {
        ++pipe_->calls;
        pipe_->largest_read = std::max(pipe_->largest_read, max);
        if (pipe_->broken) return Read::closed;
        if (!pipe_->to_server.empty()) {
            const size_t n = std::min(max, pipe_->to_server.size());
            out.append(pipe_->to_server, 0, n);
            pipe_->to_server.erase(0, n);
            return Read::data;
        }
        return pipe_->client_shut ? Read::closed : Read::nothing;
    }

    std::optional<size_t> write(std::string_view data) override {
        ++pipe_->calls;
        if (pipe_->broken) return std::nullopt;
        const size_t n = std::min(pipe_->capacity, data.size());
        pipe_->to_client.append(data.substr(0, n));
        return n;
    }

private:
    std::shared_ptr<Pipe> pipe_;
};

class FakeListener final : public scfg::local_server::Listener {
public:
    explicit FakeListener(std::shared_ptr<Net> net) : net_(std::move(net)) {}
    ~FakeListener() override { net_->listening = false; }
    int port() const override { return net_->port; }
    std::unique_ptr<scfg::local_server::Socket> accept() override {
        if (net_->backlog.empty()) return nullptr;
        std::shared_ptr<Pipe> pipe = net_->backlog.front();
        net_->backlog.pop_front();
        return std::make_unique<FakeSocket>(std::move(pipe));
    }

private:
    std::shared_ptr<Net> net_;
};

}  // namespace detail

std::shared_ptr<Pipe> Net::connect() {
    if (!listening) return nullptr;
    auto pipe = std::make_shared<Pipe>();
    backlog.push_back(pipe);
    all.push_back(pipe);
    return pipe;
}

scfg::local_server::Listen listen_on(std::shared_ptr<Net> net) {
    return [net](const std::string& host) -> scfg::Result<std::unique_ptr<scfg::local_server::Listener>> {
        net->last_host = host;
        if (net->refuse) return *net->refuse;
        ++net->listens;
        ++net->port;  // a new random port each time
        net->listening = true;
        return std::unique_ptr<scfg::local_server::Listener>(std::make_unique<detail::FakeListener>(net));
    };
}

std::optional<Answer> answer(const std::string& to_client) {
    const size_t end = to_client.find("\r\n\r\n");
    if (end == std::string::npos) return std::nullopt;
    Answer a;
    size_t at = 0;
    bool first = true;
    while (at < end) {
        size_t eol = to_client.find("\r\n", at);
        if (eol == std::string::npos || eol > end) eol = end;
        const std::string line = to_client.substr(at, eol - at);
        at = eol + 2;
        if (first) {
            first = false;
            const size_t sp = line.find(' ');
            if (sp == std::string::npos) return std::nullopt;
            const scfg::Result<scfg::BigInt> code = scfg::py::py_int(line.substr(sp + 1, 3));
            if (!code.ok()) return std::nullopt;
            a.status = static_cast<int>(code.value().to_int64().value());
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = scfg::py::lower_ascii(line.substr(0, colon));
        a.headers.emplace(std::move(name), scfg::py::strip(line.substr(colon + 1)));
    }
    size_t length = 0;
    if (auto it = a.headers.find("content-length"); it != a.headers.end()) {
        const scfg::Result<scfg::BigInt> n = scfg::py::py_int(it->second);
        if (!n.ok()) return std::nullopt;
        length = static_cast<size_t>(n.value().to_int64().value());
    }
    if (to_client.size() < end + 4 + length) return std::nullopt;
    a.body = to_client.substr(end + 4, length);
    return a;
}

std::string head(const scfg::local_server::CheckoutServer& srv, std::string_view method, std::string_view sub,
                 std::optional<size_t> length) {
    std::string h = std::string(method) + " /c/" + srv.secret() + "/" + std::string(sub) +
                    " HTTP/1.1\r\nHost: 127.0.0.1:" + scfg::py::dec(srv.port()) + "\r\n";
    if (length) h += "Content-Length: " + scfg::py::dec(*length) + "\r\n";
    return h + "\r\n";
}

}  // namespace scfg_test::sockets
