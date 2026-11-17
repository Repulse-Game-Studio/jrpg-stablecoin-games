// No Python twin: the JSON-lines driver that runs the C++ Engine as a child process for the Python integration
// tests (tests/integration/cpp_driver.py), so the same Anvil / solana-test-validator fixtures exercise every core.
// A port of unity/core-driver/Program.cs.
//
// Protocol: rpgmaker/test/integration/engine_driver.ts. One JSON command per stdin line, one JSON reply per stdout
// line, in order: {"ok": <result>} or {"error": {"type", "code", "message"}}. Nothing but replies goes to stdout;
// diagnostics go to stderr. Ops: init, transport_stats, start_session, report_hint, tick, reverify, restore_lookup,
// restore_apply, build_solana_payment, snapshot, set_receipt, clear_receipts. Results are encoded exactly like the
// Tier 2 `returns` / `snapshot` (core-tests/support/Tier2.h).
//
// Each command's Engine action runs in a Job over the blocking PosixHttpTransport, which completes every request
// inside post_json, so the action has finished when Job::start returns; the next line is read only after the reply.
#include "PosixHttpTransport.h"

#include "support/Tier2.h"

#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Config.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Py.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scfg_driver {

using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace tier2 = scfg_test::tier2;

inline constexpr std::string_view DRIVER_ERROR = "DriverError";

// Program.cs's DriverError: a malformed command, not an error of the core. Not a kind in Err.cpp's table, so it is
// never given to is_a.
Err driver_error(std::string message) {
    Err e;
    e.kind = std::string(DRIVER_ERROR);
    e.message = std::move(message);
    return e;
}

// A missing key reads as null (JavaScript's `cmd.key`), not as py::get's KeyError.
Json opt(const Json::Dict& d, std::string_view key) { return d.get(key); }

// The str at key, or TypeError("<key> must be a str").
Result<std::string> need_str(const Json::Dict& d, std::string_view key) {
    Json v = opt(d, key);
    if (!v.is_str()) return Err::type_error(std::string(key) + " must be a str");
    return v.as_str();
}

// The action's result: a blocking transport completes every request inside post_json, so the Job is done when
// start() returns. It is destroyed here, before the Engine and the transport (Co.h, "Teardown order").
template <class T>
Result<T> run_job(scfg::Co<T> work) {
    std::optional<Result<T>> out;
    {
        scfg::Job<T> job;
        job.start(std::move(work), [&out](Result<T> r) { out.emplace(std::move(r)); });
    }
    if (!out) return driver_error("the action suspended, yet the blocking transport answers every request at once");
    return std::move(*out);
}

// The fixture's registry monkeypatch (Program.cs's ApplyOverride): {network, key, rpc, confirmations?, tokens?}.
scfg::Status apply_override(const Json::Dict& o) {
    Json network = opt(o, "network");
    scfg::OrderedMap<scfg::chains::Chain>& registry =
        (network.is_str() && network.as_str() == "testnet") ? scfg::chains::TESTNET() : scfg::chains::MAINNET();
    Json key = opt(o, "key");
    const scfg::chains::Chain* base = key.is_str() ? registry.find(key.as_str()) : nullptr;
    if (base == nullptr) {
        SCFG_TRY(std::string shown, scfg::json::dumps(key));
        return driver_error("unknown chain " + shown);
    }
    Json rpc_json = opt(o, "rpc");
    if (!rpc_json.is_list()) return driver_error("override.rpc must be a list");
    std::vector<std::string> rpc;
    for (const Json& u : rpc_json.as_list()) {
        if (!u.is_str()) return driver_error("override.rpc must be a list of str");
        rpc.push_back(u.as_str());
    }
    Json conf = opt(o, "confirmations");
    std::optional<scfg::BigInt> confirmations;
    if (conf.is_int()) confirmations = conf.as_bigint();
    std::optional<scfg::OrderedMap<scfg::chains::Token>> tokens;
    Json t = opt(o, "tokens");
    if (t.is_dict() && !t.as_dict().empty()) {
        tokens.emplace();
        for (const auto& [name, address] : t.as_dict()) {
            if (!address.is_str()) return driver_error("override.tokens values must be str");
            SCFG_TRY(scfg::chains::Token token, scfg::chains::Token::make(address.as_str()));
            tokens->set(name, std::move(token));
        }
    }
    scfg::chains::Chain patched = base->with(std::move(rpc), std::move(confirmations), std::move(tokens));
    registry.set(key.as_str(), std::move(patched));
    return scfg::ok();
}

// Python's whole-second wall clock (the Tier 2 encoders refuse fractional timestamps).
int64_t unix_seconds() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void print_engine_error(const Err& e) {
    const std::string line = "engine error: " + e.str() + "\n";
    std::fputs(line.c_str(), stderr);
}

class Driver {
public:
    Driver() : transport_(std::make_unique<PosixHttpTransport>()) {}
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    // The Engine goes before the transport it refers to.
    ~Driver() { engine_.reset(); }

    Result<Json> run(const Json::Dict& cmd) {
        Json op_json = opt(cmd, "op");
        if (!op_json.is_str()) return driver_error("a command needs a string op");
        const std::string& op = op_json.as_str();
        if (op == "init") return init(cmd);
        if (op == "transport_stats") {
            return Json::dict({{"kind", Json("PosixHttpTransport")},
                               {"posts", Json(static_cast<int64_t>(transport_->posts()))}});
        }
        if (!engine_) return driver_error("init first");
        scfg::engine::Engine& engine = *engine_;
        if (op == "start_session") {
            Json opts = Json::dict();
            for (const auto& [k, v] : cmd) {
                if (k != "op" && k != "kind" && k != "chain") opts.as_dict().set(k, v);
            }
            SCFG_TRY(std::string kind, need_str(cmd, "kind"));
            SCFG_TRY(std::string chain, need_str(cmd, "chain"));
            SCFG_TRY(Json rec, run_job(engine.start_session(Json(kind), Json(chain), opts)));
            return tier2::encode_returns(rec);
        }
        if (op == "report_hint") {
            SCFG_TRY(std::string sid, need_str(cmd, "sid"));
            SCFG_TRY_VOID(engine.report_hint(Json(sid), opt(cmd, "tx")));
            return Json();
        }
        if (op == "tick") {
            SCFG_TRY(std::vector<scfg::engine::StepOutcome> outcomes, run_job(engine.tick()));
            return tier2::encode_returns(outcomes);
        }
        if (op == "reverify") {
            SCFG_TRY(Json removed, run_job(engine.reverify()));
            return tier2::encode_returns(removed);
        }
        if (op == "restore_lookup") {
            SCFG_TRY(std::string tx, need_str(cmd, "tx"));
            SCFG_TRY(std::vector<scfg::restore::RestoreCandidate> found, run_job(engine.restore_lookup(Json(tx))));
            return tier2::encode_returns(found);
        }
        if (op == "restore_apply") return restore_apply(engine, cmd);
        if (op == "build_solana_payment") {
            SCFG_TRY(std::string sid, need_str(cmd, "sid"));
            SCFG_TRY(std::string account, need_str(cmd, "account"));
            SCFG_TRY(Json built, run_job(engine.build_solana_payment(Json(sid), Json(account))));
            return tier2::encode_returns(built);
        }
        if (op == "snapshot") return tier2::snapshot_json(engine);
        if (op == "set_receipt") {
            SCFG_TRY(std::string key, need_str(cmd, "key"));
            Json receipts = engine.receipts();  // the Engine's own store (a shared handle)
            receipts.as_dict().set(key, opt(cmd, "receipt"));
            return Json();
        }
        if (op == "clear_receipts") {
            Json receipts = engine.receipts();
            for (const std::string& k : receipts.as_dict().keys()) receipts.as_dict().erase(k);
            return Json();
        }
        SCFG_TRY(std::string shown, scfg::json::dumps(op_json));
        return driver_error("unknown op " + shown);
    }

private:
    Result<Json> init(const Json::Dict& cmd) {
        Json o = opt(cmd, "override");
        if (o.is_dict()) {
            SCFG_TRY_VOID(apply_override(o.as_dict()));
        }
        Json custom = opt(cmd, "custom_chains");
        Json items = opt(cmd, "items");
        Json setup = Json::dict({{"config", opt(cmd, "config")},
                                 {"custom_chains", custom.is_null() ? Json::list() : custom},
                                 {"items", items.is_null() ? Json::list() : items}});
        SCFG_TRY(scfg::config::Config config, tier2::build_config_from(setup));
        // A failed init keeps the Engine it had; a new one gets a fresh transport (its post count starts at 0).
        engine_.reset();
        transport_ = std::make_unique<PosixHttpTransport>();
        engine_ = std::make_unique<scfg::engine::Engine>(std::move(config), *transport_, Json(), Json(), unix_seconds,
                                                         scfg::engine::RpcFactory{}, print_engine_error);
        return Json();
    }

    static Result<Json> restore_apply(scfg::engine::Engine& engine, const Json::Dict& cmd) {
        std::optional<std::map<int64_t, Json>> choices;
        Json cs = opt(cmd, "choices");
        if (cs.is_dict()) {
            choices.emplace();
            for (const auto& [k, v] : cs.as_dict()) {
                SCFG_TRY(scfg::BigInt index, scfg::py::py_int(k));
                if (index.fits_int64()) (*choices)[index.to_int64().value()] = v;  // beyond int64: names no candidate
            }
        }
        Json list = opt(cmd, "candidates");
        if (!list.is_list()) return Err::type_error("candidates must be a list");
        std::vector<scfg::restore::RestoreCandidate> candidates;
        for (const Json& c : list.as_list()) {
            if (!c.is_dict()) return Err::type_error("a candidate must be an object");
            SCFG_TRY(scfg::restore::RestoreCandidate candidate, tier2::candidate_from_json(c));
            candidates.push_back(std::move(candidate));
        }
        SCFG_TRY(Json added, engine.restore_apply(candidates, choices));
        return tier2::encode_returns(added);
    }

    std::unique_ptr<PosixHttpTransport> transport_;
    std::unique_ptr<scfg::engine::Engine> engine_;
};

// The error as JSON: its class, its code (ConfigError/RestoreError strings, RpcError numbers) or null.
Json describe(const Err& e) {
    Json code;
    if (e.kind != DRIVER_ERROR) {
        if (e.is_a("ConfigError") || e.is_a("RestoreError")) {
            code = Json(e.code);
        } else if (e.is_a("RpcError") && e.rpc_code) {
            code = Json(*e.rpc_code);
        }
    }
    return Json::dict({{"type", Json(e.kind)}, {"code", code}, {"message", Json(e.message)}});
}

// One reply line (without its newline) for one command line.
std::string reply_to(Driver& driver, std::string_view line) {
    Result<Json> result = [&]() -> Result<Json> {
        SCFG_TRY(Json cmd, scfg::json::loads(line));
        if (!cmd.is_dict()) {
            return driver_error("not a JSON object: " + std::string(line.substr(0, 200)));
        }
        return driver.run(cmd.as_dict());
    }();
    Json reply = result.ok() ? Json::dict({{"ok", result.value()}}) : Json::dict({{"error", describe(result.err())}});
    Result<std::string> text = scfg::json::dumps(reply);
    if (text.ok()) return text.take();
    // A result json.dumps refuses (an int of more than 4300 digits) is answered as that ValueError. An error reply
    // holds strs and an int64 code only, so its dumps cannot fail.
    return scfg::json::dumps(Json::dict({{"error", describe(text.err())}})).value();
}

bool blank(std::string_view line) {
    for (char c : line) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != '\v') return false;
    }
    return true;
}

}  // namespace scfg_driver

int main() {
    std::signal(SIGPIPE, SIG_IGN);  // a peer that hangs up is an EPIPE (TransportError), not the driver's death
    scfg_driver::Driver driver;
    std::string line;
    while (std::getline(std::cin, line)) {  // sequential: the next command waits for the previous reply
        if (scfg_driver::blank(line)) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string reply = scfg_driver::reply_to(driver, line);
        reply += '\n';
        std::fwrite(reply.data(), 1, reply.size(), stdout);
        std::fflush(stdout);
    }
    return 0;
}
