// No Python twin: the scanner behind LintTests (literal blanking, the core's file list, line rules, file checks).
#pragma once

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace scfg_test::lint {

// The lines of a source file three ways: raw; `code` with comments removed and the contents of string and char
// literals (raw strings and prefixed ones included) removed, delimiters kept; `kept` with comments removed only.
// All three have the same number of lines.
struct Views {
    std::vector<std::string> raw, code, kept;
};
Views views(std::string_view text);

struct File {
    std::string rel;  // relative to Source/ScfgCore, '/'-separated: "Private/Version.cpp"
    std::string text;
};
// Absolute path of unreal/Plugins/Scfg/Source/ScfgCore.
std::string core_dir();
// Every .h and .cpp under core_dir(), sorted by rel. A directory or read error is appended to *error.
std::vector<File> core_files(std::string* error = nullptr);
// Absolute path of unreal/core-tests.
std::string tests_dir();
// Every .h and .cpp under tests_dir(), rel prefixed "core-tests/" ("core-tests/CoTests.cpp"), sorted.
std::vector<File> test_files(std::string* error = nullptr);
// "Private/Version.cpp" -> "Version.cpp".
std::string basename(std::string_view rel);
bool ends_with(std::string_view s, std::string_view suffix);

struct Rule {
    std::string label;  // printed as the rule; unique across all tables
    std::string why;    // one short clause
    std::string ext;    // only files with this suffix ("" = all)
    std::vector<std::string> any;  // the regex runs only on a line that contains one of these ({} = every line)
    std::regex re;
};
struct Waiver {
    std::string file;  // basename
    std::string label;
};

// "rel:line: label (why)" for every line of f that a rule matches, unless a waiver names f and the rule.
std::vector<std::string> scan(const File& f, const std::vector<Rule>& rules, const std::vector<Waiver>& waivers);
// One message per line, for a failure report.
std::string join(const std::vector<std::string>& lines);

// The file-level checks. Each returns "rel:line: rule (why)" messages, none when the file passes.
std::vector<std::string> pragma_once(const File& f);        // a header without #pragma once
std::vector<std::string> coroutine_params(const File& f);   // Co<T> f(const X&), auto f(X*) -> Co<T>, lambdas too
std::vector<std::string> coroutine_lambdas(const File& f);  // co_await/co_return/co_yield in a lambda body (R8)
std::vector<std::string> only_std_includes(const File& f);  // <x> not standard C++20, "x" not Public/Scfg/<x>.h
std::vector<std::string> names_its_twin(const File& f);     // first comment line names no (existing) twin
std::vector<std::string> duplicate_basenames(const std::vector<File>& all);  // case-insensitive, as on Windows

}  // namespace scfg_test::lint
