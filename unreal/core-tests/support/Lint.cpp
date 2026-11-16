// No Python twin: the scanner and the file-level checks behind LintTests.
#include "support/Lint.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::vector<File> core_files() {
    std::vector<File> out;
    std::error_code ec;
    fs::path root(core_dir());
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        if (ext != ".h" && ext != ".cpp") continue;
        std::ifstream in(it->path(), std::ios::binary);
        File f;
        f.rel = it->path().lexically_relative(root).generic_string();
        f.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const File& a, const File& b) { return a.rel < b.rel; });
    return out;
}

std::string basename(std::string_view rel) {
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
            params.find("string_view") == std::string::npos)
            continue;
        size_t line = 1 + static_cast<size_t>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(i), '\n'));
        hits.push_back(at(f, line, "Co<...> parameter by reference",
                          "a reference, string_view or pointer dangles after the first co_await; pass by value"));
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
            if (!std::filesystem::exists(core_dir() + "/Public/" + x[1].str()))
                hits.push_back(at(f, n + 1, "#include \"" + x[1].str() + "\"", why_own));
        } else {
            hits.push_back(at(f, n + 1, "#include " + what, why_own));
        }
    }
    return hits;
}

std::vector<std::string> names_its_twin(const File& f) {
    static const std::string py_root = std::string(SCFG_REPO_ROOT) + "/plugin/game/python-packages/";
    static const std::regex twin("scfg_core/[A-Za-z0-9_/]+\\.py");
    const std::string why = "the first comment line says `Twin of scfg_core/<module>.py` or `No Python twin: ...`";
    Views v = lint::views(f.text);
    for (size_t n = 0; n < v.raw.size(); ++n) {
        std::string line = v.raw[n];
        if (n == 0 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        if (line.rfind("//", 0) != 0) break;
        if (line.find("No Python twin") != std::string::npos) return {};
        if (line.find("Twin of scfg_core/") == std::string::npos) break;
        std::vector<std::string> hits;
        for (std::sregex_iterator it(line.begin(), line.end(), twin), end; it != end; ++it) {
            if (!std::filesystem::exists(py_root + it->str()))
                hits.push_back(at(f, n + 1, it->str() + " does not exist", why));
        }
        if (std::sregex_iterator(line.begin(), line.end(), twin) == std::sregex_iterator())
            hits.push_back(at(f, n + 1, "names no scfg_core/<module>.py", why));
        return hits;
    }
    return {at(f, 1, "no twin comment", why)};
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
