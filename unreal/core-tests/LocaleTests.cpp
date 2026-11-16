// No Python twin: the core's text under non-English C locales (tr_TR.UTF-8 and de_DE.UTF-8, or another installed
// locale with a decimal comma), where printf("%g"), strtod and toupper('i') change.
#include "support/Locales.h"
#include "support/Test.h"

#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/PyFloat.h"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <locale>
#include <string>

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
