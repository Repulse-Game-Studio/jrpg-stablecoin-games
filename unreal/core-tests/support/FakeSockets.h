// No Python twin: in-memory sockets for local_server::CheckoutServer (LocalServer.h), the stand-in for the real ones
// that godot/tests/test_local_server.gd drives through its Recorder: every connection is a Pipe the test writes to
// and reads from as the client, every socket call the server makes is counted (a pump that waited on a socket would
// call it again and again), and nothing ever blocks. A fake clock stands in for the passing seconds.
#pragma once

#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/LocalServer.h"
#include "Scfg/Result.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace scfg_test::sockets {

// One connection: the client's end is the test, the server's is the FakeSocket the listener hands out.
struct Pipe {
    std::string to_server;        // written by the client, not yet read by the server
    std::string to_client;        // written by the server
    bool client_shut = false;     // the client closed its sending side (the server reads "closed" once it is empty)
    bool broken = false;          // the connection failed: reads are "closed", writes fail
    bool server_closed = false;   // the server destroyed its socket
    size_t capacity = SIZE_MAX;   // bytes one write may take (0: the socket is full)
    int calls = 0;                // socket calls the server made (reset by the tests)
    size_t largest_read = 0;      // the largest max a read asked for

    // Appends to what the server will read.
    void send(std::string_view bytes) { to_server.append(bytes); }
};

// The listening side: Listen() hands the server a FakeListener over this net.
struct Net {
    int port = 4242;
    bool listening = false;
    int listens = 0;
    std::string last_host;
    std::optional<scfg::Err> refuse;  // listen() fails with it
    std::deque<std::shared_ptr<Pipe>> backlog;
    std::deque<std::shared_ptr<Pipe>> all;

    // A new client in the backlog (it is accepted at the next pump), or null when nothing listens.
    std::shared_ptr<Pipe> connect();
};

// The server's Listen over a shared Net (the net must outlive the server).
scfg::local_server::Listen listen_on(std::shared_ptr<Net> net);

// A fake monotonic clock.
struct Clock {
    double now = 1000.0;
};

// An HTTP answer as the client reads it, or nothing while it is not complete.
struct Answer {
    int status = 0;
    std::map<std::string, std::string> headers;  // lowercase names
    std::string body;
};
// The first answer in what the server wrote (Content-Length bytes of body after the head), if it is all there.
std::optional<Answer> answer(const std::string& to_client);

// "<method> /c/<secret>/<sub> HTTP/1.1\r\nHost: 127.0.0.1:<port>\r\n[Content-Length: n\r\n]\r\n"
std::string head(const scfg::local_server::CheckoutServer& srv, std::string_view method, std::string_view sub,
                 std::optional<size_t> length = std::nullopt);

}  // namespace scfg_test::sockets
