// Copyright 2026 Repulse
// No Python twin: checks of BackendsSolana.cpp below what the Tier 2 vectors pin, ported from
// unity/core-tests/SolanaBackendTests.cs and extended where the C# ledger kept a receipt Python drops. Every expected
// value here was produced by the Python reference (scfg_core.backends.solana) on the same input: transfer_to's
// readings, reverify's verdicts on the solana/reverify_receipts transactions as recorded and as tampered, lookup's
// candidates as solana/restore_lookup recorded them, and build_payment's transaction and errors.
#include "support/JsonEq.h"
#include "support/Test.h"
#include "support/Tier2.h"
#include "support/Vectors.h"

#include "Scfg/BackendsBase.h"
#include "Scfg/BackendsSolana.h"
#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/Receipts.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"
#include "Scfg/Transfers.h"

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using scfg::BigInt;
using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace sol = scfg::backends::solana;
namespace tier2 = scfg_test::tier2;
using scfg_test::json_eq;

namespace scfg_test::solana_backend_tests {

constexpr const char* kMint = "MINT";
constexpr const char* kOwner = "OWNER";

template <class T>
std::optional<Result<T>> run_now(scfg::Co<T> co) {
    if (!co.done()) return std::nullopt;
    return co.take();
}

std::optional<Json> load(const std::string& id) {
    std::optional<std::string> text = vectors::read_text("tier2/" + id + ".json");
    if (!text) return std::nullopt;
    Result<Json> v = tier2::parse(*text);
    if (!v.ok()) return std::nullopt;
    return v.take();
}

Json bal(Json index, Json owner, int64_t amount, const char* mint = kMint) {
    return Json::dict({{"accountIndex", std::move(index)},
                       {"mint", mint},
                       {"owner", std::move(owner)},
                       {"uiTokenAmount", Json::dict({{"amount", scfg::py::int_str(BigInt(amount)).take()}})}});
}

Json tx(Json pre, Json post, Json keys = Json(), Json slot = Json(7), Json err = Json()) {
    Json t = Json::dict({{"slot", std::move(slot)},
                         {"meta", Json::dict({{"err", std::move(err)},
                                              {"preTokenBalances", std::move(pre)},
                                              {"postTokenBalances", std::move(post)}})}});
    if (!keys.is_null()) t.as_dict().set("transaction", Json::dict({{"message", Json::dict({{"accountKeys", keys}})}}));
    return t;
}

Result<std::optional<scfg::transfers::Transfer>> read(const Json& t) { return sol::transfer_to(t, Json("S"), kMint, kOwner); }

// The transfer Python's transfer_to gives: Transfer(token='MINT', sender=payer, recipient='OWNER', value=value,
// tx_hash='S', log_index=index, block_number=slot).
void expect_transfer(Ctx& t, const Json& in, const Json& payer, int64_t value, const Json& index, std::string_view what,
                     const BigInt& slot = BigInt(7)) {
    Result<std::optional<scfg::transfers::Transfer>> got = read(in);
    if (!t.check(got.ok(), std::string(what) + ": " + (got.ok() ? "" : got.err().str()))) return;
    if (!t.check(got.value().has_value(), std::string(what) + ": no transfer")) return;
    const scfg::transfers::Transfer& x = *got.value();
    t.check(json_eq(x.sender, payer) && x.sender.type() == payer.type(), std::string(what) + ": payer " + scfg::py::repr(x.sender).value());
    t.check(x.value == BigInt(value), std::string(what) + ": value " + x.value.to_decimal());
    t.check(json_eq(x.log_index, index) && x.log_index.type() == index.type(),
            std::string(what) + ": index " + scfg::py::repr(x.log_index).value());
    t.check(x.block_number == slot, std::string(what) + ": slot " + x.block_number.to_decimal());
    t.check(x.token == kMint && x.recipient == kOwner && json_eq(x.tx_hash, Json("S")), std::string(what) + ": fields");
}

void expect_none(Ctx& t, const Json& in, std::string_view what) {
    Result<std::optional<scfg::transfers::Transfer>> got = read(in);
    t.check(got.ok() && !got.value().has_value(), std::string(what) + ": " + (got.ok() ? "a transfer" : got.err().str()));
}

void expect_err(Ctx& t, const Json& in, std::string_view kind, std::string_view message, std::string_view what) {
    Result<std::optional<scfg::transfers::Transfer>> got = read(in);
    t.check(!got.ok() && got.err().kind == kind && got.err().message == message,
            std::string(what) + ": " + (got.ok() ? "no error" : got.err().str()));
}

// An Engine over a vector's setup, its transport loaded with one step's calls (members go in reverse: the Engine
// before the entropy and the transport).
struct Rig {
    tier2::ReplayTransport transport;
    tier2::ScriptedEntropy entropy;
    std::unique_ptr<scfg::engine::Engine> engine;
    std::vector<std::string> reported;

    bool build(Ctx& t, const Json& setup) {
        Result<std::unique_ptr<scfg::engine::Engine>> e = tier2::build_engine(
            setup, transport, [] { return int64_t{1700000000}; },
            [this](const Err& err) { reported.push_back(err.kind); }, &entropy);
        if (!t.check(e.ok(), "build_engine")) return false;
        engine = e.take();
        return true;
    }
    scfg::backends::base::Rpc rpc() { return engine->rpc("solana").take(); }
    scfg::chains::Chain chain() { return engine->chain("solana").take(); }
};

Json& step_of(Json& v, size_t i) { return v.as_dict().find("steps")->as_list()[i]; }

// The recorded getTransaction answer for sig in calls, with every `from` replaced by `to`.
void tamper(Ctx& t, Json& calls, std::string_view sig, std::string_view from, std::string_view to) {
    bool found = false;
    for (Json& c : calls.as_list()) {
        Json::Dict& call = c.as_dict();
        if (call.get("method").as_str() != "getTransaction" || !json_eq(call.get("params").as_list()[0], Json(sig))) continue;
        std::string text = call.get("response_text").as_str();
        size_t at = text.find(from);
        found = at != std::string::npos;
        while (at != std::string::npos) {
            text.replace(at, from.size(), to);
            at = text.find(from, at + to.size());
        }
        call.set("response_text", Json(text));
    }
    t.check(found, std::string(sig.substr(0, 8)) + ": nothing to tamper with: " + std::string(from));
}

// Answers each Solana method with a canned result, at once, and records the requests.
struct CannedTransport final : scfg::coio::Transport {
    std::map<std::string, Json> results;
    std::vector<Json> requests;
    scfg::Future<Json> post_json(std::string url, Json payload, double timeout) override {
        (void)url, (void)timeout;
        requests.push_back(payload);
        const std::string method = payload.as_dict().get("method").as_str();
        auto it = results.find(method);
        Json result = it == results.end() ? Json() : scfg::deep_copy(it->second);
        return scfg::Future<Json>::completed(Json::dict({{"jsonrpc", "2.0"}, {"id", 1}, {"result", result}}));
    }
};

}  // namespace scfg_test::solana_backend_tests

using namespace scfg_test::solana_backend_tests;

// ---- transfer_to ----

SCFG_TEST(SolanaBackendTests, ThePayerIsTheFirstOfTheLargestDrops) {
    expect_transfer(t,
                    tx(Json::list({bal(0, "A", 50), bal(1, "B", 50), bal(2, kOwner, 0), bal(3, "C", 10)}),
                       Json::list({bal(0, "A", 20), bal(1, "B", 20), bal(2, kOwner, 70), bal(3, "C", 0)})),
                    "A", 70, 2, "first of the largest");
    expect_transfer(t,
                    tx(Json::list({bal(0, "A", 10), bal(1, "B", 15), bal(3, "A", 10)}),
                       Json::list({bal(0, "A", 0), bal(1, "B", 0), bal(3, "A", 0), bal(2, kOwner, 35)})),
                    "A", 35, 2, "drops summed per owner");
}

SCFG_TEST(SolanaBackendTests, WithoutDropsThePayerIsTheFeePayerOrNone) {
    Json pre = Json::list({bal(2, kOwner, 0)});
    Json post = Json::list({bal(2, kOwner, 5)});
    expect_transfer(t, tx(pre, post, Json::list({"FEE", "X"})), "FEE", 5, 2, "a str key");
    expect_transfer(t, tx(pre, post, Json::list({Json::dict({{"pubkey", "FEEP"}})})), "FEEP", 5, 2, "a {pubkey} key");
    expect_transfer(t, tx(pre, post), Json(), 5, 2, "no keys");
    // accountKeys as Python reads it: a str's first character, a dict's KeyError, an int's missing .get
    expect_transfer(t, tx(pre, post, "FEE"), "F", 5, 2, "a str of keys");
    expect_err(t, tx(pre, post, Json::dict({{"a", 1}})), "KeyError", "0", "a dict of keys");
    expect_err(t, tx(pre, post, Json::list({5})), "AttributeError", "'int' object has no attribute 'get'", "an int key");
}

SCFG_TEST(SolanaBackendTests, TheIndexIsTheFirstAccountThatGained) {
    expect_transfer(t, tx(Json::list({bal(4, kOwner, 3)}), Json::list({bal(1, kOwner, 0), bal(4, kOwner, 10), bal(5, kOwner, 2)})),
                    Json(), 9, 4, "net gain, first gainer");
    expect_transfer(t,
                    tx(Json::list(), Json::list({bal(5, kOwner, 1), bal(3, kOwner, 1), bal(1, kOwner, 1), bal(4, kOwner, 1)})),
                    Json(), 4, 1, "indexes sorted");
}

SCFG_TEST(SolanaBackendTests, NoTransferForAFailedTxAnotherMintOrALoss) {
    expect_none(t, tx(Json::list({bal(2, kOwner, 0)}), Json::list({bal(2, kOwner, 5)}), Json(), 7, Json::dict({{"x", 1}})),
                "failed");
    expect_none(t, tx(Json::list({bal(2, kOwner, 0, "Z")}), Json::list({bal(2, kOwner, 5, "Z")})), "another mint");
    expect_none(t, tx(Json::list({bal(2, kOwner, 9)}), Json::list({bal(2, kOwner, 5)})), "a loss");
}

// What the C# port read differently: an accountIndex, an owner or a slot that no node sends is carried as Python
// carries it, and its verdict is Python's.
SCFG_TEST(SolanaBackendTests, MalformedNodeDataIsReadAsPythonReadsIt) {
    expect_transfer(t, tx(Json::list({bal("2", kOwner, 0)}), Json::list({bal("2", kOwner, 5)})), Json(), 5, "2",
                    "a str accountIndex stays a str");
    expect_transfer(t, tx(Json::list({bal(2.5, kOwner, 0)}), Json::list({bal(2.5, kOwner, 5)})), Json(), 5, 2.5,
                    "a float accountIndex");
    expect_transfer(t, tx(Json::list({bal(0, 5, 9), bal(2, kOwner, 0)}), Json::list({bal(0, 5, 4), bal(2, kOwner, 5)})),
                    5, 5, 2, "an int owner is the payer");
    expect_none(t, tx(Json::list({bal(2, 7, 0)}), Json::list({bal(2, 7, 5)})), "the developer's row with an int owner");
    expect_transfer(t, tx(Json::list({bal(2, kOwner, 0)}), Json::list({bal(2, kOwner, 5)}), Json(), Json(BigInt(1) << 64)),
                    Json(), 5, 2, "a slot of 2**64", BigInt(1) << 64);
    // 1, 1.0 and True are one key; the set keeps pre's 1, the table the last value written under it
    expect_transfer(t, tx(Json::list({bal(1, kOwner, 0)}), Json::list({bal(1.0, kOwner, 4), bal(true, kOwner, 6)})),
                    Json(), 6, 1, "1, 1.0 and True");
    // True and 1 are one owner in drops; the first one stays its key
    expect_transfer(t,
                    tx(Json::list({bal(0, true, 5), bal(1, 1, 5), bal(2, kOwner, 0)}),
                       Json::list({bal(0, true, 4), bal(1, 1, 4), bal(2, kOwner, 2)})),
                    true, 2, 2, "True and 1 drop together");
    expect_none(t, tx(Json::list({bal(Json(), kOwner, 0)}), Json::list({bal(Json(), kOwner, 5)})),
                "an accountIndex of None is no index");
    expect_err(t,
               tx(Json::list({bal(Json(), kOwner, 0), bal(3, kOwner, 0)}), Json::list({bal(Json(), kOwner, 5), bal(3, kOwner, 1)})),
               "TypeError", "'<' not supported between instances of 'int' and 'NoneType'", "None beside an int");
    Result<std::optional<scfg::transfers::Transfer>> mixed =
        read(tx(Json::list({bal("1", "A", 5), bal(2, kOwner, 0)}), Json::list({bal("1", "A", 0), bal(2, kOwner, 5)})));
    t.check(!mixed.ok() && mixed.err().kind == "TypeError", "a str and an int cannot be sorted");
    expect_err(t, tx(Json::list({bal(0, Json::dict({{"a", 1}}), 5)}), Json::list({bal(0, Json::dict({{"a", 1}}), 1), bal(2, kOwner, 4)})),
               "TypeError", "unhashable type: 'dict'", "an unhashable owner");
    expect_err(t, Json::dict({{"slot", 1}, {"meta", "x"}}), "AttributeError", "'str' object has no attribute 'get'", "meta");
    expect_err(t, Json::dict({{"slot", 1}, {"meta", Json::dict({{"err", Json()}, {"postTokenBalances", "ab"}})}}),
               "AttributeError", "'str' object has no attribute 'get'", "a str of balances");
    expect_err(t, Json::dict({{"slot", 1}, {"meta", Json::dict({{"err", Json()}, {"postTokenBalances", 5}})}}), "TypeError",
               "'int' object is not iterable", "an int of balances");
    expect_err(t, Json::dict({{"meta", Json::dict({{"err", Json()}, {"postTokenBalances", Json::list({bal(2, kOwner, 5)})}})}}),
               "KeyError", "'slot'", "no slot");
    Json no_index = Json::dict({{"mint", kMint}, {"owner", kOwner}, {"uiTokenAmount", Json::dict({{"amount", "5"}})}});
    expect_err(t, Json::dict({{"slot", 1}, {"meta", Json::dict({{"err", Json()}, {"postTokenBalances", Json::list({no_index})}})}}),
               "KeyError", "'accountIndex'", "no accountIndex");
    Json infinite = bal(2, kOwner, 0);
    infinite.as_dict().set("uiTokenAmount", Json::dict({{"amount", Json(std::numeric_limits<double>::infinity())}}));
    Result<std::optional<scfg::transfers::Transfer>> inf = read(tx(Json::list(), Json::list({infinite})));
    t.check(!inf.ok() && inf.err().kind == "OverflowError", "an infinite amount: " + (inf.ok() ? std::string("read") : inf.err().str()));
}

// A NaN accountIndex (json.loads gives every NaN the same object, so it is one key) is ordered last.
SCFG_TEST(SolanaBackendTests, ANanAccountIndexIsOneKeyOrderedLast) {
    Result<Json> in = scfg::json::loads_text(
        R"({"slot": 7, "meta": {"err": null, "preTokenBalances": [{"accountIndex": NaN, "mint": "MINT", "owner": "OWNER",)"
        R"( "uiTokenAmount": {"amount": "0"}}], "postTokenBalances": [{"accountIndex": NaN, "mint": "MINT", "owner": "OWNER",)"
        R"( "uiTokenAmount": {"amount": "5"}}, {"accountIndex": 1, "mint": "MINT", "owner": "OWNER", "uiTokenAmount": {"amount": "2"}}]}})");
    if (!t.check(in.ok(), "parse")) return;
    expect_transfer(t, in.value(), Json(), 7, 1, "NaN and 1");
}

// ---- reverify ----

// solana/reverify_receipts' receipts judged one by one (reverify_receipts validates their fields first), then with
// the node's answers tampered as the C# ledger's malformed data: Python drops a receipt whose transaction has a slot
// of 2**64, or str accountIndexes, or a developer row whose owner is not a str; it keeps one whose payer's owner is an
// int (the payer is not compared), and an infinite amount is its OverflowError (for reverify_receipts to contain).
SCFG_TEST(SolanaBackendTests, ReverifyGivesPythonsVerdicts) {
    std::optional<Json> v = load("solana/reverify_receipts");
    if (!t.check(v.has_value(), "solana/reverify_receipts")) return;
    const Json setup = v->as_dict().get("setup");
    const Json recorded = step_of(*v, 0).as_dict().get("calls");
    const Json removed = step_of(*v, 0).as_dict().get("returns");
    const std::string k5 = "solana:5jRvfZECEfN9pyWHFUF3GZXj3asr8dfVfZWVg6JLg3hP2bw7WZ6JErRVJxy3fSCqQi2c9oFo9YvSQL6LPtQ8s3mn:2";
    const std::string k4 = "solana:4yo3LtPZ3x5oz5XTid5pbHMdERC9JTsrFGfpruW3YG5m9a1ox5PQYV663q3BfHExK2shap9pKt31TYzho5GCCJXQ:2";
    const sol::SolanaBackend backend;

    // one receipt's verdict ("keep", "drop", or the Err's kind) over the recorded calls, tampered by `change`
    auto verdict = [&](const std::string& key, const std::function<void(Json&)>& change) -> std::string {
        Rig rig;
        if (!rig.build(t, setup)) return "no engine";
        Json calls = scfg::deep_copy(recorded);
        change(calls);
        rig.transport.load(calls);
        const Json receipt = setup.as_dict().get("receipts").as_dict().get(key);
        std::optional<Result<std::string>> got = run_now(backend.reverify(
            receipt, rig.rpc(), rig.chain(), rig.engine->config().accepted_wallets_for("solana")));
        t.check(rig.transport.problems.empty(), key + ": " + (rig.transport.problems.empty() ? "" : rig.transport.problems[0]));
        if (!got) return "suspended";
        return got->ok() ? got->value() : got->err().kind;
    };
    auto untouched = [](Json&) {};
    for (const auto& [key, receipt] : setup.as_dict().get("receipts").as_dict()) {
        (void)receipt;
        bool dropped = false;
        for (const Json& r : removed.as_list()) dropped = dropped || json_eq(r, Json(key));
        SCFG_CHECK_EQ(t, verdict(key, untouched), std::string(dropped ? "drop" : "keep"));
    }
    const std::string sig5 = k5.substr(7, 88), sig4 = k4.substr(7, 88);
    SCFG_CHECK_EQ(t, verdict(k5, [&](Json& c) { tamper(t, c, sig5, "\"slot\": 950", "\"slot\": 18446744073709551616"); }), "drop");
    SCFG_CHECK_EQ(t, verdict(k5, [&](Json& c) {
                      tamper(t, c, sig5, "\"accountIndex\": 2", "\"accountIndex\": \"2\"");
                      tamper(t, c, sig5, "\"accountIndex\": 1", "\"accountIndex\": \"1\"");
                  }), "drop");
    // one str index beside an int one cannot be sorted: a payload this build can't read, kept
    SCFG_CHECK_EQ(t, verdict(k5, [&](Json& c) { tamper(t, c, sig5, "\"accountIndex\": 2", "\"accountIndex\": \"2\""); }), "keep");
    SCFG_CHECK_EQ(t, verdict(k5, [&](Json& c) { tamper(t, c, sig5, "\"accountIndex\": 2", "\"accountIndex\": 2.0"); }), "keep");
    SCFG_CHECK_EQ(t, verdict(k5, [&](Json& c) {
                      tamper(t, c, sig5, "\"owner\": \"AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe\"", "\"owner\": 5");
                  }), "keep");
    SCFG_CHECK_EQ(t, verdict(k5, [&](Json& c) {
                      tamper(t, c, sig5, "\"owner\": \"AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe\"", "\"owner\": [5]");
                  }), "keep");
    SCFG_CHECK_EQ(t, verdict(k5, [&](Json& c) {
                      tamper(t, c, sig5, "\"owner\": \"7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU\"", "\"owner\": 5");
                  }), "drop");
    SCFG_CHECK_EQ(t, verdict(k4, [&](Json& c) { tamper(t, c, sig4, "\"amount\": \"995006258\"", "\"amount\": Infinity"); }),
                  "OverflowError");
}

// ---- restore lookup ----

// solana/restore_lookup's Solana steps through the backend's lookup (restore::lookup picks the chain): the
// recorded candidates, already_restored once the receipt is stored, and the RestoreErrors.
SCFG_TEST(SolanaBackendTests, LookupGivesTheRecordedCandidates) {
    std::optional<Json> v = load("solana/restore_lookup");
    if (!t.check(v.has_value(), "solana/restore_lookup")) return;
    const Json setup = v->as_dict().get("setup");
    const sol::SolanaBackend backend;
    Json applied = Json::dict();  // step 1's restore_apply stored its receipts
    for (const Json& r : step_of(*v, 1).as_dict().get("returns").as_list()) {
        const Json::Dict& d = r.as_dict();
        applied.as_dict().set(scfg::receipts::receipt_key(d.get("chain"), d.get("tx_hash"), d.get("log_index")).take(), r);
    }
    struct Case {
        size_t step;
        Json receipts;
    };
    for (const Case& c : {Case{0, Json::dict()}, Case{2, applied}, Case{3, Json::dict()}, Case{4, Json::dict()}, Case{5, Json::dict()}}) {
        const std::string where = "step " + std::to_string(c.step);
        Rig rig;
        if (!rig.build(t, setup)) return;
        const Json::Dict& s = step_of(*v, c.step).as_dict();
        rig.transport.load(s.get("calls"));
        std::vector<std::string> reported;
        std::optional<Result<std::optional<std::vector<scfg::restore::RestoreCandidate>>>> got = run_now(backend.lookup(
            s.get("args").as_dict().get("tx").as_str(), rig.engine->config(), rig.chain(), rig.rpc(), c.receipts,
            [&reported](const Err& e) { reported.push_back(e.kind); }));
        t.check(rig.transport.problems.empty() && rig.transport.leftover().empty(), where + ": the recorded calls");
        if (!t.check(got.has_value(), where + ": suspended")) continue;
        t.check(reported.empty(), where + ": nothing reported");
        const Json raises = s.get("raises");
        if (c.step == 3) {  // no endpoint has it: None, which restore.lookup turns into tx_not_found
            t.check(got->ok() && !got->value().has_value(), where + ": None");
        } else if (!raises.is_null()) {
            t.check(!got->ok() && got->err().kind == "RestoreError" &&
                        json_eq(Json(got->err().code), raises.as_dict().get("code")),
                    where + ": " + (got->ok() ? std::string("no error") : got->err().str()));
        } else if (t.check(got->ok() && got->value().has_value(), where + ": candidates")) {
            Result<Json> encoded = tier2::encode_returns(*got->value());
            t.check(encoded.ok() && json_eq(encoded.value(), s.get("returns")),
                    where + ": " + (encoded.ok() ? scfg::json::dumps(encoded.value()).value() : encoded.err().str()));
        }
    }
}

// A transfer classify cannot price (10**32 units or more) is skipped and reported.
SCFG_TEST(SolanaBackendTests, LookupSkipsAnUnpriceableTransfer) {
    std::optional<Json> v = load("solana/restore_lookup");
    if (!t.check(v.has_value(), "solana/restore_lookup")) return;
    Rig rig;
    if (!rig.build(t, v->as_dict().get("setup"))) return;
    Json calls = scfg::deep_copy(step_of(*v, 0).as_dict().get("calls"));
    const std::string sig = "5jRvfZECEfN9pyWHFUF3GZXj3asr8dfVfZWVg6JLg3hP2bw7WZ6JErRVJxy3fSCqQi2c9oFo9YvSQL6LPtQ8s3mn";
    tamper(t, calls, sig, "\"amount\": \"4993742\"", "\"amount\": \"100000000000000000000000000000000\"");
    rig.transport.load(calls);
    std::vector<std::string> reported;
    const sol::SolanaBackend backend;
    std::optional<Result<std::optional<std::vector<scfg::restore::RestoreCandidate>>>> got =
        run_now(backend.lookup(sig, rig.engine->config(), rig.chain(), rig.rpc(), Json::dict(),
                               [&reported](const Err& e) { reported.push_back(e.kind); }));
    t.check(got && !got->ok() && got->err().kind == "RestoreError" && got->err().code == "tx_not_a_payment",
            "the only transfer skipped: tx_not_a_payment");
    t.check(reported == std::vector<std::string>{"InvalidOperation"}, "reported once");
}

// Ruling R10: a restore candidate carries the transfer as Python's does, whatever the node sent, and the receipt it
// stores holds those values as they are. solana/restore_lookup's step 0, its getTransaction answer mutated; every
// expectation is the Python reference's on the same mutation (re-recorded with the script below, whose MUTATIONS are
// r10_cases() below; nothing under conformance/ is changed):
//
// ---- r10_restore.py (run from the repo root: python3 -I r10_restore.py .) ----
//   import copy, json, sys
//   sys.path[:0] = [sys.argv[1] + "/plugin/game/python-packages", sys.argv[1] + "/conformance/python"]
//   from scfg_conformance import replay as rp
//   v = json.load(open(sys.argv[1] + "/conformance/vectors/tier2/solana/restore_lookup.json"))
//
//   def rows(tx):
//       return [b for t in ("preTokenBalances", "postTokenBalances") for b in tx["meta"][t]]
//
//   def indexes(a, b):  # the payer's row (accountIndex 1) and the developer's (2)
//       def f(tx):
//           for r in rows(tx):
//               r["accountIndex"] = a if r["accountIndex"] == 1 else b
//       return f
//
//   def owner5(tx):
//       for r in rows(tx):
//           if r["accountIndex"] == 1:
//               r["owner"] = 5
//
//   def fee_payer(first):  # only the developer's row is left; accountKeys[0] is `first`, or there is no key
//       def f(tx):
//           for t in ("preTokenBalances", "postTokenBalances"):
//               tx["meta"][t] = [b for b in tx["meta"][t] if b["accountIndex"] != 1]
//           keys = tx["transaction"]["message"]["accountKeys"]
//           if first is None:
//               keys.clear()
//           else:
//               keys[0] = first
//       return f
//
//   def slot(tx):
//       tx["slot"] = 2**63
//
//   MUTATIONS = [("str accountIndex", indexes("1", "2")), ("float accountIndex", indexes(1.5, 2.5)),
//                ("bool accountIndex", indexes(False, True)), ("int owner on the payer's row", owner5),
//                ("fee payer {pubkey: 5}", fee_payer({"pubkey": 5})), ("no fee payer", fee_payer(None)),
//                ("slot 2**63", slot), ("accountIndex 2**63", indexes(1, 2**63))]
//   for name, mutate in MUTATIONS:
//       s = copy.deepcopy(v["steps"][0])
//       call = s["calls"][0]
//       body = json.loads(call["response_text"])
//       mutate(body["result"])
//       call["response_text"] = json.dumps(body)
//       transport = rp.ReplayTransport()
//       errors = []
//       engine = rp.build_engine(v["setup"], transport=transport, clock=lambda: s["at"], on_error=errors.append)
//       transport.load(s["calls"])
//       cands = engine.restore_lookup(s["args"]["tx"])
//       print(name, cands, engine.restore_apply(cands), list(engine.receipts))
//       transport.load(s["calls"])
//       print([c.already_restored for c in engine.restore_lookup(s["args"]["tx"])], errors)
// ---- end of r10_restore.py ----
//
// Python printed, for each: one candidate (unlock chapter2, value 4993742) whose transfer holds the sender, log_index
// and block_number below; restore_apply stored one receipt with exactly those as payer, log_index and block, under
// the key below; the second lookup found it already restored; nothing was reported. The receipts hold ints at or
// beyond 2**53, which no vector may (canonical.py), so the test compares them here rather than through a vector.
namespace scfg_test::solana_backend_tests {

struct R10Case {
    std::string name;
    std::function<void(Json& result)> mutate;  // the getTransaction result
    Json sender, log_index;
    BigInt block;
    std::string key_suffix;  // str(log_index) in the receipt key
};

const char* const kR10Sig = "5jRvfZECEfN9pyWHFUF3GZXj3asr8dfVfZWVg6JLg3hP2bw7WZ6JErRVJxy3fSCqQi2c9oFo9YvSQL6LPtQ8s3mn";
const char* const kR10Payer = "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe";

void each_balance(Json& result, const std::function<void(Json::Dict& row)>& fn) {
    Json::Dict& meta = result.as_dict().find("meta")->as_dict();
    for (const char* table : {"preTokenBalances", "postTokenBalances"}) {
        for (Json& row : meta.find(table)->as_list()) fn(row.as_dict());
    }
}

// The accountIndex 1 (payer) and 2 (developer) rows' indexes become a and b.
std::function<void(Json&)> indexes(Json a, Json b) {
    return [a, b](Json& result) {
        each_balance(result, [&](Json::Dict& row) {
            const bool payer = json_eq(row.get("accountIndex"), Json(1));
            row.set("accountIndex", payer ? a : b);
        });
    };
}

// Only the developer's row is left, and accountKeys[0] (the fee payer) is `first` (or no key at all).
std::function<void(Json&)> fee_payer(std::optional<Json> first) {
    return [first](Json& result) {
        Json::Dict& meta = result.as_dict().find("meta")->as_dict();
        for (const char* table : {"preTokenBalances", "postTokenBalances"}) {
            Json::List& rows = meta.find(table)->as_list();
            Json::List kept;
            for (const Json& row : rows) {
                if (!json_eq(row.as_dict().get("accountIndex"), Json(1))) kept.push_back(row);
            }
            rows = kept;
        }
        Json& keys = *result.as_dict().find("transaction")->as_dict().find("message")->as_dict().find("accountKeys");
        if (first) {
            keys.as_list()[0] = *first;
        } else {
            keys = Json::list();
        }
    };
}

std::vector<R10Case> r10_cases() {
    const BigInt two63 = BigInt(1) << 63;
    return {
        {"str accountIndex", indexes(Json("1"), Json("2")), Json(kR10Payer), Json("2"), BigInt(950), "2"},
        {"float accountIndex", indexes(Json(1.5), Json(2.5)), Json(kR10Payer), Json(2.5), BigInt(950), "2.5"},
        {"bool accountIndex", indexes(Json(false), Json(true)), Json(kR10Payer), Json(true), BigInt(950), "True"},
        {"int owner on the payer's row",
         [](Json& result) {
             each_balance(result, [](Json::Dict& row) {
                 if (json_eq(row.get("accountIndex"), Json(1))) row.set("owner", 5);
             });
         },
         Json(5), Json(2), BigInt(950), "2"},
        {"fee payer {pubkey: 5}", fee_payer(Json::dict({{"pubkey", 5}})), Json(5), Json(2), BigInt(950), "2"},
        {"no fee payer", fee_payer(std::nullopt), Json(), Json(2), BigInt(950), "2"},
        {"slot 2**63", [two63](Json& result) { result.as_dict().set("slot", Json(two63)); }, Json(kR10Payer), Json(2),
         two63, "2"},
        {"accountIndex 2**63", indexes(Json(1), Json(two63)), Json(kR10Payer), Json(two63), BigInt(950),
         "9223372036854775808"},
    };
}

// solana/restore_lookup step 0's calls, the getTransaction result mutated.
Json r10_calls(Ctx& t, const Json& vector_step, const R10Case& c) {
    Json calls = scfg::deep_copy(vector_step.as_dict().get("calls"));
    Json::Dict& call = calls.as_list()[0].as_dict();
    Result<Json> body = scfg::json::loads(call.get("response_text").as_str());
    if (!t.check(body.ok(), c.name + ": the recorded answer")) return calls;
    Json answer = body.take();
    c.mutate(*answer.as_dict().find("result"));
    call.set("response_text", Json(scfg::json::dumps(answer).value()));
    return calls;
}

bool same(const Json& a, const Json& b) { return json_eq(a, b) && a.type() == b.type(); }

// The receipt Python's restore_apply stored for case c.
Json r10_receipt(const R10Case& c) {
    return Json::dict({{"chain", "solana"}, {"tx_hash", kR10Sig}, {"log_index", c.log_index}, {"block", Json(c.block)},
                       {"kind", "unlock"}, {"item", "chapter2"}, {"amount", "4.993742"}, {"token", "USDC"},
                       {"payer", c.sender}, {"recipient", "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"},
                       {"source", "restore"}});
}

// Each receipt field compared with its type (json_eq alone has "2" != 2 but 2 == 2.0 == True).
void expect_receipt(Ctx& t, const Json& got, const Json& want, const std::string& what) {
    if (!t.check(got.is_dict() && got.as_dict().size() == want.as_dict().size(), what + ": " + scfg::py::repr(got).value())) return;
    for (const auto& [k, v] : want.as_dict()) {
        t.check(same(got.as_dict().get(k), v), what + ": " + k + " " + scfg::py::repr(got.as_dict().get(k)).value());
    }
}

}  // namespace scfg_test::solana_backend_tests

SCFG_TEST(SolanaBackendTests, ACandidateCarriesTheTransferAsPythonDoes) {
    std::optional<Json> v = load("solana/restore_lookup");
    if (!t.check(v.has_value(), "solana/restore_lookup")) return;
    for (const R10Case& c : r10_cases()) {
        Rig rig;
        if (!rig.build(t, v->as_dict().get("setup"))) return;
        const Json calls = r10_calls(t, step_of(*v, 0), c);
        // restore_lookup, restore_apply, then restore_lookup again (the candidate is already restored)
        for (bool second : {false, true}) {
            rig.transport.load(calls);
            std::optional<Result<std::vector<scfg::restore::RestoreCandidate>>> got =
                run_now(rig.engine->restore_lookup(Json(kR10Sig)));
            t.check(rig.transport.problems.empty() && rig.transport.leftover().empty(), c.name + ": the recorded calls");
            if (!t.check(got && got->ok() && got->value().size() == 1,
                         c.name + ": " + (got && !got->ok() ? got->err().str() : std::string("not one candidate")))) {
                break;
            }
            const std::vector<scfg::restore::RestoreCandidate>& candidates = got->value();
            const scfg::restore::RestoreCandidate& cand = candidates[0];
            t.check(cand.chain == "solana" && cand.token == "USDC" && cand.choices.empty() &&
                        cand.kind == std::optional<std::string>("unlock") &&
                        cand.item == std::optional<std::string>("chapter2"),
                    c.name + ": what it bought");
            t.check(cand.already_restored == second, c.name + ": already_restored");
            const auto* tr = std::get_if<scfg::transfers::Transfer>(&cand.transfer);
            if (!t.check(tr != nullptr, c.name + ": a Transfer")) break;
            t.check(same(tr->sender, c.sender), c.name + ": sender " + scfg::py::repr(tr->sender).value());
            t.check(same(tr->log_index, c.log_index), c.name + ": log_index " + scfg::py::repr(tr->log_index).value());
            t.check(tr->block_number == c.block, c.name + ": block_number " + tr->block_number.to_decimal());
            t.check(same(tr->tx_hash, Json(kR10Sig)) && tr->value == BigInt(4993742), c.name + ": the rest");
            if (second) break;
            Result<Json> added = rig.engine->restore_apply(candidates);
            if (!t.check(added.ok() && added.value().is_list() && added.value().as_list().size() == 1,
                         c.name + ": restore_apply " + (added.ok() ? scfg::py::repr(added.value()).value() : added.err().str()))) {
                break;
            }
            expect_receipt(t, added.value().as_list()[0], r10_receipt(c), c.name);
            const Json::Dict& store = rig.engine->receipts().as_dict();
            const Json* stored = store.find(std::string("solana:") + kR10Sig + ":" + c.key_suffix);
            if (t.check(store.size() == 1 && stored != nullptr, c.name + ": the key, in " + scfg::py::repr(rig.engine->receipts()).value())) {
                expect_receipt(t, *stored, r10_receipt(c), c.name + " (stored)");
            }
        }
        t.check(rig.reported.empty(), c.name + ": nothing reported");
    }
}

// ---- readiness ----

SCFG_TEST(SolanaBackendTests, AnAccountInfoResultWithoutValueIsAKeyErrorNotAnRpcError) {
    // tier2/solana/readiness_crash: the Engine contains it (reported, the route stays ready)
    std::optional<Json> v = load("solana/readiness_crash");
    if (!t.check(v.has_value(), "solana/readiness_crash")) return;
    Rig rig;
    if (!rig.build(t, v->as_dict().get("setup"))) return;
    rig.transport.load(step_of(*v, 0).as_dict().get("calls"));
    int built = 0;
    scfg::backends::base::LazyRpc lazy = [&rig, &built]() {
        ++built;
        return rig.engine->rpc("solana");
    };
    const sol::SolanaBackend backend;
    std::optional<Result<scfg::OrderedMap<bool>>> got =
        run_now(backend.check_ready(lazy, rig.chain(), rig.engine->config().solana_wallet));
    t.check(got && !got->ok() && got->err().kind == "KeyError" && got->err().message == "'value'",
            got && !got->ok() ? got->err().str() : "no KeyError");
    SCFG_CHECK_EQ(t, built, 1);  // the first token's check failed
}

// ---- build_payment ----

namespace scfg_test::solana_backend_tests {

constexpr const char* kDest = "EVADx2QYJwSufAQ5Ps5RHRrxKbA5AbTYd28kUwcPcRnm";
constexpr const char* kRef = "9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb9PusVFin";
constexpr const char* kAccount = "AUn431TLHb8hD7r95QKArw2ezfSR5YBZipFo6vu1iZpe";

CannedTransport canned(Json blockhash = "EkSnNWid2cvwEVnVx9aBqawnmiCNiDgp3gUdkDPTKN1N", int64_t fee = 5000) {
    CannedTransport tr;
    Json amount = Json::dict({{"amount", "10000000"}});
    tr.results["getAccountInfo"] = Json::dict(
        {{"value", Json::dict({{"data", Json::dict({{"parsed", Json::dict({{"info", Json::dict({{"tokenAmount", amount}})}})}})}})}});
    tr.results["getBalance"] = Json::dict({{"value", 500000000}});
    tr.results["getLatestBlockhash"] = Json::dict({{"value", Json::dict({{"blockhash", blockhash}})}});
    tr.results["getRecentPrioritizationFees"] = Json::list({Json::dict({{"prioritizationFee", fee}})});
    return tr;
}

Json session(std::initializer_list<std::pair<std::string, Json>> changes = {}) {
    Json r = Json::dict({{"token", "USDC"}, {"dest", kDest}, {"units", 4993742}, {"reference", kRef}});
    for (const auto& [k, value] : changes) r.as_dict().set(k, value);
    return r;
}

// build_payment on the testnet's solana chain: the dict, or "Kind: message".
std::string build(CannedTransport& tr, const Json& rec, Json account = kAccount) {
    scfg::chains::Chain chain = *scfg::chains::TESTNET().find("solana");
    const sol::SolanaBackend backend;
    Result<scfg::backends::base::Rpc> rpc = backend.make_rpc(chain, tr);
    if (!rpc.ok()) return rpc.err().str();
    std::optional<Result<Json>> got = run_now(backend.build_payment(rec, rpc.value(), chain, account));
    if (!got) return "suspended";
    return got->ok() ? scfg::json::dumps(got->value()).value() : got->err().kind + ": " + got->err().message;
}

}  // namespace scfg_test::solana_backend_tests

SCFG_TEST(SolanaBackendTests, BuildPaymentIsPythonsTransaction) {
    CannedTransport tr = canned();
    SCFG_CHECK_EQ(t, build(tr, session()),
                  "{\"transaction\": \"AQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAQHjNbOFL0g6SXUv"
                  "/u01gEFZ9l+G0s9Wqw1QHV9lSx4AfMOjOf6V3IDf9DaQz76yAn4IoS6tC7RBWgOfEXJIrVVcshenxgqTEdhGGtdX/91+0gV8g6eHNV2qiBs"
                  "ofeWko92AwZGb+UhFzL/7K26csOb57yM5bvF9xJrLEObOkAAAAAG3fbh12Whk9nL4UbO63msHLSF7V9bN5E6jPWFfv8AqTtELLORIVfxOp"
                  "M9ATQoLQMrX/7NAaLb8bd5BgjfAC6nhQ8tbgKkevgk0Jq2ncQtcMsoy/okn7fuV7nSVsEnYu/MSQ6SjNLjhzuzQ/yV2jMXnKYPTb9GwsNukS"
                  "mdVdTmuQMDAAUCMHUAAAMACQOIEwAAAAAAAAQFAQUCAAYKDM4yTAAAAAAABg==\", \"token_balance\": \"10000000\", "
                  "\"sol_balance\": \"500000000\"}");
    std::vector<std::string> methods;
    for (const Json& r : tr.requests) methods.push_back(r.as_dict().get("method").as_str());
    t.check(methods == std::vector<std::string>{"getAccountInfo", "getBalance", "getLatestBlockhash",
                                                 "getRecentPrioritizationFees"},
            "the requests, in Python's order");
    SCFG_CHECK_EQ(t, scfg::json::dumps(tr.requests.back().as_dict().get("params")).value(), "[[\"" + std::string(kDest) + "\"]]");
    // a fee above MAX_PRIORITY_FEE is clamped to it: SetComputeUnitPrice(1000000)
    CannedTransport high = canned("EkSnNWid2cvwEVnVx9aBqawnmiCNiDgp3gUdkDPTKN1N", 1000000000);
    std::string paid = build(high, session());
    t.check(paid.find("CQNAQg8AAAAAAAQFAQUCAAYKDM4yTAAAAAAABg==\"") != std::string::npos, paid);
}

SCFG_TEST(SolanaBackendTests, BuildPaymentReadsTheSessionAsPythonDoes) {
    struct Row {
        Json rec;
        Json blockhash;
        Json account;
        std::string expected;
    };
    const Json bh = "EkSnNWid2cvwEVnVx9aBqawnmiCNiDgp3gUdkDPTKN1N";
    const Row rows[] = {
        {session({{"units", "5"}}), bh, kAccount, "AttributeError: 'str' object has no attribute 'to_bytes'"},
        {session({{"units", 5.0}}), bh, kAccount, "AttributeError: 'float' object has no attribute 'to_bytes'"},
        {session({{"units", -1}}), bh, kAccount, "OverflowError: can't convert negative int to unsigned"},
        {session({{"units", Json(BigInt(1) << 64)}}), bh, kAccount, "OverflowError: int too big to convert"},
        {session({{"dest", 5}}), bh, kAccount, "ValueError: not base58: 5"},
        {session({{"dest", "0OIl"}}), bh, kAccount, "ValueError: invalid base58 character '0'"},
        {session({{"reference", Json()}}), bh, kAccount, "ValueError: not base58: None"},
        {session({{"dest", "0OIl"}, {"reference", Json()}}), bh, kAccount, "ValueError: invalid base58 character '0'"},
        {session(), 7, kAccount, "ValueError: not base58: 7"},
        {session({{"reference", Json()}}), 7, kAccount, "ValueError: not base58: None"},
        {session(), bh, "xyz", "ValueError: expected 32 bytes, got 3: 'xyz'"},
        {Json::dict({{"token", "USDC"}, {"dest", kDest}, {"reference", kRef}}), bh, kAccount, "KeyError: 'units'"},
        {Json::dict({{"dest", kDest}}), bh, kAccount, "KeyError: 'token'"},
    };
    for (const Row& r : rows) {
        CannedTransport tr = canned(r.blockhash);
        SCFG_CHECK_EQ(t, build(tr, r.rec, r.account), r.expected);
    }
    CannedTransport tr = canned();
    std::string with_true = build(tr, session({{"units", true}}));  // True.to_bytes: a bool is an int
    t.check(with_true.find("\"token_balance\": \"10000000\"") != std::string::npos, with_true);
}
