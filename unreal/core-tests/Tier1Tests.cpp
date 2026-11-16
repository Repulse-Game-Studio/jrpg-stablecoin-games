// No Python twin: Tier1.<topic> replays each conformance/vectors/tier1 topic through support/Tier1 (one runtime test
// per vector file, so a topic whose task has not landed fails), and Tier1Tests checks the runner itself.
#include "support/Test.h"
#include "support/Tier1.h"
#include "support/Vectors.h"

#include "Scfg/Err.h"
#include "Scfg/Json.h"

#include <algorithm>
#include <optional>
#include <string>

using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace tier1 = scfg_test::tier1;

static bool contains(const std::optional<std::string>& s, std::string_view part) {
    return s.has_value() && s->find(part) != std::string::npos;
}

SCFG_TEST_SOURCE(Tier1Topics) {
    std::string error;
    std::vector<std::string> topics = scfg_test::vectors::tier1_topics(&error);
    if (!error.empty() || topics.empty()) {
        scfg_test::add("Tier1", "listing", [error](scfg_test::Ctx& t) {
            t.fail("cannot list " + scfg_test::vectors::root() + "/tier1: " + (error.empty() ? "no topics" : error));
        });
    }
    for (const std::string& topic : topics) {
        scfg_test::add("Tier1", topic, [topic](scfg_test::Ctx& t) {
            std::vector<std::string> failures = tier1::run_topic(topic);
            t.check(failures.empty(), tier1::report("tier1/" + topic, failures));
        });
    }
}

// A private registry with one adapter per outcome a case can meet.
static tier1::Registry probes() {
    tier1::Registry r;
    r.topics.push_back("probes");
    tier1::Topic fn(r, "probes");
    fn("value", [](const Json&) -> Result<Json> { return Err::value_error("bad"); });
    fn("type", [](const Json&) -> Result<Json> { return Err::type_error("bad"); });
    fn("config", [](const Json&) -> Result<Json> { return Err::config_error("a.b", "bad"); });
    fn("key", [](const Json&) -> Result<Json> { return Err::key_error("'k'"); });
    fn("arith", [](const Json&) -> Result<Json> { return Err::make("InvalidOperation", "big"); });
    fn("restore", [](const Json&) -> Result<Json> { return Err::restore_error("tx_not_found", "gone"); });
    fn("one", [](const Json&) -> Result<Json> { return Json(1); });
    fn("echo", [](const Json& in) -> Result<Json> { return in; });
    return r;
}

static Json make_case(std::string_view fn, std::string_view key, Json value) {
    Json c = Json::dict({{"fn", Json(fn)}, {"in", Json::dict()}});
    c.as_dict().set(std::string(key), std::move(value));
    return c;
}

SCFG_TEST(Tier1Tests, OnlyValueTypeAndConfigErrorsAreRejections) {
    tier1::Registry r = probes();
    auto run = [&r](std::string_view fn, std::string_view key, Json v) { return tier1::run_case(make_case(fn, key, std::move(v)), r); };
    SCFG_CHECK(t, !run("value", "error", "invalid").has_value());
    SCFG_CHECK(t, !run("type", "error", "invalid").has_value());
    SCFG_CHECK(t, !run("config", "error", "a.b").has_value());
    SCFG_CHECK(t, contains(run("key", "error", "invalid"), "KeyError"));
    SCFG_CHECK(t, contains(run("arith", "error", "invalid"), "InvalidOperation"));
    SCFG_CHECK(t, contains(run("restore", "error", "invalid"), "RestoreError"));
    // a ConfigError is a ValueError, yet "invalid" means a rejection that is not a ConfigError
    SCFG_CHECK(t, run("config", "error", "invalid").has_value());
    SCFG_CHECK(t, run("config", "error", "a.c").has_value());
    SCFG_CHECK(t, run("value", "error", "a.b").has_value());
    SCFG_CHECK(t, run("value", "out", 1).has_value());
    SCFG_CHECK(t, run("one", "error", "invalid").has_value());
}

SCFG_TEST(Tier1Tests, OutputsCompareAsJson) {
    tier1::Registry r = probes();
    auto run = [&r](std::string_view fn, std::string_view key, Json v) { return tier1::run_case(make_case(fn, key, std::move(v)), r); };
    SCFG_CHECK(t, !run("one", "out", 1).has_value());
    SCFG_CHECK(t, !run("one", "out", 1.0).has_value());  // JSON equality: 1 == 1.0
    SCFG_CHECK(t, contains(run("one", "out", "1"), "out"));
    SCFG_CHECK(t, run("one", "out", true).has_value());  // true is not 1
    // the input reaches the adapter, and key order does not matter
    Json c = Json::dict({{"fn", "echo"}, {"in", Json::dict({{"a", 1}, {"b", Json::list({"x"})}})},
                         {"out", Json::dict({{"b", Json::list({"x"})}, {"a", 1}})}});
    SCFG_CHECK(t, !tier1::run_case(c, r).has_value());
    // the message names the first difference
    c.as_dict().set("out", Json::dict({{"a", 2}, {"b", Json::list({"x"})}}));
    std::optional<std::string> diff = tier1::run_case(c, r);
    t.check(contains(diff, "a: 1 != 2"), diff.value_or("(passed)"));
}

SCFG_TEST(Tier1Tests, TheExpectationIsTheKeysPresent) {
    tier1::Registry r = probes();
    // an explicit "error": null is an expected error null, never an "out" case (unlike Tier1.cs)
    SCFG_CHECK(t, tier1::run_case(make_case("value", "error", Json()), r).has_value());
    SCFG_CHECK(t, tier1::run_case(make_case("one", "error", Json()), r).has_value());
    // "out": null expects None
    SCFG_CHECK(t, tier1::run_case(make_case("one", "out", Json()), r).has_value());
    // neither key: nothing can agree
    Json bare = Json::dict({{"fn", "one"}, {"in", Json::dict()}});
    SCFG_CHECK(t, tier1::run_case(bare, r).has_value());
    // both keys: nothing can agree either
    Json both = make_case("one", "out", 1);
    both.as_dict().set("error", "invalid");
    SCFG_CHECK(t, tier1::run_case(both, r).has_value());
}

SCFG_TEST(Tier1Tests, MalformedCasesAndMissingAdaptersFail) {
    tier1::Registry r = probes();
    SCFG_CHECK(t, contains(tier1::run_case(make_case("nope", "out", 1), r), "no C++ adapter for nope"));
    SCFG_CHECK(t, tier1::run_case(Json::list(), r).has_value());
    SCFG_CHECK(t, tier1::run_case(Json::dict({{"in", Json::dict()}, {"out", 1}}), r).has_value());
    SCFG_CHECK(t, tier1::run_case(Json::dict({{"fn", "one"}, {"in", Json::list()}, {"out", 1}}), r).has_value());
}

SCFG_TEST(Tier1Tests, ATopicWithoutAdaptersOrCasesFails) {
    tier1::Registry none;  // nothing registered: every vector topic fails, naming why
    std::vector<std::string> failures = tier1::run_topic("amounts", none);
    SCFG_CHECK(t, !failures.empty());
    SCFG_CHECK(t, !failures.empty() && failures[0].find("SCFG_TIER1_TOPIC(amounts)") != std::string::npos);
    SCFG_CHECK(t, std::any_of(failures.begin(), failures.end(),
                              [](const std::string& f) { return f.find("no C++ adapter for parse_amount") != std::string::npos; }));
    // a topic file that does not exist fails too
    tier1::Registry r = probes();
    r.topics.push_back("no_such_topic");
    failures = tier1::run_topic("no_such_topic", r);
    SCFG_CHECK(t, failures.size() == 1 && failures[0].find("cannot be read") != std::string::npos);
}

SCFG_TEST(Tier1Tests, ReportShowsTheFirstFailures) {
    std::vector<std::string> many;
    for (int k = 0; k < 25; ++k) many.push_back("case " + std::string(1, static_cast<char>('a' + k)));
    std::string r = tier1::report("tier1/x", many);
    SCFG_CHECK(t, r.find("tier1/x: 25 failing case(s)") == 0);
    SCFG_CHECK(t, r.find("case t") != std::string::npos && r.find("case u") == std::string::npos);
    SCFG_CHECK(t, r.find("... and 5 more") != std::string::npos);
}

// Every topic file has its adapters' registration, once, and every registration has a topic file.
SCFG_TEST(Tier1Tests, EveryTopicIsRegisteredOnceAndHasVectors) {
    const tier1::Registry& r = tier1::registry();
    for (const std::string& e : r.errors) t.fail(e);
    std::vector<std::string> files = scfg_test::vectors::tier1_topics();
    SCFG_CHECK(t, !files.empty());
    for (const std::string& topic : files) {
        t.check(std::find(r.topics.begin(), r.topics.end(), topic) != r.topics.end(),
                "tier1/" + topic + ".json has no SCFG_TIER1_TOPIC(" + topic + ")");
    }
    for (const std::string& topic : r.topics) {
        t.check(std::find(files.begin(), files.end(), topic) != files.end(), "SCFG_TIER1_TOPIC(" + topic + ") has no vector file");
    }
}
