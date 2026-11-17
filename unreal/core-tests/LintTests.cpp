// No Python twin: the lints that every file under Source/ScfgCore must pass.
//
// THE LINE RULES LIVE HERE: (label, regex, why, prefilter), matched against each line with comments removed and
// the text of string and char literals blanked (so "try again" in a message is fine). The file-level checks
// (#pragma once, Co<> parameters, includes, the twin comment, basenames) are in support/Lint.cpp. A failure prints
// "Private/Foo.cpp:12: rule (why)". A file is exempt from a line rule only through kWaivers (ruling R3).
// RulesBite and FileChecksBite feed every rule an offending and a clean input.
#include "support/Lint.h"
#include "support/Test.h"
#include "support/Vectors.h"

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>

namespace lint = scfg_test::lint;
using lint::File;
using lint::Rule;

// any: literal substrings, one of which every match contains (a cheap prefilter; std::regex is slow under ASan).
static Rule R(std::string label, std::string pattern, std::string why, std::vector<std::string> any) {
    Rule r;
    r.label = std::move(label);
    r.any = std::move(any);
    r.re = std::regex(pattern);
    r.why = std::move(why);
    return r;
}
// `#include <a>` or `#include <b>`, labelled "<a>, <b>".
static Rule header(std::initializer_list<std::string_view> names, std::string why) {
    std::string label, alt;
    for (std::string_view n : names) {
        label += (label.empty() ? "<" : ", <") + std::string(n) + ">";
        std::string esc;
        for (char c : n) esc += (c == '.' ? std::string("\\.") : std::string(1, c));
        alt += (alt.empty() ? "" : "|") + esc;
    }
    return R(label, "^\\s*#\\s*include\\s*<(?:" + alt + ")>", std::move(why), {"include"});
}

// Before an unqualified C name: not a member (.x, ->x) and not another namespace's x (py::x), but ::x and
// std::x (spaces around :: allowed) are the C library's.
#define CALLER "(?:^|[^\\w.>:]|(?:^|\\W)::)(?:std\\s*::\\s*)?"

static const std::string kNoExc = "Unreal builds the core without exceptions; errors are scfg::Err values";
static const std::string kNoRtti = "Unreal builds the core without RTTI; use std::variant or a tag";

static const std::vector<Rule>& exception_rules() {
    static const std::vector<Rule> rules = {
        R("throw", "\\bthrow\\b", kNoExc, {"throw"}),
        R("try", "\\btry\\b", kNoExc, {"try"}),
        R("catch", "\\bcatch\\b", kNoExc, {"catch"}),
        R("std::exception", "\\bstd\\s*::\\s*exception\\b", kNoExc, {"exception"}),
        R("dynamic_cast", "\\bdynamic_cast\\b", kNoRtti, {"dynamic_cast"}),
        R("typeid", "\\btypeid\\b", kNoRtti, {"typeid"}),
    };
    return rules;
}

static const std::string kNoThreads = "the core is single-threaded; the host drives it from one thread";
static const std::string kNoClock = "the clock is injected, so tests and replays are deterministic";

static const std::vector<Rule>& thread_rules() {
    static const std::vector<Rule> rules = {
        header({"thread"}, kNoThreads),
        header({"mutex"}, kNoThreads),
        header({"shared_mutex"}, kNoThreads),
        header({"future"}, kNoThreads),
        header({"condition_variable"}, kNoThreads),
        header({"semaphore"}, kNoThreads),
        header({"latch"}, kNoThreads),
        header({"barrier"}, kNoThreads),
        header({"stop_token"}, kNoThreads),
        R("std::thread", "\\bstd\\s*::\\s*j?thread\\b", kNoThreads, {"thread"}),
        R("std::async", "\\bstd\\s*::\\s*async\\b", kNoThreads, {"async"}),
        R("sleep", "\\b(?:sleep|usleep|nanosleep|sleep_for|sleep_until|Sleep)\\s*\\(", "the core never blocks", {"sleep", "Sleep"}),
        header({"chrono"}, kNoClock + " (Rpc.cpp's monotonic default excepted)"),
        header({"ctime", "time.h"}, kNoClock),
        R("steady_clock", "\\bsteady_clock\\b", kNoClock + " (only rpc's default monotonic clock may use it)", {"steady_clock"}),
        R("system_clock", "\\b(?:system_clock|high_resolution_clock|utc_clock|file_clock)\\b", kNoClock, {"_clock"}),
        R("time()", CALLER "time\\s*\\(", kNoClock, {"time"}),
        R("rand()", CALLER "s?rand\\s*\\(", "entropy is injected, so tests are deterministic", {"rand"}),
        header({"random"}, "entropy is injected (Entropy.cpp's default source excepted)"),
        R("random_device", "\\brandom_device\\b", "entropy is injected (Entropy.cpp's default source excepted)", {"random_device"}),
    };
    return rules;
}

static const std::string kLocale = "locale-dependent; Python's rules come from scfg::py";

static const std::vector<Rule>& locale_rules() {
    static const std::vector<Rule> rules = {
        header({"locale", "clocale", "locale.h"}, kLocale),
        header({"iostream", "istream", "ostream", "iomanip"}, kLocale),
        header({"sstream", "fstream"}, kLocale),
        header({"regex"}, kLocale + " (and std::regex classes are locale-aware)"),
        header({"cctype", "ctype.h", "cwctype", "wctype.h"}, kLocale),
        header({"cstdio", "stdio.h"}, "the core does not print or parse with stdio (PyFloat.cpp and Err.cpp excepted)"),
        R("to_string", "\\bto_w?string\\b", "locale-dependent for floats; use scfg::py", {"to_"}),
        R("sto*", "\\bsto(?:i|l|ll|ul|ull|f|d|ld)\\b", kLocale, {"sto"}),
        R("strto*", "\\b(?:strto|wcsto)\\w*", kLocale, {"strto", "wcsto"}),
        R("ato*", "\\bato(?:i|l|ll|f)\\b", kLocale, {"ato"}),
        R("printf", "\\b\\w*printf\\b", kLocale, {"printf"}),
        R("scanf", "\\b\\w*scanf\\b", kLocale, {"scanf"}),
        R("puts", "\\bf?puts\\b", "the core does not print (Err.cpp's print_error excepted)", {"puts"}),
        R("localeconv", "\\b(?:localeconv|setlocale|nl_langinfo)\\b", kLocale, {"localeconv", "setlocale", "nl_langinfo"}),
        // Called or passed by name (std::transform(b, e, b, ::tolower)). Unlike CALLER, a name right after another
        // identifier is a declaration (scfg::py's own `bool isdigit(std::string_view s)`), not a use.
        R("ctype",
          "(?:^|[^\\w\\s.>:]|\\b(?:return|co_return|co_await|case)|(?:^|\\W)::)\\s*(?:std\\s*::\\s*)?"
          "(?:isw?(?:alnum|alpha|blank|cntrl|digit|graph|lower|print|punct|space|upper|xdigit)|tow?(?:lower|upper))"
          "\\s*[(,);]",
          kLocale, {"is", "to"}),
    };
    return rules;
}

static const std::vector<Rule>& portable_rules() {
    static const std::vector<Rule> rules = {
        R("long", "\\blong\\b", "long is 32 bits on Windows; use int64_t, uint64_t or size_t", {"long"}),
        R("__int128", "__u?int128", "not on MSVC; use BigInt", {"int128"}),
        R("wchar_t", "\\bwchar_t\\b|\\bw(?:string|string_view)\\b", "text is UTF-8 in std::string", {"wchar_t", "wstring"}),
    };
    return rules;
}

static const std::string kUnity = "Unreal's unity build puts every .cpp in one TU; use a named detail namespace";

static const std::vector<Rule>& anonymous_rules() {
    static const std::vector<Rule> rules = {
        R("namespace {", "\\bnamespace\\s*\\{", kUnity, {"namespace"}),
        [] {
            Rule r = R("static at namespace scope", "^static\\b", kUnity, {"static"});
            r.ext = ".cpp";
            return r;
        }(),
    };
    return rules;
}

// Ruling R3: the only files exempt from a rule, and the one thing each needs.
static const std::vector<lint::Waiver> kWaivers = {
    {"PyFloat.cpp", "printf"},      {"PyFloat.cpp", "strto*"},      {"PyFloat.cpp", "<cstdio>, <stdio.h>"},
    {"Err.cpp", "puts"},            {"Err.cpp", "<cstdio>, <stdio.h>"},
    {"Entropy.cpp", "<random>"},    {"Entropy.cpp", "random_device"},
    {"Rpc.cpp", "steady_clock"},    {"Rpc.cpp", "<chrono>"},
};

static std::string g_files_error;
static const std::vector<File>& files() {
    static const std::vector<File> all = lint::core_files(&g_files_error);
    return all;
}

// Runs check over every core file and fails with what it reports.
template <class F>
static void run_check(scfg_test::Ctx& t, F check) {
    std::vector<std::string> hits;
    for (const File& f : files()) {
        for (std::string& h : check(f)) hits.push_back(std::move(h));
    }
    SCFG_CHECK(t, !files().empty());
    t.check(g_files_error.empty(), "listing the core failed: " + g_files_error);
    t.check(hits.empty(), "lint violations under " + lint::core_dir() + ":\n" + lint::join(hits));
}

static void run_rules(scfg_test::Ctx& t, const std::vector<Rule>& rules) {
    run_check(t, [&rules](const File& f) { return lint::scan(f, rules, kWaivers); });
}

// ---- the lints ----

SCFG_TEST(LintTests, NoExceptionsOrRtti) { run_rules(t, exception_rules()); }
SCFG_TEST(LintTests, NoThreadsOrClocks) { run_rules(t, thread_rules()); }
SCFG_TEST(LintTests, LocaleFree) { run_rules(t, locale_rules()); }
SCFG_TEST(LintTests, PortableTypes) {
    run_rules(t, portable_rules());
    run_check(t, lint::pragma_once);
}
SCFG_TEST(LintTests, NoAnonymousHelpers) { run_rules(t, anonymous_rules()); }
SCFG_TEST(LintTests, CoroutineParamsByValue) { run_check(t, lint::coroutine_params); }
// Ruling R8, over the tests too: they run on the Clang CI leg as well.
SCFG_TEST(LintTests, NoCoroutineLambdas) {
    run_check(t, lint::coroutine_lambdas);
    std::string error;
    std::vector<File> tests = lint::test_files(&error);
    t.check(error.empty(), "listing the tests failed: " + error);
    SCFG_CHECK(t, tests.size() > 20);
    std::vector<std::string> hits;
    for (const File& f : tests) {
        for (std::string& h : lint::coroutine_lambdas(f)) hits.push_back(std::move(h));
    }
    t.check(hits.empty(), "lint violations under " + lint::tests_dir() + ":\n" + lint::join(hits));
}
// The one file of the module that is Unreal code: the module's IMPLEMENT_MODULE, which needs Unreal's
// Modules/ModuleManager.h. The CMake build leaves it out; it holds exactly these two lines, so nothing else creeps in.
static const std::string kModuleFile = "Private/ScfgCoreModule.cpp";
static const std::vector<std::string> kModuleFileCode = {
    "#include \"Modules/ModuleManager.h\"",
    "IMPLEMENT_MODULE(FDefaultModuleImpl, ScfgCore)",
};

SCFG_TEST(LintTests, OnlyStdIncludes) {
    run_check(t, [](const File& f) {
        return f.rel == kModuleFile ? std::vector<std::string>() : lint::only_std_includes(f);
    });
}
SCFG_TEST(LintTests, ModuleFileIsMinimal) {
    const File* module = nullptr;
    for (const File& f : files()) {
        if (f.rel == kModuleFile) module = &f;
    }
    if (!t.check(module != nullptr, kModuleFile + " is missing")) return;
    std::vector<std::string> code;
    for (const std::string& line : lint::views(module->text).kept) {
        size_t b = line.find_first_not_of(" \t");
        if (b != std::string::npos) code.push_back(line.substr(b, line.find_last_not_of(" \t") - b + 1));
    }
    t.check(code == kModuleFileCode,
            kModuleFile + " holds more than its include and IMPLEMENT_MODULE:\n" + lint::join(code));
}
SCFG_TEST(LintTests, ExportsMarked) { run_check(t, lint::exports); }
SCFG_TEST(LintTests, EveryFileNamesItsTwin) { run_check(t, lint::names_its_twin); }
SCFG_TEST(LintTests, UniqueBasenames) {
    SCFG_CHECK(t, !files().empty());
    t.check(g_files_error.empty(), "listing the core failed: " + g_files_error);
    std::vector<std::string> hits = lint::duplicate_basenames(files());
    t.check(hits.empty(), "lint violations:\n" + lint::join(hits));
}

SCFG_TEST(LintTests, VectorsFound) {
    std::string error;
    SCFG_CHECK_EQ(t, scfg_test::vectors::tier1_topics(&error).size(), 18);
    t.check(error.empty(), "listing tier1 failed: " + error);
    std::vector<std::string> ids = scfg_test::vectors::tier2_ids(&error);
    t.check(error.empty(), "listing tier2 failed: " + error);
    t.check(ids.size() > 100, "tier2_ids() found " + std::to_string(ids.size()) + " under " + scfg_test::vectors::root());
}

// ---- the lints bite ----

struct Bite {
    std::string label;  // the rule that must fire on bad and not on good
    std::string bad, good;
};

static const std::vector<Bite>& line_bites() {
    static const std::vector<Bite> b = {
        {"throw", "    throw Err();", "    return Err::value_error(\"throw it\");  // throw"},
        {"try", "try {", "SCFG_TRY(x, parse(s));"},
        {"catch", "} catch (...) {", "auto catches = 1;"},
        {"std::exception", "struct E : std :: exception {};", "struct E : std::exceptional {};"},
        {"dynamic_cast", "auto* p = dynamic_cast<B*>(a);", "auto* p = static_cast<B*>(a);"},
        {"typeid", "auto& i = typeid(x);", "auto typeidx = 1;"},
        {"<thread>", "#include <thread>", "#include <threads_are_not_here> // <thread>"},
        {"<mutex>", "#include <mutex>", "#include <string>"},
        {"<shared_mutex>", "#include <shared_mutex>", "#include <string>"},
        {"<future>", "#include <future>", "#include <string>"},
        {"<condition_variable>", "# include <condition_variable>", "#include <string>"},
        {"<semaphore>", "#include <semaphore>", "#include <string>"},
        {"<latch>", "#include <latch>", "#include <string>"},
        {"<barrier>", "#include <barrier>", "#include <string>"},
        {"<stop_token>", "#include <stop_token>", "#include <string>"},
        {"std::thread", "std::jthread w(f);", "auto thread = 1;"},
        {"std::thread", "std :: thread w(f);", "my::thread w(f);"},
        {"std::async", "auto f = std::async(g);", "Co<Json> async_call(Json x);"},
        {"sleep", "std::this_thread::sleep_for(d);", "int sleepy = 0;"},
        {"<chrono>", "#include <chrono>", "#include <string>"},
        {"<ctime>, <time.h>", "#include <ctime>", "#include <string>"},
        {"steady_clock", "auto n = std::chrono::steady_clock::now();", "auto n = clock_();"},
        {"system_clock", "auto n = std::chrono::system_clock::now();", "auto n = clock_();"},
        {"time()", "auto n = time(nullptr);", "auto n = self.time(); auto m = Clock::time();"},
        {"time()", "auto n = ::time(nullptr);", "auto n = clock->time();"},
        {"rand()", "int r = std::rand();", "int r = entropy.randbelow(6);"},
        {"rand()", "int r = ::rand();", "int r = py::rand();"},
        {"rand()", "int r = std :: rand();", "int r = a.rand();"},
        {"<random>", "#include <random>", "#include <string>"},
        {"random_device", "std::random_device rd;", "Entropy rd;"},
        {"<locale>, <clocale>, <locale.h>", "#include <clocale>", "#include <string>"},
        {"<iostream>, <istream>, <ostream>, <iomanip>", "#include <iostream>", "#include <iosfwd>"},
        {"<sstream>, <fstream>", "#include <sstream>", "#include <string>"},
        {"<regex>", "#include <regex>", "#include <string>"},
        {"<cctype>, <ctype.h>, <cwctype>, <wctype.h>", "#include <cctype>", "#include <cstring>"},
        {"<cstdio>, <stdio.h>", "#include <stdio.h>", "#include <cstdint>"},
        {"to_string", "auto s = std::to_string(1.5);", "auto s = py::str(1.5);"},
        {"sto*", "int n = std::stoi(s);", "int store = 1;"},
        {"strto*", "double d = strtod(p, nullptr);", "auto str_to = 1;  // 'strtod'"},
        {"ato*", "int n = atoi(p);", "int atom = 1;"},
        {"printf", "std::snprintf(buf, 8, \"%g\", d);", "auto s = py::format(\"printf\");"},
        {"scanf", "sscanf(p, \"%d\", &n);", "auto scanfree = 1;"},
        {"puts", "std::fputs(\"x\", stderr);", "auto input = 1;"},
        {"localeconv", "auto* lc = localeconv();", "auto lc = 1;"},
        {"ctype", "if (isdigit(c)) {", "if (py::isdigit(c)) {"},
        {"ctype", "if (::isdigit(c)) {", "if (s.isdigit()) {"},
        {"ctype", "std::transform(b, e, b, ::tolower);", "std::transform(b, e, b, py::tolower);"},
        {"ctype", "auto f = std::toupper;", "auto f = py::toupper;"},
        {"ctype", "apply(std::tolower, s);", "apply(py::tolower, s);"},
        {"ctype", "    return isdigit(c);", "bool isdigit(std::string_view s);"},
        {"ctype", "ok = ok && std :: isspace(c);", "[[nodiscard]] Result<bool> isspace(std::string_view s) {"},
        {"long", "unsigned long n = 0;", "uint64_t n = 0; auto s = \"long\";"},
        {"__int128", "unsigned __int128 n = 0;", "BigInt n;"},
        {"__int128", "__uint128_t n = 0;", "uint64_t n = 0;  // __uint128_t"},
        {"wchar_t", "std::wstring w;", "std::string w;  // wchar_t"},
        {"namespace {", "namespace {", "namespace scfg::config::detail {"},
        {"static at namespace scope", "static int helper(int x) {", "    static int counter = 0;"},
    };
    return b;
}

SCFG_TEST(LintTests, RulesBite) {
    std::vector<const std::vector<Rule>*> tables = {&exception_rules(), &thread_rules(), &locale_rules(),
                                                    &portable_rules(), &anonymous_rules()};
    std::set<std::string> bitten;
    for (const Bite& b : line_bites()) {
        const Rule* rule = nullptr;
        for (const auto* table : tables) {
            for (const Rule& r : *table) {
                if (r.label == b.label) rule = &r;
            }
        }
        if (!t.check(rule != nullptr, "no rule labelled " + b.label)) continue;
        bitten.insert(b.label);
        File bad{"Private/Bite.cpp", b.bad + "\n"}, good{"Private/Bite.cpp", b.good + "\n"};
        t.check(lint::scan(bad, {*rule}, {}).size() == 1, b.label + " does not fire on: " + b.bad);
        t.check(lint::scan(good, {*rule}, {}).empty(), b.label + " fires on: " + b.good);
    }
    std::set<std::string> labels;
    for (const auto* table : tables) {
        for (const Rule& r : *table) {
            t.check(bitten.count(r.label) == 1, "no bite case for " + r.label);
            t.check(labels.insert(r.label).second, "two rules are labelled " + r.label);
        }
    }
    for (const lint::Waiver& w : kWaivers) t.check(labels.count(w.label) == 1, "waiver names no rule: " + w.label);
    // A waiver exempts its file from its rule only.
    File rpc{"Private/Rpc.cpp", "auto n = std::chrono::steady_clock::now();\n"};
    File other{"Private/Engine.cpp", rpc.text};
    SCFG_CHECK(t, lint::scan(rpc, thread_rules(), kWaivers).empty());
    SCFG_CHECK_EQ(t, lint::scan(other, thread_rules(), kWaivers).size(), 1);
    // The message names the file, the line, the rule and why.
    File two{"Private/Two.cpp", "// x\nthrow 1;\n"};
    std::vector<std::string> msg = lint::scan(two, exception_rules(), {});
    if (SCFG_CHECK_EQ(t, msg.size(), 1)) SCFG_CHECK_EQ(t, msg[0], "Private/Two.cpp:2: throw (" + kNoExc + ")");
    // The static rule is for .cpp files; a header's column-0 static is a class member's problem.
    SCFG_CHECK(t, lint::scan({"Public/Scfg/A.h", "static int f();\n"}, anonymous_rules(), {}).empty());
}

SCFG_TEST(LintTests, BlankingKeepsLinesAndDropsLiterals) {
    lint::Views v = lint::views("a /* throw\n try */ b // catch\nR\"x(throw\n)\" )x\" c 'x' 1'000 u8\"y\" L'z'\n");
    if (!SCFG_CHECK_EQ(t, v.code.size(), 5)) return;
    SCFG_CHECK_EQ(t, v.code[0], "a ");
    SCFG_CHECK_EQ(t, v.code[1], " b ");
    SCFG_CHECK_EQ(t, v.code[2], "R\"");
    SCFG_CHECK_EQ(t, v.code[3], "\" c '' 1'000 u8\"\" L''");
    SCFG_CHECK_EQ(t, v.code[4], "");
    SCFG_CHECK_EQ(t, v.kept[2], "R\"x(throw");
    SCFG_CHECK_EQ(t, v.kept[3], ")\" )x\" c 'x' 1'000 u8\"y\" L'z'");
    SCFG_CHECK_EQ(t, v.raw[0], "a /* throw");
    lint::Views esc = lint::views("s = \"a\\\"throw\"; t = '\\''; // x\n");
    SCFG_CHECK_EQ(t, esc.code[0], "s = \"\"; t = ''; ");
}

SCFG_TEST(LintTests, FileChecksBite) {
    SCFG_CHECK_EQ(t, lint::pragma_once({"Public/Scfg/A.h", "// No Python twin: a\nint f();\n"}).size(), 1);
    SCFG_CHECK(t, lint::pragma_once({"Public/Scfg/A.h", "// No Python twin: a\n#pragma once\n"}).empty());
    SCFG_CHECK(t, lint::pragma_once({"Private/A.cpp", "int f();\n"}).empty());
    SCFG_CHECK_EQ(t, lint::pragma_once({"Public/Scfg/A.h", "// #pragma once\nint f();\n"}).size(), 1);

    SCFG_CHECK_EQ(t, lint::coroutine_params({"Private/A.cpp", "Co<Json> get(const Json& x);\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::coroutine_params({"Private/A.cpp", "scfg::Co<Result<Json>> Rpc::call(\n  std::string_view m);\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::coroutine_params({"Private/A.cpp", "Co<Json> f(Transport* t);\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::coroutine_params({"Private/A.cpp", "auto f(Json& x) const -> Co<Json>;\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::coroutine_params({"Private/A.cpp", "auto g = [](const Json& x) -> Co<Json> {};\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::coroutine_params({"Private/A.cpp", "auto f(int* p) -> scfg::Co<Json>;\n"}).size(), 1);
    SCFG_CHECK(t, lint::coroutine_params({"Private/A.cpp", "Co<Json> get(Json x, std::string m, std::function<void(int)> f);\n"}).empty());
    SCFG_CHECK(t, lint::coroutine_params({"Private/A.cpp", "Co<Json> c = get(x); return Co<int>(&p); MyCo<int> f(int& x);\n"}).empty());
    SCFG_CHECK(t, lint::coroutine_params({"Private/A.cpp", "auto g = [](Json x) -> Co<Json> {}; std::vector<Co<int>> v;\n"}).empty());
    SCFG_CHECK_EQ(t, lint::coroutine_params({"Private/A.cpp", "Co<Json> f(std::span<const uint8_t> b);\n"}).size(), 1);
    SCFG_CHECK(t, lint::coroutine_params({"Private/A.cpp", "Co<Json> f(std::vector<uint8_t> b, Json timespan);\n"}).empty());

    // Ruling R8: a lambda whose body holds co_await, co_return or co_yield.
    for (const char* bad : {
             "auto f = [](Json x) -> Co<Json> { co_return x; };\n",
             "run([&] { co_await step(); });\n",
             "auto g = [x]<class T>(T y) -> Co<std::pair<T, int>> {\n  co_yield y;\n};\n",
             "return [this](int n) mutable noexcept -> Co<int> { SCFG_CO_TRY(int v, co_await f(n)); co_return v; };\n",
             "auto h = [] { return 1; }; auto k = [] { co_return 2; };\n",
             "Co<int> f() { auto g = [](int a) -> Co<int> { co_return a; }; co_return 1; }\n",
         }) {
        t.check(lint::coroutine_lambdas({"Private/A.cpp", bad}).size() == 1, std::string("coroutine lambda missed: ") + bad);
    }
    for (const char* good : {
             "Co<Json> f(Json x) { auto g = [](int a) { return a; }; co_return co_await h(x[0]); }\n",
             "Co<int> f() { a[i] = co_await g(); auto& [k, v] = co_await h(); co_return k; }\n",
             "Co<int> f(Json d) { for (auto& [k, v] : d) { co_await g(k); } co_return 0; }\n",
             "[[nodiscard]] Co<int> f() { co_return 1; }\n",
             "auto s = \"[] { co_return 1; }\"; // [] { co_await x; }\n",
             "Co<int> f() { int arr[3] = {}; co_return arr[0]; }\n",
             "Co<int> f() { std::sort(v.begin(), v.end(), [](int a, int b) { return a < b; }); co_return v[0]; }\n",
         }) {
        t.check(lint::coroutine_lambdas({"Private/A.cpp", good}).empty(), std::string("not a coroutine lambda: ") + good);
    }
    std::vector<std::string> where = lint::coroutine_lambdas({"Private/A.cpp", "int x;\nauto f = [] {\n  co_return 1;\n};\n"});
    if (SCFG_CHECK_EQ(t, where.size(), 1)) SCFG_CHECK(t, where[0].rfind("Private/A.cpp:2: coroutine lambda (", 0) == 0);

    SCFG_CHECK_EQ(t, lint::only_std_includes({"Private/A.cpp", "#include <windows.h>\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::only_std_includes({"Private/A.cpp", "#include <stdint.h>\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::only_std_includes({"Private/A.cpp", "#include \"CoreMinimal.h\"\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::only_std_includes({"Private/A.cpp", "#include \"Scfg/NoSuchHeader.h\"\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::only_std_includes({"Private/A.cpp", "#include SOME_MACRO\n"}).size(), 1);
    SCFG_CHECK(t, lint::only_std_includes({"Private/A.cpp", "#include <string_view>\n# include \"Scfg/Version.h\"\n"}).empty());
    SCFG_CHECK(t, lint::only_std_includes({"Private/A.cpp", "// #include <windows.h>\n"}).empty());

    SCFG_CHECK(t, lint::names_its_twin({"Private/A.cpp", "// Twin of scfg_core/config.py\n"}).empty());
    SCFG_CHECK(t, lint::names_its_twin({"Private/A.cpp", "\xEF\xBB\xBF\n// Twin of scfg_core/solana/rpc.py (the client)\n"}).empty());
    SCFG_CHECK(t, lint::names_its_twin({"Private/A.cpp", "// No Python twin: the error values\n"}).empty());
    SCFG_CHECK_EQ(t, lint::names_its_twin({"Private/A.cpp", "// Twin of scfg_core/nosuch.py\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::names_its_twin({"Private/A.cpp", "#pragma once\n// No Python twin: late\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::names_its_twin({"Private/A.cpp", "// Copyright\n// No Python twin: second\n"}).size(), 1);
    SCFG_CHECK_EQ(t, lint::names_its_twin({"Private/A.cpp", ""}).size(), 1);

    std::vector<File> same = {{"Private/Rpc.cpp", ""}, {"Private/Solana/rpc.cpp", ""}, {"Public/Scfg/Rpc.h", ""}};
    SCFG_CHECK_EQ(t, lint::duplicate_basenames(same).size(), 1);
    SCFG_CHECK(t, lint::duplicate_basenames({{"Private/Rpc.cpp", ""}, {"Private/SolanaRpc.cpp", ""}}).empty());
}

// F3: every declaration a .cpp defines carries SCFGCORE_API, and a header that uses it includes Scfg/Export.h.
SCFG_TEST(LintTests, ExportsBite) {
    const std::string inc = "#include \"Scfg/Export.h\"\n";
    auto hits = [](const std::string& text) { return lint::exports({"Public/Scfg/A.h", text}); };
    for (const std::string& bad : std::vector<std::string>{
             inc + "namespace scfg {\nResult<Dec> parse(const Json& v);\n}\n",
             inc + "namespace a::b {\n[[noreturn]] void fail(\n    const char* what);\n}\n",
             inc + "extern const std::string_view VERSION;\n",
             inc + "class Dec {\npublic:\n    Dec();\n    int x() const { return 1; }\n};\n",
             inc + "struct Token {\n    static Result<Token> make(std::string_view a, int d = 6);\n};\n",
             inc + "class E final : public B {\n    std::string_view f() const override;\n};\n",
             inc + "class J { };\nclass J::D {\n    const J* find(std::string_view k) const;\n};\n",
             inc + "class SCFGCORE_API B {\n    friend B operator+(const B& a, const B& b);\n};\n",
             inc + "class SCFGCORE_API B {\n    friend std::strong_ordering operator<=>(const B&, const B&);\n};\n",
             inc + "class SCFGCORE_API O {\n    struct In {\n        void f();\n    };\n};\n",
             inc + "class SCFGCORE_API C {\n    explicit operator bool() const;\n};\nint f(int);\n",
             "SCFGCORE_API void f();\n",
         }) {
        t.check(hits(bad).size() == 1, "export lint missed:\n" + bad);
    }
    for (const std::string& good : std::vector<std::string>{
             inc + "namespace scfg {\nSCFGCORE_API Result<Dec> parse(const Json& v);\n}\n",
             inc + "namespace a::b {\n[[noreturn]] SCFGCORE_API void fail(\n    const char* what);\n}\n",
             inc + "extern SCFGCORE_API const std::string_view VERSION;\n",
             inc + "class SCFGCORE_API Dec {\npublic:\n    Dec();\n};\nstruct SCFGCORE_API J::D {\n    void f();\n};\n",
             inc + "class SCFGCORE_API B {\n    friend SCFGCORE_API B operator+(const B& a, const B& b);\n"
                   "    friend bool operator==(const B&, const B&) = default;\n};\n",
             "inline Status ok() { return Status(); }\ninline Result<BigInt> py_int(const char* s) { return x(s); }\n",
             "template <class T>\nCo<void> root(Co<T> w, std::function<void(Result<T>)> d) {\n    co_return ok();\n}\n",
             "template <class T>\nclass Co {\n    Result<T> take();\n"
             "    struct Awaiter {\n        void f();\n    };\n};\n",
             "template <>\nclass [[nodiscard]] Result<void> {\n    const Err& err() const&;\n};\n",
             "struct Item {\n    std::string id;\n    std::function<double()> clock = {};\n    Json kind = Json();\n"
             "    friend bool operator==(const Item&, const Item&) = default;\n};\n",
             "class T {\npublic:\n    virtual ~T() = default;\n    virtual Future<Json> post(std::string u) = 0;\n"
             "    T(const T&) = delete;\n    explicit T(int& c) : c_(&c) {}\n    int g() const { return 1; }\n};\n",
             "inline constexpr std::array<std::pair<int, int>, 1> X = {{\n    {1, 2},\n}};\n"
             "using F = std::function<int(int)>;\n",
             "enum class Cmp { Lt, Le };\nstruct Fwd;\n#define M(x) \\\n    auto t = (x);   \\\n    f(t)\n",
             "namespace scfg {\ntemplate <std::integral T>\n    requires(!std::same_as<T, bool>)\n"
             "std::string dec(T v) {\n    return std::string();\n}\n}\n",
         }) {
        std::vector<std::string> h = hits(good);
        t.check(h.empty(), "export lint fired on:\n" + good + "\n" + lint::join(h));
    }
    // Only public headers: a .cpp defines what its header exports.
    SCFG_CHECK(t, lint::exports({"Private/A.cpp", "Result<Dec> parse(const Json& v);\n"}).empty());
    std::vector<std::string> where = hits(inc + "namespace scfg {\n\nResult<Dec> parse(\n    const Json& v);\n}\n");
    if (SCFG_CHECK_EQ(t, where.size(), 1))
        SCFG_CHECK(t, where[0].rfind("Public/Scfg/A.h:4: free function without SCFGCORE_API (", 0) == 0);
}

// Every macro UnrealMacroPoison.h defines expands to SCFG_UE_POISON_<name>::unreal_macro, and none of the names
// below (the Unreal and Windows macros known to break plain C++) has gone missing.
SCFG_TEST(LintTests, PoisonHeaderIsWellFormed) {
    std::ifstream in(std::string(SCFG_REPO_ROOT) + "/unreal/core-tests/support/UnrealMacroPoison.h");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const std::regex def("^#define (\\w+)(\\(\\.\\.\\.\\))? SCFG_UE_POISON_(\\w+)::unreal_macro$");
    std::set<std::string> names;
    for (const std::string& line : lint::views(text).code) {
        if (line.find("#define") == std::string::npos) continue;
        std::smatch m;
        if (t.check(std::regex_match(line, m, def) && m[1] == m[3], "not a poison define: " + line))
            names.insert(m[1].str());
    }
    for (const char* name :
         {"check", "checkf", "checkSlow", "verify", "verifyf", "ensure", "ensureMsgf", "TEXT", "PI", "INDEX_NONE",
          "FORCEINLINE", "TRUE", "FALSE", "BOOL", "INT", "UINT", "DWORD", "CHAR", "TCHAR", "ERROR", "DELETE", "IN",
          "OUT", "OPTIONAL", "CONST", "VOID", "interface", "small", "hyper", "near", "far", "DOMAIN", "SING",
          "OVERFLOW", "UNDERFLOW", "TLOSS", "PLOSS", "NO_ERROR", "INFINITE", "ABSOLUTE", "RELATIVE", "TRANSPARENT",
          "OPAQUE", "Yield", "GetObject", "SendMessage", "PostMessage", "GetMessage", "LoadString", "DrawText",
          "CreateFile", "DeleteFile", "CopyFile", "MoveFile", "GetCurrentTime", "GetUserName", "GetClassName",
          "FormatMessage", "GetCommandLine", "LoadLibrary", "MessageBox", "CreateEvent", "CreateMutex", "major",
          "minor", "linux", "unix"}) {
        t.check(names.count(name) == 1, std::string("UnrealMacroPoison.h does not poison ") + name);
    }
}
