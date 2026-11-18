// Copyright 2026 Repulse
// No Python twin: the scanner and the file-level checks behind LintTests.
#include "support/Lint.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace scfg_test::lint {

static bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out(1);
    for (char c : s) {
        if (c == '\n') {
            out.emplace_back();
        } else if (c != '\r') {
            out.back() += c;
        }
    }
    return out;
}

Views views(std::string_view t) {
    std::string code, kept;
    auto both = [&](std::string_view s) {
        code += s;
        kept += s;
    };
    // Literal text: kept in full, reduced to its newlines (so line numbers hold) in code.
    auto literal = [&](std::string_view s) {
        kept += s;
        for (char c : s) {
            if (c == '\n') code += '\n';
        }
    };
    size_t i = 0;
    while (i < t.size()) {
        char c = t[i];
        char n1 = i + 1 < t.size() ? t[i + 1] : '\0';
        if (c == '/' && n1 == '/') {
            while (i < t.size() && t[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && n1 == '*') {
            size_t end = t.find("*/", i + 2);
            end = end == std::string_view::npos ? t.size() : end + 2;
            for (size_t k = i; k < end; ++k) {
                if (t[k] == '\n') both("\n");
            }
            i = end;
            continue;
        }
        if (ident_start(c)) {
            size_t j = i;
            while (j < t.size() && ident_char(t[j])) ++j;
            std::string_view word = t.substr(i, j - i);
            both(word);
            i = j;
            bool raw = word == "R" || word == "u8R" || word == "uR" || word == "UR" || word == "LR";
            bool prefix = word == "u8" || word == "u" || word == "U" || word == "L";
            size_t open = raw && i < t.size() && t[i] == '"' ? t.find('(', i + 1) : std::string_view::npos;
            if (open != std::string_view::npos) {
                // R"delim( ... )delim"
                std::string close = ")" + std::string(t.substr(i + 1, open - i - 1)) + "\"";
                size_t end = t.find(close, open + 1);
                end = end == std::string_view::npos ? t.size() : end + close.size();
                both("\"");
                literal(t.substr(i + 1, end - i - 2));
                both("\"");
                i = end;
                continue;
            }
            if (!prefix || i >= t.size() || (t[i] != '"' && t[i] != '\'')) continue;
            c = t[i];  // u8"...", L'x': the quote handling below takes the literal
        } else if (c >= '0' && c <= '9') {
            // A pp-number, so the digit separator in 1'000 does not open a char literal.
            size_t j = i + 1;
            while (j < t.size() && (ident_char(t[j]) || t[j] == '.' || t[j] == '\'' ||
                                    ((t[j] == '+' || t[j] == '-') && (t[j - 1] == 'e' || t[j - 1] == 'E' ||
                                                                      t[j - 1] == 'p' || t[j - 1] == 'P')))) {
                ++j;
            }
            both(t.substr(i, j - i));
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < t.size() && t[j] != c && t[j] != '\n') j += (t[j] == '\\' && j + 1 < t.size()) ? 2 : 1;
            bool closed = j < t.size() && t[j] == c;
            both(std::string_view(&t[i], 1));
            literal(t.substr(i + 1, j - i - 1));
            if (closed) both(std::string_view(&t[j], 1));
            i = closed ? j + 1 : j;
            continue;
        }
        both(std::string_view(&t[i], 1));
        ++i;
    }
    Views v;
    v.raw = split_lines(std::string(t));
    v.code = split_lines(code);
    v.kept = split_lines(kept);
    return v;
}

std::string core_dir() {
    return (fs::path(SCFG_REPO_ROOT) / "unreal" / "Plugins" / "Scfg" / "Source" / "ScfgCore").generic_string();
}

std::string tests_dir() { return (fs::path(SCFG_REPO_ROOT) / "unreal" / "core-tests").generic_string(); }

// Every .h and .cpp (and .cs, with cs) under root, rel = prefix + the path relative to root, sorted by rel.
static std::vector<File> files_under(const std::string& dir, const std::string& prefix, std::string* error,
                                     bool cs = false) {
    auto report = [error](const std::string& what) {
        if (error != nullptr) *error += (error->empty() ? "" : "; ") + what;
    };
    std::vector<File> out;
    std::error_code ec;
    fs::path root(dir);
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code kind;
        if (!it->is_regular_file(kind)) {
            if (kind) report(it->path().generic_string() + ": " + kind.message());
            continue;
        }
        std::string ext = it->path().extension().string();
        if (ext != ".h" && ext != ".cpp" && !(cs && ext == ".cs")) continue;
        std::ifstream in(it->path(), std::ios::binary);
        File f;
        f.rel = prefix + it->path().lexically_relative(root).generic_string();
        f.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (!in && !in.eof()) report(f.rel + ": cannot read");
        out.push_back(std::move(f));
    }
    if (ec) report(root.generic_string() + ": " + ec.message());
    std::sort(out.begin(), out.end(), [](const File& a, const File& b) { return a.rel < b.rel; });
    return out;
}

std::vector<File> core_files(std::string* error) { return files_under(core_dir(), "", error); }

std::vector<File> test_files(std::string* error) { return files_under(tests_dir(), "core-tests/", error); }

std::vector<File> source_files(std::string* error) {
    const fs::path unreal = fs::path(SCFG_REPO_ROOT) / "unreal";
    std::vector<File> out;
    for (const char* dir : {"Plugins/Scfg/Source", "Project/ScfgDev/Source", "core-tests", "core-driver",
                            "core-exports"}) {
        std::vector<File> some = files_under((unreal / dir).generic_string(), std::string(dir) + "/", error, true);
        std::move(some.begin(), some.end(), std::back_inserter(out));
    }
    std::sort(out.begin(), out.end(), [](const File& a, const File& b) { return a.rel < b.rel; });
    return out;
}

// "Private/Version.cpp" -> "Version.cpp".
static std::string basename(std::string_view rel) {
    size_t slash = rel.rfind('/');
    return std::string(slash == std::string_view::npos ? rel : rel.substr(slash + 1));
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::vector<std::string> scan(const File& f, const std::vector<Rule>& rules, const std::vector<Waiver>& waivers) {
    std::string base = basename(f.rel);
    std::vector<const Rule*> active;
    for (const Rule& r : rules) {
        if (!r.ext.empty() && !ends_with(f.rel, r.ext)) continue;
        bool waived = std::any_of(waivers.begin(), waivers.end(),
                                  [&](const Waiver& w) { return w.file == base && w.label == r.label; });
        if (!waived) active.push_back(&r);
    }
    std::vector<std::string> hits;
    if (active.empty()) return hits;
    Views v = views(f.text);
    for (size_t n = 0; n < v.code.size(); ++n) {
        const std::string& line = v.code[n];
        for (const Rule* r : active) {
            bool maybe = r->any.empty() || std::any_of(r->any.begin(), r->any.end(), [&](const std::string& s) {
                             return line.find(s) != std::string::npos;
                         });
            if (maybe && std::regex_search(line, r->re))
                hits.push_back(f.rel + ":" + std::to_string(n + 1) + ": " + r->label + " (" + r->why + ")");
        }
    }
    return hits;
}

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) out += "  " + l + "\n";
    return out;
}

// ---- file-level checks ----

static std::string at(const File& f, size_t line, std::string_view rule, std::string_view why) {
    return f.rel + ":" + std::to_string(line) + ": " + std::string(rule) + " (" + std::string(why) + ")";
}

std::vector<std::string> pragma_once(const File& f) {
    if (!ends_with(f.rel, ".h")) return {};
    Views v = lint::views(f.text);
    static const std::regex re("^\\s*#\\s*pragma\\s+once\\b");
    for (const std::string& line : v.code) {
        if (std::regex_search(line, re)) return {};
    }
    return {at(f, 1, "#pragma once", "every header is included many times in a unity build")};
}

// Index just past the bracket that closes the one at i (open/close are '<'/'>' or '('/')'), or npos.
static size_t skip_balanced(std::string_view s, size_t i, char open, char close) {
    int depth = 0;
    for (; i < s.size(); ++i) {
        if (s[i] == open) ++depth;
        if (s[i] == close && --depth == 0) return i + 1;
    }
    return std::string_view::npos;
}

static size_t skip_space(std::string_view s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
    return i;
}

// The parameter list of the function whose return type is the Co<...> at i, or "" if it is not a declaration:
// `Co<T> name(params)` or `auto name(params) -> Co<T>` (lambdas too).
static std::string co_params(std::string_view s, size_t i) {
    size_t k = i;
    while (k >= 2 && s.substr(k - 2, 2) == "::") {  // scfg::Co<T>
        k -= 2;
        while (k > 0 && ident_char(s[k - 1])) --k;
    }
    while (k > 0 && (s[k - 1] == ' ' || s[k - 1] == '\t' || s[k - 1] == '\n')) --k;
    if (k >= 2 && s.substr(k - 2, 2) == "->") {
        size_t close = k - 2;
        while (close > 0 && s[close - 1] != ')') {
            char c = s[close - 1];
            if (c != ' ' && c != '\t' && c != '\n' && !ident_char(c)) return "";  // `const`, `noexcept`, ...
            --close;
        }
        if (close == 0) return "";
        int depth = 0;
        for (size_t j = close; j-- > 0;) {
            if (s[j] == ')') ++depth;
            if (s[j] == '(' && --depth == 0) return std::string(s.substr(j + 1, close - 1 - (j + 1)));
        }
        return "";
    }
    size_t j = skip_balanced(s, i + 2, '<', '>');
    if (j == std::string_view::npos) return "";
    j = skip_space(s, j);
    size_t name = j;
    while (j < s.size() && (ident_char(s[j]) || s[j] == ':')) ++j;
    if (j == name) return "";
    j = skip_space(s, j);
    if (j >= s.size() || s[j] != '(') return "";
    size_t end = skip_balanced(s, j, '(', ')');
    if (end == std::string_view::npos) return "";
    return std::string(s.substr(j + 1, end - 1 - (j + 1)));
}

std::vector<std::string> coroutine_params(const File& f) {
    Views v = lint::views(f.text);
    std::string code;
    for (const std::string& line : v.code) code += line + "\n";
    std::vector<std::string> hits;
    for (size_t i = code.find("Co<"); i != std::string::npos; i = code.find("Co<", i + 1)) {
        if (i > 0 && ident_char(code[i - 1])) continue;
        std::string params = co_params(code, i);
        if (params.find('&') == std::string::npos && params.find('*') == std::string::npos &&
            params.find("string_view") == std::string::npos && params.find("span<") == std::string::npos)
            continue;
        size_t line = 1 + static_cast<size_t>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(i), '\n'));
        hits.push_back(at(f, line, "Co<...> parameter by reference",
                          "a reference, string_view, span or pointer dangles after the first co_await; pass by value"));
    }
    return hits;
}

// Is the '[' at i a lambda introducer? Not the [[ of an attribute, and not a subscript or an array declarator
// (a '[' after a name, ')' or ']'), except after the keywords a lambda can follow.
static bool lambda_introducer(std::string_view s, size_t i) {
    if (i + 1 < s.size() && s[i + 1] == '[') return false;
    size_t k = i;
    while (k > 0 && (s[k - 1] == ' ' || s[k - 1] == '\t' || s[k - 1] == '\n')) --k;
    if (k == 0) return true;
    char p = s[k - 1];
    if (ident_char(p)) {
        size_t w = k;
        while (w > 0 && ident_char(s[w - 1])) --w;
        std::string_view word = s.substr(w, k - w);
        return word == "return" || word == "co_return" || word == "co_yield" || word == "co_await";
    }
    return p != ')' && p != ']' && p != '[';
}

// [begin, end) of the body of the lambda whose introducer is at i, or {npos, npos} when the '[' starts none:
// [captures] <template params> (params) specifiers -> return type { body }. A ';', ',', '=', ']' or '}' (or an
// unbalanced ')') before the body's '{' means it was something else (a structured binding, say).
static std::pair<size_t, size_t> lambda_body(std::string_view s, size_t i) {
    const std::pair<size_t, size_t> none{std::string_view::npos, std::string_view::npos};
    size_t j = skip_balanced(s, i, '[', ']');
    if (j == std::string_view::npos) return none;
    j = skip_space(s, j);
    if (j < s.size() && s[j] == '<') {
        j = skip_balanced(s, j, '<', '>');
        if (j == std::string_view::npos) return none;
        j = skip_space(s, j);
    }
    if (j < s.size() && s[j] == '(') {
        j = skip_balanced(s, j, '(', ')');
        if (j == std::string_view::npos) return none;
    }
    int paren = 0, angle = 0;
    for (; j < s.size(); ++j) {
        char c = s[j];
        if (c == '-' && j + 1 < s.size() && s[j + 1] == '>') {
            ++j;
        } else if (c == '(') {
            ++paren;
        } else if (c == ')') {
            if (paren-- == 0) return none;
        } else if (c == '<') {
            ++angle;
        } else if (c == '>') {
            if (angle > 0) --angle;
        } else if (paren == 0 && angle == 0 && (c == ';' || c == ',' || c == '=' || c == ']' || c == '}')) {
            return none;
        } else if (paren == 0 && angle == 0 && c == '{') {
            size_t end = skip_balanced(s, j, '{', '}');
            return {j + 1, end == std::string_view::npos ? s.size() : end - 1};
        }
    }
    return none;
}

std::vector<std::string> coroutine_lambdas(const File& f) {
    Views v = lint::views(f.text);
    std::string code;
    for (const std::string& line : v.code) code += line + "\n";
    std::vector<std::string> hits;
    for (size_t i = code.find('['); i != std::string::npos; i = code.find('[', i + 1)) {
        if (!lambda_introducer(code, i)) continue;
        auto [begin, end] = lambda_body(code, i);
        if (begin == std::string::npos) continue;
        bool coroutine = false;
        for (size_t k = begin; !coroutine && k + 3 <= end; ++k) {
            if (code.compare(k, 3, "co_") != 0 || (k > 0 && ident_char(code[k - 1]))) continue;
            size_t w = k + 3;
            while (w < end && ident_char(code[w])) ++w;
            std::string_view word = std::string_view(code).substr(k, w - k);
            coroutine = word == "co_await" || word == "co_return" || word == "co_yield";
        }
        if (!coroutine) continue;
        size_t line = 1 + static_cast<size_t>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(i), '\n'));
        hits.push_back(at(f, line, "coroutine lambda",
                          "Clang miscompiles lambda coroutines (llvm#91983); write a named function"));
    }
    return hits;
}

// The C++20 standard library headers (the C ones in their <cxxx> spelling only).
static const std::set<std::string>& std_headers() {
    static const std::set<std::string> h = {
        "algorithm", "any", "array", "atomic", "barrier", "bit", "bitset", "cassert", "cctype", "cerrno",
        "cfenv", "cfloat", "charconv", "chrono", "cinttypes", "climits", "clocale", "cmath", "codecvt",
        "compare", "complex", "concepts", "condition_variable", "coroutine", "csetjmp", "csignal", "cstdarg",
        "cstddef", "cstdint", "cstdio", "cstdlib", "cstring", "ctime", "cuchar", "cwchar", "cwctype", "deque",
        "exception", "execution", "filesystem", "format", "forward_list", "fstream", "functional", "future",
        "initializer_list", "iomanip", "ios", "iosfwd", "iostream", "istream", "iterator", "latch", "limits",
        "list", "locale", "map", "memory", "memory_resource", "mutex", "new", "numbers", "numeric", "optional",
        "ostream", "queue", "random", "ranges", "ratio", "regex", "scoped_allocator", "semaphore", "set",
        "shared_mutex", "source_location", "span", "sstream", "stack", "stdexcept", "stop_token", "streambuf",
        "string", "string_view", "syncstream", "system_error", "thread", "tuple", "type_traits", "typeindex",
        "typeinfo", "unordered_map", "unordered_set", "utility", "valarray", "variant", "vector", "version",
    };
    return h;
}

std::vector<std::string> only_std_includes(const File& f) {
    static const std::regex inc("^\\s*#\\s*include\\b\\s*(.*)$");
    static const std::regex angle("^<([^>]+)>\\s*$");
    static const std::regex quote("^\"(Scfg/[A-Za-z0-9_]+\\.h)\"\\s*$");
    const std::string why_std = "the core includes only the standard library: no Unreal, OS or third-party header";
    const std::string why_own = "a quoted include must be a public header of this module, Scfg/<Name>.h";
    Views v = lint::views(f.text);
    std::vector<std::string> hits;
    for (size_t n = 0; n < v.kept.size(); ++n) {
        std::smatch m, x;
        if (v.kept[n].find("include") == std::string::npos || !std::regex_search(v.kept[n], m, inc)) continue;
        std::string what = m[1].str();
        if (std::regex_match(what, x, angle)) {
            if (!std_headers().count(x[1].str())) hits.push_back(at(f, n + 1, "#include <" + x[1].str() + ">", why_std));
        } else if (std::regex_match(what, x, quote)) {
            std::error_code ec;
            if (!fs::exists(core_dir() + "/Public/" + x[1].str(), ec))
                hits.push_back(at(f, n + 1, "#include \"" + x[1].str() + "\"" + (ec ? " (" + ec.message() + ")" : ""),
                                  why_own));
        } else {
            hits.push_back(at(f, n + 1, "#include " + what, why_own));
        }
    }
    return hits;
}

std::vector<std::string> names_its_twin(const File& f) {
    static const std::string py_root = std::string(SCFG_REPO_ROOT) + "/plugin/game/python-packages/";
    static const std::regex twin("scfg_core/[A-Za-z0-9_/]+\\.py");
    const std::string why =
        "the first comment line after the copyright line says `Twin of scfg_core/<module>.py` or `No Python twin: ...`";
    Views v = lint::views(f.text);
    if (!v.raw.empty() && v.raw[0].rfind("\xEF\xBB\xBF", 0) == 0) v.raw[0].erase(0, 3);
    // The copyright line comes first (copyright() checks it); the twin comment is the first comment after it.
    size_t first = !v.raw.empty() && v.raw[0] == COPYRIGHT ? 1 : 0;
    for (size_t n = first; n < v.raw.size(); ++n) {
        const std::string& line = v.raw[n];
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        if (line.rfind("//", 0) != 0) break;
        if (line.find("No Python twin") != std::string::npos) return {};
        if (line.find("Twin of scfg_core/") == std::string::npos) break;
        std::vector<std::string> hits;
        for (std::sregex_iterator it(line.begin(), line.end(), twin), end; it != end; ++it) {
            std::error_code ec;
            if (!fs::exists(py_root + it->str(), ec))
                hits.push_back(at(f, n + 1, it->str() + (ec ? " cannot be checked: " + ec.message() : " does not exist"), why));
        }
        if (std::sregex_iterator(line.begin(), line.end(), twin) == std::sregex_iterator())
            hits.push_back(at(f, n + 1, "names no scfg_core/<module>.py", why));
        return hits;
    }
    return {at(f, 1, "no twin comment", why)};
}

std::vector<std::string> copyright(const File& f) {
    const std::string why = "Fab 4.3.6.1.b: every source file starts with the publisher's notice, " +
                            std::string(COPYRIGHT);
    std::vector<std::string> lines = split_lines(f.text);
    std::vector<std::string> hits;
    if (lines[0] != COPYRIGHT) hits.push_back(at(f, 1, "line 1 is not the copyright line", why));
    for (size_t n = 1; n < lines.size(); ++n) {
        if (lines[n] == COPYRIGHT) hits.push_back(at(f, n + 1, "the copyright line again", why));
    }
    return hits;
}

// ---- exports ----

// s with every balanced (...) emptied to "()" and every <...> removed, innermost first, after the operator names are
// replaced (so operator<< and operator() are not brackets): the shape of a declaration at its top level.
static std::string top_level(std::string s) {
    static const std::regex op("operator\\s*(<=>|<<=|>>=|<<|>>|<=|>=|==|!=|->\\*?|\\(\\s*\\)|\\[\\s*\\]|"
                               "[-+*/%^&|~!=<>,]=?|co_await|new|delete|[A-Za-z_][\\w:]*)");
    s = std::regex_replace(s, op, "operator_");
    static const std::regex parens("\\([^()]*\\)"), angles("<[^<>]*>");
    for (std::string prev; prev != s;) {
        prev = s;
        s = std::regex_replace(s, parens, "@");
        s = std::regex_replace(s, angles, "");
    }
    return s;
}

// A declaration (not a definition) of a function that some .cpp defines: `... name(...) [const] [volatile]
// [&|&&] [noexcept[(...)]] [override] [final] [-> T]`, with no `= default`, `= delete` or `= 0` (a top-level '=' is
// a variable).
static bool declares_function(const std::string& stmt) {
    static const std::regex fn("^[^=]*[\\w\\]]\\s*@\\s*(const\\s*)?(volatile\\s*)?(&&?\\s*)?(noexcept\\s*@?\\s*)?"
                               "((override|final)\\s*)*(->[^=]*)?$");
    return std::regex_match(top_level(stmt), fn);
}

static bool starts_with_word(std::string_view s, std::string_view w) {
    return s.substr(0, w.size()) == w && (s.size() == w.size() || !ident_char(s[w.size()]));
}

std::vector<std::string> exports(const File& f) {
    if (!ends_with(f.rel, ".h") || basename(f.rel) == "Export.h") return {};
    const std::string why_fn = "a module exports only what is marked: a call from another module fails to link";
    const std::string why_class = "a class with a member a .cpp defines is exported whole: class SCFGCORE_API X";
    const std::string why_inc = "a header that uses SCFGCORE_API includes Scfg/Export.h, which defines it outside UBT";
    Views v = lint::views(f.text);
    // The code without preprocessor lines (a #define's continuation lines included), one '\n' per line.
    static const std::regex export_include("^\\s*#\\s*include\\s*\"Scfg/Export\\.h\"");
    std::string code;
    bool continued = false;
    bool includes_export = false;
    for (size_t n = 0; n < v.code.size(); ++n) {
        const std::string& l = v.code[n];
        size_t first = l.find_first_not_of(" \t");
        bool directive = continued || (first != std::string::npos && l[first] == '#');
        continued = directive && !l.empty() && l.back() == '\\';
        if (directive && std::regex_search(v.kept[n], export_include)) includes_export = true;
        code += (directive ? std::string() : l) + "\n";
    }

    enum class Kind { Ns, Class, Other };
    struct Scope {
        Kind kind;
        bool tmpl = false;     // a class template (or a class nested in one): nothing in it is checked
        bool has_api = false;  // class SCFGCORE_API X
        bool needs = false;    // declares a member that a .cpp defines
        size_t line = 0;
    };
    std::vector<Scope> scopes = {{Kind::Ns}};
    std::vector<std::string> hits;
    bool uses_api = code.find("SCFGCORE_API") != std::string::npos;
    std::string stmt;
    size_t stmt_line = 0, line = 1;
    static const std::regex access("^((public|private|protected)\\s*:\\s*)+");
    static const std::regex attrs("^(\\[\\[[^\\]]*\\]\\]\\s*)+");
    static const std::regex template_prefix("^template\\s*");
    static const std::regex extern_kw("^extern\\s*");
    static const std::regex inline_data("\\b(constexpr|inline)\\b");
    auto clean = [&](const std::string& s) {
        std::string t = std::regex_replace(s, access, "");
        size_t b = t.find_first_not_of(" \t\n");
        size_t e = t.find_last_not_of(" \t\n");
        return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
    };
    auto finish = [&]() {  // the statement in stmt ended with ';' at a namespace or class scope
        std::string s = clean(stmt);
        stmt.clear();
        if (s.empty()) return;
        Scope& sc = scopes.back();
        bool api = s.find("SCFGCORE_API") != std::string::npos;
        std::string bare = std::regex_replace(s, attrs, "");  // [[noreturn]] void f();
        bool fn = declares_function(bare);
        // `class X;` is a declaration of X, `struct X make();` one of make()
        bool elaborated = starts_with_word(bare, "class") || starts_with_word(bare, "struct") ||
                          starts_with_word(bare, "union") || starts_with_word(bare, "enum");
        if (sc.kind == Kind::Ns) {
            for (const char* w : {"inline", "template", "constexpr", "consteval", "static", "using", "typedef",
                                  "static_assert", "namespace", "friend"}) {
                if (starts_with_word(bare, w)) return;
            }
            if (elaborated && !fn) return;
            if (bare.rfind("extern \"\"", 0) == 0) {  // extern "C" void f(); (the literal is blanked)
                bare = bare.substr(9);
                fn = declares_function(bare);
            } else if (starts_with_word(bare, "extern")) {
                // an explicit instantiation declaration is not a variable, and has nothing of ours to export
                if (starts_with_word(std::regex_replace(bare, extern_kw, ""), "template")) return;
                if (!api) hits.push_back(at(f, stmt_line, "extern variable without SCFGCORE_API", why_fn));
                return;
            }
            if (fn && !api) {
                hits.push_back(at(f, stmt_line, "free function without SCFGCORE_API", why_fn));
            }
            return;
        }
        if (sc.tmpl) return;
        if (starts_with_word(bare, "friend")) {
            if (declares_function(bare) && !api)
                hits.push_back(at(f, stmt_line, "friend function without SCFGCORE_API", why_fn));
            return;
        }
        for (const char* w : {"template", "using", "typedef", "static_assert"}) {
            if (starts_with_word(bare, w)) return;
        }
        if (elaborated && !fn) return;
        if (fn) {
            sc.needs = true;
        } else if (starts_with_word(bare, "static") && !std::regex_search(bare, inline_data)) {
            sc.needs = true;  // a static data member: some .cpp defines it
        }
    };

    for (size_t i = 0; i < code.size(); ++i) {
        char c = code[i];
        if (c == '\n') ++line;
        Kind kind = scopes.back().kind;
        if (kind == Kind::Other) {
            if (c == '{') scopes.push_back({Kind::Other});
            if (c != '}') continue;
            scopes.pop_back();
            if (scopes.back().kind == Kind::Other) continue;
            // A function body ends its statement; an initializer ({...} then ';', ',' or ')') does not.
            size_t j = code.find_first_not_of(" \t\n", i + 1);
            if (j == std::string::npos || (code[j] != ';' && code[j] != ',' && code[j] != ')')) stmt.clear();
            continue;
        }
        if (c == '{') {
            std::string head = clean(stmt);
            std::string shape = top_level(head);
            bool tmpl = scopes.back().tmpl || starts_with_word(head, "template");
            std::string after = std::regex_replace(shape, template_prefix, "");
            if (starts_with_word(head, "namespace") || head.rfind("extern \"\"", 0) == 0) {
                scopes.push_back({Kind::Ns});
                stmt.clear();
            } else if (starts_with_word(after, "class") || starts_with_word(after, "struct") ||
                       starts_with_word(after, "union")) {
                bool api = head.find("SCFGCORE_API") != std::string::npos;
                scopes.push_back({Kind::Class, tmpl, api, false, stmt_line});
                stmt.clear();
            } else {
                scopes.push_back({Kind::Other});  // a function body, an enum's list or an initializer: stmt goes on
            }
            continue;
        }
        if (c == '}') {
            Scope sc = scopes.back();
            if (scopes.size() > 1) scopes.pop_back();
            if (sc.kind == Kind::Class && !sc.tmpl && sc.needs && !sc.has_api)
                hits.push_back(at(f, sc.line, "class without SCFGCORE_API", why_class));
            stmt.clear();
            continue;
        }
        if (c == ';') {
            finish();
            continue;
        }
        bool blank = stmt.find_first_not_of(" \t\n") == std::string::npos;
        if (blank && c != ' ' && c != '\t' && c != '\n') stmt_line = line;
        stmt += c;
    }
    if (uses_api && !includes_export)
        hits.push_back(at(f, 1, "SCFGCORE_API without #include \"Scfg/Export.h\"", why_inc));
    return hits;
}

static std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// Basenames that collide (case-insensitively, as on Windows) among files.
std::vector<std::string> duplicate_basenames(const std::vector<File>& all) {
    std::vector<std::string> hits;
    std::vector<std::pair<std::string, std::string>> seen;  // lowered basename, rel
    for (const File& f : all) {
        std::string key = lower(basename(f.rel));
        for (const auto& [k, rel] : seen) {
            if (k == key)
                hits.push_back(at(f, 1, "same basename as " + rel,
                                  "UBT names objects and unity files by basename, so they collide"));
        }
        seen.emplace_back(key, f.rel);
    }
    return hits;
}

}  // namespace scfg_test::lint
