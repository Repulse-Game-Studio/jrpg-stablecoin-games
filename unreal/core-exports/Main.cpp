// No Python twin: the export check of ruling R2-3. Linked against scfg_core_shared, the core built as a shared
// library with -fvisibility=hidden where only SCFGCORE_API is visible, as an Unreal module that depends on ScfgCore
// links against libUnrealEditor-ScfgCore.so. It calls into several of the core's files (Json, Amounts, Config,
// Engine, Co); a symbol it needs that lost its export is an undefined reference, so the build fails before ctest
// runs it. The run checks the answers too, so a link that succeeded by accident still fails the test.
#include "Scfg/Amounts.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Config.h"
#include "Scfg/Dec.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::Json;
using scfg::Result;

// Answers every request at once with eth_blockNumber 0x3e8 (a subclass of an exported interface, defined here).
struct Answering final : scfg::coio::Transport {
    int posts = 0;
    scfg::Future<Json> post_json(std::string url, Json payload, double timeout) override {
        (void)url, (void)payload, (void)timeout;
        ++posts;
        return scfg::Future<Json>::completed(Json::dict({{"jsonrpc", "2.0"}, {"id", 1}, {"result", "0x3e8"}}));
    }
};

static int failures = 0;

static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    Result<Json> doc = scfg::json::loads(R"({"price": "4.99", "n": 12345678901234567890})");
    check(doc.ok() && doc.value().is_dict(), "json::loads");
    if (doc.ok()) {
        Result<std::string> text = scfg::json::dumps(doc.value(), {.sort_keys = true});
        check(text.ok() && text.value() == R"({"n": 12345678901234567890, "price": "4.99"})", "json::dumps");
    }

    Result<scfg::Dec> price = scfg::amounts::parse_amount(Json("4.99"));
    check(price.ok() && price.value().str() == "4.99", "amounts::parse_amount");
    check(!scfg::amounts::parse_amount(Json("-1")).ok(), "amounts::parse_amount rejects -1");

    Result<scfg::config::Config> config = scfg::config::build_config(
        Json::dict({{"wallet", "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"}, {"chains", Json::list({"base"})},
                    {"testnet", true}}));
    check(config.ok(), "config::build_config");
    if (!config.ok()) return 1;
    Result<scfg::models::Item> item = scfg::config::add_item(
        config.value(), "chapter2", Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}));
    check(item.ok(), "config::add_item");

    Answering transport;
    scfg::engine::Engine engine(config.take(), transport, Json(), Json(), [] { return int64_t{1700000000}; });
    std::optional<Result<Json>> session;
    scfg::Job<Json> job;
    job.start(engine.start_session("unlock", "base", Json::dict({{"item", "chapter2"}})),
              [&session](Result<Json> r) { session.emplace(std::move(r)); });
    check(job.done() && session && session->ok() && transport.posts == 1, "Engine::start_session");
    Result<scfg::engine::Snapshot> snap = engine.snapshot();
    check(snap.ok() && snap.value().pending.as_dict().size() == 1 && !engine.running(), "Engine::snapshot");

    Result<std::unique_ptr<scfg::engine::Engine>> refused = scfg::engine::Engine::make(
        engine.config(), transport, Json::list({1}), Json(), [] { return int64_t{0}; });
    check(!refused.ok() && refused.err().kind == "TypeError", "Engine::make");

    std::printf("%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
