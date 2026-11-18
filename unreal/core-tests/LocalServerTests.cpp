// Copyright 2026 Repulse
// No Python twin: local_server::CheckoutServer and its Handler (LocalServer.h) over the in-memory sockets of
// support/FakeSockets.h, with a fake clock: what only a server without Python's http.server has to prove, the twins of
// godot/tests/test_local_server.gd. Its HTTP behaviour against local_server.py lives in the shared suite
// (tests/server_security/test_server_security.py, through core-driver/ServerMain.cpp on real sockets).
//
// "Never waits" is checked by structure, as the Godot tests do: every socket call the server makes is counted, and a
// pump makes a bounded number of them on each connection (a loop waiting for bytes would call again and again).
//
// Godot test (godot/tests/test_local_server.gd)             -> C++ test (LocalServerTests.<name>)
//   test_url_and_secret                                      -> the same name
//   test_start_is_idempotent_and_a_restart_keeps_the_secret  -> the same name
//   test_start_failure_is_an_err                             -> the same name (the listener refuses; on_error is told)
//   test_slow_client_does_not_block_the_frame                -> the same name
//   test_a_partial_body_is_dropped_after_the_deadline        -> the same name
//   test_byte_at_a_time_client_is_served                     -> the same name
//   test_a_client_that_never_reads_does_not_block_the_frame  -> the same name (a 2 MiB asset; the socket full, then
//       taking everything: at most WRITE_CHUNKS * WRITE_CHUNK bytes per pump)
//   test_build_waits_for_the_job_without_blocking            -> the same name
//   test_build_timeout_answers_503_and_cancels_the_job       -> the same name
//   test_failed_or_odd_build_jobs_are_503                    -> the same name (a failed build, a non-dict result,
//       build_tx's Err; "not a job" and "null" cannot be written in C++)
//   test_stop_cancels_a_pending_build                        -> the same name
//   test_invalid_callbacks_answer_without_a_script_error     -> test_missing_callbacks_answer_without_a_crash (an
//       empty std::function is the C++ form of a freed Callable)
//   test_logo_must_be_a_raster_image                         -> the same name
//   test_oversized_head_is_431                               -> the same name
//   test_on_tx_gets_the_normalised_hint                      -> the same name
//   test_a_callback_that_stops_the_server_is_safe            -> the same name
//   test_connections_are_capped                              -> the same name
// C++ only: an answer's exact bytes, a complete request closed after its answer, the linger rules (drained, bounded in
// time and bytes), a client that hangs up mid-request, a connection that breaks while answering, a build that finishes
// after the server is gone, the destructor, the controller's factory, and the required clock and listen.
#include "support/FakeSockets.h"
#include "support/Test.h"

#include "Scfg/Checkout.h"
#include "Scfg/Entropy.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/LocalServer.h"
#include "Scfg/PageMessages.h"
#include "Scfg/Py.h"
#include "Scfg/Result.h"
#include "Scfg/Submitter.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace local_server_tests {

using scfg::Err;
using scfg::Json;
using scfg::Result;
using scfg_test::sockets::Answer;
using scfg_test::sockets::Pipe;
namespace ls = scfg::local_server;
namespace sk = scfg_test::sockets;

constexpr const char* ACCOUNT = "CejwupnRUhnYHb72dZ8rqnWsvFw49MDYKZ2SvurkX4K3";
constexpr const char* TX = "0xabababababababababababababababababababababababababababababababab";
// The socket calls one pump may make on one connection: its reads, then its writes.
constexpr int MAX_CALLS_PER_PUMP = static_cast<int>(ls::READS_PER_PUMP + ls::WRITE_CHUNKS);

std::string dumps(const Json& v) {
    const Result<std::string> text = scfg::json::dumps(v);
    return text.ok() ? text.value() : std::string("<dumps failed>");
}

// A build handed out: done and cancel, as a test drives them (Godot's ScfgJob).
struct Job {
    scfg::worker::OnDone<Json> done;
    bool cancelled = false;
    int cancels = 0;
};

// test_local_server.gd's _server(): the defaults, a fake clock, the fake net, every hint and Err recorded.
struct Rig {
    std::shared_ptr<sk::Net> net = std::make_shared<sk::Net>();
    std::shared_ptr<sk::Clock> clock = std::make_shared<sk::Clock>();
    std::shared_ptr<std::vector<std::pair<std::string, std::optional<std::string>>>> hints =
        std::make_shared<std::vector<std::pair<std::string, std::optional<std::string>>>>();
    std::shared_ptr<std::vector<std::string>> errors = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<std::vector<std::string>> asked = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<std::vector<std::shared_ptr<Job>>> jobs = std::make_shared<std::vector<std::shared_ptr<Job>>>();
    std::unique_ptr<ls::CheckoutServer> srv;

    ls::Options options() const {
        ls::Options o;
        o.load_asset = [](const std::string& name) -> std::optional<std::string> { return "<" + name + ">"; };
        o.session_info = Json::dict({{"chain_id", 42161}});
        o.on_tx = [hints = hints](std::string tx, std::optional<std::string> sender) {
            hints->emplace_back(std::move(tx), std::move(sender));
        };
        o.get_status = [] { return Json::dict({{"state", "waiting"}}); };
        o.clock = [clock = clock] { return clock->now; };
        o.listen = sk::listen_on(net);
        o.on_error = [errors = errors](const Err& e) { errors->push_back(e.str()); };
        return o;
    }

    // build_tx for test_build_*: records the account, hands out a Job the test finishes or watches being cancelled.
    ls::BuildTx job_builder() const {
        return [asked = asked, jobs = jobs](std::string account, scfg::worker::OnDone<Json> done) -> Result<ls::CancelBuild> {
            asked->push_back(account);
            auto job = std::make_shared<Job>();
            job->done = std::move(done);
            jobs->push_back(job);
            return ls::CancelBuild([job] {
                job->cancelled = true;
                ++job->cancels;
            });
        };
    }

    bool start(scfg_test::Ctx& t, ls::Options o) {
        Result<std::unique_ptr<ls::CheckoutServer>> made = ls::CheckoutServer::make(std::move(o));
        if (!t.check(made.ok(), "the server is made")) return false;
        srv = made.take();
        return t.check(srv->start().ok(), "the server starts");
    }
    bool start(scfg_test::Ctx& t) { return start(t, options()); }

    // A client the server has accepted.
    std::shared_ptr<Pipe> connect() {
        std::shared_ptr<Pipe> p = net->connect();
        srv->pump();
        return p;
    }

    // Pumps `frames` times; the most socket calls one pump made on one connection.
    int pump(int frames = 1) {
        int worst = 0;
        for (int i = 0; i < frames; ++i) {
            for (const std::shared_ptr<Pipe>& p : net->all) p->calls = 0;
            srv->pump();
            for (const std::shared_ptr<Pipe>& p : net->all) worst = std::max(worst, p->calls);
        }
        return worst;
    }

    // The whole answer the client received, pumping meanwhile (at most `frames` pumps).
    std::optional<Answer> response(const std::shared_ptr<Pipe>& p, int frames = 50) {
        for (int i = 0; i < frames; ++i) {
            if (std::optional<Answer> a = sk::answer(p->to_client)) return a;
            srv->pump();
        }
        return sk::answer(p->to_client);
    }

    int status(const std::shared_ptr<Pipe>& p, int frames = 50) {
        const std::optional<Answer> a = response(p, frames);
        return a ? a->status : 0;
    }

    // True once the server closed the client's connection (pumping meanwhile).
    bool closed(const std::shared_ptr<Pipe>& p, int frames = 50) {
        for (int i = 0; i < frames && !p->server_closed; ++i) srv->pump();
        return p->server_closed;
    }

    std::string head(std::string_view method, std::string_view sub, std::optional<size_t> length = std::nullopt) const {
        return sk::head(*srv, method, sub, length);
    }
};

// A started server's single answer to `request` (a new connection), or nothing.
std::optional<Answer> ask(Rig& r, const std::string& request) {
    std::shared_ptr<Pipe> c = r.connect();
    if (c == nullptr) return std::nullopt;
    c->send(request);
    return r.response(c);
}

int status_of(Rig& r, const std::string& request) {
    const std::optional<Answer> a = ask(r, request);
    return a ? a->status : 0;
}

}  // namespace local_server_tests

using namespace local_server_tests;

SCFG_TEST(LocalServerTests, test_url_and_secret) {
    Rig r;
    if (!r.start(t)) return;
    SCFG_CHECK(t, r.srv->running());
    SCFG_CHECK(t, r.srv->port() > 0);
    SCFG_CHECK_EQ(t, r.srv->url(), "http://127.0.0.1:" + scfg::py::dec(r.srv->port()) + "/c/" + r.srv->secret() + "/");
    SCFG_CHECK_EQ(t, r.net->last_host, "127.0.0.1");
    SCFG_CHECK_EQ(t, r.srv->secret().size(), 22);  // 16 random bytes, base64url without padding
    bool urlsafe = true;
    for (char c : r.srv->secret()) {
        urlsafe = urlsafe && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                              c == '_');
    }
    SCFG_CHECK(t, urlsafe);
    Rig other;
    if (!other.start(t)) return;
    SCFG_CHECK(t, other.srv->secret() != r.srv->secret());  // a new secret per server
    // The secret comes from the given Entropy (Python's secrets.token_urlsafe(16))
    struct Ones final : scfg::Entropy {
        std::vector<uint8_t> token_bytes(int n) override { return std::vector<uint8_t>(static_cast<size_t>(n), 0xff); }
    } ones;
    ls::Options o = r.options();
    o.entropy = &ones;
    Result<std::unique_ptr<ls::CheckoutServer>> scripted = ls::CheckoutServer::make(std::move(o));
    SCFG_CHECK(t, scripted.ok() && scripted.value()->secret() == "_____________________w");
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_start_is_idempotent_and_a_restart_keeps_the_secret) {
    Rig r;
    if (!r.start(t)) return;
    const int port = r.srv->port();
    const std::string secret = r.srv->secret();
    SCFG_CHECK(t, r.srv->start().ok());  // a second start
    SCFG_CHECK_EQ(t, r.srv->port(), port);
    SCFG_CHECK_EQ(t, r.net->listens, 1);
    r.srv->stop();
    SCFG_CHECK(t, !r.srv->running() && !r.net->listening);
    r.srv->stop();  // twice is fine
    SCFG_CHECK(t, r.srv->start().ok());
    SCFG_CHECK(t, r.srv->running() && r.srv->port() != port);  // a new port
    SCFG_CHECK_EQ(t, r.srv->secret(), secret);                // the same secret, as Python's
    SCFG_CHECK_EQ(t, status_of(r, r.head("GET", "status")), 200);
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_start_failure_is_an_err) {
    Rig r;
    r.net->refuse = Err::make("RuntimeError", "cannot listen on 192.0.2.1: Cannot assign requested address");
    ls::Options o = r.options();
    o.host = "192.0.2.1";  // TEST-NET-1: not on this machine
    Result<std::unique_ptr<ls::CheckoutServer>> made = ls::CheckoutServer::make(std::move(o));
    if (!t.check(made.ok(), "made")) return;
    r.srv = made.take();
    const scfg::Status started = r.srv->start();
    SCFG_CHECK(t, !started.ok() && started.err().kind == "RuntimeError");
    SCFG_CHECK(t, !r.srv->running());
    SCFG_CHECK_EQ(t, r.errors->size(), 1);  // told to on_error once (the controller drops the Err)
    SCFG_CHECK(t, !r.errors->empty() && r.errors->front() == started.err().str());
    r.srv->pump();  // harmless
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_slow_client_does_not_block_the_frame) {
    Rig r;
    if (!r.start(t)) return;
    std::shared_ptr<Pipe> c = r.connect();
    c->send("GET /c/" + r.srv->secret() + "/ HTTP/1.1\r\nHost: 127.0");  // and never the rest
    const int worst = r.pump(50);
    SCFG_CHECK(t, worst <= MAX_CALLS_PER_PUMP);
    SCFG_CHECK_EQ(t, r.srv->handler().open_count(), 1);  // held before the deadline
    // another client is served meanwhile
    std::shared_ptr<Pipe> other = r.connect();
    other->send(r.head("GET", "status"));
    SCFG_CHECK_EQ(t, r.status(other), 200);
    other->client_shut = true;
    r.pump(20);
    r.clock->now += ls::DEADLINE_S - 0.5;
    r.pump();
    SCFG_CHECK_EQ(t, r.srv->handler().open_count(), 1);  // the slow client is still held at 9.5 s
    SCFG_CHECK(t, !c->server_closed);
    r.clock->now += 1.0;
    SCFG_CHECK(t, r.pump(3) <= MAX_CALLS_PER_PUMP);  // dropping is immediate too
    SCFG_CHECK(t, c->server_closed && c->to_client.empty());  // dropped after the deadline, without an answer
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_a_partial_body_is_dropped_after_the_deadline) {
    Rig r;
    if (!r.start(t)) return;
    std::shared_ptr<Pipe> c = r.connect();
    c->send(r.head("POST", "tx", 100) + "{\"tx");
    SCFG_CHECK(t, r.pump(30) <= MAX_CALLS_PER_PUMP);  // pump() does not wait for the body
    SCFG_CHECK_EQ(t, r.srv->handler().open_count(), 1);  // held while the body may still come
    r.clock->now += ls::DEADLINE_S + 0.1;
    SCFG_CHECK(t, r.closed(c));
    SCFG_CHECK(t, r.hints->empty());
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_byte_at_a_time_client_is_served) {
    Rig r;
    if (!r.start(t)) return;
    std::shared_ptr<Pipe> c = r.connect();
    const std::string req = r.head("GET", "session.json");
    for (char ch : req) {
        c->send(std::string(1, ch));
        r.srv->pump();
    }
    const std::optional<Answer> got = r.response(c);
    SCFG_CHECK(t, got && got->status == 200);
    SCFG_CHECK(t, got && got->body == "{\"chain_id\": 42161}");
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_a_client_that_never_reads_does_not_block_the_frame) {
    Rig r;
    ls::Options o = r.options();
    const std::string big(2 << 20, 'x');  // more than one pump writes
    o.load_asset = [&big](const std::string&) -> std::optional<std::string> { return big; };
    if (!r.start(t, std::move(o))) return;
    std::shared_ptr<Pipe> c = r.connect();
    c->capacity = 0;  // the socket is full: the client never reads
    c->send(r.head("GET", "app.js"));
    r.pump();
    const std::vector<ls::Handler::Conn> conns = r.srv->handler().connections();
    SCFG_CHECK(t, conns.size() == 1 && r.srv->handler().state(conns[0]) == "writing");
    SCFG_CHECK(t, r.pump(40) <= MAX_CALLS_PER_PUMP);  // writes what the socket takes and returns
    SCFG_CHECK_EQ(t, r.srv->handler().open_count(), 1);  // still writing
    // a socket that takes everything gets at most WRITE_CHUNKS * WRITE_CHUNK bytes per pump
    c->capacity = SIZE_MAX;
    r.srv->pump();
    SCFG_CHECK_EQ(t, c->to_client.size(), ls::WRITE_CHUNKS * ls::WRITE_CHUNK);
    c->capacity = 0;
    r.clock->now += ls::DEADLINE_S + 0.1;
    r.srv->pump();
    SCFG_CHECK(t, c->server_closed && r.srv->handler().open_count() == 0);  // dropped after the deadline
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_build_waits_for_the_job_without_blocking) {
    Rig r;
    ls::Options o = r.options();
    o.build_tx = r.job_builder();
    if (!r.start(t, std::move(o))) return;
    std::shared_ptr<Pipe> c = r.connect();
    const std::string body = dumps(Json::dict({{"account", ACCOUNT}}));
    c->send(r.head("POST", "build", body.size()) + body);
    SCFG_CHECK(t, r.pump(30) <= MAX_CALLS_PER_PUMP);  // returns at once while the job runs
    SCFG_CHECK(t, *r.asked == std::vector<std::string>{ACCOUNT});  // the normalised account, once
    SCFG_CHECK(t, c->to_client.empty());  // no answer before the job finishes
    // held past the 10 s request deadline: the build has its own
    r.clock->now += ls::DEADLINE_S + 1.0;
    r.pump();
    SCFG_CHECK_EQ(t, r.srv->handler().open_count(), 1);
    if (!t.check(r.jobs->size() == 1, "one job")) return;
    r.jobs->front()->done(Json::dict({{"transaction", "AA=="}, {"sol_balance", "2"}}));
    const std::optional<Answer> got = r.response(c);
    SCFG_CHECK(t, got && got->status == 200);
    SCFG_CHECK(t, got && got->body == "{\"transaction\": \"AA==\", \"sol_balance\": \"2\"}");
    SCFG_CHECK(t, got && got->headers.at("content-type") == "application/json");
    SCFG_CHECK(t, !r.jobs->front()->cancelled);
    r.srv->stop();
    SCFG_CHECK_EQ(t, r.jobs->front()->cancels, 0);  // a finished build is never cancelled
}

SCFG_TEST(LocalServerTests, test_build_timeout_answers_503_and_cancels_the_job) {
    Rig r;
    ls::Options o = r.options();
    o.build_tx = r.job_builder();
    if (!r.start(t, std::move(o))) return;
    std::shared_ptr<Pipe> c = r.connect();
    const std::string body = dumps(Json::dict({{"account", ACCOUNT}}));
    c->send(r.head("POST", "build", body.size()) + body);
    r.pump(10);
    if (!t.check(r.jobs->size() == 1, "one job")) return;
    r.clock->now += static_cast<double>(scfg::page_messages::BUILD_TIMEOUT_S) - 0.5;
    r.pump();
    SCFG_CHECK(t, !r.jobs->front()->cancelled);  // not before BUILD_TIMEOUT_S
    r.clock->now += 1.0;
    const std::optional<Answer> got = r.response(c);
    SCFG_CHECK(t, got && got->status == 503 && got->body == "{\"error\": \"unavailable\"}");
    SCFG_CHECK(t, r.jobs->front()->cancelled && r.jobs->front()->cancels == 1);
    r.jobs->front()->done(Json::dict({{"transaction", "AA=="}}));  // too late: dropped
    r.pump(3);
    SCFG_CHECK(t, got && sk::answer(c->to_client)->status == 503);
    r.srv->stop();
    SCFG_CHECK_EQ(t, r.jobs->front()->cancels, 1);
}

SCFG_TEST(LocalServerTests, test_failed_or_odd_build_jobs_are_503) {
    struct Row {
        const char* what;
        ls::BuildTx build;
    };
    const std::vector<Row> rows = {
        {"failed job",
         [](std::string, scfg::worker::OnDone<Json> done) -> Result<ls::CancelBuild> {
             done(Err::make("RuntimeError", "rpc down"));
             return ls::CancelBuild();
         }},
        {"a non-dict result",
         [](std::string, scfg::worker::OnDone<Json> done) -> Result<ls::CancelBuild> {
             done(Json("not a dict"));
             return ls::CancelBuild();
         }},
        {"a cancelled build",
         [](std::string, scfg::worker::OnDone<Json> done) -> Result<ls::CancelBuild> {
             done(Err::make("CancelledError", ""));
             return ls::CancelBuild();
         }},
        {"an error", [](std::string, scfg::worker::OnDone<Json>) -> Result<ls::CancelBuild> {
             return Err::make("RuntimeError", "busy");
         }},
    };
    for (const Row& row : rows) {
        Rig r;
        ls::Options o = r.options();
        o.build_tx = row.build;
        if (!r.start(t, std::move(o))) return;
        const std::string body = dumps(Json::dict({{"account", ACCOUNT}}));
        t.check(status_of(r, r.head("POST", "build", body.size()) + body) == 503, row.what);
        r.srv->stop();
    }
}

SCFG_TEST(LocalServerTests, test_stop_cancels_a_pending_build) {
    Rig r;
    ls::Options o = r.options();
    o.build_tx = r.job_builder();
    if (!r.start(t, std::move(o))) return;
    std::shared_ptr<Pipe> c = r.connect();
    const std::string body = dumps(Json::dict({{"account", ACCOUNT}}));
    c->send(r.head("POST", "build", body.size()) + body);
    r.pump(10);
    r.srv->stop();
    SCFG_CHECK(t, r.jobs->size() == 1 && r.jobs->front()->cancelled);  // a page that is gone gets no build
    SCFG_CHECK(t, c->server_closed && r.srv->handler().connections().empty());
    SCFG_CHECK(t, !r.net->listening);
}

SCFG_TEST(LocalServerTests, test_missing_callbacks_answer_without_a_crash) {
    Rig r;
    ls::Options o = r.options();
    o.on_tx = nullptr;
    o.get_status = nullptr;
    o.load_asset = nullptr;
    o.load_logo = []() -> std::optional<ls::Logo> { return std::nullopt; };  // the file cannot be read
    if (!r.start(t, std::move(o))) return;
    const std::string tx_body = dumps(Json::dict({{"tx_hash", TX}}));
    SCFG_CHECK_EQ(t, status_of(r, r.head("POST", "tx", tx_body.size()) + tx_body), 503);
    SCFG_CHECK_EQ(t, status_of(r, r.head("GET", "status")), 503);
    SCFG_CHECK_EQ(t, status_of(r, r.head("GET", "app.js")), 500);
    SCFG_CHECK_EQ(t, status_of(r, r.head("GET", "")), 500);
    SCFG_CHECK_EQ(t, status_of(r, r.head("GET", "logo")), 404);
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_logo_must_be_a_raster_image) {
    struct Row {
        const char* ctype;
        int status;
    };
    for (const Row row : {Row{"image/png", 200}, Row{"image/jpeg", 200}, Row{"image/webp", 200},
                          Row{"image/svg+xml", 404}, Row{"text/html", 404}, Row{"image/png\r\nX-Evil: 1", 404}}) {
        Rig r;
        ls::Options o = r.options();
        o.load_logo = [ctype = std::string(row.ctype)]() -> std::optional<ls::Logo> { return ls::Logo{"<svg/>", ctype}; };
        if (!r.start(t, std::move(o))) return;
        const std::optional<Answer> got = ask(r, r.head("GET", "logo"));
        t.check(got && got->status == row.status, row.ctype);
        t.check(got && got->headers.count("x-evil") == 0, "no header injection");
        if (got && row.status == 200) t.check(got->headers.at("content-type") == row.ctype, "served as its type");
        r.srv->stop();
    }
    Rig none;  // no load_logo: no route
    if (!none.start(t)) return;
    SCFG_CHECK_EQ(t, status_of(none, none.head("GET", "logo")), 404);
}

SCFG_TEST(LocalServerTests, test_oversized_head_is_431) {
    Rig r;
    if (!r.start(t)) return;
    // lines each under MAX_LINE (as http.client reads them) that add up to more than MAX_HEAD
    const std::string pad = "X-Pad: " + std::string(ls::MAX_LINE - 10, 'a') + "\r\n";
    std::string lines;
    for (size_t i = 0; i < ls::MAX_HEAD / pad.size() + 1; ++i) lines += pad;
    SCFG_CHECK_EQ(t, status_of(r, "GET /c/" + r.srv->secret() + "/ HTTP/1.1\r\n" + lines), 431);
    // a header line over MAX_LINE, before its end arrives
    SCFG_CHECK_EQ(t, status_of(r, "GET /c/" + r.srv->secret() + "/ HTTP/1.1\r\nX-Pad: " + std::string(ls::MAX_LINE, 'a')),
                  431);
    // http.client counts the blank line that ends the head as one of its 100: 99 headers at most
    std::string most = "GET /c/" + r.srv->secret() + "/ HTTP/1.1\r\nHost: 127.0.0.1:" + scfg::py::dec(r.srv->port()) +
                       "\r\n";
    for (int i = 0; i < 98; ++i) most += "X-" + scfg::py::dec(i) + ": 1\r\n";
    SCFG_CHECK_EQ(t, status_of(r, most + "\r\n"), 200);
    SCFG_CHECK_EQ(t, status_of(r, most + "X-98: 1\r\n\r\n"), 431);
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_on_tx_gets_the_normalised_hint) {
    Rig r;
    if (!r.start(t)) return;
    std::string upper = TX;  // the hash in capitals after its 0x, as test_on_tx_gets_the_normalised_hint sends it
    for (size_t i = 2; i < upper.size(); ++i) {
        if (upper[i] >= 'a' && upper[i] <= 'f') upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
    }
    const std::string body =
        dumps(Json::dict({{"tx_hash", upper}, {"from", "0x52908400098527886e0f7030069857d2e4169ee7"}}));
    const std::optional<Answer> got = ask(r, r.head("POST", "tx", body.size()) + body);
    SCFG_CHECK(t, got && got->status == 202 && got->body == "{\"ok\": true}");
    SCFG_CHECK(t, r.hints->size() == 1 && r.hints->front().first == TX &&
                      r.hints->front().second == std::optional<std::string>("0x52908400098527886E0F7030069857D2E4169EE7"));
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, test_a_callback_that_stops_the_server_is_safe) {
    Rig r;
    ls::Options o = r.options();
    auto holder = std::make_shared<ls::CheckoutServer*>(nullptr);
    o.on_tx = [holder](std::string, std::optional<std::string>) { (*holder)->stop(); };
    if (!r.start(t, std::move(o))) return;
    *holder = r.srv.get();
    std::shared_ptr<Pipe> c = r.connect();
    std::shared_ptr<Pipe> other = r.connect();  // a second connection, after the one that stops the server
    other->send("GET /c/");
    const std::string body = dumps(Json::dict({{"tx_hash", TX}}));
    c->send(r.head("POST", "tx", body.size()) + body);
    r.pump(5);
    SCFG_CHECK(t, !r.srv->running());  // stopped from inside pump()
    SCFG_CHECK(t, r.srv->handler().connections().empty());  // no connection left
    SCFG_CHECK(t, c->server_closed && other->server_closed);
    SCFG_CHECK(t, c->to_client.empty());  // (on_tx stopped the server: nothing is answered after it)
}

SCFG_TEST(LocalServerTests, test_connections_are_capped) {
    Rig r;
    if (!r.start(t)) return;
    std::vector<std::shared_ptr<Pipe>> clients;
    for (size_t i = 0; i < ls::MAX_CONNECTIONS + 4; ++i) clients.push_back(r.net->connect());
    r.pump(50);
    SCFG_CHECK_EQ(t, r.srv->handler().open_count(), ls::MAX_CONNECTIONS);  // at most MAX_CONNECTIONS are taken
    SCFG_CHECK_EQ(t, r.net->backlog.size(), 4);                             // the rest wait in the backlog
    r.clock->now += ls::DEADLINE_S + 0.1;
    r.srv->pump();  // the idle ones are dropped, and the waiting ones taken next
    SCFG_CHECK_EQ(t, r.srv->handler().open_count(), 4);
    SCFG_CHECK(t, r.net->backlog.empty());
    r.srv->stop();
}

// --- C++ only ------------------------------------------------------------------------------------------------------

SCFG_TEST(LocalServerTests, AnAnswerIsExactlyTheseBytes) {
    Rig r;
    if (!r.start(t)) return;
    std::shared_ptr<Pipe> c = r.connect();
    c->send(r.head("GET", "nope"));
    r.pump();
    SCFG_CHECK_EQ(t, c->to_client,
                  "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 9\r\n"
                  "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
                  "Connection: close\r\n\r\nnot found");
    std::shared_ptr<Pipe> h = r.connect();
    h->send("HEAD /c/" + r.srv->secret() + "/ HTTP/1.1\r\n\r\n");
    r.pump();
    SCFG_CHECK_EQ(t, h->to_client,
                  "HTTP/1.1 501 Not Implemented\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 18\r\n"
                  "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
                  "Connection: close\r\n\r\n");  // HEAD: no body
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, ACompleteRequestIsClosedAfterItsAnswer) {
    Rig r;
    if (!r.start(t)) return;
    std::shared_ptr<Pipe> c = r.connect();
    c->send(r.head("GET", ""));
    r.pump();
    SCFG_CHECK(t, sk::answer(c->to_client) && sk::answer(c->to_client)->body == "<index.html>");
    r.pump();  // the socket holds nothing more: closed without waiting for the client
    SCFG_CHECK(t, c->server_closed);
    SCFG_CHECK_EQ(t, r.srv->handler().connections().size(), 0);
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, AnUnreadBodyIsDrainedBeforeClosing) {
    // A 413 answered before the body arrives: the rest is read and dropped until the client closes...
    Rig r;
    if (!r.start(t)) return;
    std::shared_ptr<Pipe> c = r.connect();
    c->send(r.head("POST", "tx", 99999) + std::string(1000, 'x'));
    r.pump();
    SCFG_CHECK(t, sk::answer(c->to_client) && sk::answer(c->to_client)->status == 413);
    r.pump(5);
    SCFG_CHECK(t, !c->server_closed);  // lingering
    c->send(std::string(5000, 'y'));
    r.pump();
    SCFG_CHECK(t, c->to_server.empty() && !c->server_closed);  // drained
    c->client_shut = true;
    SCFG_CHECK(t, r.closed(c, 2));
    // ...or LINGER_S passes...
    std::shared_ptr<Pipe> slow = r.connect();
    slow->send(r.head("POST", "tx", 99999));
    r.pump(3);
    r.clock->now += ls::LINGER_S - 0.1;
    r.pump();
    SCFG_CHECK(t, !slow->server_closed);
    r.clock->now += 0.2;
    SCFG_CHECK(t, r.closed(slow, 2));
    // ...or LINGER_MAX_BYTES are drained
    std::shared_ptr<Pipe> flood = r.connect();
    flood->send(r.head("POST", "tx", 99999));
    r.pump(2);
    flood->send(std::string(ls::LINGER_MAX_BYTES, 'z'));
    SCFG_CHECK(t, r.closed(flood, 3));
    SCFG_CHECK(t, flood->largest_read <= ls::READ_CHUNK);
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, AClientThatHangsUpMidRequestIsClosed) {
    Rig r;
    if (!r.start(t)) return;
    std::shared_ptr<Pipe> c = r.connect();
    c->send("GET /c/" + r.srv->secret() + "/ HTTP/1.1\r\n");
    c->client_shut = true;
    SCFG_CHECK(t, r.closed(c, 2) && c->to_client.empty());
    // its FIN with a whole request is still answered (test_a_client_that_closes_its_side_after_the_request...)
    std::shared_ptr<Pipe> whole = r.connect();
    whole->send(r.head("GET", ""));
    whole->client_shut = true;
    SCFG_CHECK(t, r.closed(whole, 3));
    SCFG_CHECK(t, sk::answer(whole->to_client) && sk::answer(whole->to_client)->status == 200);
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, AConnectionThatBreaksWhileAnsweringIsClosed) {
    Rig r;
    if (!r.start(t)) return;
    std::shared_ptr<Pipe> c = r.connect();
    c->capacity = 0;
    c->send(r.head("GET", ""));
    r.pump(2);
    SCFG_CHECK(t, !c->server_closed);
    c->broken = true;
    SCFG_CHECK(t, r.closed(c, 1));
    SCFG_CHECK_EQ(t, r.srv->handler().connections().size(), 0);
    r.srv->stop();
}

SCFG_TEST(LocalServerTests, ABuildThatFinishesAfterTheServerIsGoneIsHarmless) {
    Rig r;
    ls::Options o = r.options();
    o.build_tx = r.job_builder();
    if (!r.start(t, std::move(o))) return;
    std::shared_ptr<Pipe> c = r.connect();
    const std::string body = dumps(Json::dict({{"account", ACCOUNT}}));
    c->send(r.head("POST", "build", body.size()) + body);
    r.pump(3);
    if (!t.check(r.jobs->size() == 1, "one job")) return;
    r.srv.reset();  // the destructor stops: the build is cancelled, the socket closed
    SCFG_CHECK(t, r.jobs->front()->cancelled && c->server_closed && !r.net->listening);
    r.jobs->front()->done(Json::dict({{"transaction", "AA=="}}));  // the worker answers later: nothing to reach
    SCFG_CHECK(t, c->to_client.empty());
}

SCFG_TEST(LocalServerTests, DestroyingTheServerStopsIt) {
    Rig r;
    if (!r.start(t)) return;
    std::shared_ptr<Pipe> a = r.connect();
    std::shared_ptr<Pipe> b = r.connect();
    a->send("GET /");
    r.pump();
    r.srv.reset();
    SCFG_CHECK(t, a->server_closed && b->server_closed && !r.net->listening);
}

SCFG_TEST(LocalServerTests, TheControllersFactoryServesItsArguments) {
    Rig r;
    auto built = std::make_shared<std::vector<std::string>>();
    auto dones = std::make_shared<std::vector<scfg::worker::OnDone<Json>>>();
    scfg::checkout::ServerArgs args;
    args.session_info = Json::dict({{"title", "Chapter 2"}, {"family", "solana"}});
    args.on_tx = [hints = r.hints](std::string tx, std::optional<std::string> sender) {
        hints->emplace_back(std::move(tx), std::move(sender));
    };
    args.get_status = [] { return Json::dict({{"state", "confirming"}}); };
    args.build_tx = [built, dones](Json account, scfg::worker::OnDone<Json> done) {
        built->push_back(account.is_str() ? account.as_str() : std::string("<not a str>"));
        dones->push_back(std::move(done));
    };
    ls::Options page = r.options();
    page.session_info = Json::dict({{"ignored", true}});  // the controller's own replace the page's
    page.on_tx = nullptr;
    const scfg::checkout::ServerFactory make = ls::factory(page);
    Result<std::unique_ptr<scfg::checkout::Server>> made = make(args);
    if (!t.check(made.ok(), "the factory makes a server")) return;
    std::unique_ptr<scfg::checkout::Server> server = made.take();
    if (!t.check(server->start().ok(), "started")) return;
    // drive it as a CheckoutServer through the rig
    r.srv.reset(static_cast<ls::CheckoutServer*>(server.release()));
    std::optional<Answer> got = ask(r, r.head("GET", "session.json"));
    SCFG_CHECK(t, got && got->body == "{\"title\": \"Chapter 2\", \"family\": \"solana\"}");
    got = ask(r, r.head("GET", "status"));
    SCFG_CHECK(t, got && got->body == "{\"state\": \"confirming\"}");
    const std::string tx = dumps(Json::dict({{"tx_hash", TX}}));
    SCFG_CHECK_EQ(t, status_of(r, r.head("POST", "tx", tx.size()) + tx), 202);
    SCFG_CHECK(t, r.hints->size() == 1 && r.hints->front().first == TX && !r.hints->front().second);
    std::shared_ptr<Pipe> c = r.connect();
    const std::string body = dumps(Json::dict({{"account", ACCOUNT}}));
    c->send(r.head("POST", "build", body.size()) + body);
    r.pump(3);
    SCFG_CHECK(t, *built == std::vector<std::string>{ACCOUNT} && dones->size() == 1);
    if (dones->size() == 1) dones->front()(Json::dict({{"transaction", "AA=="}}));  // the worker's answer, later
    SCFG_CHECK_EQ(t, r.status(c), 200);
    // without build_tx (EVM) there is no /build route
    scfg::checkout::ServerArgs evm = args;
    evm.build_tx = nullptr;
    Result<std::unique_ptr<scfg::checkout::Server>> evm_made = make(evm);
    if (!t.check(evm_made.ok(), "an EVM page")) return;
    r.srv.reset(static_cast<ls::CheckoutServer*>(evm_made.take().release()));
    if (!t.check(r.srv->start().ok(), "started")) return;
    SCFG_CHECK_EQ(t, status_of(r, r.head("POST", "build", body.size()) + body), 404);
    r.srv->stop();
    // a page without a clock: the factory's Err, told to on_error first
    ls::Options broken = r.options();
    broken.clock = nullptr;
    Result<std::unique_ptr<scfg::checkout::Server>> refused = ls::factory(broken)(args);
    SCFG_CHECK(t, !refused.ok() && refused.err().kind == "TypeError");
    SCFG_CHECK(t, !r.errors->empty() && r.errors->back() == refused.err().str());
}

SCFG_TEST(LocalServerTests, AClockAndAListenAreRequired) {
    Rig r;
    ls::Options no_clock = r.options();
    no_clock.clock = nullptr;
    const Result<std::unique_ptr<ls::Handler>> handler = ls::Handler::make(no_clock);
    SCFG_CHECK(t, !handler.ok() && handler.err().kind == "TypeError");
    SCFG_CHECK(t, !ls::CheckoutServer::make(no_clock).ok());
    ls::Options no_listen = r.options();
    no_listen.listen = nullptr;
    const Result<std::unique_ptr<ls::CheckoutServer>> server = ls::CheckoutServer::make(no_listen);
    SCFG_CHECK(t, !server.ok() && server.err().kind == "TypeError");
    ls::Options handler_only = r.options();
    handler_only.listen = nullptr;  // the Handler needs no listener
    SCFG_CHECK(t, ls::Handler::make(handler_only).ok());
}
