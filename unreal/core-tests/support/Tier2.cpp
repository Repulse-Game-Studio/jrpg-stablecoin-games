// No Python twin: the Tier 2 runner (see Tier2.h).
#include "support/Tier2.h"

#include "support/JsonEq.h"

#include "Scfg/Backends.h"
#include "Scfg/BigInt.h"
#include "Scfg/Bytes.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Evm.h"
#include "Scfg/Models.h"
#include "Scfg/Py.h"
#include "Scfg/PyFloat.h"
#include "Scfg/Transfers.h"

#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

using scfg::BigInt;
using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace py = scfg::py;
namespace engine = scfg::engine;
namespace restore = scfg::restore;

namespace scfg_test::tier2::detail {

std::string number(size_t n) { return std::to_string(n); }

// json.dumps of v for a message, cut to about 300 bytes (or the error dumps raised, in <>).
std::string show(const Json& v) {
    Result<std::string> text = scfg::json::dumps(v, {.sort_keys = true, .ensure_ascii = false});
    if (!text.ok()) return "<" + text.err().str() + ">";
    std::string s = text.take();
    if (s.size() <= 300) return s;
    size_t cut = 297;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

// str(v) or repr(v) for a message (or the error they raised, in <>).
std::string text(const Result<std::string>& r) { return r.ok() ? r.value() : "<" + r.err().str() + ">"; }

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += sep;
        out += parts[i];
    }
    return out;
}

// _decoded(d): {k: decode_value(v)}; d.items() of a non-dict is its AttributeError.
Result<Json> decoded(const Json& d) {
    if (!d.is_dict()) return Err::make("AttributeError", "'" + std::string(d.type_name()) + "' object has no attribute 'items'");
    Json out = Json::dict();
    for (const auto& [key, value] : d.as_dict()) {
        SCFG_TRY(Json v, decode_value(value));
        out.as_dict().set(key, std::move(v));
    }
    return out;
}

// kw.pop(key): the value, removed, or the KeyError.
Result<Json> pop(Json& kw, std::string_view key) {
    const Json* found = kw.as_dict().find(key);
    if (found == nullptr) return py::key_error(key);
    Json v = *found;
    kw.as_dict().erase(key);
    return v;
}

Result<std::string> str_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_str()) return Err::type_error(std::string(key) + " must be a str, not " + std::string(v.type_name()));
    return v.as_str();
}

Result<std::optional<std::string>> str_or_none_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (v.is_null()) return std::optional<std::string>();
    if (!v.is_str()) return Err::type_error(std::string(key) + " must be a str or None, not " + std::string(v.type_name()));
    return std::optional<std::string>(v.as_str());
}

Result<BigInt> int_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_int()) {
        SCFG_TRY(std::string shown, py::repr(v));
        return Err::type_error(std::string(key) + " must be an int, not " + shown);
    }
    return v.as_bigint();
}

Json optional_text(const std::optional<std::string>& s) { return s ? Json(*s) : Json(); }

Json payment_json(const scfg::models::PaymentResult& r) {
    return Json::dict({{"status", Json(r.status)},
                       {"kind", r.kind},
                       {"item", r.item},
                       {"amount", r.amount ? Json(r.amount->str()) : Json()},
                       {"token", r.token},
                       {"chain", r.chain},
                       {"tx_hash", r.tx_hash},
                       {"payer", r.payer},
                       {"session_id", r.session_id},
                       {"error", optional_text(r.error)}});
}

Json outcome_json(const scfg::watcher::StepOutcome& o) {
    return Json::dict({{"session_id", Json(o.session_id)},
                       {"status", Json(o.status)},
                       {"confirmations", Json(o.confirmations)},
                       {"required", Json(o.required)},
                       {"result", o.result ? payment_json(*o.result) : Json()},
                       {"error", optional_text(o.error)},
                       {"hint_tx", o.hint_tx}});
}

// The candidate's dataclass fields as JSON, before canonical's checks.
Json candidate_raw(const restore::RestoreCandidate& c) {
    Json transfer;
    if (const auto* log = std::get_if<scfg::evm::TransferLog>(&c.transfer)) {
        transfer = Json::dict({{"token", Json(log->token)},
                               {"sender", Json(log->sender)},
                               {"recipient", Json(log->recipient)},
                               {"value", Json(log->value)},
                               {"tx_hash", Json(log->tx_hash)},
                               {"log_index", Json(log->log_index)},
                               {"block_number", Json(log->block_number)},
                               {"block_hash", Json(log->block_hash)},
                               {"class", "TransferLog"}});
    } else {
        const auto& t = std::get<scfg::transfers::Transfer>(c.transfer);
        transfer = Json::dict({{"token", Json(t.token)},
                               {"sender", t.sender},
                               {"recipient", Json(t.recipient)},
                               {"value", Json(t.value)},
                               {"tx_hash", t.tx_hash},
                               {"log_index", t.log_index},
                               {"block_number", Json(t.block_number)},
                               {"class", "Transfer"}});
    }
    Json choices = Json::list();
    for (const std::string& choice : c.choices) choices.as_list().push_back(Json(choice));
    return Json::dict({{"chain", Json(c.chain)},
                       {"token", Json(c.token)},
                       {"transfer", transfer},
                       {"kind", optional_text(c.kind)},
                       {"item", optional_text(c.item)},
                       {"choices", choices},
                       {"already_restored", Json(c.already_restored)}});
}

// A Job driving one action's coroutine: its result (nullopt when it is waiting with nothing queued), and whether it
// suspended at all.
template <class T>
struct Driven {
    std::optional<Result<T>> result;
    bool suspended = false;
};

template <class T>
Driven<T> drive(scfg::Co<T> work, ReplayTransport& transport) {
    Driven<T> d;
    {
        scfg::Job<T> job;
        job.start(std::move(work), [&d](Result<T> r) { d.result.emplace(std::move(r)); });
        d.suspended = !job.done();
        while (!job.done() && transport.pump_one()) {
        }
    }  // the Job goes before the Engine: an unfinished one is cancelled here
    return d;
}

struct ActionOutcome {
    Json returns;
    Json raises;
    std::optional<std::string> failure;  // a difference the runner itself found (not the vector's raises)
    bool suspended = false;
};

// run_action's except clauses.
void raised(ActionOutcome& out, const Err& e) {
    std::optional<Json> r = raises_json(e);
    if (r) {
        out.raises = std::move(*r);
    } else {
        out.failure = "raised an Err that no recorded raise stands for: " + e.str();
    }
}

ActionOutcome raising(const Err& e) {
    ActionOutcome out;
    raised(out, e);
    return out;
}

void returned(ActionOutcome& out, Result<Json> encoded) {
    if (encoded.ok()) {
        out.returns = encoded.take();
    } else {
        out.failure = "returns: " + encoded.err().message;
    }
}

template <class T, class Encode>
void finish(ActionOutcome& out, Driven<T> d, Encode encode) {
    out.suspended = d.suspended;
    if (!d.result) {
        out.failure = "the action is waiting, but no request is queued to answer";
        return;
    }
    if (!d.result->ok()) {
        raised(out, d.result->err());
        return;
    }
    returned(out, encode(d.result->value()));
}

void finish_plain(ActionOutcome& out, const scfg::Status& s) {
    if (!s.ok()) raised(out, s.err());
}

// steps[from_step] of the steps before this one, as Python indexes a list (a negative index counts from the end).
Result<Json> prior_step(const Json::List& steps, size_t index, const Json& from) {
    if (!from.is_int() && !from.is_bool()) {
        return Err::type_error("list indices must be integers or slices, not " + std::string(from.type_name()));
    }
    SCFG_TRY(BigInt k, py::index(from));
    if (k < BigInt(0)) k += BigInt(static_cast<int64_t>(index));
    if (k < BigInt(0) || k >= BigInt(static_cast<int64_t>(index))) return Err::index_error("list index out of range");
    return steps[static_cast<size_t>(k.to_int64().value())];
}

// restore_apply's arguments, as run_action reads them: the candidates recorded as returns of step from_step, and
// {int(k): v} of choices (a key beyond int64 is dropped: it cannot name a candidate).
struct ApplyArgs {
    std::vector<restore::RestoreCandidate> candidates;
    std::optional<std::map<int64_t, Json>> choices;
};

Result<ApplyArgs> apply_args(const Json& kw, const Json::List& steps, size_t index) {
    ApplyArgs out;
    SCFG_TRY(Json from, py::get(kw, "from_step"));
    SCFG_TRY(Json prior, prior_step(steps, index, from));
    SCFG_TRY(Json recorded, py::get(prior, "returns"));
    SCFG_TRY(std::vector<Json> each, py::iterate(recorded));
    for (const Json& c : each) {
        SCFG_TRY(restore::RestoreCandidate candidate, candidate_from_json(c));
        out.candidates.push_back(std::move(candidate));
    }
    SCFG_TRY(Json choices, py::get(kw, "choices"));
    if (!choices.is_null()) {
        if (!choices.is_dict()) {
            return Err::make("AttributeError", "'" + std::string(choices.type_name()) + "' object has no attribute 'items'");
        }
        std::map<int64_t, Json> by_index;
        for (const auto& [key, value] : choices.as_dict()) {
            SCFG_TRY(BigInt k, py::py_int(key));
            if (k.fits_int64()) by_index[k.to_int64().value()] = value;
        }
        out.choices = std::move(by_index);
    }
    return out;
}

Result<Json> encode_json(const Json& r) { return encode_returns(r); }
Result<Json> encode_outcomes(const std::vector<scfg::watcher::StepOutcome>& r) { return encode_returns(r); }
Result<Json> encode_candidates(const std::vector<restore::RestoreCandidate>& r) { return encode_returns(r); }

// replay.py's run_action: one action, its raise classified as the vector records it.
ActionOutcome run_action(engine::Engine& eng, const std::string& action, const Json& args, const Json::List& steps,
                         size_t index, ReplayTransport& transport) {
    ActionOutcome out;
    Result<Json> decoded_args = decoded(args);  // outside the try in Python: a malformed vector
    if (!decoded_args.ok()) {
        out.failure = "args: " + decoded_args.err().str();
        return out;
    }
    Json kw = decoded_args.take();
    auto arg = [&kw](std::string_view key) { return py::get(kw, key); };
    if (action == "start_session") {
        Result<Json> kind = pop(kw, "kind");
        if (!kind.ok()) return raising(kind.err());
        Result<Json> chain = pop(kw, "chain");
        if (!chain.ok()) return raising(chain.err());
        finish(out, drive(eng.start_session(kind.take(), chain.take(), kw), transport), encode_json);
    } else if (action == "build_solana_payment") {
        Result<Json> sid = arg("sid");
        if (!sid.ok()) return raising(sid.err());
        Result<Json> account = arg("account");
        if (!account.ok()) return raising(account.err());
        finish(out, drive(eng.build_solana_payment(sid.take(), account.take()), transport), encode_json);
    } else if (action == "report_hint" || action == "set_ui_status") {
        Result<Json> sid = arg("sid");
        if (!sid.ok()) return raising(sid.err());
        Result<Json> second = arg(action == "report_hint" ? "tx" : "status");
        if (!second.ok()) return raising(second.err());
        finish_plain(out, action == "report_hint" ? eng.report_hint(sid.value(), second.value())
                                                  : eng.set_ui_status(sid.value(), second.value()));
    } else if (action == "discard_session") {
        Result<Json> sid = arg("sid");
        if (!sid.ok()) return raising(sid.err());
        finish_plain(out, eng.discard_session(sid.value()));
    } else if (action == "tick") {
        finish(out, drive(eng.tick(), transport), encode_outcomes);
    } else if (action == "reverify") {
        finish(out, drive(eng.reverify(), transport), encode_json);
    } else if (action == "restore_lookup") {
        Result<Json> tx = arg("tx");
        if (!tx.ok()) return raising(tx.err());
        finish(out, drive(eng.restore_lookup(tx.take()), transport), encode_candidates);
    } else if (action == "restore_apply") {
        Result<ApplyArgs> a = apply_args(kw, steps, index);
        if (!a.ok()) return raising(a.err());
        Result<Json> added = eng.restore_apply(a.value().candidates, a.value().choices);
        if (!added.ok()) return raising(added.err());
        returned(out, encode_returns(added.value()));
    } else {
        raised(out, Err::value_error("unknown action " + py::repr_str(action)));
    }
    return out;
}

// destroy_engine_while_suspended's last step: starts work in a Job, answers `answered` of its requests, then destroys
// the Engine while the Job is suspended, and the Job after it.
template <class T>
std::optional<std::string> drop_engine_first(scfg::Co<T> work, ReplayTransport& transport, size_t answered,
                                             std::unique_ptr<engine::Engine>& eng) {
    std::optional<scfg::Job<T>> job(std::in_place);
    bool reported = false;
    job->start(std::move(work), [&reported](Result<T>) { reported = true; });
    for (size_t k = 0; k < answered && !job->done(); ++k) {
        if (!transport.pump_one()) return "nothing queued after " + std::to_string(k) + " answer(s)";
    }
    if (job->done()) return "the action finished after " + std::to_string(answered) + " answer(s)";
    if (transport.queued() == 0) return "the action is waiting with nothing queued";
    eng.reset();  // the Engine goes first (N1)
    job.reset();  // then its suspended command: only destructors run
    if (reported) return std::string("a destroyed action reported a result");
    return std::nullopt;
}

// The network actions of run_action, started for drop_engine_first.
std::optional<std::string> probe_action(std::unique_ptr<engine::Engine>& eng, const std::string& action, const Json& args,
                                        ReplayTransport& transport, size_t answered) {
    Result<Json> decoded_args = decoded(args);
    if (!decoded_args.ok()) return "args: " + decoded_args.err().str();
    Json kw = decoded_args.take();
    if (action == "start_session") {
        Result<Json> kind = pop(kw, "kind");
        Result<Json> chain = pop(kw, "chain");
        if (!kind.ok() || !chain.ok()) return std::string("start_session without kind or chain");
        return drop_engine_first(eng->start_session(kind.take(), chain.take(), kw), transport, answered, eng);
    }
    if (action == "build_solana_payment") {
        Result<Json> sid = py::get(kw, "sid");
        Result<Json> account = py::get(kw, "account");
        if (!sid.ok() || !account.ok()) return std::string("build_solana_payment without sid or account");
        return drop_engine_first(eng->build_solana_payment(sid.take(), account.take()), transport, answered, eng);
    }
    if (action == "tick") return drop_engine_first(eng->tick(), transport, answered, eng);
    if (action == "reverify") return drop_engine_first(eng->reverify(), transport, answered, eng);
    if (action == "restore_lookup") {
        Result<Json> tx = py::get(kw, "tx");
        if (!tx.ok()) return std::string("restore_lookup without tx");
        return drop_engine_first(eng->restore_lookup(tx.take()), transport, answered, eng);
    }
    return action + " makes no request";
}

}  // namespace scfg_test::tier2::detail

namespace scfg_test::tier2 {

std::optional<std::string> deferred_problem(const ReplayTransport& transport, const Json& calls, bool suspended) {
    if (transport.mode() != ReplayMode::Deferred) return std::nullopt;
    if (transport.queued() != 0) {
        return std::to_string(transport.queued()) + " request(s) left queued after the action finished (deferred mode)";
    }
    if (calls.is_list() && !calls.as_list().empty() && !suspended) {
        return std::string("the step's requests were answered, yet the action never suspended (deferred mode)");
    }
    return std::nullopt;
}

// ---- entropy ----

void ScriptedEntropy::load(const Json& draws) {
    draws_.clear();
    next_ = 0;
    problems.clear();
    if (!draws.is_list()) {
        problems.push_back("entropy is not a list: " + detail::show(draws));
        return;
    }
    draws_ = draws.as_list();
}

std::optional<Json> ScriptedEntropy::take(std::string_view fn, int64_t n) {
    const std::string drew = std::string(fn) + "(" + std::to_string(n) + ")";
    if (next_ >= draws_.size()) {
        problems.push_back("entropy exhausted at " + drew);
        return std::nullopt;
    }
    const Json& draw = draws_[next_++];
    if (!draw.is_dict() || !draw.as_dict().contains("value")) {
        problems.push_back("an entropy draw is malformed: " + detail::show(draw));
        return std::nullopt;
    }
    const Json dfn = draw.as_dict().get("fn");
    const Json dn = draw.as_dict().get("n");
    if (!dfn.is_str() || dfn.as_str() != fn || !dn.is_int64() || dn.as_int64() != n) {
        problems.push_back("entropy mismatch: drew " + drew + ", recorded " + detail::text(py::str(dfn)) + "(" +
                           detail::text(py::str(dn)) + ")");
        return std::nullopt;
    }
    return draw.as_dict().get("value");
}

std::vector<uint8_t> ScriptedEntropy::token_bytes(int n) {
    const size_t want = n > 0 ? static_cast<size_t>(n) : 0;
    std::optional<Json> v = take("token_bytes", n);
    if (v) {
        Result<std::vector<uint8_t>> b = v->is_str() ? scfg::bytes::from_hex(v->as_str())
                                                     : Result<std::vector<uint8_t>>(Err::type_error("not a str"));
        if (b.ok() && b.value().size() == want) return b.take();
        problems.push_back("a token_bytes(" + std::to_string(n) + ") value is not " + std::to_string(want) +
                           " bytes of hex: " + detail::show(*v));
    }
    return std::vector<uint8_t>(want, 0);  // Entropy's contract holds even for a step that already failed
}

std::string ScriptedEntropy::token_hex(int n) {
    std::optional<Json> v = take("token_hex", n);
    if (v && v->is_str()) return v->as_str();  // as recorded, as Python's ScriptedEntropy returns it
    if (v) problems.push_back("a token_hex value is not a str: " + detail::show(*v));
    return std::string(n > 0 ? 2 * static_cast<size_t>(n) : 0, '0');
}

Result<int64_t> ScriptedEntropy::randbelow(int64_t n) {
    std::optional<Json> v = take("randbelow", n);
    if (v && v->is_int64()) return v->as_int64();
    if (v) problems.push_back("a randbelow value is not an int: " + detail::show(*v));
    return Err::make("AssertionError", problems.back());
}

std::vector<std::string> ScriptedEntropy::leftover() const {
    if (next_ >= draws_.size()) return {};
    std::string left;
    for (size_t i = next_; i < draws_.size(); ++i) {
        const Json& d = draws_[i];
        Json fn = d.is_dict() ? d.as_dict().get("fn") : Json();
        Json n = d.is_dict() ? d.as_dict().get("n") : Json();
        left += (left.empty() ? "(" : ", (") + detail::text(py::repr(fn)) + ", " + detail::text(py::repr(n)) + ")";
    }
    return {"unconsumed entropy: [" + left + "]"};
}

// ---- transport ----

ReplayTransport::~ReplayTransport() {
    std::deque<Waiting> dropped;
    dropped.swap(queue_);  // cancelled (CancelledError) as dropped goes, after the queue is already empty
}

void ReplayTransport::load(const Json& calls) {
    calls_.clear();
    used_.clear();
    problems.clear();
    if (!calls.is_list()) {
        problems.push_back("calls is not a list: " + detail::show(calls));
        return;
    }
    for (const Json& c : calls.as_list()) {
        const bool well_formed = c.is_dict() && c.as_dict().get("url").is_str() && c.as_dict().get("method").is_str() &&
                                 c.as_dict().contains("params") &&
                                 (c.as_dict().contains("response_text") || c.as_dict().contains("transport_error"));
        if (!well_formed) {
            problems.push_back("a recorded call is malformed: " + detail::show(c));
            continue;
        }
        calls_.push_back(c);
    }
    used_.assign(calls_.size(), false);
}

scfg::Future<Json> ReplayTransport::post_json(std::string url, Json payload, double timeout) {
    (void)timeout;
    ++posts_;
    if (on_call) on_call(url, payload);
    const Json method = payload.is_dict() ? payload.as_dict().get("method") : Json();
    const Json params = payload.is_dict() ? payload.as_dict().get("params") : Json();
    std::optional<Result<Json>> answer;
    for (size_t i = 0; i < calls_.size() && !answer; ++i) {
        const Json::Dict& c = calls_[i].as_dict();
        if (used_[i] || c.get("url").as_str() != url || !json_eq(c.get("method"), method) ||
            !json_eq(c.get("params"), params)) {
            continue;
        }
        used_[i] = true;
        if (const Json* failure = c.find("transport_error")) {
            const Json message = failure->is_dict() ? failure->as_dict().get("message") : Json();
            const Json status = failure->is_dict() ? failure->as_dict().get("status") : Json();
            std::optional<int> code;
            if (status.is_int64() && status.as_int64() >= INT32_MIN && status.as_int64() <= INT32_MAX) {
                code = static_cast<int>(status.as_int64());
            } else if (!status.is_null()) {
                problems.push_back("a recorded transport_error has a malformed status: " + detail::show(*failure));
            }
            if (!message.is_str()) problems.push_back("a recorded transport_error has no message: " + detail::show(*failure));
            answer = Err::transport_error(message.is_str() ? message.as_str() : "", code);
        } else {
            const Json text = c.get("response_text");
            Result<Json> parsed = text.is_str() ? scfg::json::loads_text(text.as_str())
                                                : Result<Json>(Err::type_error("response_text is not a str"));
            if (!parsed.ok()) {
                problems.push_back("a recorded response_text cannot be read: " + parsed.err().message);
                answer = Err::make("AssertionError", problems.back());
            } else {
                answer = std::move(parsed);
            }
        }
    }
    if (!answer) {
        problems.push_back("unexpected call " + url + " " + detail::text(py::str(method)) + " " + detail::show(params));
        answer = Err::make("AssertionError", problems.back());
    }
    if (fault) {
        if (std::optional<Err> e = fault(url, payload)) answer = std::move(*e);
    }
    if (mode_ == ReplayMode::Immediate) return scfg::Future<Json>::completed(std::move(*answer));
    scfg::Promise<Json> promise;
    scfg::Future<Json> future = promise.get_future();
    queue_.push_back(Waiting{std::move(promise), std::move(*answer)});
    return future;
}

bool ReplayTransport::pump_one() {
    if (queue_.empty()) return false;
    Waiting w = std::move(queue_.front());
    queue_.pop_front();
    w.promise.complete(std::move(w.answer));  // may queue the next request before it returns
    return true;
}

std::vector<std::string> ReplayTransport::leftover() const {
    std::vector<std::string> out;
    for (size_t i = 0; i < calls_.size(); ++i) {
        if (used_[i]) continue;
        const Json::Dict& c = calls_[i].as_dict();
        out.push_back("expected call not made: " + c.get("url").as_str() + " " + c.get("method").as_str() + " " +
                      detail::show(c.get("params")));
    }
    return out;
}

// ---- values, config and engine ----

Result<Json> decode_value(const Json& v) {
    if (!v.is_dict() || v.as_dict().size() != 1 || !v.as_dict().contains("float")) return v;
    const Json& x = *v.as_dict().find("float");
    if (x.is_float()) return x;
    if (x.is_bool()) return Json(x.as_bool() ? 1.0 : 0.0);
    if (x.is_int()) {  // float(int): correctly rounded, an OverflowError beyond the doubles
        std::optional<double> f = scfg::pyfloat::parse(x.as_bigint().to_decimal());
        if (!f || std::isinf(*f)) return Err::overflow_error("int too large to convert to float");
        return Json(*f);
    }
    if (!x.is_str()) {
        return Err::type_error("float() argument must be a string or a number, not '" + std::string(x.type_name()) + "'");
    }
    std::optional<double> f = scfg::pyfloat::parse(x.as_str());
    if (!f) return Err::value_error("could not convert string to float: " + py::repr_str(x.as_str()));
    return Json(*f);
}

Result<scfg::config::Config> build_config_from(const Json& setup) {
    SCFG_TRY(Json config_kw, py::get(setup, "config"));
    SCFG_TRY(Json kw, detail::decoded(config_kw));
    SCFG_TRY(Json custom, py::get(setup, "custom_chains"));
    std::vector<scfg::config::CustomChain> custom_chains;
    const bool has_custom = py::truthy(custom);
    if (has_custom) {
        SCFG_TRY(std::vector<Json> entries, py::iterate(custom));
        for (const Json& entry : entries) {
            SCFG_TRY(Json chain_kw, detail::decoded(entry));
            SCFG_TRY(scfg::chains::Chain chain, scfg::config::build_chain(chain_kw));
            custom_chains.emplace_back(std::move(chain));
        }
        kw.as_dict().erase("custom_chains");  // kw["custom_chains"] = [...] replaces it
    }
    Result<scfg::config::Config> built = has_custom ? scfg::config::build_config(kw, std::move(custom_chains))
                                                    : scfg::config::build_config(kw);
    SCFG_TRY(scfg::config::Config config, std::move(built));
    SCFG_TRY(Json items, py::get(setup, "items"));
    SCFG_TRY(std::vector<Json> each, py::iterate(items));
    for (const Json& item : each) {
        SCFG_TRY(Json item_kw, detail::decoded(item));
        SCFG_TRY(Json item_id, detail::pop(item_kw, "item_id"));
        SCFG_TRY(scfg::models::Item added, scfg::config::add_item(config, item_id, item_kw));
        (void)added;
    }
    return config;
}

Result<std::unique_ptr<engine::Engine>> build_engine(const Json& setup, scfg::coio::Transport& transport,
                                                     std::function<int64_t()> clock,
                                                     std::function<void(const Err&)> on_error,
                                                     scfg::Entropy* entropy) {
    SCFG_TRY(scfg::config::Config config, build_config_from(setup));
    SCFG_TRY(Json pending, py::get(setup, "pending"));
    SCFG_TRY(Json receipts, py::get(setup, "receipts"));
    if (!pending.is_dict() || !receipts.is_dict()) return Err::type_error("setup pending and receipts must be dicts");
    std::function<double()> rpc_clock = [clock]() { return static_cast<double>(clock()); };
    engine::RpcFactory factory = [&transport, rpc_clock](const scfg::chains::Chain& chain) -> Result<scfg::backends::base::Rpc> {
        SCFG_TRY(const scfg::backends::base::Backend* backend, scfg::backends::backend_for(chain));
        SCFG_TRY(scfg::backends::base::Rpc rpc, backend->make_rpc(chain, transport));
        scfg::backends::base::client_of(rpc).clock = rpc_clock;  // SolanaRpc wraps its RpcClient
        return rpc;
    };
    return std::make_unique<engine::Engine>(std::move(config), transport, scfg::deep_copy(pending),
                                            scfg::deep_copy(receipts), std::move(clock), std::move(factory),
                                            std::move(on_error), entropy);
}

// ---- restore candidates ----

Result<Json> candidate_to_json(const restore::RestoreCandidate& c) { return to_jsonable(detail::candidate_raw(c)); }

Result<restore::RestoreCandidate> candidate_from_json(const Json& d) {
    SCFG_TRY(Json raw, py::get(d, "transfer"));
    if (!raw.is_dict()) return Err::type_error("'" + std::string(raw.type_name()) + "' object is not a mapping");
    Json fields = scfg::deep_copy(raw);  // dict(d["transfer"])
    SCFG_TRY(Json cls, detail::pop(fields, "class"));
    SCFG_TRY(std::optional<std::string> name, py::str_key(cls));
    const bool is_log = name && *name == "TransferLog";
    if (!name || (*name != "Transfer" && !is_log)) return py::key_error_for(cls);  // _TRANSFER_CLASSES[cls]
    std::vector<std::string_view> want = {"token", "sender", "recipient", "value", "tx_hash", "log_index", "block_number"};
    if (is_log) want.push_back("block_hash");
    bool exact = fields.as_dict().size() == want.size();
    for (std::string_view k : want) exact = exact && fields.as_dict().contains(k);
    if (!exact) return Err::type_error(*name + "() got the wrong fields: " + detail::show(fields));
    SCFG_TRY(Json value, py::get(fields, "value"));
    if (!value.is_int()) return Err::type_error("value must be an int, not " + std::string(value.type_name()));
    restore::RestoreCandidate c;
    if (is_log) {
        scfg::evm::TransferLog t;
        SCFG_TRY(t.token, detail::str_field(fields, "token"));
        SCFG_TRY(t.sender, detail::str_field(fields, "sender"));
        SCFG_TRY(t.recipient, detail::str_field(fields, "recipient"));
        t.value = value.as_bigint();
        SCFG_TRY(t.tx_hash, detail::str_field(fields, "tx_hash"));
        SCFG_TRY(t.log_index, detail::int_field(fields, "log_index"));
        SCFG_TRY(t.block_number, detail::int_field(fields, "block_number"));
        SCFG_TRY(t.block_hash, detail::str_field(fields, "block_hash"));
        c.transfer = std::move(t);
    } else {
        // Python's Transfer carries the node's values as they are: the sender, tx_hash and log_index as recorded.
        scfg::transfers::Transfer t;
        SCFG_TRY(t.token, detail::str_field(fields, "token"));
        SCFG_TRY(t.sender, py::get(fields, "sender"));
        SCFG_TRY(t.recipient, detail::str_field(fields, "recipient"));
        t.value = value.as_bigint();
        SCFG_TRY(t.tx_hash, py::get(fields, "tx_hash"));
        SCFG_TRY(t.log_index, py::get(fields, "log_index"));
        SCFG_TRY(t.block_number, detail::int_field(fields, "block_number"));
        c.transfer = std::move(t);
    }
    SCFG_TRY(c.chain, detail::str_field(d, "chain"));
    SCFG_TRY(c.token, detail::str_field(d, "token"));
    SCFG_TRY(c.kind, detail::str_or_none_field(d, "kind"));
    SCFG_TRY(c.item, detail::str_or_none_field(d, "item"));
    SCFG_TRY(Json choices, py::get(d, "choices"));
    SCFG_TRY(std::vector<Json> each, py::iterate(choices));  // tuple(d["choices"])
    for (const Json& choice : each) {
        if (!choice.is_str()) return Err::type_error("a choice must be a str, not " + std::string(choice.type_name()));
        c.choices.push_back(choice.as_str());
    }
    SCFG_TRY(Json restored, py::get(d, "already_restored"));
    if (!restored.is_bool()) return Err::type_error("already_restored must be a bool, not " + std::string(restored.type_name()));
    c.already_restored = restored.as_bool();
    return c;
}

// ---- encoders ----

Result<Json> to_jsonable(const Json& v, const std::string& path) {
    const std::string where = path.empty() ? "<root>" : path;
    auto check_int = [&where](const BigInt& n) -> Result<Json> {
        if (n.abs() >= BigInt(int64_t{1} << 53)) {
            SCFG_TRY(std::string shown, py::int_str(n));  // f"integer {n}": str()'s ValueError beyond 4300 digits
            return Err::value_error("integer " + shown + " at " + where +
                                    " is not safe in JSON (|n| >= 2**53); write it as a string");
        }
        return Json(n);
    };
    switch (v.type()) {
        case Json::Type::Null:
        case Json::Type::Bool:
        case Json::Type::Str:
            return v;
        case Json::Type::Int:
            return check_int(v.as_bigint());
        case Json::Type::Float: {
            const double f = v.as_float();
            if (!std::isfinite(f) || std::trunc(f) != f) {
                return Err::value_error("non-integral float " + scfg::pyfloat::repr(f) + " at " + where);
            }
            SCFG_TRY(BigInt n, py::py_int(v));
            return check_int(n);
        }
        case Json::Type::List: {
            Json out = Json::list();
            const Json::List& l = v.as_list();
            for (size_t i = 0; i < l.size(); ++i) {
                SCFG_TRY(Json x, to_jsonable(l[i], path + "[" + detail::number(i) + "]"));
                out.as_list().push_back(std::move(x));
            }
            return out;
        }
        case Json::Type::Dict: {
            Json out = Json::dict();
            for (const auto& [key, value] : v.as_dict()) {
                SCFG_TRY(Json x, to_jsonable(value, path.empty() ? key : path + "." + key));
                out.as_dict().set(key, std::move(x));
            }
            return out;
        }
    }
    return v;
}

Result<Json> encode_returns(const Json& result) { return to_jsonable(result); }

Result<Json> encode_returns(const std::vector<scfg::watcher::StepOutcome>& outcomes) {
    Json raw = Json::list();
    for (const scfg::watcher::StepOutcome& o : outcomes) raw.as_list().push_back(detail::outcome_json(o));
    return to_jsonable(raw);
}

Result<Json> encode_returns(const std::vector<restore::RestoreCandidate>& candidates) {
    Json raw = Json::list();
    for (const restore::RestoreCandidate& c : candidates) raw.as_list().push_back(detail::candidate_raw(c));
    return to_jsonable(raw);
}

Result<Json> snapshot_json(const engine::Engine& eng) {
    SCFG_TRY(engine::Snapshot snap, eng.snapshot());
    SCFG_TRY(Json pending, to_jsonable(snap.pending, "pending"));
    SCFG_TRY(Json receipts, to_jsonable(snap.receipts, "receipts"));
    Json owned = Json::list();
    for (const std::string& item : snap.owned) owned.as_list().push_back(Json(item));
    Json unready = Json::list();
    for (const auto& [chain, token] : snap.unready) unready.as_list().push_back(Json::list({Json(chain), Json(token)}));
    return Json::dict({{"pending", pending},
                       {"receipts", receipts},
                       {"owned", owned},
                       {"tip_total", Json(snap.tip_total.str())},
                       {"unready", unready}});
}

bool stands_for_python(const Err& e) {
    return e.is_a("Exception") && !e.is_a("HostGone");
}

std::optional<Json> raises_json(const Err& e) {
    if (e.is_a("ConfigError")) return Json::dict({{"type", "ConfigError"}, {"code", Json(e.code)}});
    if (e.is_a("RestoreError")) return Json::dict({{"type", "RestoreError"}, {"code", Json(e.code)}});
    if (!stands_for_python(e)) return std::nullopt;
    return Json::dict({{"type", "other"}});
}

Result<Json> parse(std::string_view text) { return scfg::json::loads(text); }

// ---- replay ----

std::optional<std::string> replay(const Json& vector, ReplayMode mode) {
    ReplayTransport transport(mode);
    return replay(vector, transport);  // the Engine inside goes before the transport
}

std::optional<std::string> replay(const Json& vector, ReplayTransport& transport) {
    if (!vector.is_dict() || !json_eq(vector.as_dict().get("tier"), Json(2))) return "not a Tier 2 vector";
    const Json setup = vector.as_dict().get("setup");
    if (!setup.is_dict()) return "setup is not an object";
    const Json clock0 = setup.as_dict().get("clock");
    if (!clock0.is_int64()) return "setup.clock is not an int";
    const Json steps = vector.as_dict().get("steps");
    if (!steps.is_list()) return "steps is not a list";

    // Declared before the Engine, so the Engine (and every RpcClient's use of them) goes first.
    int64_t now = clock0.as_int64();
    int64_t errors = 0;
    std::optional<std::string> bad_error;
    ScriptedEntropy entropy;
    std::function<int64_t()> clock = [&now]() { return now; };
    std::function<void(const Err&)> on_error = [&errors, &bad_error](const Err& e) {
        ++errors;
        if (!stands_for_python(e) && !bad_error) bad_error = e.str();
    };
    Result<std::unique_ptr<engine::Engine>> built = build_engine(setup, transport, clock, on_error, &entropy);
    if (!built.ok()) return "setup: " + built.err().str();
    std::unique_ptr<engine::Engine> eng = built.take();

    const Json::List& list = steps.as_list();
    for (size_t i = 0; i < list.size(); ++i) {
        const Json& step = list[i];
        const std::string at_step = "step " + detail::number(i);
        if (!step.is_dict()) return at_step + ": not an object";
        const Json action = step.as_dict().get("action");
        if (!action.is_str()) return at_step + ": action is not a str";
        const std::string where = at_step + " (" + action.as_str() + ")";
        for (std::string_view key : {"at", "args", "entropy", "calls", "raises", "returns", "internal_errors", "snapshot"}) {
            if (!step.as_dict().contains(key)) return where + ": nothing recorded for " + std::string(key);
        }
        const Json at = step.as_dict().get("at");
        if (!at.is_int64()) return where + ": at is not an int";
        now = at.as_int64();
        errors = 0;
        bad_error.reset();
        const Json calls = step.as_dict().get("calls");
        transport.load(calls);
        entropy.load(step.as_dict().get("entropy"));

        detail::ActionOutcome got = detail::run_action(*eng, action.as_str(), step.as_dict().get("args"), list, i, transport);

        std::vector<std::string> problems = transport.problems;
        for (std::string& p : transport.leftover()) problems.push_back(std::move(p));
        for (const std::string& p : entropy.problems) problems.push_back(p);
        for (std::string& p : entropy.leftover()) problems.push_back(std::move(p));
        if (!problems.empty()) return where + ": " + detail::join(problems, "; ");
        if (std::optional<std::string> d = deferred_problem(transport, calls, got.suspended)) return where + ": " + *d;
        if (got.failure) return where + ": " + *got.failure;
        if (bad_error) return where + ": on_error got an Err that no contained Python exception stands for: " + *bad_error;
        const Json recorded_raises = step.as_dict().get("raises");
        if (!json_eq(recorded_raises, got.raises)) {
            return where + ": raises: recorded " + detail::show(recorded_raises) + ", got " + detail::show(got.raises);
        }
        if (std::optional<std::string> d = first_diff(step.as_dict().get("returns"), got.returns)) {
            return where + ": returns: " + *d + " (recorded != got)";
        }
        const Json recorded_errors = step.as_dict().get("internal_errors");
        if (!json_eq(recorded_errors, Json(errors))) {
            return where + ": internal_errors: recorded " + detail::show(recorded_errors) + ", got " + std::to_string(errors);
        }
        Result<Json> snapshot = snapshot_json(*eng);
        if (!snapshot.ok()) return where + ": snapshot: " + snapshot.err().str();
        if (std::optional<std::string> d = first_diff(step.as_dict().get("snapshot"), snapshot.value())) {
            return where + ": snapshot: " + *d + " (recorded != got)";
        }
    }
    return std::nullopt;
}

std::optional<std::string> destroy_engine_while_suspended(const Json& vector, size_t step, size_t answered) {
    const Json setup = vector.as_dict().get("setup");
    const Json steps = vector.as_dict().get("steps");
    const Json::List& list = steps.as_list();
    if (step >= list.size()) return std::string("no such step");
    // As replay(): declared before the transport and the Engine, which therefore go first.
    int64_t now = setup.as_dict().get("clock").as_int64();
    ScriptedEntropy entropy;
    std::function<int64_t()> clock = [&now]() { return now; };
    std::function<void(const Err&)> on_error = [](const Err&) {};
    ReplayTransport transport(ReplayMode::Deferred);
    Result<std::unique_ptr<engine::Engine>> built = build_engine(setup, transport, clock, on_error, &entropy);
    if (!built.ok()) return "setup: " + built.err().str();
    std::unique_ptr<engine::Engine> eng = built.take();
    for (size_t i = 0; i <= step; ++i) {
        const Json& s = list[i];
        now = s.as_dict().get("at").as_int64();
        transport.load(s.as_dict().get("calls"));
        entropy.load(s.as_dict().get("entropy"));
        const std::string action = s.as_dict().get("action").as_str();
        if (i < step) {
            detail::run_action(*eng, action, s.as_dict().get("args"), list, i, transport);
            continue;
        }
        return detail::probe_action(eng, action, s.as_dict().get("args"), transport, answered);
    }
    return std::nullopt;
}

}  // namespace scfg_test::tier2
