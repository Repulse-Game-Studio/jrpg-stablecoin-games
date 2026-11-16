// No Python twin: the JSON value, deep_copy, and Python's json.loads / json.dumps (see Json.h). The reader is
// CPython's _json scanner, by hand, over UTF-8 bytes.
#include "Scfg/Json.h"

#include "Scfg/Ints.h"
#include "Scfg/Py.h"
#include "Scfg/PyFloat.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace scfg::json_detail {

[[noreturn]] void misuse(const std::string& what) { scfg::detail::bad_result_access(what.c_str(), nullptr); }

[[noreturn]] void wrong_type(const char* accessor, const Json& v) {
    misuse(std::string("Json::") + accessor + "() of a " + std::string(v.type_name()));
}

std::string dec(uint64_t v) {
    char buf[24];
    auto res = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, res.ptr);
}

// The containers on the path from the root to the value being written or copied: meeting one again is a cycle.
struct Path {
    std::vector<const void*> on;
    void enter(const void* p, const char* what) {
        for (const void* q : on) {
            if (q == p) misuse(std::string(what) + " of a list or dict that contains itself (Python: ValueError: Circular "
                                                   "reference detected)");
        }
        on.push_back(p);
    }
    void leave() { on.pop_back(); }
};

Json copy(const Json& v, Path& path) {
    if (v.is_list()) {
        const Json::List& l = v.as_list();
        path.enter(&l, "deep_copy");
        Json::List out;
        out.reserve(l.size());
        for (const Json& x : l) out.push_back(copy(x, path));
        path.leave();
        return Json(std::move(out));
    }
    if (v.is_dict()) {
        const Json::Dict& d = v.as_dict();
        path.enter(&d, "deep_copy");
        Json::Dict out;
        for (const auto& [key, value] : d) out.set(key, copy(value, path));
        path.leave();
        return Json(std::move(out));
    }
    return v;
}

// ---- loads ----

bool digit_at(std::string_view s, size_t k) { return k < s.size() && s[k] >= '0' && s[k] <= '9'; }

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The four hex digits at s[p..p+4) as a number, or -1.
int32_t hex4(std::string_view s, size_t p) {
    int32_t u = 0;
    for (size_t k = 0; k < 4; ++k) {
        int h = hex_value(s[p + k]);
        if (h < 0) return -1;
        u = u * 16 + h;
    }
    return u;
}

class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}

    Result<Json> document() {
        SCFG_TRY_VOID(py::utf8_check(s_));
        if (s_.substr(0, 3) == "\xEF\xBB\xBF") return error("Unexpected UTF-8 BOM (decode using utf-8-sig)", 0);
        skip_ws();
        SCFG_TRY(Json v, value());
        skip_ws();
        if (i_ != s_.size()) return error("Extra data", i_);
        return v;
    }

private:
    std::string_view s_;
    size_t i_ = 0;
    size_t depth_ = 0;

    // JSONDecodeError's "msg: line L column C (char N)", counted in code points as Python does.
    Err error(std::string_view msg, size_t pos) const {
        std::string_view before = s_.substr(0, pos);
        size_t nl = before.rfind('\n');
        size_t line = 1 + static_cast<size_t>(std::count(before.begin(), before.end(), '\n'));
        size_t col = py::len(nl == std::string_view::npos ? before : before.substr(nl + 1)) + 1;
        return Err::value_error(std::string(msg) + ": line " + dec(line) + " column " + dec(col) + " (char " +
                                dec(py::len(before)) + ")");
    }

    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }

    bool at(std::string_view word) const { return s_.substr(i_, word.size()) == word; }

    Result<Json> value() {
        if (i_ >= s_.size()) return error("Expecting value", i_);
        char c = s_[i_];
        switch (c) {
            case '"': {
                SCFG_TRY(std::string str, string());
                return Json(std::move(str));
            }
            case '{':
                return object();
            case '[':
                return array();
            default:
                break;
        }
        // json's default parse_constant: exactly NaN, Infinity and -Infinity (not nan, +Infinity, -NaN).
        struct Word {
            std::string_view text;
            Json value;
        };
        double inf = std::numeric_limits<double>::infinity();
        const Word words[] = {{"null", Json()},
                              {"true", Json(true)},
                              {"false", Json(false)},
                              {"NaN", Json(std::numeric_limits<double>::quiet_NaN())},
                              {"Infinity", Json(inf)},
                              {"-Infinity", Json(-inf)}};
        for (const Word& w : words) {
            if (at(w.text)) {
                i_ += w.text.size();
                return w.value;
            }
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        return error("Expecting value", i_);
    }

    Status enter(size_t pos) {
        if (++depth_ > json::MAX_DEPTH) return error("JSON nested deeper than " + dec(json::MAX_DEPTH) + " levels", pos);
        return ok();
    }

    Result<Json> object() {
        SCFG_TRY_VOID(enter(i_));
        ++i_;  // {
        Json::Dict d;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            --depth_;
            return Json(std::move(d));
        }
        while (true) {
            if (i_ >= s_.size() || s_[i_] != '"') return error("Expecting property name enclosed in double quotes", i_);
            SCFG_TRY(std::string key, string());
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return error("Expecting ':' delimiter", i_);
            ++i_;
            skip_ws();
            SCFG_TRY(Json v, value());
            d.set(std::move(key), std::move(v));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                --depth_;
                return Json(std::move(d));
            }
            if (i_ >= s_.size() || s_[i_] != ',') return error("Expecting ',' delimiter", i_);
            ++i_;
            skip_ws();
        }
    }

    Result<Json> array() {
        SCFG_TRY_VOID(enter(i_));
        ++i_;  // [
        Json::List l;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            --depth_;
            return Json(std::move(l));
        }
        while (true) {
            SCFG_TRY(Json v, value());
            l.push_back(std::move(v));
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                --depth_;
                return Json(std::move(l));
            }
            if (i_ >= s_.size() || s_[i_] != ',') return error("Expecting ',' delimiter", i_);
            ++i_;
            skip_ws();
        }
    }

    // CPython's scanstring (strict): the text up to the closing quote, escapes decoded; an escaped surrogate pair is
    // one code point and a lone surrogate stays (as WTF-8).
    Result<std::string> string() {
        size_t open = i_++;
        std::string out;
        size_t run = i_;
        while (true) {
            if (i_ >= s_.size()) return error("Unterminated string starting at", open);
            auto c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                out.append(s_.substr(run, i_ - run));
                ++i_;
                return out;
            }
            if (c < 0x20) return error("Invalid control character at", i_);
            if (c != '\\') {
                ++i_;
                continue;
            }
            out.append(s_.substr(run, i_ - run));
            size_t esc = i_;
            if (esc + 1 >= s_.size()) return error("Unterminated string starting at", open);
            char e = s_[esc + 1];
            i_ = esc + 2;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    // CPython wants a character after the four digits (the closing quote at least), so text that ends
                    // right after them reports the escape; errors point at the 'u'.
                    size_t p = esc + 2;
                    if (p + 4 >= s_.size()) return error("Invalid \\uXXXX escape", esc + 1);
                    int32_t u = hex4(s_, p);
                    if (u < 0) return error("Invalid \\uXXXX escape", esc + 1);
                    i_ = p + 4;
                    if (u >= 0xD800 && u <= 0xDBFF && p + 10 < s_.size() && s_[p + 4] == '\\' && s_[p + 5] == 'u') {
                        int32_t low = hex4(s_, p + 6);
                        if (low < 0) return error("Invalid \\uXXXX escape", p + 5);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
                            i_ = p + 10;
                        }
                    }
                    py::utf8_encode(static_cast<uint32_t>(u), out);
                    break;
                }
                default:
                    return error("Invalid \\escape", esc);
            }
            run = i_;
        }
    }

    // CPython's _json number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)? in ASCII digits. A '.' or an exponent
    // without digits after it is not part of the number (what follows is then an error).
    Result<Json> number() {
        size_t start = i_;
        if (s_[i_] == '-') ++i_;
        if (i_ < s_.size() && s_[i_] == '0') {
            ++i_;
        } else if (digit_at(s_, i_)) {
            while (digit_at(s_, i_)) ++i_;
        } else {
            return error("Expecting value", start);
        }
        bool is_float = false;
        if (i_ < s_.size() && s_[i_] == '.' && digit_at(s_, i_ + 1)) {
            ++i_;
            while (digit_at(s_, i_)) ++i_;
            is_float = true;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            size_t k = i_ + 1;
            if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) ++k;
            if (digit_at(s_, k)) {
                while (digit_at(s_, k)) ++k;
                i_ = k;
                is_float = true;
            }
        }
        std::string_view literal = s_.substr(start, i_ - start);
        if (is_float) {
            std::optional<double> f = pyfloat::parse(literal);
            if (!f) return error("Expecting value", start);  // unreachable: the grammar above is float()'s
            return Json(*f);
        }
        bool neg = literal[0] == '-';
        size_t digits = literal.size() - (neg ? 1 : 0);
        if (digits <= 18) {
            int64_t v = 0;
            for (char c : literal.substr(neg ? 1 : 0)) v = v * 10 + (c - '0');
            return Json(neg ? -v : v);
        }
        // ints.int_from: more than INT_MAX_STR_DIGITS digits is refused before any conversion.
        if (digits > ints::INT_MAX_STR_DIGITS) return ints::detail::too_many_digits(digits);
        SCFG_TRY(BigInt big, BigInt::from_decimal(literal));
        return Json(std::move(big));
    }
};

// ---- dumps ----

class Writer {
public:
    explicit Writer(const json::DumpOptions& o) : o_(o), indent_(o.indent ? std::max(0, *o.indent) : 0) {}

    std::string out;

    void write(const Json& v, size_t level) {
        switch (v.type()) {
            case Json::Type::Null:
                out += "null";
                return;
            case Json::Type::Bool:
                out += v.as_bool() ? "true" : "false";
                return;
            case Json::Type::Int:
                if (v.is_int64()) {
                    char buf[24];
                    auto res = std::to_chars(buf, buf + sizeof buf, v.as_int64());
                    out.append(buf, res.ptr);
                } else {
                    out += v.as_bigint().to_decimal();
                }
                return;
            case Json::Type::Float: {
                double d = v.as_float();
                if (std::isnan(d)) {
                    out += "NaN";
                } else if (std::isinf(d)) {
                    out += d > 0 ? "Infinity" : "-Infinity";
                } else {
                    out += pyfloat::repr(d);
                }
                return;
            }
            case Json::Type::Str:
                write_string(v.as_str());
                return;
            case Json::Type::List:
                write_list(v.as_list(), level);
                return;
            case Json::Type::Dict:
                write_dict(v.as_dict(), level);
                return;
        }
    }

private:
    const json::DumpOptions& o_;
    int indent_;
    Path path_;

    void newline(size_t level) {
        out += '\n';
        out.append(level * static_cast<size_t>(indent_), ' ');
    }

    void separator(bool first, size_t level) {
        if (!first) out += o_.indent ? "," : ", ";
        if (o_.indent) newline(level);
    }

    void write_list(const Json::List& l, size_t level) {
        if (l.empty()) {
            out += "[]";
            return;
        }
        path_.enter(&l, "json::dumps");
        out += '[';
        for (size_t k = 0; k < l.size(); ++k) {
            separator(k == 0, level + 1);
            write(l[k], level + 1);
        }
        if (o_.indent) newline(level);
        out += ']';
        path_.leave();
    }

    void write_dict(const Json::Dict& d, size_t level) {
        if (d.empty()) {
            out += "{}";
            return;
        }
        path_.enter(&d, "json::dumps");
        std::vector<const Json::Dict::Entry*> items;
        items.reserve(d.size());
        for (const Json::Dict::Entry& e : d) items.push_back(&e);
        if (o_.sort_keys) {
            std::stable_sort(items.begin(), items.end(), [](const Json::Dict::Entry* a, const Json::Dict::Entry* b) {
                return py::compare_code_points(a->first, b->first) < 0;
            });
        }
        out += '{';
        for (size_t k = 0; k < items.size(); ++k) {
            separator(k == 0, level + 1);
            write_string(items[k]->first);
            out += ": ";
            write(items[k]->second, level + 1);
        }
        if (o_.indent) newline(level);
        out += '}';
        path_.leave();
    }

    void hex_escape(uint32_t unit) {
        static constexpr char kHex[] = "0123456789abcdef";
        out += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4) out += kHex[(unit >> shift) & 15];
    }

    // ensure_ascii: everything outside ' '..'~' as \uXXXX (astral characters as surrogate pairs, a lone surrogate as
    // itself), as Python writes them. Otherwise only the quote, the backslash and the controls are escaped.
    void write_string(std::string_view s) {
        out += '"';
        for (size_t i = 0; i < s.size();) {
            size_t start = i;
            uint32_t cp;
            if (o_.ensure_ascii) {
                cp = py::utf8_decode(s, i);
            } else {
                cp = static_cast<unsigned char>(s[i++]);
            }
            switch (cp) {
                case '"': out += "\\\""; continue;
                case '\\': out += "\\\\"; continue;
                case '\n': out += "\\n"; continue;
                case '\r': out += "\\r"; continue;
                case '\t': out += "\\t"; continue;
                case '\b': out += "\\b"; continue;
                case '\f': out += "\\f"; continue;
                default: break;
            }
            if (cp < 0x20) {
                hex_escape(cp);
            } else if (!o_.ensure_ascii || cp < 0x7F) {
                out.append(s.substr(start, i - start));
            } else if (cp >= 0x10000) {
                uint32_t v = cp - 0x10000;
                hex_escape(0xD800 + (v >> 10));
                hex_escape(0xDC00 + (v & 0x3FF));
            } else {
                hex_escape(cp);
            }
        }
        out += '"';
    }
};

}  // namespace scfg::json_detail

namespace scfg {

// ---- Json ----

Json::Json(const Json& other) = default;
Json::Json(Json&& other) noexcept = default;
Json& Json::operator=(const Json& other) = default;
Json& Json::operator=(Json&& other) noexcept = default;
Json::~Json() = default;

Json::Json(BigInt i) {
    if (i.fits_int64()) {
        v_ = i.to_int64().value();
    } else {
        v_ = std::move(i);
    }
}

Json::Json(List l) : v_(std::make_shared<List>(std::move(l))) {}
Json::Json(Dict d) : v_(std::make_shared<Dict>(std::move(d))) {}

Json Json::list(std::initializer_list<Json> items) { return Json(List(items)); }

Json Json::dict(std::initializer_list<std::pair<std::string, Json>> items) { return Json(Dict(items)); }

Json::Type Json::type() const {
    switch (v_.index()) {
        case 0: return Type::Null;
        case 1: return Type::Bool;
        case 2:
        case 3: return Type::Int;
        case 4: return Type::Float;
        case 5: return Type::Str;
        case 6: return Type::List;
        default: return Type::Dict;
    }
}

std::string_view Json::type_name() const {
    switch (type()) {
        case Type::Null: return "NoneType";
        case Type::Bool: return "bool";
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::Str: return "str";
        case Type::List: return "list";
        case Type::Dict: return "dict";
    }
    return "?";
}

bool Json::as_bool() const {
    if (const bool* b = std::get_if<bool>(&v_)) return *b;
    json_detail::wrong_type("as_bool", *this);
}

int64_t Json::as_int64() const {
    if (const int64_t* i = std::get_if<int64_t>(&v_)) return *i;
    json_detail::wrong_type("as_int64", *this);
}

BigInt Json::as_bigint() const {
    if (const int64_t* i = std::get_if<int64_t>(&v_)) return BigInt(*i);
    if (const BigInt* b = std::get_if<BigInt>(&v_)) return *b;
    json_detail::wrong_type("as_bigint", *this);
}

double Json::as_float() const {
    if (const double* d = std::get_if<double>(&v_)) return *d;
    json_detail::wrong_type("as_float", *this);
}

const std::string& Json::as_str() const {
    if (const std::string* s = std::get_if<std::string>(&v_)) return *s;
    json_detail::wrong_type("as_str", *this);
}

Json::List& Json::as_list() {
    if (auto* l = std::get_if<std::shared_ptr<List>>(&v_)) return **l;
    json_detail::wrong_type("as_list", *this);
}

const Json::List& Json::as_list() const { return const_cast<Json*>(this)->as_list(); }

Json::Dict& Json::as_dict() {
    if (auto* d = std::get_if<std::shared_ptr<Dict>>(&v_)) return **d;
    json_detail::wrong_type("as_dict", *this);
}

const Json::Dict& Json::as_dict() const { return const_cast<Json*>(this)->as_dict(); }

bool Json::same_object(const Json& other) const {
    if (is_list() && other.is_list()) return &as_list() == &other.as_list();
    if (is_dict() && other.is_dict()) return &as_dict() == &other.as_dict();
    return false;
}

// ---- Json::Dict ----

Json::Dict::Dict(std::initializer_list<Entry> items) {
    for (const Entry& e : items) set(e.first, e.second);
}

const Json* Json::Dict::find(std::string_view key) const {
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : &entries_[it->second].second;
}

Json* Json::Dict::find(std::string_view key) {
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : &entries_[it->second].second;
}

Json Json::Dict::get(std::string_view key, Json fallback) const {
    const Json* v = find(key);
    return v != nullptr ? *v : fallback;
}

void Json::Dict::set(std::string key, Json value) {
    auto it = index_.find(std::string_view(key));
    if (it != index_.end()) {
        entries_[it->second].second = std::move(value);
        return;
    }
    index_.emplace(key, entries_.size());
    entries_.emplace_back(std::move(key), std::move(value));
}

bool Json::Dict::erase(std::string_view key) {
    auto it = index_.find(key);
    if (it == index_.end()) return false;
    size_t pos = it->second;
    index_.erase(it);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(pos));
    for (size_t j = pos; j < entries_.size(); ++j) index_.find(std::string_view(entries_[j].first))->second = j;
    return true;
}

std::vector<std::string> Json::Dict::keys() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.first);
    return out;
}

Json deep_copy(const Json& v) {
    json_detail::Path path;
    return json_detail::copy(v, path);
}

namespace json {

Result<Json> loads(std::string_view text) { return json_detail::Reader(text).document(); }

std::string dumps(const Json& v, const DumpOptions& opts) {
    json_detail::Writer w(opts);
    w.write(v, 0);
    return std::move(w.out);
}

}  // namespace json
}  // namespace scfg
