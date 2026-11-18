// Copyright 2026 Repulse
// No Python twin: reruns a test body under the non-English C locales that change the core's text if it ever leans
// on the C library: de_DE's decimal comma (printf("%g"), strtod) and tr_TR's dotted and dotless i (toupper('i')).
// Does not depend on the test runner (ruling R6): rerun() reports, and the calling test asserts on the report.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace scfg_test::locales {

struct Options {
    // Run under each of these that setlocale accepts.
    std::vector<std::string> named = {"tr_TR.UTF-8", "de_DE.UTF-8"};
    // When none of `named` is installed: the first of these that setlocale accepts and whose decimal point is not ".".
    std::vector<std::string> fallbacks = {"bg_BG.UTF-8", "ru_RU.UTF-8", "fr_FR.UTF-8", "es_ES.UTF-8", "it_IT.UTF-8",
                                          "nl_NL.UTF-8", "pt_BR.UTF-8", "pl_PL.UTF-8", "cs_CZ.UTF-8", "da_DK.UTF-8"};
    // Every one of `named` must be installed (env SCFG_REQUIRE_LOCALES=1, for CI).
    bool require_named = false;
};

// The default Options, with require_named from the environment.
Options from_env();

struct Report {
    // The locales the body ran under, in order.
    std::vector<std::string> ran;
    // Non-empty when the calling test must fail: no usable locale (naming every one tried), or a required one is
    // missing. The body has then not run under the missing ones.
    std::string error;
};

// Runs body(name) once per locale chosen by `o`: setlocale(LC_ALL, name), and only after that succeeded (a name
// std::locale refuses would abort without exceptions) std::locale::global(std::locale(name)). Restores "C" (both)
// after each run.
Report rerun(const Options& o, const std::function<void(std::string_view name)>& body);
inline Report rerun(const std::function<void(std::string_view name)>& body) { return rerun(from_env(), body); }

// "tr_TR.UTF-8, bg_BG.UTF-8": for the test to print, so that a run under no locale is visible.
std::string describe(const Report& r);

}  // namespace scfg_test::locales
