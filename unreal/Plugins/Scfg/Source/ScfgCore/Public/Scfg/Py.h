// No Python twin: the CPython 3.9 built-ins the core relies on (str.strip(), str.isdecimal(), int(), len(), ==,
// d[key], truthiness, repr(), str.format() with one argument, str.lower() for an ASCII search, s.encode("utf-8"),
// urllib.parse.quote, str() of an int), mirroring unity/.../Py.cs and rpgmaker/src/core/pystr.ts, whose tables (the
// whitespace set and the Unicode 13.0.0 decimal digits) are copied here. Use these instead of <cctype>, <regex> or the
// C library: their Unicode tables differ, and they follow the C locale.
//
// Text is UTF-8 in std::string; a lone surrogate (from a JSON \ud800 escape) is WTF-8, and every helper here reads
// it as one code point, as Python's str holds it.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <charconv>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scfg::py {

// ---- UTF-8 (and WTF-8) ----

// What utf8_decode returns for a byte that does not start a well-formed sequence.
inline constexpr uint32_t REPLACEMENT = 0xFFFD;

// The code point at s[i], advancing i past it. A 3-byte surrogate (ED A0..BF xx) is its own code point. A byte that
// does not start a well-formed sequence (or a truncated one) is REPLACEMENT and advances i by one byte.
uint32_t utf8_decode(std::string_view s, size_t& i);
// Appends cp's UTF-8 (a surrogate as its WTF-8 3-byte form). cp must be at most 0x10FFFF.
void utf8_encode(uint32_t cp, std::string& out);
std::string utf8_encode(uint32_t cp);
// Every code point of s, in order (utf8_decode's).
std::vector<uint32_t> code_points(std::string_view s);
// OK when s is well-formed UTF-8 (Python's strict b.decode("utf-8")): an encoded surrogate (ED A0..BF xx) is an
// error too. Otherwise the ValueError Python's UnicodeDecodeError carries ("'utf-8' codec can't decode byte 0xff
// in position 3: invalid start byte"; ED A0 is "byte 0xed ...: invalid continuation byte").
Status utf8_check(std::string_view s);
// The same with surrogates allowed (WTF-8; Python's decode("utf-8", "surrogatepass")): for text that is already a
// Python str, which may hold lone surrogates.
Status wtf8_check(std::string_view s);
// s.encode("utf-8"): the bytes of s; a lone surrogate is a ValueError (UnicodeEncodeError's message).
Result<std::vector<uint8_t>> utf8_bytes(std::string_view s);

// len(s): code points.
size_t len(std::string_view s);
// Python's str ordering, by code point: <0, 0 or >0. (UTF-8 byte order is code point order, WTF-8's too.)
int compare_code_points(std::string_view a, std::string_view b);

// ---- character classes ----

// str.isspace() for one code point, what str.strip() removes: \t \n \v \f \r, the separators \x1c-\x1f, space,
// \x85, \xa0, U+1680, U+2000-U+200A, U+2028, U+2029, U+202F, U+205F, U+3000. Not U+FEFF, U+180E or U+200B.
bool is_space(uint32_t cp);
// The value 0-9 of a CPython 3.9 decimal digit (Unicode 13.0.0's Nd) of any script, or -1. Not a newer table:
// Tangsa U+16AC0 (Unicode 14) and later digit sets are not digits to Python 3.9.
int decimal_value(uint32_t cp);
// str.isdecimal() for one code point, which is also re's \d.
bool is_decimal_digit(uint32_t cp);

// ---- strings ----

// s.strip() with no argument.
std::string strip(std::string_view s);
// A-Z to a-z, nothing else (the locale never matters).
std::string lower_ascii(std::string_view s);
// s.lower() as far as a search for an ASCII phrase can tell: A-Z become a-z, U+212A (Kelvin) becomes k and U+0130
// becomes "i" + U+0307, the only non-ASCII characters whose lowercase holds an ASCII character (checked over every
// code point with CPython 3.9); every other character stays itself, since it and its lowercase are both non-ASCII.
std::string lower_for_ascii(std::string_view s);

// repr(s) of a str: single quotes unless s has a single quote and no double quote; \\, the quote, \t \n \r
// escaped; other non-printable characters as \xNN, \uNNNN or \UNNNNNNNN. Port limit: "non-printable" is CPython's
// set for the control, format, separator, surrogate and private-use characters; an unassigned code point is printed
// as it is.
std::string repr_str(std::string_view s);
// ascii(s): repr_str with every non-ASCII character escaped too.
std::string ascii_str(std::string_view s);
// repr(v): None, True, False, ints, floats (pyfloat::repr), str (repr_str), lists and dicts as Python prints them;
// a list or dict inside itself is [...] / {...}. An int anywhere in v is int_str's: one of more than 4300 digits is
// CPython's ValueError, refused from its bit length before any conversion (so repr is linear in its output).
Result<std::string> repr(const Json& v);

// template.format(arg) for ONE positional str argument, raising what CPython raises: IndexError for {1}, KeyError
// for {name}, ValueError for malformed braces or a switch between automatic and manual numbering. Fields: {}, {0},
// {!r}, {!s}, {!a} (and an empty spec, {:}). Attribute access, indexing and format specs are ValueErrors, which is
// stricter than Python (as pyFormat1 of the TypeScript twin).
Result<std::string> format1(std::string_view tmpl, std::string_view arg);

// urllib.parse.quote(s, safe): UTF-8 percent-encoding (uppercase hex) of every byte except A-Za-z0-9_.-~ and the
// ASCII characters of safe (a non-ASCII one is ignored, as Python's safe.encode('ascii', 'ignore')). A lone
// surrogate is a ValueError.
Result<std::string> quote(std::string_view s, std::string_view safe = "/");

// ---- int() ----

// int(s): surrounding whitespace (str.isspace()'s set minus \x1c-\x1f, which int() refuses), an optional sign,
// then decimal digits of any script with single underscores between digits. Malformed text is a ValueError with
// CPython's "invalid literal for int() with base 10: %.200R"; a leading run of more than 4300 digits is one with
// CPython 3.9's limit message ("Exceeds the limit (4300) for integer string conversion: value has N digits; ...").
// Linear in the length of s.
Result<BigInt> py_int(std::string_view s);
inline Result<BigInt> py_int(const char* s) { return py_int(std::string_view(s)); }
inline Result<BigInt> py_int(const std::string& s) { return py_int(std::string_view(s)); }
// int(x) of a JSON value: a bool is 0 or 1, an int itself, a finite float truncated towards zero (int(-1.9) == -1),
// a str as above. NaN is a ValueError and an infinity an OverflowError (as Python); anything else a TypeError.
Result<BigInt> py_int(const Json& x);

// str(n) of a machine integer (int64_t, size_t, int, ...): at most 20 digits, so no digit limit applies and it
// cannot fail. For an int that may be big, int_str.
template <std::integral T>
    requires(!std::same_as<T, bool>)
std::string dec(T v) {
    char buf[24];
    std::to_chars_result r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

// str(n) (and f"{n}") of an int: its decimal digits, "-" first when negative. More than 4300 digits (the sign not
// counted) is CPython 3.9's ValueError "Exceeds the limit (4300) for integer string conversion; use
// sys.set_int_max_str_digits() to increase the limit", decided from the bit length first so that a huge n is
// refused without being converted.
Result<std::string> int_str(const BigInt& n);

// int(s, 16): the same surrounding whitespace and script-independent decimal digits as py_int, an optional sign, an
// optional 0x/0X prefix (with one optional underscore after it), then hex digits with single underscores between
// them. Malformed text is CPython's "invalid literal for int() with base 16: %.200R" ValueError. No digit limit
// (CPython exempts the power-of-two bases) and linear in the length of s.
Result<BigInt> py_int_hex(std::string_view s);
inline Result<BigInt> py_int_hex(const char* s) { return py_int_hex(std::string_view(s)); }
inline Result<BigInt> py_int_hex(const std::string& s) { return py_int_hex(std::string_view(s)); }
// int(x, 16) of a JSON value: a str as above; anything else (an int or a bool too) is CPython's TypeError "int()
// can't convert non-string with explicit base".
Result<BigInt> py_int_hex(const Json& x);
// int(x, 16) for a caller that keeps the value as an int64_t: the same errors, and nullopt for a well-formed literal
// outside int64 (decided from its count of significant digits, before anything is converted).
Result<std::optional<int64_t>> py_int_hex_int64(const Json& x);

// hex(n): "0x" and lowercase hex digits ("0x0" for zero), "-0x5" for a negative. No digit limit (base 16).
std::string hex(const BigInt& n);

// ---- values ----

// type(v).__name__ of a JSON value: "NoneType", "bool", "int", "float", "str", "list", "dict".
inline std::string type_name(const Json& v) { return std::string(v.type_name()); }
// The AttributeError CPython raises for v.attribute when v has no such attribute: "'int' object has no attribute
// 'lower'".
Err no_attribute(const Json& v, std::string_view attribute);

// str(v) of a JSON value: a str as it is, anything else its repr (str and repr agree on None, bools, ints,
// floats, lists and dicts), with repr's ValueError for an int of more than 4300 digits.
Result<std::string> str(const Json& v);
// `for x in v` (list(v), tuple(v), list.extend(v)) of a JSON value: a list's items, a dict's keys, a str's
// characters (each a str); anything else is CPython's TypeError "'int' object is not iterable".
Result<std::vector<Json>> iterate(const Json& v);

// bool(v): None, False, 0, 0.0, -0.0, "", [] and {} are falsy (a NaN is truthy).
bool truthy(const Json& v);
// a == b as Python compares JSON values: numbers by exact value across bool, int and float (True == 1 == 1.0, a
// NaN equals nothing), str by content, lists in order, dicts by key set and values (order ignored), None only None.
// Values must be acyclic.
bool eq(const Json& a, const Json& b);
// The KeyError Python raises for d[key] (its message is repr(key), ruling R4).
Err key_error(std::string_view key);
// The same for a key of any type. Python raises KeyError(key) and formats it only when it is printed, so a key
// whose repr fails (an int of more than 4300 digits) still makes a KeyError; its message is then what
// traceback.print_exception (print_error) writes for it, "<unprintable KeyError object>".
Err key_error_for(const Json& key);
// d[key] on a JSON value: the value, a KeyError for a missing key, or the TypeError CPython 3.9 raises when d is not
// a dict ("'NoneType' object is not subscriptable", "list indices must be integers or slices, not str", ...).
Result<Json> get(const Json& d, std::string_view key);
// The str key that d[k], `k in d` and d.get(k) look up in a dict with str keys, for a JSON value k: k itself for a
// str, nullopt for any other hashable value (None, a bool, a number: no such dict holds one), and CPython's
// TypeError "unhashable type: 'list'" for a list or a dict.
Result<std::optional<std::string>> str_key(const Json& k);

// ---- numbers (Python's arithmetic on values read back from storage) ----

// A Python number: a bool or an int, exactly (is_int, i), or a float (f).
struct Number {
    bool is_int = false;
    BigInt i;
    double f = 0.0;
};
// The number v holds (a bool as 0 or 1), or nullopt when v is not a number.
std::optional<Number> number(const Json& v);
// float(n): an int correctly rounded (as PyLong_AsDouble), an OverflowError beyond the float range.
Result<double> to_float(const Number& n);
// The exact order of two numbers, as Python compares an int with a float (never through a lossy conversion);
// unordered when either is a NaN.
std::partial_ordering compare_numbers(const Number& a, const Number& b);
// A total order for sorting numbers: compare_numbers, with a NaN after every number and NaNs equivalent (Python's
// comparisons with a NaN are all false, so where sorted() puts one depends on the comparisons it happens to make).
std::weak_ordering number_order(const Number& a, const Number& b);

enum class Cmp { Lt, Le, Gt, Ge };
// a < b (<=, >, >=) as Python evaluates it for two JSON values: numbers by compare_numbers (False when a NaN is
// involved), two strs by code point; anything else is CPython's TypeError "'<' not supported between instances of
// 'int' and 'str'". Port limit: two lists (which Python orders item by item) are refused too; the core only ever
// compares a stored value with a number.
Result<bool> compare(const Json& a, Cmp op, const Json& b);
// a - b of two numbers: exact for two ints, else in floats (an int beyond the float range is an OverflowError);
// anything else is CPython's TypeError "unsupported operand type(s) for -: 'int' and 'str'".
Result<Json> sub(const Json& a, const Json& b);
// operator.index(v), what hex() and range() take: an int, or a bool as 0 or 1; anything else is CPython's
// TypeError "'float' object cannot be interpreted as an integer".
Result<BigInt> index(const Json& v);

}  // namespace scfg::py
