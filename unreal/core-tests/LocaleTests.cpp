// No Python twin: the core's text under non-English C locales (tr_TR.UTF-8 and de_DE.UTF-8, or another installed
// locale with a decimal comma), where printf("%g"), strtod and toupper('i') change.
#include "support/Locales.h"
#include "support/Test.h"
#include "support/Tier1.h"
#include "support/Tier2.h"
#include "support/Vectors.h"

#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/PyFloat.h"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <locale>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::Json;
namespace locales = scfg_test::locales;
namespace py = scfg::py;
namespace pyfloat = scfg::pyfloat;

static void print_ran(const char* test, const locales::Report& r) {
    std::printf("    %s ran under: %s\n", test, locales::describe(r).c_str());
}

SCFG_TEST(LocaleTests, FloatsUnderEveryLocale) {
    locales::Report r = locales::rerun([&](std::string_view name) {
        std::string at = " under " + std::string(name);
        // the locale really is in effect: the C library's decimal point is not "."
        t.check(std::strcmp(std::localeconv()->decimal_point, ".") != 0, "decimal point is \".\"" + at);
        t.check(std::locale().name() != "C", "std::locale::global did not change" + at);
        t.check(pyfloat::repr(1.5) == "1.5", "repr(1.5)" + at);
        t.check(pyfloat::repr(-1234.5e-10) == "-1.2345e-07", "repr(-1.2345e-07)" + at);
        t.check(pyfloat::repr(0.1 + 0.2) == "0.30000000000000004", "repr(0.1 + 0.2)" + at);
        t.check(pyfloat::repr(std::ldexp(1.0, -1017)) == "7.120236347223045e-307", "repr(2.0**-1017)" + at);
        t.check(pyfloat::parse("2.5") == 2.5, "parse(2.5)" + at);
        t.check(!pyfloat::parse("2,5").has_value(), "parse(2,5) accepted" + at);
        t.check(pyfloat::parse("-1e-05") == -1e-05, "parse(-1e-05)" + at);
        t.check(scfg::json::dumps(Json(0.25)) == "0.25", "dumps(0.25)" + at);
        scfg::Result<Json> v = scfg::json::loads("[1.5, -2, 12345678901234567890123, 1e-07, 2.5E3]");
        t.check(v.ok() && scfg::json::dumps(v.value()) == "[1.5, -2, 12345678901234567890123, 1e-07, 2500.0]",
                "loads/dumps of numbers" + at);
        t.check(py::lower_ascii("I") == "i", "lower_ascii(I)" + at);
        t.check(py::lower_for_ascii("LIMIT") == "limit", "lower_for_ascii(LIMIT)" + at);
        t.check(py::strip(" x ") == "x", "strip" + at);
        scfg::Result<scfg::BigInt> n = py::py_int(" -42 ");
        t.check(n.ok() && n.value() == -42, "py_int( -42 )" + at);
        t.check(py::repr(Json::list({1.5, "i"})) == "[1.5, 'i']", "repr" + at);
    });
    print_ran("LocaleTests.FloatsUnderEveryLocale", r);
    t.check(r.error.empty(), r.error);
    t.check(!r.ran.empty(), "ran under no locale");
    SCFG_CHECK_EQ(t, std::string(std::setlocale(LC_ALL, nullptr)), "C");
    SCFG_CHECK_EQ(t, std::locale().name(), "C");
}

// Every Tier 1 topic again under each locale.
SCFG_TEST(LocaleTests, Tier1UnderEveryLocale) {
    std::vector<std::string> topics = scfg_test::vectors::tier1_topics();
    t.check(!topics.empty(), "no Tier 1 topic to rerun");
    locales::Report r = locales::rerun([&](std::string_view name) {
        t.check(std::strcmp(std::localeconv()->decimal_point, ".") != 0, "decimal point is \".\" under " + std::string(name));
        for (const std::string& topic : topics) {
            std::vector<std::string> failures = scfg_test::tier1::run_topic(topic);
            t.check(failures.empty(), scfg_test::tier1::report("tier1/" + topic + " under " + std::string(name), failures));
        }
    });
    print_ran("LocaleTests.Tier1UnderEveryLocale", r);
    t.check(r.error.empty(), r.error);
    t.check(!r.ran.empty(), "ran under no locale");
    SCFG_CHECK_EQ(t, std::string(std::setlocale(LC_ALL, nullptr)), "C");
}

// Every Tier 2 vector again under each locale, immediate mode.
SCFG_TEST(LocaleTests, Tier2UnderEveryLocale) {
    std::vector<std::pair<std::string, std::string>> texts;
    for (const std::string& id : scfg_test::vectors::tier2_ids()) {
        std::optional<std::string> text = scfg_test::vectors::read_text("tier2/" + id + ".json");
        if (t.check(text.has_value(), "tier2/" + id + " cannot be read")) texts.emplace_back(id, std::move(*text));
    }
    t.check(!texts.empty(), "no Tier 2 vector to rerun");
    locales::Report r = locales::rerun([&](std::string_view name) {
        t.check(std::strcmp(std::localeconv()->decimal_point, ".") != 0, "decimal point is \".\" under " + std::string(name));
        for (const auto& [id, text] : texts) {
            scfg::Result<Json> v = scfg_test::tier2::parse(text);
            if (!t.check(v.ok(), "tier2/" + id + " cannot be parsed under " + std::string(name))) continue;
            std::optional<std::string> diff = scfg_test::tier2::replay(v.value());
            t.check(!diff.has_value(), "tier2/" + id + " under " + std::string(name) + ": " + diff.value_or(""));
        }
    });
    print_ran("LocaleTests.Tier2UnderEveryLocale", r);
    t.check(r.error.empty(), r.error);
    t.check(!r.ran.empty(), "ran under no locale");
    SCFG_CHECK_EQ(t, std::string(std::setlocale(LC_ALL, nullptr)), "C");
}

SCFG_TEST(LocaleTests, RerunReportsWhatItCouldNotRun) {
    int calls = 0;
    auto body = [&](std::string_view) { ++calls; };
    locales::Options none;
    none.named = {"xx_XX.UTF-8"};
    none.fallbacks = {"yy_YY.UTF-8", "C"};  // C exists but its decimal point is "."
    locales::Report r = locales::rerun(none, body);
    SCFG_CHECK_EQ(t, calls, 0);
    SCFG_CHECK(t, r.ran.empty());
    SCFG_CHECK(t, r.error.find("xx_XX.UTF-8") != std::string::npos && r.error.find("yy_YY.UTF-8") != std::string::npos &&
                      r.error.find("C") != std::string::npos);
    SCFG_CHECK_EQ(t, locales::describe(r), "(none)");

    // a required locale that is missing is an error even when another one runs
    locales::Options required = locales::Options();
    required.named = {"xx_XX.UTF-8"};
    required.require_named = true;
    locales::Report q = locales::rerun(required, body);
    SCFG_CHECK(t, q.error.find("SCFG_REQUIRE_LOCALES=1") != std::string::npos && q.error.find("xx_XX.UTF-8") != std::string::npos);
    // ...and the fallback still ran (when this machine has one)
    SCFG_CHECK_EQ(t, static_cast<int64_t>(q.ran.size()), calls);
    SCFG_CHECK_EQ(t, std::string(std::setlocale(LC_ALL, nullptr)), "C");
}
