// No Python twin: the CPython 3.9 built-ins the core relies on (see Py.h), ported from unity/.../Py.cs and
// rpgmaker/src/core/pystr.ts, whose tables are copied here.
#include "Scfg/Py.h"

#include "Scfg/Ints.h"
#include "Scfg/PyFloat.h"

#include <array>
#include <charconv>
#include <cmath>
#include <compare>
#include <optional>
#include <string>
#include <vector>

namespace scfg::py::detail {

// The zero of every run of ten Unicode decimal digits (Nd) in CPython 3.9's unicodedata (Unicode 13.0.0, the
// project's interpreter), copied from pystr.ts ND_ZEROS / Py.cs NdZeros: 65 runs, 650 digits. Not a newer table: a
// newer one has more digit sets (Tangsa U+16AC0, Kawi, Nag Mundari, ...) that Python 3.9 refuses.
constexpr std::array<uint32_t, 65> kNdZeros = {
    0x30,    0x660,   0x6f0,   0x7c0,   0x966,   0x9e6,   0xa66,   0xae6,   0xb66,   0xbe6,   0xc66,   0xce6,
    0xd66,   0xde6,   0xe50,   0xed0,   0xf20,   0x1040,  0x1090,  0x17e0,  0x1810,  0x1946,  0x19d0,  0x1a80,
    0x1a90,  0x1b50,  0x1bb0,  0x1c40,  0x1c50,  0xa620,  0xa8d0,  0xa900,  0xa9d0,  0xa9f0,  0xaa50,  0xabf0,
    0xff10,  0x104a0, 0x10d30, 0x11066, 0x110f0, 0x11136, 0x111d0, 0x112f0, 0x11450, 0x114d0, 0x11650, 0x116c0,
    0x11730, 0x118e0, 0x11950, 0x11c50, 0x11d50, 0x11da0, 0x16a60, 0x16b50, 0x1d7ce, 0x1d7d8, 0x1d7e2, 0x1d7ec,
    0x1d7f6, 0x1e140, 0x1e2f0, 0x1e950, 0x1fbf0,
};

constexpr char kLowerHex[] = "0123456789abcdef";
constexpr char kUpperHex[] = "0123456789ABCDEF";

std::string dec(uint64_t v) {
    char buf[24];
    auto res = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, res.ptr);
}

std::string dec_signed(int64_t v) {
    char buf[24];
    auto res = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, res.ptr);
}

uint8_t byte_at(std::string_view s, size_t i) { return static_cast<uint8_t>(s[i]); }

// The length of the well-formed UTF-8 sequence (surrogates allowed) at s[i], or 0 when there is none.
size_t sequence_length(std::string_view s, size_t i) {
    uint8_t b0 = byte_at(s, i);
    if (b0 < 0x80) return 1;
    size_t n;
    uint8_t lo = 0x80, hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        n = 2;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        n = 3;
        if (b0 == 0xE0) lo = 0xA0;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        n = 4;
        if (b0 == 0xF0) lo = 0x90;
        if (b0 == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        uint8_t b = byte_at(s, i + k);
        if (b < (k == 1 ? lo : 0x80) || b > (k == 1 ? hi : 0xBF)) return 0;
    }
    return n;
}

// The start of the code point that ends at s[end - 1] (for scanning backwards), when a well-formed one does.
size_t start_before(std::string_view s, size_t end) {
    size_t j = end - 1;
    while (j > 0 && end - j < 4 && (byte_at(s, j) & 0xC0) == 0x80) --j;
    size_t n = sequence_length(s, j);
    return n != 0 && j + n == end ? j : end - 1;
}

void append_escape(std::string& out, uint32_t cp) {
    int digits;
    if (cp <= 0xff) {
        out += "\\x";
        digits = 2;
    } else if (cp <= 0xffff) {
        out += "\\u";
        digits = 4;
    } else {
        out += "\\U";
        digits = 8;
    }
    for (int k = digits - 1; k >= 0; --k) out += kLowerHex[(cp >> (4 * k)) & 15];
}

// CPython's Py_UNICODE_ISPRINTABLE for the non-ASCII characters that matter in practice: Cc, Cf, Zs, Zl, Zp, Cs and
// Co (not Cn, which would need the whole Unicode 13 table). Copied from Py.cs.
bool is_printable(uint32_t cp) {
    if (cp >= 0x80 && cp <= 0xa0) return false;
    if (cp == 0xad || cp == 0x61c || cp == 0x6dd || cp == 0x70f || cp == 0x8e2 || cp == 0x180e) return false;
    if (cp >= 0x600 && cp <= 0x605) return false;
    if (cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200f) || (cp >= 0x2028 && cp <= 0x202f)) return false;
    if ((cp >= 0x205f && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206f) || cp == 0x3000) return false;
    if (cp >= 0xd800 && cp <= 0xf8ff) return false;  // lone surrogates, private use
    if (cp == 0xfeff || (cp >= 0xfff9 && cp <= 0xfffb) || cp == 0xfffe || cp == 0xffff) return false;
    if (cp == 0x110bd || cp == 0x110cd || (cp >= 0x13430 && cp <= 0x13438)) return false;
    if ((cp >= 0x1bca0 && cp <= 0x1bca3) || (cp >= 0x1d173 && cp <= 0x1d17a)) return false;
    if (cp == 0xe0001 || (cp >= 0xe0020 && cp <= 0xe007f) || cp >= 0xf0000) return false;
    return true;
}

std::string repr_impl(std::string_view s, bool ascii_only) {
    bool has_single = s.find('\'') != std::string_view::npos;
    bool has_double = s.find('"') != std::string_view::npos;
    char q = has_single && !has_double ? '"' : '\'';
    std::string out;
    out.reserve(s.size() + 2);
    out += q;
    for (size_t i = 0; i < s.size();) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        if (cp == static_cast<uint32_t>(q) || cp == '\\') {
            out += '\\';
            out += static_cast<char>(cp);
        } else if (cp == '\t') {
            out += "\\t";
        } else if (cp == '\n') {
            out += "\\n";
        } else if (cp == '\r') {
            out += "\\r";
        } else if (cp < 0x20 || cp == 0x7f) {
            append_escape(out, cp);
        } else if (cp < 0x7f) {
            out += static_cast<char>(cp);
        } else if (ascii_only || !is_printable(cp)) {
            append_escape(out, cp);
        } else {
            out.append(s.substr(start, i - start));
        }
    }
    out += q;
    return out;
}

// The first n code points of s.
std::string_view first_code_points(std::string_view s, size_t n) {
    size_t i = 0;
    for (size_t k = 0; k < n && i < s.size(); ++k) utf8_decode(s, i);
    return s.substr(0, i);
}

// CPython: "invalid literal for int() with base %d: %.200R".
Err invalid_literal(std::string_view s, int base = 10) {
    std::string r = repr_str(s);
    return Err::value_error("invalid literal for int() with base " + dec(static_cast<uint64_t>(base)) + ": " +
                            std::string(first_code_points(r, 200)));
}

bool ascii_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// The int of an integral double (Python's int() of a finite float after truncation).
BigInt from_integral_double(double d) {
    if (std::fabs(d) < 9223372036854775808.0) return BigInt(static_cast<int64_t>(d));
    int e = 0;
    double f = std::frexp(std::fabs(d), &e);  // |d| = f * 2**e, 0.5 <= f < 1, e > 63
    auto m = static_cast<uint64_t>(std::ldexp(f, 53));
    BigInt v = BigInt::from_uint64(m) << static_cast<size_t>(e - 53);
    return d < 0 ? -v : v;
}

// Python's == for two numbers (bool, int or float).
bool number_eq(const Json& a, const Json& b) {
    if (a.is_float() && b.is_float()) return a.as_float() == b.as_float();
    auto as_int = [](const Json& v) { return v.is_bool() ? BigInt(v.as_bool() ? 1 : 0) : v.as_bigint(); };
    if (!a.is_float() && !b.is_float()) {
        if (a.is_int64() && b.is_int64()) return a.as_int64() == b.as_int64();
        return as_int(a) == as_int(b);
    }
    const Json& i = a.is_float() ? b : a;
    double f = (a.is_float() ? a : b).as_float();
    if (!std::isfinite(f) || std::trunc(f) != f) return false;
    return from_integral_double(f) == as_int(i);
}

bool is_number(const Json& v) { return v.is_bool() || v.is_int() || v.is_float(); }

Status repr_into(const Json& v, std::string& out, std::vector<const void*>& path) {
    switch (v.type()) {
        case Json::Type::Null:
            out += "None";
            return ok();
        case Json::Type::Bool:
            out += v.as_bool() ? "True" : "False";
            return ok();
        case Json::Type::Int:
            if (v.is_int64()) {
                out += dec_signed(v.as_int64());  // at most 20 characters: no limit applies
            } else {
                SCFG_TRY(std::string digits, int_str(v.as_bigint()));
                out += digits;
            }
            return ok();
        case Json::Type::Float:
            out += pyfloat::repr(v.as_float());
            return ok();
        case Json::Type::Str:
            out += repr_str(v.as_str());
            return ok();
        case Json::Type::List: {
            const Json::List& l = v.as_list();
            for (const void* p : path) {
                if (p == &l) {
                    out += "[...]";
                    return ok();
                }
            }
            path.push_back(&l);
            out += '[';
            for (size_t k = 0; k < l.size(); ++k) {
                if (k > 0) out += ", ";
                SCFG_TRY_VOID(repr_into(l[k], out, path));
            }
            out += ']';
            path.pop_back();
            return ok();
        }
        case Json::Type::Dict: {
            const Json::Dict& d = v.as_dict();
            for (const void* p : path) {
                if (p == &d) {
                    out += "{...}";
                    return ok();
                }
            }
            path.push_back(&d);
            out += '{';
            bool first = true;
            for (const auto& [key, value] : d) {
                if (!first) out += ", ";
                first = false;
                out += repr_str(key);
                out += ": ";
                SCFG_TRY_VOID(repr_into(value, out, path));
            }
            out += '}';
            path.pop_back();
            return ok();
        }
    }
    return ok();
}

bool ascii_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

Status check_utf8(std::string_view s, bool surrogates) {
    for (size_t i = 0; i < s.size();) {
        size_t n = sequence_length(s, i);
        // sequence_length allows a surrogate (ED A0..BF xx); the strict decoder refuses it at its second byte.
        if (n == 3 && !surrogates && byte_at(s, i) == 0xED && byte_at(s, i + 1) >= 0xA0) n = 0;
        if (n != 0) {
            i += n;
            continue;
        }
        // CPython's UTF-8 decoder: the error covers the start byte and the valid prefix after it.
        uint8_t b0 = byte_at(s, i);
        const char* reason = "invalid start byte";
        size_t k = 1;
        if ((b0 >= 0xC2 && b0 <= 0xF4)) {
            size_t need = b0 <= 0xDF ? 2 : b0 <= 0xEF ? 3 : 4;
            uint8_t lo = b0 == 0xE0 ? 0xA0 : b0 == 0xF0 ? 0x90 : 0x80;
            uint8_t hi = b0 == 0xED ? 0x9F : b0 == 0xF4 ? 0x8F : 0xBF;  // strict: ED A0..BF is a surrogate
            reason = "invalid continuation byte";
            for (; k < need; ++k) {
                if (i + k >= s.size()) {
                    reason = "unexpected end of data";
                    break;
                }
                uint8_t b = byte_at(s, i + k);
                if (b < (k == 1 ? lo : 0x80) || b > (k == 1 ? hi : 0xBF)) break;
            }
        }
        std::string where = k == 1 ? "byte 0x" + std::string{kLowerHex[b0 >> 4], kLowerHex[b0 & 15]} +
                                         " in position " + dec(i)
                                   : "bytes in position " + dec(i) + "-" + dec(i + k - 1);
        return Err::value_error("'utf-8' codec can't decode " + where + ": " + reason);
    }
    return ok();
}

// CPython 3.9's _PyUnicode_TransformDecimalAndSpaceToASCII, what int() parses: ASCII kept, non-ASCII whitespace
// becomes ' ', a decimal digit of any script its ASCII digit, and the first other non-ASCII character '?', where
// the text ends.
std::string int_ascii(std::string_view s) {
    std::string a;
    a.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = utf8_decode(s, i);
        if (cp < 127) {
            a += static_cast<char>(cp);
        } else if (is_space(cp)) {
            a += ' ';
        } else if (int d = decimal_value(cp); d >= 0) {
            a += static_cast<char>('0' + d);
        } else {
            a += '?';
            break;
        }
    }
    return a;
}

// The value of an ASCII hex digit, or -1.
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A well-formed int(s, 16) literal: its sign and its significant hex digits (ASCII, leading zeros dropped, so ""
// is zero).
struct HexLiteral {
    bool negative = false;
    std::string digits;
};

// CPython 3.9's PyLong_FromString for base 16 on int_ascii(s): ASCII whitespace, an optional sign, an optional
// 0x/0X prefix with one optional underscore after it, then hex digits with single underscores between them (no
// leading or trailing one), ASCII whitespace, the end. Anything else is the ValueError int() raises. One pass.
Result<HexLiteral> hex_literal(std::string_view s) {
    std::string a = int_ascii(s);
    size_t i = 0;
    while (i < a.size() && ascii_space(a[i])) ++i;
    HexLiteral lit;
    if (i < a.size() && (a[i] == '+' || a[i] == '-')) lit.negative = a[i++] == '-';
    if (i + 1 < a.size() && a[i] == '0' && (a[i + 1] == 'x' || a[i + 1] == 'X')) {
        i += 2;
        if (i < a.size() && a[i] == '_') ++i;  // "One underscore allowed here."
    }
    if (i < a.size() && a[i] == '_') return invalid_literal(s, 16);
    size_t first = i;
    char prev = 0;
    for (; i < a.size() && (hex_value(a[i]) >= 0 || a[i] == '_'); ++i) {
        if (a[i] == '_') {
            if (prev == '_') return invalid_literal(s, 16);
        } else if (!lit.digits.empty() || a[i] != '0') {
            lit.digits += a[i];
        }
        prev = a[i];
    }
    if (prev == '_' || i == first) return invalid_literal(s, 16);
    while (i < a.size() && ascii_space(a[i])) ++i;
    if (i != a.size()) return invalid_literal(s, 16);
    return lit;
}

Err int_base16_type_error() { return Err::type_error("int() can't convert non-string with explicit base"); }

}  // namespace scfg::py::detail

namespace scfg::py {

// ---- UTF-8 ----

uint32_t utf8_decode(std::string_view s, size_t& i) {
    size_t n = detail::sequence_length(s, i);
    uint8_t b0 = detail::byte_at(s, i);
    if (n == 0) {
        ++i;
        return REPLACEMENT;
    }
    uint32_t cp;
    if (n == 1) {
        cp = b0;
    } else if (n == 2) {
        cp = (b0 & 0x1Fu) << 6 | (detail::byte_at(s, i + 1) & 0x3Fu);
    } else if (n == 3) {
        cp = (b0 & 0x0Fu) << 12 | (detail::byte_at(s, i + 1) & 0x3Fu) << 6 | (detail::byte_at(s, i + 2) & 0x3Fu);
    } else {
        cp = (b0 & 0x07u) << 18 | (detail::byte_at(s, i + 1) & 0x3Fu) << 12 | (detail::byte_at(s, i + 2) & 0x3Fu) << 6 |
             (detail::byte_at(s, i + 3) & 0x3Fu);
    }
    i += n;
    return cp;
}

void utf8_encode(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string utf8_encode(uint32_t cp) {
    std::string out;
    utf8_encode(cp, out);
    return out;
}

std::vector<uint32_t> code_points(std::string_view s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) out.push_back(utf8_decode(s, i));
    return out;
}

Status utf8_check(std::string_view s) { return detail::check_utf8(s, false); }

Status wtf8_check(std::string_view s) { return detail::check_utf8(s, true); }

Result<std::vector<uint8_t>> utf8_bytes(std::string_view s) {
    std::vector<uint8_t> out;
    out.reserve(s.size());
    size_t pos = 0;
    for (size_t i = 0; i < s.size(); ++pos) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return Err::value_error("'utf-8' codec can't encode character " + repr_str(s.substr(start, i - start)) +
                                    " in position " + detail::dec(pos) + ": surrogates not allowed");
        }
        for (size_t k = start; k < i; ++k) out.push_back(detail::byte_at(s, k));
    }
    return out;
}

size_t len(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++n) utf8_decode(s, i);
    return n;
}

int compare_code_points(std::string_view a, std::string_view b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        uint8_t x = detail::byte_at(a, i), y = detail::byte_at(b, i);
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
}

// ---- character classes ----

bool is_space(uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0d) || (cp >= 0x1c && cp <= 0x20) || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f ||
           cp == 0x3000;
}

int decimal_value(uint32_t cp) {
    if (cp >= 0x30 && cp <= 0x39) return static_cast<int>(cp - 0x30);
    if (cp < 0x660) return -1;
    for (uint32_t z : detail::kNdZeros) {
        if (cp >= z && cp < z + 10) return static_cast<int>(cp - z);
    }
    return -1;
}

bool is_decimal_digit(uint32_t cp) { return decimal_value(cp) >= 0; }

// ---- strings ----

std::string strip(std::string_view s) {
    size_t start = 0, end = s.size();
    while (start < end) {
        size_t i = start;
        if (!is_space(utf8_decode(s, i))) break;
        start = i;
    }
    while (end > start) {
        size_t j = detail::start_before(s, end);
        size_t i = j;
        if (j < start || !is_space(utf8_decode(s, i)) || i != end) break;
        end = j;
    }
    return std::string(s.substr(start, end - start));
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string lower_for_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        if (cp >= 'A' && cp <= 'Z') {
            out += static_cast<char>(cp - 'A' + 'a');
        } else if (cp == 0x212A) {
            out += 'k';
        } else if (cp == 0x130) {
            out += "i\xCC\x87";
        } else {
            out.append(s.substr(start, i - start));
        }
    }
    return out;
}

std::string repr_str(std::string_view s) { return detail::repr_impl(s, false); }

std::string ascii_str(std::string_view s) { return detail::repr_impl(s, true); }

Result<std::string> repr(const Json& v) {
    std::string out;
    std::vector<const void*> path;
    SCFG_TRY_VOID(detail::repr_into(v, out, path));
    return out;
}

Result<std::string> format1(std::string_view tmpl, std::string_view arg) {
    std::string out;
    int numbering = 0;  // 0: undecided, 1: automatic, 2: manual
    uint64_t next_index = 0;
    size_t i = 0;
    while (i < tmpl.size()) {
        char ch = tmpl[i];
        bool doubled = i + 1 < tmpl.size() && tmpl[i + 1] == ch;
        if (ch == '}') {
            if (!doubled) return Err::value_error("Single '}' encountered in format string");
            out += '}';
            i += 2;
            continue;
        }
        if (ch != '{') {
            out += ch;
            ++i;
            continue;
        }
        if (doubled) {
            out += '{';
            i += 2;
            continue;
        }
        if (i + 1 == tmpl.size()) return Err::value_error("Single '{' encountered in format string");
        size_t close = tmpl.find('}', i + 1);
        if (close == std::string_view::npos) return Err::value_error("expected '}' before end of string");
        std::string_view field = tmpl.substr(i + 1, close - i - 1);
        if (field.find('{') != std::string_view::npos) return Err::value_error("unsupported nested replacement field");
        size_t n = 0;
        while (n < field.size() && field[n] != '.' && field[n] != '[' && field[n] != '!' && field[n] != ':') ++n;
        if (n < field.size() && (field[n] == '.' || field[n] == '[')) return Err::value_error("unsupported replacement field");
        std::string_view name = field.substr(0, n);
        std::string_view conversion;
        bool has_conversion = false;
        if (n < field.size() && field[n] == '!') {
            size_t colon = n + 1;
            while (colon < field.size() && field[colon] != ':') ++colon;
            conversion = field.substr(n + 1, colon - n - 1);
            has_conversion = true;
            n = colon;
        }
        if (n < field.size() && n + 1 < field.size()) return Err::value_error("unsupported replacement field");  // a spec
        uint64_t index = 0;
        if (name.empty()) {
            if (numbering == 2)
                return Err::value_error("cannot switch from manual field specification to automatic field numbering");
            numbering = 1;
            index = next_index++;
        } else if (detail::ascii_digits(name)) {
            if (numbering == 1)
                return Err::value_error("cannot switch from automatic field numbering to manual field specification");
            numbering = 2;
            for (char d : name) {
                if (index > 100000000) return Err::value_error("Too many decimal digits in format string");
                index = index * 10 + static_cast<uint64_t>(d - '0');
            }
        } else {
            return key_error(name);
        }
        if (index != 0)
            return Err::index_error("Replacement index " + detail::dec(index) + " out of range for positional args tuple");
        if (has_conversion && conversion != "r" && conversion != "s" && conversion != "a")
            return Err::value_error("Unknown conversion specifier " + std::string(conversion));
        if (has_conversion && conversion == "r") {
            out += repr_str(arg);
        } else if (has_conversion && conversion == "a") {
            out += ascii_str(arg);
        } else {
            out.append(arg);
        }
        i = close + 1;
    }
    return out;
}

Result<std::string> quote(std::string_view s, std::string_view safe) {
    std::array<bool, 128> keep{};
    for (char c = 'A'; c <= 'Z'; ++c) keep[static_cast<size_t>(c)] = true;
    for (char c = 'a'; c <= 'z'; ++c) keep[static_cast<size_t>(c)] = true;
    for (char c = '0'; c <= '9'; ++c) keep[static_cast<size_t>(c)] = true;
    for (char c : {'_', '.', '-', '~'}) keep[static_cast<size_t>(c)] = true;
    for (char c : safe) {
        auto b = static_cast<uint8_t>(c);
        if (b < 128) keep[b] = true;
    }
    SCFG_TRY(std::vector<uint8_t> bytes, utf8_bytes(s));
    std::string out;
    out.reserve(bytes.size());
    for (uint8_t b : bytes) {
        if (b < 128 && keep[b]) {
            out += static_cast<char>(b);
        } else {
            out += '%';
            out += detail::kUpperHex[b >> 4];
            out += detail::kUpperHex[b & 15];
        }
    }
    return out;
}

// ---- hex(), str(), iteration ----

std::string hex(const BigInt& n) { return n.sign() < 0 ? "-0x" + n.abs().to_hex() : "0x" + n.to_hex(); }

Result<std::string> str(const Json& v) {
    if (v.is_str()) return v.as_str();
    return repr(v);
}

Result<std::vector<Json>> iterate(const Json& v) {
    if (v.is_list()) return v.as_list();
    if (v.is_dict()) {
        std::vector<Json> out;
        for (const auto& [key, value] : v.as_dict()) out.emplace_back(key);
        return out;
    }
    if (v.is_str()) {
        std::vector<Json> out;
        std::string_view s = v.as_str();
        for (size_t i = 0; i < s.size();) {
            size_t start = i;
            utf8_decode(s, i);
            out.emplace_back(s.substr(start, i - start));
        }
        return out;
    }
    return Err::type_error("'" + std::string(v.type_name()) + "' object is not iterable");
}

// ---- str(int) ----

Result<std::string> int_str(const BigInt& n) {
    // A number of b bits is at least 2**(b-1); 2**14285 > 10**4300, so 14286 bits or more always exceed the limit.
    auto too_long = [] {
        return Err::value_error("Exceeds the limit (4300) for integer string conversion; use "
                                "sys.set_int_max_str_digits() to increase the limit");
    };
    if (n.bit_length() >= 14286) return too_long();
    std::string text = n.to_decimal();  // at most 14285 bits: 4301 digits
    if (text.size() - (n.sign() < 0 ? 1 : 0) > ints::INT_MAX_STR_DIGITS) return too_long();
    return text;
}

// ---- int() ----

Result<BigInt> py_int(std::string_view s) {
    // CPython 3.9: _PyUnicode_TransformDecimalAndSpaceToASCII, then PyLong_FromString: ASCII whitespace, a sign,
    // the digit run (single underscores between digits; more than 4300 digits is refused right there), ASCII
    // whitespace, the end.
    std::string a = detail::int_ascii(s);
    size_t i = 0;
    while (i < a.size() && detail::ascii_space(a[i])) ++i;
    bool neg = false;
    if (i < a.size() && (a[i] == '+' || a[i] == '-')) neg = a[i++] == '-';
    if (i < a.size() && a[i] == '_') return detail::invalid_literal(s);
    std::string digits;
    char prev = 0;
    for (; i < a.size() && ((a[i] >= '0' && a[i] <= '9') || a[i] == '_'); ++i) {
        if (a[i] == '_') {
            if (prev == '_') return detail::invalid_literal(s);
        } else {
            digits += a[i];
        }
        prev = a[i];
    }
    if (prev == '_') return detail::invalid_literal(s);
    if (digits.size() > 4300) {
        return Err::value_error("Exceeds the limit (4300) for integer string conversion: value has " +
                                detail::dec(digits.size()) + " digits; use sys.set_int_max_str_digits() to increase the limit");
    }
    if (digits.empty()) return detail::invalid_literal(s);
    while (i < a.size() && detail::ascii_space(a[i])) ++i;
    if (i != a.size()) return detail::invalid_literal(s);
    if (digits.size() <= 18) {
        int64_t v = 0;
        for (char c : digits) v = v * 10 + (c - '0');
        return BigInt(neg ? -v : v);
    }
    SCFG_TRY(BigInt v, BigInt::from_decimal(digits));
    return neg ? -v : v;
}

Result<BigInt> py_int(const Json& x) {
    switch (x.type()) {
        case Json::Type::Bool:
            return BigInt(x.as_bool() ? 1 : 0);
        case Json::Type::Int:
            return x.as_bigint();
        case Json::Type::Float: {
            double d = x.as_float();
            if (std::isnan(d)) return Err::value_error("cannot convert float NaN to integer");
            if (std::isinf(d)) return Err::overflow_error("cannot convert float infinity to integer");
            return detail::from_integral_double(std::trunc(d));
        }
        case Json::Type::Str:
            return py_int(std::string_view(x.as_str()));
        default:
            return Err::type_error("int() argument must be a string, a bytes-like object or a number, not '" +
                                   std::string(x.type_name()) + "'");
    }
}

Result<BigInt> py_int_hex(std::string_view s) {
    SCFG_TRY(detail::HexLiteral lit, detail::hex_literal(s));
    if (lit.digits.empty()) return BigInt();
    SCFG_TRY(BigInt v, BigInt::from_hex(lit.digits));  // linear: hex digits are packed, not multiplied in
    return lit.negative ? -v : v;
}

Result<BigInt> py_int_hex(const Json& x) {
    if (!x.is_str()) return detail::int_base16_type_error();
    return py_int_hex(std::string_view(x.as_str()));
}

Result<std::optional<int64_t>> py_int_hex_int64(const Json& x) {
    if (!x.is_str()) return detail::int_base16_type_error();
    SCFG_TRY(detail::HexLiteral lit, detail::hex_literal(x.as_str()));
    if (lit.digits.size() > 16) return std::optional<int64_t>();  // at least 2**64: decided before converting
    uint64_t mag = 0;
    for (char c : lit.digits) mag = (mag << 4) | static_cast<uint64_t>(detail::hex_value(c));
    constexpr uint64_t kInt64Max = 0x7fffffffffffffff;
    if (!lit.negative) {
        if (mag > kInt64Max) return std::optional<int64_t>();
        return std::optional<int64_t>(static_cast<int64_t>(mag));
    }
    if (mag > kInt64Max + 1) return std::optional<int64_t>();
    // -mag in int64_t without overflow: -(mag - 1) - 1
    return std::optional<int64_t>(mag == 0 ? 0 : -static_cast<int64_t>(mag - 1) - 1);
}

// ---- values ----

bool truthy(const Json& v) {
    switch (v.type()) {
        case Json::Type::Null:
            return false;
        case Json::Type::Bool:
            return v.as_bool();
        case Json::Type::Int:
            return !v.is_int64() || v.as_int64() != 0;
        case Json::Type::Float:
            return v.as_float() != 0.0;  // NaN is truthy
        case Json::Type::Str:
            return !v.as_str().empty();
        case Json::Type::List:
            return !v.as_list().empty();
        case Json::Type::Dict:
            return !v.as_dict().empty();
    }
    return true;
}

bool eq(const Json& a, const Json& b) {
    if (detail::is_number(a) || detail::is_number(b)) return detail::is_number(a) && detail::is_number(b) && detail::number_eq(a, b);
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Json::Type::Null:
            return true;
        case Json::Type::Str:
            return a.as_str() == b.as_str();
        case Json::Type::List: {
            const Json::List& l = a.as_list();
            const Json::List& r = b.as_list();
            if (l.size() != r.size()) return false;
            for (size_t k = 0; k < l.size(); ++k) {
                if (!eq(l[k], r[k])) return false;
            }
            return true;
        }
        case Json::Type::Dict: {
            const Json::Dict& l = a.as_dict();
            const Json::Dict& r = b.as_dict();
            if (l.size() != r.size()) return false;
            for (const auto& [key, value] : l) {
                const Json* other = r.find(key);
                if (other == nullptr || !eq(value, *other)) return false;
            }
            return true;
        }
        default:
            return false;  // numbers are handled above
    }
}

Err key_error(std::string_view key) { return Err::key_error(repr_str(key)); }

Err key_error_for(const Json& key) {
    Result<std::string> shown = repr(key);
    return Err::key_error(shown.ok() ? shown.take() : std::string("<unprintable KeyError object>"));
}

Result<Json> get(const Json& d, std::string_view key) {
    switch (d.type()) {
        case Json::Type::Dict: {
            const Json* v = d.as_dict().find(key);
            if (v == nullptr) return key_error(key);
            return *v;
        }
        case Json::Type::List:
            return Err::type_error("list indices must be integers or slices, not str");
        case Json::Type::Str:
            return Err::type_error("string indices must be integers");
        default:
            return Err::type_error("'" + std::string(d.type_name()) + "' object is not subscriptable");
    }
}

Result<std::optional<std::string>> str_key(const Json& k) {
    if (k.is_list() || k.is_dict()) return Err::type_error("unhashable type: '" + std::string(k.type_name()) + "'");
    if (k.is_str()) return std::optional<std::string>(k.as_str());
    return std::optional<std::string>();
}

// ---- numbers ----

std::optional<Number> number(const Json& v) {
    if (v.is_bool()) return Number{true, BigInt(v.as_bool() ? 1 : 0), 0.0};
    if (v.is_int()) return Number{true, v.as_bigint(), 0.0};
    if (v.is_float()) return Number{false, BigInt(), v.as_float()};
    return std::nullopt;
}

Result<double> to_float(const Number& n) {
    if (!n.is_int) return n.f;
    auto too_large = [] { return Err::overflow_error("int too large to convert to float"); };
    if (n.i.fits_int64()) return static_cast<double>(n.i.to_int64().value());  // round-to-nearest-even, as Python
    // 1025 bits or more is at least 2**1024, beyond the largest double: refused before any conversion, so the
    // decimal text below has at most 309 digits.
    if (n.i.bit_length() > 1024) return too_large();
    std::optional<double> f = pyfloat::parse(n.i.to_decimal());  // strtod rounds correctly too
    if (!f || std::isinf(*f)) return too_large();
    return *f;
}

std::partial_ordering compare_numbers(const Number& a, const Number& b) {
    if (a.is_int && b.is_int) return a.i <=> b.i;
    if (!a.is_int && !b.is_int) return a.f <=> b.f;
    const BigInt& i = a.is_int ? a.i : b.i;
    const double f = a.is_int ? b.f : a.f;
    std::partial_ordering r = std::partial_ordering::unordered;  // (int i) <=> (float f)
    constexpr double kExact = 9007199254740992.0;                 // 2**53: every int up to it is a double
    if (std::isnan(f)) {
        r = std::partial_ordering::unordered;
    } else if (std::isinf(f)) {
        r = f > 0 ? std::partial_ordering::less : std::partial_ordering::greater;
    } else if (i >= BigInt(-(int64_t{1} << 53)) && i <= BigInt(int64_t{1} << 53)) {
        r = static_cast<double>(i.to_int64().value()) <=> f;
    } else if (std::fabs(f) < kExact) {
        r = i.sign() < 0 ? std::partial_ordering::less : std::partial_ordering::greater;  // |i| > 2**53 > |f|
    } else {
        r = i <=> detail::from_integral_double(f);  // a double that large is an integer: compare exactly
    }
    if (a.is_int) return r;
    return r == std::partial_ordering::less      ? std::partial_ordering::greater
           : r == std::partial_ordering::greater ? std::partial_ordering::less
                                                 : r;
}

Result<bool> compare(const Json& a, Cmp op, const Json& b) {
    std::partial_ordering r = std::partial_ordering::unordered;
    std::optional<Number> x = number(a);
    std::optional<Number> y = number(b);
    if (x && y) {
        r = compare_numbers(*x, *y);
    } else if (a.is_str() && b.is_str()) {
        r = compare_code_points(a.as_str(), b.as_str()) <=> 0;
    } else {
        const char* name = op == Cmp::Lt ? "<" : op == Cmp::Le ? "<=" : op == Cmp::Gt ? ">" : ">=";
        return Err::type_error(std::string("'") + name + "' not supported between instances of '" +
                               std::string(a.type_name()) + "' and '" + std::string(b.type_name()) + "'");
    }
    switch (op) {
        case Cmp::Lt:
            return r == std::partial_ordering::less;
        case Cmp::Le:
            return r == std::partial_ordering::less || r == std::partial_ordering::equivalent;
        case Cmp::Gt:
            return r == std::partial_ordering::greater;
        case Cmp::Ge:
            return r == std::partial_ordering::greater || r == std::partial_ordering::equivalent;
    }
    return false;
}

Result<Json> sub(const Json& a, const Json& b) {
    std::optional<Number> x = number(a);
    std::optional<Number> y = number(b);
    if (!x || !y) {
        return Err::type_error("unsupported operand type(s) for -: '" + std::string(a.type_name()) + "' and '" +
                               std::string(b.type_name()) + "'");
    }
    if (x->is_int && y->is_int) return Json(x->i - y->i);
    SCFG_TRY(double p, to_float(*x));
    SCFG_TRY(double q, to_float(*y));
    return Json(p - q);
}

Result<BigInt> index(const Json& v) {
    if (v.is_bool()) return BigInt(v.as_bool() ? 1 : 0);
    if (v.is_int()) return v.as_bigint();
    return Err::type_error("'" + std::string(v.type_name()) + "' object cannot be interpreted as an integer");
}

}  // namespace scfg::py
