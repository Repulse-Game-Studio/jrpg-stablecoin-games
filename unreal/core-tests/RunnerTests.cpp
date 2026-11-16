// No Python twin: tests of the test runner itself, and that ctest runs every group.
#include "support/Test.h"

#include "Scfg/Err.h"
#include "Scfg/Version.h"

#include <algorithm>
#include <cstdlib>
#include <set>

using scfg_test::Ctx;
using scfg_test::Registry;

static std::string run(Registry& r, std::vector<std::string> args, int& code) {
    std::string out;
    code = r.run(args, [&out](std::string_view s) { out += s; });
    return out;
}

static bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

static std::set<std::string> split(std::string_view csv) {
    std::set<std::string> out;
    while (!csv.empty()) {
        size_t comma = csv.find(',');
        out.insert(std::string(csv.substr(0, comma)));
        csv = comma == std::string_view::npos ? std::string_view() : csv.substr(comma + 1);
    }
    return out;
}

SCFG_TEST(RunnerTests, CoreLinks) { SCFG_CHECK_EQ(t, scfg::VERSION, "0.3.0"); }

SCFG_TEST(RunnerTests, RunsAndCounts) {
    Registry r;
    int ran = 0;
    r.add("G", "Pass", [&ran](Ctx&) { ++ran; });
    r.add("G", "Fail", [&ran](Ctx& c) {
        ++ran;
        c.fail("boom");
    });
    r.add("H", "Other", [&ran](Ctx&) { ++ran; });
    int code = 0;
    std::string out = run(r, {}, code);
    SCFG_CHECK_EQ(t, ran, 3);
    SCFG_CHECK(t, code != 0);
    SCFG_CHECK(t, contains(out, "2 passed, 1 failed"));
    SCFG_CHECK(t, contains(out, "FAIL G.Fail"));
    SCFG_CHECK(t, contains(out, "boom"));

    out = run(r, {"--group", "H"}, code);
    SCFG_CHECK_EQ(t, code, 0);
    SCFG_CHECK(t, contains(out, "1 passed, 0 failed"));

    out = run(r, {"--test", "G.Pass"}, code);
    SCFG_CHECK_EQ(t, code, 0);
    SCFG_CHECK(t, contains(out, "1 passed, 0 failed"));
    SCFG_CHECK_EQ(t, ran, 5);
}

SCFG_TEST(RunnerTests, AnEmptySelectionFails) {
    Registry r;
    r.add("G", "Pass", [](Ctx&) {});
    int code = 0;
    std::string out = run(r, {"--group", "NoSuchGroup"}, code);
    SCFG_CHECK(t, code != 0);
    SCFG_CHECK(t, contains(out, "0 passed, 0 failed"));
    run(r, {"--test", "G.NoSuchTest"}, code);
    SCFG_CHECK(t, code != 0);
    run(r, {"--test", "G"}, code);
    SCFG_CHECK(t, code != 0);
    Registry empty;
    run(empty, {}, code);
    SCFG_CHECK(t, code != 0);
}

SCFG_TEST(RunnerTests, AnEmptySelectionFailsTheProcess) {
    std::string cmd = "\"" + scfg_test::self_path() + "\" --group NoSuchGroup > /dev/null 2>&1";
    SCFG_CHECK(t, std::system(cmd.c_str()) != 0);
}

SCFG_TEST(RunnerTests, BadArgumentsFail) {
    Registry r;
    r.add("G", "Pass", [](Ctx&) {});
    int code = 0;
    for (std::vector<std::string> args : std::vector<std::vector<std::string>>{
             {"--nope"}, {"--group"}, {"--test"}, {"--group", "G", "--test", "G.Pass"}}) {
        std::string out = run(r, args, code);
        SCFG_CHECK(t, code != 0);
        SCFG_CHECK(t, contains(out, "usage"));
    }
}

SCFG_TEST(RunnerTests, ATestAddedAtRunTimeRuns) {
    Registry r;
    bool ran = false;
    r.on_start([&ran](Registry& reg) {
        for (const char* topic : {"amounts", "base58"}) {
            reg.add("Tier1", topic, [&ran](Ctx&) { ran = true; });
        }
    });
    int code = 0;
    std::string out = run(r, {"--test", "Tier1.base58"}, code);
    SCFG_CHECK_EQ(t, code, 0);
    SCFG_CHECK(t, ran);
    SCFG_CHECK(t, contains(out, "1 passed, 0 failed"));
    SCFG_CHECK(t, r.groups() == std::vector<std::string>{"Tier1"});
    out = run(r, {"--list"}, code);  // on_start ran once; the tests stay registered
    SCFG_CHECK_EQ(t, code, 0);
    SCFG_CHECK_EQ(t, out, "Tier1.amounts\nTier1.base58\n");
}

// The binary's own registry runs its sources before it selects, so --test can name a test a source added.
SCFG_TEST_SOURCE(RunnerSelfCheck) {
    scfg_test::add("RunnerTests", "AddedBySource", [](Ctx& c) { SCFG_CHECK_EQ(c, scfg_test::self_path().empty(), false); });
}
SCFG_TEST(RunnerTests, SourcesRunBeforeSelection) {
    std::string cmd = "\"" + scfg_test::self_path() + "\" --test RunnerTests.AddedBySource > /dev/null 2>&1";
    SCFG_CHECK_EQ(t, std::system(cmd.c_str()), 0);
}

SCFG_TEST(RunnerTests, DuplicateNamesFail) {
    Registry r;
    r.add("G", "Same", [](Ctx&) {});
    r.add("G", "Same", [](Ctx&) {});
    int code = 0;
    std::string out = run(r, {}, code);
    SCFG_CHECK(t, code != 0);
    SCFG_CHECK(t, contains(out, "G.Same"));
}

// Ruling R5: an unknown error kind is recorded, not asserted; the runner turns a leftover entry into a failure of the
// test that made it, and clears it so the next test starts clean.
SCFG_TEST(RunnerTests, AnUnknownErrorKindFailsTheTest) {
    Registry r;
    r.add("G", "Leaves", [](Ctx&) { (void)scfg::Err::make("ValueEror", "typo"); });
    r.add("G", "Asks", [](Ctx&) { (void)scfg::Err::value_error("v").is_a("RpcErorr"); });
    r.add("G", "Clears", [](Ctx&) {
        (void)scfg::Err::make("ValueEror", "typo");
        scfg::errors::clear_misuse();
    });
    r.add("G", "Next", [](Ctx&) {});
    int code = 0;
    std::string out = run(r, {}, code);
    SCFG_CHECK(t, code != 0);
    SCFG_CHECK(t, contains(out, "2 passed, 2 failed"));
    SCFG_CHECK(t, contains(out, "FAIL G.Leaves"));
    SCFG_CHECK(t, contains(out, "FAIL G.Asks"));
    SCFG_CHECK(t, contains(out, "ok   G.Clears"));
    SCFG_CHECK(t, contains(out, "ok   G.Next"));
    SCFG_CHECK(t, contains(out, "unknown kind ValueEror"));
    SCFG_CHECK(t, contains(out, "unknown kind RpcErorr"));
    SCFG_CHECK(t, scfg::errors::misuse().empty());
}

SCFG_TEST(RunnerTests, ChecksReportBothValues) {
    Ctx c;
    SCFG_CHECK(c, 1 + 1 == 2);
    SCFG_CHECK_EQ(c, std::string("abc"), "abc");
    SCFG_CHECK_EQ(c, size_t{3}, 3);
    SCFG_CHECK_EQ(c, int64_t{-1}, -1);
    SCFG_CHECK_EQ(c, true, true);
    SCFG_CHECK(t, !c.failed());

    SCFG_CHECK_EQ(c, std::string("a\"\n\x01"), "b");
    SCFG_CHECK_EQ(c, uint64_t{18446744073709551615u}, -1);
    SCFG_CHECK_EQ(c, true, false);
    SCFG_CHECK(c, 1 + 1 == 3);
    if (!SCFG_CHECK_EQ(t, c.messages().size(), 4)) return;
    SCFG_CHECK(t, contains(c.messages()[0], "\"a\\\"\\n\\x01\""));
    SCFG_CHECK(t, contains(c.messages()[0], "\"b\""));
    SCFG_CHECK(t, contains(c.messages()[1], "18446744073709551615"));
    SCFG_CHECK(t, contains(c.messages()[1], "-1"));
    SCFG_CHECK(t, contains(c.messages()[2], "true") && contains(c.messages()[2], "false"));
    SCFG_CHECK(t, contains(c.messages()[3], "RunnerTests.cpp:"));
    SCFG_CHECK(t, contains(c.messages()[3], "1 + 1 == 3"));
}

// Ruling R1: a group in the binary that ctest does not run must be listed as pending in CMakeLists.txt.
SCFG_TEST(RunnerTests, EveryGroupIsRegistered) {
    std::set<std::string> ctest = split(SCFG_CTEST_GROUPS), pending = split(SCFG_PENDING_GROUPS);
    std::vector<std::string> groups = scfg_test::registry().groups();
    for (const std::string& g : groups) {
        t.check(ctest.count(g) || pending.count(g),
                "group " + g + " is not run by ctest: add it to SCFG_TEST_GROUPS in unreal/CMakeLists.txt");
    }
    for (const std::string& g : ctest) {
        t.check(std::find(groups.begin(), groups.end(), g) != groups.end(),
                "ctest runs group " + g + " but the binary has no test in it");
        t.check(!pending.count(g), "group " + g + " is in both SCFG_TEST_GROUPS and SCFG_PENDING_GROUPS");
    }
}
