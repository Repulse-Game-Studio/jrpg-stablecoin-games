// Copyright 2026 Repulse
// No Python twin: checks of Reverify.cpp below what the Tier 2 vectors pin. Every expectation is the Python
// reference's (scfg_core.engine.Engine.reverify through conformance's replay.build_engine and ReplayTransport) on the
// same setup and node answers, printed by the script quoted in full above each test: a stored log_index (or block)
// of any size is valid and judged against what the node sent, as Python's int is unbounded, and an invalid entry's
// report names its reason as Python's repr of the exception.
#include "support/Helpers.h"
#include "support/JsonEq.h"
#include "support/Test.h"
#include "support/Tier2.h"
#include "support/Vectors.h"

#include "Scfg/BigInt.h"
#include "Scfg/Co.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/Result.h"

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::BigInt;
using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace tier2 = scfg_test::tier2;
using scfg_test::json_eq;
using scfg_test::load_vector;

namespace scfg_test::reverify_tests {

// What Engine.reverify() did with a vector's setup, its receipts replaced, over its first step's calls as `mutate`
// changed them: the removed keys, the kinds and messages reported, and how many recorded calls were never made.
struct Outcome {
    Json removed;
    std::vector<std::string> kinds, messages;
    size_t unused = 0;
    std::vector<std::string> problems;
};

Outcome reverify_with(const Json& vector, const Json& receipts, const std::function<void(Json& calls)>& mutate) {
    Outcome out;
    Json setup = scfg::deep_copy(vector.as_dict().get("setup"));
    setup.as_dict().set("receipts", receipts);
    Json calls = scfg::deep_copy(vector.as_dict().get("steps").as_list()[0].as_dict().get("calls"));
    mutate(calls);
    tier2::ReplayTransport transport;
    tier2::ScriptedEntropy entropy;
    Result<std::unique_ptr<scfg::engine::Engine>> built = tier2::build_engine(
        setup, transport, [] { return int64_t{1700000000}; },
        [&out](const Err& e) {
            out.kinds.push_back(e.kind);
            out.messages.push_back(e.message);
        },
        &entropy);
    if (!built.ok()) {
        out.problems.push_back("build_engine: " + built.err().str());
        return out;
    }
    std::unique_ptr<scfg::engine::Engine> engine = built.take();
    transport.load(calls);
    scfg::Co<Json> co = engine->reverify();
    if (!co.done()) {
        out.problems.push_back("suspended");
        return out;
    }
    Result<Json> removed = co.take();
    if (!removed.ok()) {
        out.problems.push_back("raised " + removed.err().str());
        return out;
    }
    out.removed = removed.take();
    out.unused = transport.leftover().size();
    out.problems = transport.problems;
    return out;
}

// Every eth_getLogs answer's logs get this logIndex.
std::function<void(Json&)> evm_log_index(std::string text) {
    return [text](Json& calls) {
        for (Json& c : calls.as_list()) {
            Json::Dict& call = c.as_dict();
            if (call.get("method").as_str() != "eth_getLogs") continue;
            Json body = scfg::json::loads(call.get("response_text").as_str()).take();
            for (Json& lg : body.as_dict().find("result")->as_list()) lg.as_dict().set("logIndex", Json(text));
            call.set("response_text", Json(scfg::json::dumps(body).value()));
        }
    };
}

const char* const SIG5 = "5jRvfZECEfN9pyWHFUF3GZXj3asr8dfVfZWVg6JLg3hP2bw7WZ6JErRVJxy3fSCqQi2c9oFo9YvSQL6LPtQ8s3mn";

// The developer's row (accountIndex 2) of SIG5's getTransaction answer gets this accountIndex.
std::function<void(Json&)> solana_index(Json value) {
    return [value](Json& calls) {
        for (Json& c : calls.as_list()) {
            Json::Dict& call = c.as_dict();
            if (call.get("method").as_str() != "getTransaction" ||
                !json_eq(call.get("params").as_list()[0], Json(SIG5))) {
                continue;
            }
            Json body = scfg::json::loads(call.get("response_text").as_str()).take();
            Json& result = *body.as_dict().find("result");
            if (!result.is_null()) {
                Json::Dict& meta = result.as_dict().find("meta")->as_dict();
                for (const char* table : {"preTokenBalances", "postTokenBalances"}) {
                    for (Json& b : meta.find(table)->as_list()) {
                        if (json_eq(b.as_dict().get("accountIndex"), Json(2))) b.as_dict().set("accountIndex", value);
                    }
                }
            }
            call.set("response_text", Json(scfg::json::dumps(body).value()));
        }
    };
}

std::string shown(const Outcome& o) {
    std::string s = scfg::py::repr(o.removed).value() + " reported [";
    for (const std::string& k : o.kinds) s += k + " ";
    s += "] unused " + std::to_string(o.unused);
    for (const std::string& p : o.problems) s += "; " + p;
    return s;
}

}  // namespace scfg_test::reverify_tests

using namespace scfg_test::reverify_tests;

// The cases below, their expectations printed by this script (one line per case: the removed keys, the reports'
// classes, the number of recorded calls never made, the transport's problems):
//
// ---- big_index.py (run from the repo root: python3 -I big_index.py .) ----
//   """Carried item 2: Python's reverify of stored receipts whose log_index is 2**63 or more (and other big values).
//
//   python3 -I big_index.py <repo root>
//   """
//   import copy
//   import json
//   import sys
//
//   ROOT = sys.argv[1]
//   sys.path[:0] = [ROOT + "/plugin/game/python-packages", ROOT + "/conformance/python"]
//
//   from scfg_conformance import replay as rp  # noqa: E402
//
//
//   def load(vid):
//       return json.load(open(f"{ROOT}/conformance/vectors/tier2/{vid}.json"))
//
//
//   def run(v, receipts, mutate_calls):
//       setup = copy.deepcopy(v["setup"])
//       setup["receipts"] = receipts
//       step = copy.deepcopy(v["steps"][0])
//       mutate_calls(step["calls"])
//       transport = rp.ReplayTransport()
//       errors = []
//       engine = rp.build_engine(setup, transport=transport, clock=lambda: step["at"], on_error=errors.append)
//       transport.load(step["calls"])
//       removed = engine.reverify()
//       return removed, [type(e).__name__ for e in errors], len(transport.leftover()), transport.problems
//
//
//   def evm_log_index(text):
//       def f(calls):
//           for c in calls:
//               if c["method"] == "eth_getLogs":
//                   body = json.loads(c["response_text"])
//                   for lg in body["result"]:
//                       lg["logIndex"] = text
//                   c["response_text"] = json.dumps(body)
//       return f
//
//
//   evm = load_vector("reverify/genuine_receipt_is_kept")
//   key, rec = next(iter(evm["setup"]["receipts"].items()))
//   for name, stored, node in [
//       ("2**63 vs 0x8000000000000000", 2**63, "0x8000000000000000"),
//       ("2**63 vs 0x2", 2**63, "0x2"),
//       ("'9223372036854775808' vs 0x8000000000000000", "9223372036854775808", "0x8000000000000000"),
//       ("2**70 vs 0x400000000000000000", 2**70, "0x400000000000000000"),
//       ("2**64 + 2 vs 0x2", 2**64 + 2, "0x2"),
//       ("-2**63 vs 0x2", -2**63, "0x2"),
//       ("2.5 vs 0x2", 2.5, "0x2"),
//   ]:
//       r = dict(rec, log_index=stored)
//       print("evm", name, run(evm, {key: r}, evm_log_index(node)))
//
//   r = dict(rec, block=2**63)
//   print("evm block 2**63", run(evm, {key: r}, lambda c: None))
//   r = dict(rec, block=2**63, log_index=2**63)
//   print("evm block and index 2**63", run(evm, {key: r}, lambda c: None))
//
//   sol = load_vector("solana/reverify_receipts")
//   k5 = "solana:5jRvfZECEfN9pyWHFUF3GZXj3asr8dfVfZWVg6JLg3hP2bw7WZ6JErRVJxy3fSCqQi2c9oFo9YvSQL6LPtQ8s3mn:2"
//   rec5 = sol["setup"]["receipts"][k5]
//   sig5 = k5.split(":")[1]
//
//
//   def sol_index(value):
//       def f(calls):
//           for c in calls:
//               if c["method"] == "getTransaction" and c["params"][0] == sig5:
//                   body = json.loads(c["response_text"])
//                   if body.get("result"):
//                       for t in ("preTokenBalances", "postTokenBalances"):
//                           for b in body["result"]["meta"][t]:
//                               if b["accountIndex"] == 2:
//                                   b["accountIndex"] = value
//                   c["response_text"] = json.dumps(body)
//       return f
//
//
//   for name, stored, node in [
//       ("2**63 vs 2", 2**63, 2),
//       ("2**63 vs 2**63", 2**63, 2**63),
//       ("2 vs 2**63", 2, 2**63),
//   ]:
//       r = dict(rec5, log_index=stored)
//       print("solana", name, run(sol, {k5: r}, sol_index(node)))
// ---- end of big_index.py ----
SCFG_TEST(ReverifyTests, AStoredLogIndexOfAnySizeIsJudgedAsPythonDoes) {
    std::optional<Json> evm = load_vector("reverify/genuine_receipt_is_kept");
    std::optional<Json> sol = load_vector("solana/reverify_receipts");
    if (!t.check(evm.has_value() && sol.has_value(), "vectors")) return;
    const Json evm_receipts = evm->as_dict().get("setup").as_dict().get("receipts");
    const Json::Dict& evm_store = evm_receipts.as_dict();
    if (!t.check(evm_store.size() == 1, "one EVM receipt")) return;
    std::string evm_key;
    Json evm_receipt;
    for (const auto& [k, r] : evm_store) {
        evm_key = k;
        evm_receipt = r;
    }
    const BigInt two63 = BigInt(1) << 63;
    struct Case {
        std::string name;
        std::function<void(Json& receipt)> stored;
        std::function<void(Json& calls)> node;
        bool removed;
        std::vector<std::string> reported;
        size_t unused;
    };
    auto index = [](Json v) { return [v](Json& r) { r.as_dict().set("log_index", v); }; };
    auto none = [](Json&) {};
    const std::vector<Case> evm_cases = {
        {"2**63 vs 0x8000000000000000", index(Json(two63)), evm_log_index("0x8000000000000000"), false, {}, 0},
        {"2**63 vs 0x2", index(Json(two63)), evm_log_index("0x2"), true, {}, 0},
        {"'9223372036854775808' vs 0x8000000000000000", index(Json("9223372036854775808")),
         evm_log_index("0x8000000000000000"), false, {}, 0},
        {"2**70 vs 0x400000000000000000", index(Json(BigInt(1) << 70)), evm_log_index("0x400000000000000000"), false,
         {}, 0},
        {"2**64 + 2 vs 0x2", index(Json((BigInt(1) << 64) + BigInt(2))), evm_log_index("0x2"), true, {}, 0},
        {"-2**63 vs 0x2", index(Json(-two63)), evm_log_index("0x2"), true, {"ValueError"}, 2},
        {"2.5 vs 0x2", index(Json(2.5)), evm_log_index("0x2"), false, {}, 0},
        {"block 2**63", [two63](Json& r) { r.as_dict().set("block", Json(two63)); }, none, true, {}, 1},
        {"block and index 2**63",
         [two63](Json& r) {
             r.as_dict().set("block", Json(two63));
             r.as_dict().set("log_index", Json(two63));
         },
         none, true, {}, 1},
    };
    for (const Case& c : evm_cases) {
        Json receipt = scfg::deep_copy(evm_receipt);
        c.stored(receipt);
        Outcome o = reverify_with(*evm, Json::dict({{evm_key, receipt}}), c.node);
        const Json want = c.removed ? Json::list({Json(evm_key)}) : Json::list();
        t.check(o.problems.empty() && json_eq(o.removed, want) && o.kinds == c.reported && o.unused == c.unused,
                "evm " + c.name + ": " + shown(o));
    }

    const std::string k5 = std::string("solana:") + SIG5 + ":2";
    const Json rec5 = sol->as_dict().get("setup").as_dict().get("receipts").as_dict().get(k5);
    if (!t.check(rec5.is_dict(), k5)) return;
    struct SolCase {
        std::string name;
        Json stored, node;
        bool removed;
    };
    for (const SolCase& c : {SolCase{"2**63 vs 2", Json(two63), Json(2), true},
                             SolCase{"2**63 vs 2**63", Json(two63), Json(two63), false},
                             SolCase{"2 vs 2**63", Json(2), Json(two63), true}}) {
        Json receipt = scfg::deep_copy(rec5);
        receipt.as_dict().set("log_index", c.stored);
        Outcome o = reverify_with(*sol, Json::dict({{k5, receipt}}), solana_index(c.node));
        const Json want = c.removed ? Json::list({Json(k5)}) : Json::list();
        t.check(o.problems.empty() && json_eq(o.removed, want) && o.kinds.empty() && o.unused == 8,
                "solana " + c.name + ": " + shown(o));
    }
}

// reverify/genuine_receipt_is_kept's setup with these entries (no request is made): the removed keys in the store's
// order and each report's str(), printed by this script. (Python 3.12's int(None) wording differs from 3.9's, so
// entry "m"'s message is compared by prefix only.)
//
// ---- messages.py (run from the repo root: python3 -I messages.py .) ----
//   """The messages Python's reverify reports for invalid stored entries. python3 -I messages.py <repo root>"""
//   import copy, json, sys
//   ROOT = sys.argv[1]
//   sys.path[:0] = [ROOT + "/plugin/game/python-packages", ROOT + "/conformance/python"]
//   from scfg_conformance import replay as rp
//   v = json.load(open(f"{ROOT}/conformance/vectors/tier2/reverify/genuine_receipt_is_kept.json"))
//   key, rec = next(iter(v["setup"]["receipts"].items()))
//   entries = {
//       "a": {},
//       "b": dict(rec, chain=[1]),
//       "c": dict(rec, item={"x": 1}),
//       "d": dict(rec, tx_hash=5),
//       "e": {k: x for k, x in dict(rec, log_index=-1).items() if k != "block"},
//       "f": dict(rec, log_index="x"),
//       "g": dict(rec, block=float("inf")),
//       "h": dict(rec, amount="1e99"),
//       "i": dict(rec, tx_hash=" " + rec["tx_hash"]),
//       "j": dict(rec, tx_hash="0x12"),
//       "k": [1, 2],
//       "l": dict(rec, chain="gone", tx_hash="not a hash"),
//       "m": dict(rec, log_index=None),
//   }
//   setup = copy.deepcopy(v["setup"]); setup["receipts"] = entries
//   t = rp.ReplayTransport(); errors = []
//   engine = rp.build_engine(setup, transport=t, clock=lambda: 0, on_error=errors.append)
//   t.load([])
//   print(json.dumps(engine.reverify()))
//   for e in errors: print(json.dumps([type(e).__name__, str(e)]))
//   print(t.problems)
// ---- end of messages.py ----
SCFG_TEST(ReverifyTests, AnInvalidEntryIsRemovedWithPythonsReason) {
    std::optional<Json> evm = load_vector("reverify/genuine_receipt_is_kept");
    if (!t.check(evm.has_value(), "vector")) return;
    Json rec;
    const Json stored = evm->as_dict().get("setup").as_dict().get("receipts");
    for (const auto& [k, r] : stored.as_dict()) {
        (void)k;
        rec = r;
    }
    auto with = [&rec](std::string_view field, Json v) {
        Json r = scfg::deep_copy(rec);
        r.as_dict().set(std::string(field), std::move(v));
        return r;
    };
    const std::string tx = rec.as_dict().get("tx_hash").as_str();
    Json no_block = with("log_index", -1);
    no_block.as_dict().erase("block");
    Json gone = with("chain", "gone");
    gone.as_dict().set("tx_hash", "not a hash");
    const Json entries = Json::dict({{"a", Json::dict()},
                                     {"b", with("chain", Json::list({1}))},
                                     {"c", with("item", Json::dict({{"x", 1}}))},
                                     {"d", with("tx_hash", 5)},
                                     {"e", no_block},
                                     {"f", with("log_index", "x")},
                                     {"g", with("block", Json(std::numeric_limits<double>::infinity()))},
                                     {"h", with("amount", "1e99")},
                                     {"i", with("tx_hash", " " + tx)},
                                     {"j", with("tx_hash", "0x12")},
                                     {"k", Json::list({1, 2})},
                                     {"l", gone},
                                     {"m", with("log_index", Json())}});
    Outcome o = reverify_with(*evm, entries, [](Json& calls) { calls = Json::list(); });
    t.check(o.problems.empty(), shown(o));
    t.check(json_eq(o.removed, Json::list({"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "m"})),
            "removed " + scfg::py::repr(o.removed).value());
    const std::string p = "is invalid and was removed: ";
    const std::vector<std::string> want = {
        "stored receipt 'a' " + p + "KeyError('chain')",
        "stored receipt 'b' " + p + "TypeError(\"unhashable type: 'list'\")",
        "stored receipt 'c' " + p + "TypeError(\"unhashable type: 'dict'\")",
        "stored receipt 'd' " + p + "AttributeError(\"'int' object has no attribute 'lower'\")",
        "stored receipt 'e' " + p + "ValueError('log index and block must be >= 0')",
        "stored receipt 'f' " + p + "ValueError(\"invalid literal for int() with base 10: 'x'\")",
        "stored receipt 'g' " + p + "OverflowError('cannot convert float infinity to integer')",
        "stored receipt 'h' " + p + "ValueError(\"amount out of range: '1e99'\")",
        "stored receipt 'i' " + p + "ValueError(\"tx id with surrounding whitespace: ' " + tx + "'\")",
        "stored receipt 'j' " + p + "ValueError(\"not a transaction hash: '0x12'\")",
        "stored receipt 'k' " + p + "TypeError('list indices must be integers or slices, not str')",
    };
    if (!t.check(o.messages.size() == want.size() + 1, "12 reports, got " + std::to_string(o.messages.size()))) return;
    for (size_t i = 0; i < want.size(); ++i) SCFG_CHECK_EQ(t, o.messages[i], want[i]);
    t.check(o.messages[11].starts_with("stored receipt 'm' " + p + "TypeError(\"int() argument must be a string"),
            o.messages[11]);
    for (const std::string& k : o.kinds) SCFG_CHECK_EQ(t, k, "ValueError");
}
