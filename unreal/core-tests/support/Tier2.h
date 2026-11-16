// No Python twin: the Tier 2 runner, replaying conformance/vectors/tier2/*.json against the C++ Engine. A mirror of
// conformance/python/scfg_conformance/replay.py (build_config_from, build_engine, run_action, encode_returns,
// snapshot_json, candidate_from_json, ScriptedEntropy, ReplayTransport, replay) and of unity/core-tests/Tier2.cs;
// conformance/README.md's "Tier 2" section is the contract.
//
// This file and JsonEq do not depend on the test runner (ruling R6): the console driver (task 14) links them too.
// replay() returns the first difference as a string (nullopt when the vector passes) and the *Tests.cpp files
// assert on it. Malformed recorded data is a difference too, never an abort.
//
// Two modes: Immediate (the transport completes each request inside post_json, as a blocking transport would) and
// Deferred (it queues them, and the runner pumps one queued request per loop turn until the action's Job is done,
// as Unreal's HTTP callbacks complete them on a later tick; a step that recorded calls must have suspended).
//
// What a recorded {"type": "other"} accepts, and what may be counted as an internal error: an Err of a Python kind
// (Err.cpp's table) other than NotImplementedError (a later task's stub), CancelledError and HostGone. An Err of a
// kind outside the table already fails the test through errors::misuse().
#pragma once

#include "Scfg/Coio.h"
#include "Scfg/Config.h"
#include "Scfg/Engine.h"
#include "Scfg/Entropy.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"
#include "Scfg/Watcher.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scfg_test::tier2 {

enum class ReplayMode { Immediate, Deferred };

// The vector groups whose task has landed (each task extends it; task 13 deletes it with pending_ids). The deferred
// rerun (Tier2Deferred.<id>) and LocaleTests.Tier2UnderEveryLocale cover exactly the ids ported() accepts.
std::vector<std::string> ported_prefixes();
// Vectors under a ported prefix that still need a later task, named one by one (none is skipped silently), with
// the reason: reverify or restore (task 13). Tier2.<id> still runs and fails for each of them;
// Tier2Tests.PendingIdsAreNamedAndStillFail checks that every one exists and still fails.
struct PendingId {
    std::string id;
    std::string reason;
};
std::vector<PendingId> pending_ids();
// id is under a ported prefix and not pending.
bool ported(std::string_view id);

// Hands out one step's recorded draws in order; any difference is a problem (and the Engine gets a placeholder of
// the right shape: token_bytes(n) is always exactly n bytes, Entropy's contract).
class ScriptedEntropy final : public scfg::Entropy {
public:
    // A step's "entropy" list; anything malformed is a problem.
    void load(const scfg::Json& draws);
    std::vector<uint8_t> token_bytes(int n) override;
    std::string token_hex(int n) override;
    scfg::Result<int64_t> randbelow(int64_t n) override;
    // "unconsumed entropy: [(fn, n), ...]" when draws are left.
    std::vector<std::string> leftover() const;

    std::vector<std::string> problems;

private:
    // The value of the next draw when it is (fn, n), else nullopt and a problem.
    std::optional<scfg::Json> take(std::string_view fn, int64_t n);
    std::vector<scfg::Json> draws_;
    size_t next_ = 0;
};

// Answers each request from the current step's recorded calls, as a multiset keyed by (url, method, params): any
// order, each recorded call used once, identical requests take their copies in recorded order. The JSON-RPC id is
// ignored; params match by json_eq (key order ignored, 1 equals 1.0, true is not 1). A recorded transport_error is
// the transport's failure (Err TransportError with its message and status); response_text is read with
// json::loads_text. An unrecorded request is a problem, answered with an AssertionError.
class ReplayTransport final : public scfg::coio::Transport {
public:
    explicit ReplayTransport(ReplayMode mode = ReplayMode::Immediate) : mode_(mode) {}
    ReplayTransport(const ReplayTransport&) = delete;
    ReplayTransport& operator=(const ReplayTransport&) = delete;
    // Drops (cancels) the requests still queued.
    ~ReplayTransport() override;

    ReplayMode mode() const { return mode_; }
    // A step's "calls" list (problems reset); anything malformed is a problem.
    void load(const scfg::Json& calls);
    scfg::Future<scfg::Json> post_json(std::string url, scfg::Json payload, double timeout) override;
    // "expected call not made: <url> <method> <params>" for each recorded call left unused.
    std::vector<std::string> leftover() const;

    // Deferred mode: the requests waiting for an answer, and completing the oldest one (false when none waits).
    size_t queued() const { return queue_.size(); }
    bool pump_one();
    // Requests made so far (both modes).
    size_t posts() const { return posts_; }

    std::vector<std::string> problems;
    // Called first on every request (a test's probe).
    std::function<void(const std::string& url, const scfg::Json& payload)> on_call;

private:
    struct Waiting {
        scfg::Promise<scfg::Json> promise;
        scfg::Result<scfg::Json> answer;
    };
    ReplayMode mode_;
    std::vector<scfg::Json> calls_;
    std::vector<bool> used_;
    std::deque<Waiting> queue_;
    size_t posts_ = 0;
};

// replay.py's decode_value: {"float": "<repr>"} (exactly that one key) is that float, anything else is itself.
// (tier1::decode_value's twin, kept here so the driver links this file without the Tier 1 runner.)
scfg::Result<scfg::Json> decode_value(const scfg::Json& v);

// build_chain for each custom chain, build_config, then add_item for each item, in order.
scfg::Result<scfg::config::Config> build_config_from(const scfg::Json& setup);
// The Engine of a vector's setup over transport. One clock drives the Engine and every RpcClient (its cooldowns
// too), and on_error sees every contained error. entropy and transport must outlive the Engine.
scfg::Result<std::unique_ptr<scfg::engine::Engine>> build_engine(const scfg::Json& setup,
                                                                  scfg::coio::Transport& transport,
                                                                  std::function<int64_t()> clock,
                                                                  std::function<void(const scfg::Err&)> on_error,
                                                                  scfg::Entropy* entropy);

// candidate_to_json: the candidate as plain JSON, its transfer tagged with its Python class ("Transfer" or
// "TransferLog"). A value at or beyond 2**53 is canonical.py's ValueError.
scfg::Result<scfg::Json> candidate_to_json(const scfg::restore::RestoreCandidate& c);
// candidate_from_json, its inverse: the KeyError of a missing field or an unknown class, the TypeError of a wrong
// set of transfer fields or of a field whose type the C++ dataclass cannot hold.
scfg::Result<scfg::restore::RestoreCandidate> candidate_from_json(const scfg::Json& d);

// canonical.to_jsonable of a JSON value: ints must stay below 2**53 in magnitude, floats must be integral (they are
// written as ints); a ValueError naming the path otherwise.
scfg::Result<scfg::Json> to_jsonable(const scfg::Json& v, const std::string& path = "");
// encode_returns of each action's result: a JSON result (start_session, build_solana_payment, reverify,
// restore_apply), tick's outcomes, restore_lookup's candidates (with their class tags).
scfg::Result<scfg::Json> encode_returns(const scfg::Json& result);
scfg::Result<scfg::Json> encode_returns(const std::vector<scfg::watcher::StepOutcome>& outcomes);
scfg::Result<scfg::Json> encode_returns(const std::vector<scfg::restore::RestoreCandidate>& candidates);
// pending, receipts, owned (sorted), tip_total (str), unready (sorted [chain, token]).
scfg::Result<scfg::Json> snapshot_json(const scfg::engine::Engine& engine);

// run_action's except clauses for an Err the action raised: {"type": "ConfigError", "code"}, {"type":
// "RestoreError", "code"}, {"type": "other"} for any other Python exception; nullopt for an Err no recorded raise
// stands for (NotImplementedError, CancelledError, HostGone).
std::optional<scfg::Json> raises_json(const scfg::Err& e);
// The Err may be an Exception Python's core raised or contained (raises "other", internal_errors).
bool stands_for_python(const scfg::Err& e);

// The vector file's text as Python's json.load reads it.
scfg::Result<scfg::Json> parse(std::string_view text);

// Deferred mode's own checks of one step, once its action has run (nullopt in immediate mode or when they pass): a
// step with recorded calls must have suspended at least once, and no request may be left in the queue.
std::optional<std::string> deferred_problem(const ReplayTransport& transport, const scfg::Json& calls, bool suspended);

// Re-run a Tier 2 vector: nullopt when it agrees, otherwise "step {i} ({action}): ..." for the first difference.
std::optional<std::string> replay(const scfg::Json& vector, ReplayMode mode = ReplayMode::Immediate);
// The same over a given transport (its mode decides), for tests that probe the requests.
std::optional<std::string> replay(const scfg::Json& vector, ReplayTransport& transport);

}  // namespace scfg_test::tier2
