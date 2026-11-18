// Copyright 2026 Repulse
// No Python twin: tests of the test runner itself, and that ctest runs every group.
#include "support/Lint.h"
#include "support/Test.h"
#include "support/Vectors.h"

#include "Scfg/Err.h"
#include "Scfg/Version.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <vector>

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

// The core links, and its exported scfg::VERSION is the plugin's version: the VersionName of Scfg.uplugin, which
// tools/package_unreal.py ties to scfg_core's __version__ (and to this constant), so no test pins the literal.
SCFG_TEST(RunnerTests, CoreLinks) {
    std::ifstream in(std::string(SCFG_REPO_ROOT) + "/unreal/Plugins/Scfg/Scfg.uplugin", std::ios::binary);
    if (!SCFG_CHECK(t, in.good())) return;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::smatch m;
    if (!SCFG_CHECK(t, std::regex_search(text, m, std::regex(R"re("VersionName"\s*:\s*"([^"]+)")re")))) return;
    SCFG_CHECK_EQ(t, scfg::VERSION, m[1].str());
}

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

// An unknown error kind is recorded, not asserted; the runner turns a leftover entry into a failure of the
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

// The binary's groups are exactly the ones ctest runs (SCFG_TEST_GROUPS in unreal/CMakeLists.txt).
SCFG_TEST(RunnerTests, EveryGroupIsRegistered) {
    std::set<std::string> ctest = split(SCFG_CTEST_GROUPS);
    std::vector<std::string> groups = scfg_test::registry().groups();
    for (const std::string& g : groups) {
        t.check(ctest.count(g) != 0, "group " + g + " is not run by ctest: add it to SCFG_TEST_GROUPS in unreal/CMakeLists.txt");
    }
    for (const std::string& g : ctest) {
        t.check(std::find(groups.begin(), groups.end(), g) != groups.end(),
                "ctest runs group " + g + " but the binary has no test in it");
    }
}

// The registered names of group, compared with the names it must have (one test per vector file, no more, no less).
static void expect_names(Ctx& t, std::string_view group, std::vector<std::string> want) {
    std::vector<std::string> got = scfg_test::registry().names(group);
    t.check(got.size() == want.size(), std::string(group) + " has " + std::to_string(got.size()) + " tests for " +
                                           std::to_string(want.size()) + " vector files");
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    for (const std::string& w : want) {
        t.check(std::binary_search(got.begin(), got.end(), w), std::string(group) + "." + w + " is not registered");
    }
    for (const std::string& g : got) {
        t.check(std::binary_search(want.begin(), want.end(), g), std::string(group) + "." + g + " is no vector file");
    }
}

// No vector is left out: Tier1 has one test per topic file, Tier2 and Tier2Deferred one per vector file, all three
// are ctest groups, and neither the runner nor any test source has a way to skip, ignore or park one.
SCFG_TEST(RunnerTests, NothingLeftOut) {
    std::vector<std::string> topics = scfg_test::vectors::tier1_topics(), ids = scfg_test::vectors::tier2_ids();
    t.check(!topics.empty() && !ids.empty(), "no vector files under " + scfg_test::vectors::root());
    expect_names(t, "Tier1", topics);
    expect_names(t, "Tier2", ids);
    expect_names(t, "Tier2Deferred", ids);
    std::set<std::string> ctest = split(SCFG_CTEST_GROUPS);
    for (const char* g : {"Tier1", "Tier2", "Tier2Deferred", "LocaleTests"}) {
        t.check(ctest.count(g) != 0, std::string(g) + " is not run by ctest");
    }

    // No skip facility in the code of any test source (comments and string literals do not count): no list of ported
    // topics or prefixes, no pending ids, no skip, ignore, xfail or disabled marker.
    const std::regex banned(R"(\b(ported|ported_topics|ported_prefixes|ported_ids|pending_ids|PendingId|skip|skips|)"
                            R"(skipped|Skip|SKIP|SKIPPED|ignore|ignored|Ignore|IGNORE|xfail|XFAIL|DISABLED_\w*)\b)");
    std::string error;
    std::vector<scfg_test::lint::File> files = scfg_test::lint::test_files(&error);
    t.check(error.empty() && !files.empty(), "cannot read the test sources: " + error);
    for (const scfg_test::lint::File& f : files) {
        const scfg_test::lint::Views v = scfg_test::lint::views(f.text);
        for (size_t i = 0; i < v.code.size(); ++i) {
            std::smatch m;
            if (std::regex_search(v.code[i], m, banned)) {
                t.fail(f.rel + ":" + std::to_string(i + 1) + ": '" + m.str() + "' looks like a way to leave a test out");
            }
        }
    }
    // ...nor in ctest's own test properties
    std::ifstream in(std::string(SCFG_REPO_ROOT) + "/unreal/CMakeLists.txt", std::ios::binary);
    std::string cmake((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    t.check(!cmake.empty(), "cannot read unreal/CMakeLists.txt");
    for (const char* p : {"DISABLED", "WILL_FAIL", "SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION", "set_tests_properties"}) {
        t.check(cmake.find(p) == std::string::npos, std::string("unreal/CMakeLists.txt uses ") + p);
    }
    // ...nor in a test preset's filter (ctest --preset runs every group)
    std::ifstream presets_in(std::string(SCFG_REPO_ROOT) + "/unreal/CMakePresets.json", std::ios::binary);
    std::string presets((std::istreambuf_iterator<char>(presets_in)), std::istreambuf_iterator<char>());
    t.check(!presets.empty(), "cannot read unreal/CMakePresets.json");
    for (const char* p : {"\"filter\"", "\"exclude\"", "\"include\"", "\"execution\""}) {
        t.check(presets.find(p) == std::string::npos, std::string("unreal/CMakePresets.json's presets use ") + p);
    }
    // ...and the runner reports passed and failed only
    Registry r;
    r.add("G", "Pass", [](Ctx&) {});
    int code = 0;
    std::string out = run(r, {}, code);
    SCFG_CHECK_EQ(t, out, "ok   G.Pass\n1 passed, 0 failed\n");
}
