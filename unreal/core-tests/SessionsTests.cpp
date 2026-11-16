// No Python twin: tests of scfg::sessions (sessions.py has no vector topic of its own; normalize_tx_hash's vectors
// are tier1/tx_ids.json, task 11): create_session's record and draw order, list_sessions' order and its TypeErrors
// (the C# port gave a KeyError for a record that is not a dict), set_hint, set_ui_status, the is_past_* arithmetic
// and session_price. Every expectation is scfg_core's own output (python3 with plugin/game/python-packages, its
// `secrets` replaced by a script that records the draws).
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Dec.h"
#include "Scfg/Entropy.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/Sessions.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using scfg::BigInt;
using scfg::Dec;
using scfg::Json;
using scfg::Result;
namespace sessions = scfg::sessions;

static const char* const kHash = "0xabababababababababababababababababababababababababababababababab";
static const char* const kHashMixed = "0xAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAbAb";

template <class T>
static std::string outcome(const Result<T>& r) {
    if (r.ok()) return "ok";
    return r.err().kind + ": " + r.err().message;
}

static std::string outcome(const scfg::Status& r) {
    if (r.ok()) return "ok";
    return r.err().kind + ": " + r.err().message;
}

// The Python test's scripted `secrets`: fixed answers, every draw recorded.
class Scripted final : public scfg::Entropy {
public:
    std::vector<std::string> draws;
    std::vector<uint8_t> token_bytes(int n) override {
        draws.push_back("token_bytes(" + std::to_string(n) + ")");
        return std::vector<uint8_t>(static_cast<size_t>(n), 0x5a);
    }
    std::string token_hex(int n) override {
        draws.push_back("token_hex(" + std::to_string(n) + ")");
        return "00112233aabbccdd";
    }
    Result<int64_t> randbelow(int64_t n) override {
        draws.push_back("randbelow(" + std::to_string(n) + ")");
        return int64_t{42};
    }
    std::string joined() const {
        std::string s;
        for (const std::string& d : draws) s += (s.empty() ? "" : " ") + d;
        return s;
    }
};

static Dec dec(const char* s) { return *Dec::parse(s).value(); }

static sessions::NewSession unlock() {
    return sessions::NewSession{.kind = "unlock", .item = "chapter2", .title = "Chapter 2", .token = "USDC",
                                .chain = "base", .price = Json("4.99"), .code = 2, .start_block = 100, .now = 1000};
}

SCFG_TEST(SessionsTests, Constants) {
    SCFG_CHECK_EQ(t, sessions::SESSION_WINDOW_S, 1800);
    SCFG_CHECK_EQ(t, sessions::SCAN_HORIZON_S, 86400);
    SCFG_CHECK_EQ(t, sessions::HINT_TIMEOUT_S, 1800);
    SCFG_CHECK_EQ(t, sessions::SOLANA_HINT_TIMEOUT_S, 180);
    SCFG_CHECK_EQ(t, sessions::UI_STATUSES.size(), 4);
    SCFG_CHECK_EQ(t, sessions::UI_STATUSES[0], "open");
    SCFG_CHECK_EQ(t, sessions::UI_STATUSES[3], "expired");
}

SCFG_TEST(SessionsTests, NormalizeTxHash) {
    auto norm = [](Json v) {
        Result<std::string> r = sessions::normalize_tx_hash(v);
        return r.ok() ? r.value() : outcome(r);
    };
    SCFG_CHECK_EQ(t, norm(std::string(" ") + kHashMixed + " \n"), kHash);
    SCFG_CHECK_EQ(t, norm(std::string("\xE3\x80\x80") + kHashMixed), kHash);  // U+3000 is str.strip() whitespace
    SCFG_CHECK_EQ(t, norm(std::string(kHashMixed) + "\n"), kHash);
    SCFG_CHECK_EQ(t, norm("0Xabababababababababababababababababababababababababababababababab"),
                  "ValueError: not a transaction hash: '0Xabababababababababababababababababababababababababababababababab'");
    SCFG_CHECK_EQ(t, norm("0x" + std::string(63, 'a')), "ValueError: not a transaction hash: '0x" + std::string(63, 'a') + "'");
    SCFG_CHECK_EQ(t, norm("0x" + std::string(65, 'a')), "ValueError: not a transaction hash: '0x" + std::string(65, 'a') + "'");
    SCFG_CHECK_EQ(t, norm("0x" + std::string(63, 'a') + "g"), "ValueError: not a transaction hash: '0x" + std::string(63, 'a') + "g'");
    std::string arabic;
    for (int i = 0; i < 64; ++i) arabic += "\xD9\xA3";  // U+0663: a \d digit, not in [0-9a-fA-F]
    SCFG_CHECK_EQ(t, norm("0x" + arabic), "ValueError: not a transaction hash: '0x" + arabic + "'");
    SCFG_CHECK_EQ(t, norm(5), "ValueError: not a transaction hash: 5");
    SCFG_CHECK_EQ(t, norm(Json()), "ValueError: not a transaction hash: None");
    SCFG_CHECK_EQ(t, norm(""), "ValueError: not a transaction hash: ''");
}

SCFG_TEST(SessionsTests, CreateSessionRecordAndDraws) {
    Json::Dict store;
    Scripted e;
    Result<Json> r = sessions::create_session(store, unlock(), e);
    SCFG_CHECK_EQ(t, outcome(r), "ok");
    if (!r.ok()) return;
    SCFG_CHECK_EQ(t, scfg::json::dumps(r.value()),
                  "{\"id\": \"ps_00112233aabbccdd\", \"kind\": \"unlock\", \"item\": \"chapter2\", \"title\": \"Chapter 2\", "
                  "\"token\": \"USDC\", \"chain\": \"base\", \"price\": \"4.99\", \"code\": 2, \"nonce\": 42, "
                  "\"units\": 4990242, \"start_block\": 100, \"cursor\": 100, \"created_at\": 1000, \"state\": \"awaiting\", "
                  "\"ui_status\": \"open\", \"hint_tx\": null, \"hint_at\": null, \"match\": null}");
    SCFG_CHECK_EQ(t, e.joined(), "randbelow(100) token_hex(8)");
    // The record returned is the stored one.
    SCFG_CHECK(t, store.size() == 1 && store.find("ps_00112233aabbccdd") != nullptr &&
                      store.find("ps_00112233aabbccdd")->same_object(r.value()));
    SCFG_CHECK(t, sessions::get_session(store, "ps_00112233aabbccdd").same_object(r.value()));
    SCFG_CHECK(t, sessions::get_session(store, "ps_none").is_null());

    // A Decimal price, a given nonce (no randbelow), a float clock and extra fields after the schema's.
    sessions::NewSession tip{.kind = "tip", .item = Json(), .title = "Tip", .token = "USDT", .chain = "base",
                             .price = dec("5"), .code = 0, .start_block = 7, .now = 1.5, .nonce = 3,
                             .extra = Json::dict({{"dest", "X"}, {"reference", "R"}})};
    Scripted e2;
    Json::Dict store2;
    Result<Json> r2 = sessions::create_session(store2, tip, e2);
    SCFG_CHECK_EQ(t, outcome(r2), "ok");
    if (r2.ok()) {
        SCFG_CHECK_EQ(t, scfg::json::dumps(r2.value()),
                      "{\"id\": \"ps_00112233aabbccdd\", \"kind\": \"tip\", \"item\": null, \"title\": \"Tip\", "
                      "\"token\": \"USDT\", \"chain\": \"base\", \"price\": \"5.00\", \"code\": 0, \"nonce\": 3, "
                      "\"units\": 5000003, \"start_block\": 7, \"cursor\": 7, \"created_at\": 1.5, \"state\": \"awaiting\", "
                      "\"ui_status\": \"open\", \"hint_tx\": null, \"hint_at\": null, \"match\": null, \"dest\": \"X\", "
                      "\"reference\": \"R\"}");
    }
    SCFG_CHECK_EQ(t, e2.joined(), "token_hex(8)");
}

SCFG_TEST(SessionsTests, CreateSessionFailsInPythonsOrder) {
    auto attempt = [](sessions::NewSession a, std::string& draws) {
        Json::Dict store;
        Scripted e;
        Result<Json> r = sessions::create_session(store, a, e);
        draws = e.joined();
        if (r.ok() || !store.empty()) return std::string("stored");
        return outcome(r);
    };
    std::string draws;
    sessions::NewSession a = unlock();
    a.kind = "refund";
    SCFG_CHECK_EQ(t, attempt(a, draws), "ValueError: invalid kind 'refund'");
    SCFG_CHECK_EQ(t, draws, "");
    a = unlock();
    a.kind = Json();
    SCFG_CHECK_EQ(t, attempt(a, draws), "ValueError: invalid kind None");
    a = unlock();
    a.price = Json("abc");
    SCFG_CHECK_EQ(t, attempt(a, draws), "ValueError: not a number: 'abc'");
    SCFG_CHECK_EQ(t, draws, "randbelow(100)");
    a = unlock();
    a.price = Json(4.99);
    SCFG_CHECK_EQ(t, attempt(a, draws), "TypeError: amounts must be str, int or Decimal (never float)");
    SCFG_CHECK_EQ(t, draws, "randbelow(100)");
    a = unlock();
    a.extra = Json::dict({{"dest", "X"}, {"cursor", 1}});
    SCFG_CHECK_EQ(t, attempt(a, draws), "ValueError: session field 'cursor' is reserved");
    SCFG_CHECK_EQ(t, draws, "randbelow(100) token_hex(8)");
    a = unlock();
    a.extra = Json::list({1});
    SCFG_CHECK_EQ(t, attempt(a, draws).substr(0, 10), "TypeError:");
    SCFG_CHECK_EQ(t, draws, "randbelow(100) token_hex(8)");
    // A falsy extra is Python's `extra or {}`.
    a = unlock();
    a.extra = Json::list();
    Json::Dict store;
    Scripted e;
    SCFG_CHECK(t, sessions::create_session(store, a, e).ok());
    // encode runs while the record is built, after the id draw.
    a = unlock();
    a.code = 100;
    SCFG_CHECK_EQ(t, attempt(a, draws), "ValueError: code must be an int in 0..99");
    SCFG_CHECK_EQ(t, draws, "randbelow(100) token_hex(8)");
}

SCFG_TEST(SessionsTests, RemoveSession) {
    Json::Dict store;
    store.set("a", Json::dict());
    sessions::remove_session(store, "b");
    SCFG_CHECK_EQ(t, store.size(), 1);
    sessions::remove_session(store, "a");
    SCFG_CHECK(t, store.empty());
}

static Json rec(Json created_at) { return Json::dict({{"created_at", created_at}}); }

static std::string order(const Json::Dict& store) {
    Result<std::vector<Json>> r = sessions::list_sessions(store);
    if (!r.ok()) return outcome(r);
    std::string s;
    for (const Json& x : r.value()) s += (s.empty() ? "" : " ") + scfg::json::dumps(x.as_dict().get("created_at"));
    return s;
}

SCFG_TEST(SessionsTests, ListSessionsOldestFirst) {
    Json::Dict store;
    SCFG_CHECK_EQ(t, order(store), "");
    store.set("a", rec(5));
    store.set("b", rec(1.5));
    store.set("c", rec(true));
    store.set("d", rec(5.0));
    store.set("e", rec(Json(BigInt::from_uint64(9007199254740993ULL))));  // 2**53 + 1: above 2.0**53, exactly
    store.set("f", rec(9007199254740992.0));
    store.set("g", rec(Json(BigInt(1) << 70)));
    store.set("h", rec(-std::numeric_limits<double>::infinity()));
    // Ties keep the store's order (5 before 5.0).
    SCFG_CHECK_EQ(t, order(store), "-Infinity true 1.5 5 5.0 9007199254740992.0 9007199254740993 1180591620717411303424");
    // The records returned are the stored ones.
    Result<std::vector<Json>> r = sessions::list_sessions(store);
    SCFG_CHECK(t, r.ok() && r.value()[0].same_object(*store.find("h")));

    Json::Dict strs;
    strs.set("a", rec("b"));
    strs.set("b", rec("a"));
    strs.set("c", rec("\xEF\xBD\x9A"));
    strs.set("d", rec("\xF0\x9F\x98\x80"));
    SCFG_CHECK_EQ(t, order(strs), "\"a\" \"b\" \"\\uff5a\" \"\\ud83d\\ude00\"");
}

SCFG_TEST(SessionsTests, ListSessionsTypeErrors) {
    auto one = [](Json value) {
        Json::Dict s;
        s.set("a", value);
        return order(s);
    };
    auto two = [](Json x, Json y) {
        Json::Dict s;
        s.set("a", rec(x));
        s.set("b", rec(y));
        return order(s);
    };
    // A lone record is never compared, but its key is read.
    SCFG_CHECK_EQ(t, one(rec(Json())), "null");
    SCFG_CHECK_EQ(t, one(Json::dict()), "KeyError: 'created_at'");
    // A record that is not a dict is a TypeError, as Python's r["created_at"] (the C# port gave a KeyError).
    SCFG_CHECK_EQ(t, one(Json::list({1})), "TypeError: list indices must be integers or slices, not str");
    SCFG_CHECK_EQ(t, one("x").substr(0, 10), "TypeError:");
    SCFG_CHECK_EQ(t, one(Json()), "TypeError: 'NoneType' object is not subscriptable");
    SCFG_CHECK_EQ(t, one(7), "TypeError: 'int' object is not subscriptable");
    SCFG_CHECK_EQ(t, two("b", 1).substr(0, 39), "TypeError: '<' not supported between in");
    SCFG_CHECK_EQ(t, two(Json(), Json()), "TypeError: '<' not supported between instances of 'NoneType' and 'NoneType'");
    SCFG_CHECK_EQ(t, two(Json::dict(), 1).substr(0, 10), "TypeError:");
    // Port limit: lists are refused (Python would order them lexicographically).
    SCFG_CHECK_EQ(t, two(Json::list({1}), Json::list({2})).substr(0, 10), "TypeError:");
    // A NaN sorts after the numbers.
    SCFG_CHECK_EQ(t, two(std::nan(""), 3), "3 NaN");
}

SCFG_TEST(SessionsTests, SetHint) {
    Json::Dict store;
    Json r = Json::dict({{"hint_tx", Json()}, {"hint_at", Json()}});
    store.set("s", r);
    SCFG_CHECK_EQ(t, outcome(sessions::set_hint(store, "s", std::string(" ") + kHashMixed, 7)), "ok");
    SCFG_CHECK_EQ(t, scfg::json::dumps(r), std::string("{\"hint_tx\": \"") + kHash + "\", \"hint_at\": 7}");
    SCFG_CHECK_EQ(t, outcome(sessions::set_hint(store, "x", kHash, 7)), "KeyError: 'x'");
    SCFG_CHECK_EQ(t, outcome(sessions::set_hint(store, "s", "bad", 8)), "ValueError: not a transaction hash: 'bad'");
    SCFG_CHECK_EQ(t, r.as_dict().get("hint_at").as_int64(), 7);  // untouched by the failed hint
    // A backend's own normalisation (Solana's normalize_signature).
    sessions::Normalize upper = [](const Json& v) -> Result<std::string> { return "N:" + v.as_str(); };
    SCFG_CHECK_EQ(t, outcome(sessions::set_hint(store, "s", "sig", 9.5, upper)), "ok");
    SCFG_CHECK_EQ(t, scfg::json::dumps(r), "{\"hint_tx\": \"N:sig\", \"hint_at\": 9.5}");
    sessions::clear_hint(r.as_dict());
    SCFG_CHECK_EQ(t, scfg::json::dumps(r), "{\"hint_tx\": null, \"hint_at\": null}");
    // A record that is not a dict: normalize runs first, then the item assignment fails.
    Json::Dict bad;
    bad.set("l", Json::list({1}));
    bad.set("s", "str");
    bad.set("n", Json());
    SCFG_CHECK_EQ(t, outcome(sessions::set_hint(bad, "l", "bad", 7)), "ValueError: not a transaction hash: 'bad'");
    SCFG_CHECK_EQ(t, outcome(sessions::set_hint(bad, "l", kHash, 7)),
                  "TypeError: list indices must be integers or slices, not str");
    SCFG_CHECK_EQ(t, outcome(sessions::set_hint(bad, "s", kHash, 7)), "TypeError: 'str' object does not support item assignment");
    SCFG_CHECK_EQ(t, outcome(sessions::set_hint(bad, "n", kHash, 7)),
                  "TypeError: 'NoneType' object does not support item assignment");
}

SCFG_TEST(SessionsTests, SetUiStatus) {
    Json::Dict store;
    Json r = Json::dict({{"ui_status", "open"}});
    store.set("s", r);
    for (std::string_view s : sessions::UI_STATUSES) {
        SCFG_CHECK_EQ(t, outcome(sessions::set_ui_status(store, "s", Json(s))), "ok");
        SCFG_CHECK_EQ(t, r.as_dict().get("ui_status").as_str(), s);
    }
    // The status is checked before the session is looked up.
    SCFG_CHECK_EQ(t, outcome(sessions::set_ui_status(store, "x", "bogus")), "ValueError: invalid ui status 'bogus'");
    SCFG_CHECK_EQ(t, outcome(sessions::set_ui_status(store, "x", "open")), "KeyError: 'x'");
    SCFG_CHECK_EQ(t, outcome(sessions::set_ui_status(store, "s", 5)), "ValueError: invalid ui status 5");
    SCFG_CHECK_EQ(t, outcome(sessions::set_ui_status(store, "s", "Open")), "ValueError: invalid ui status 'Open'");
    store.set("l", Json::list({1}));
    SCFG_CHECK_EQ(t, outcome(sessions::set_ui_status(store, "l", "open")),
                  "TypeError: list indices must be integers or slices, not str");
    SCFG_CHECK_EQ(t, r.as_dict().get("ui_status").as_str(), "expired");
}

static std::string past(Json created_at, Json now, bool horizon = false) {
    Json r = Json::dict({{"created_at", created_at}});
    Result<bool> p = horizon ? sessions::is_past_horizon(r, now) : sessions::is_past_window(r, now);
    if (!p.ok()) return outcome(p);
    return p.value() ? "past" : "not";
}

SCFG_TEST(SessionsTests, IsPastWindowAndHorizon) {
    SCFG_CHECK_EQ(t, past(1000, 2799), "not");
    SCFG_CHECK_EQ(t, past(1000, 2800), "past");
    SCFG_CHECK_EQ(t, past(1000, 2799.5), "not");
    SCFG_CHECK_EQ(t, past(1000, 2800.0), "past");
    SCFG_CHECK_EQ(t, past(1000, std::nan("")), "not");
    SCFG_CHECK_EQ(t, past(1000, std::numeric_limits<double>::infinity()), "past");
    SCFG_CHECK_EQ(t, past(1000, std::numeric_limits<double>::infinity(), true), "past");
    SCFG_CHECK_EQ(t, past(1000, 2800, true), "not");
    SCFG_CHECK_EQ(t, past(0, 86400, true), "past");
    SCFG_CHECK_EQ(t, past(0, 86399.9, true), "not");
    SCFG_CHECK_EQ(t, past(true, 1801), "past");
    // Two ints subtract exactly; with a float, both are floats (2**60 + 1800 rounds to 2**60 + 1792).
    SCFG_CHECK_EQ(t, past(Json(BigInt(1) << 60), Json((BigInt(1) << 60) + BigInt(1800))), "past");
    SCFG_CHECK_EQ(t, past(1152921504606846976.0, Json((BigInt(1) << 60) + BigInt(1800))), "not");
    SCFG_CHECK_EQ(t, past(0, Json(BigInt(1) << 1100)), "past");
    SCFG_CHECK_EQ(t, past(1.0, Json(BigInt(1) << 1100)), "OverflowError: int too large to convert to float");
    SCFG_CHECK_EQ(t, past(Json(), 5), "TypeError: unsupported operand type(s) for -: 'int' and 'NoneType'");
    SCFG_CHECK_EQ(t, past("1", 5), "TypeError: unsupported operand type(s) for -: 'int' and 'str'");
    SCFG_CHECK_EQ(t, past(1, Json()), "TypeError: unsupported operand type(s) for -: 'NoneType' and 'int'");
    SCFG_CHECK_EQ(t, outcome(sessions::is_past_window(Json::dict(), 5)), "KeyError: 'created_at'");
    SCFG_CHECK_EQ(t, outcome(sessions::is_past_horizon(Json::list({1}), 5)),
                  "TypeError: list indices must be integers or slices, not str");
}

SCFG_TEST(SessionsTests, SessionPrice) {
    Result<Dec> p = sessions::session_price(Json::dict({{"price", "4.99"}}));
    SCFG_CHECK(t, p.ok() && p.value().str() == "4.99");
    SCFG_CHECK_EQ(t, outcome(sessions::session_price(Json::dict({{"price", "1E+41"}}))),
                  "ValueError: amount out of range: '1E+41'");
    SCFG_CHECK_EQ(t, outcome(sessions::session_price(Json::dict())), "KeyError: 'price'");
}

SCFG_TEST(SessionsTests, NewSessionId) {
    Scripted e;
    SCFG_CHECK_EQ(t, sessions::new_session_id(e), "ps_00112233aabbccdd");
    SCFG_CHECK_EQ(t, e.joined(), "token_hex(8)");
    scfg::SystemEntropy sys;
    std::string id = sessions::new_session_id(sys);
    SCFG_CHECK_EQ(t, id.size(), 19);
    SCFG_CHECK_EQ(t, id.substr(0, 3), "ps_");
}
