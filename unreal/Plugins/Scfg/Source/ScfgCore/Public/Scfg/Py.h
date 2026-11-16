// No Python twin: the CPython 3.9 built-ins the core relies on (str.strip(), str.isdecimal(), int(), len(), ==,
// d[key], truthiness, repr(), str.format() with one argument, str.lower() for an ASCII search, s.encode("utf-8"),
// urllib.parse.quote), mirroring unity/.../Py.cs and rpgmaker/src/core/pystr.ts, whose tables (the whitespace set
// and the Unicode 13.0.0 decimal digits) are copied here. Use these instead of <cctype>, <regex> or the C library:
// their Unicode tables differ, and they follow the C locale.
//
// Text is UTF-8 in std::string; a lone surrogate (from a JSON \ud800 escape) is WTF-8, and every helper here reads
// it as one code point, as Python's str holds it.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <cstddef>
#include <cstdint>
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
// OK when s is well-formed UTF-8 with surrogates allowed (Python's decode("utf-8", "surrogatepass")); otherwise
// the ValueError Python's UnicodeDecodeError carries ("'utf-8' codec can't decode byte 0xff in position 3:
// invalid start byte").
Status utf8_check(std::string_view s);
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
// a list or dict inside itself is [...] / {...}.
std::string repr(const Json& v);

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

// ---- values ----

// bool(v): None, False, 0, 0.0, -0.0, "", [] and {} are falsy (a NaN is truthy).
bool truthy(const Json& v);
// a == b as Python compares JSON values: numbers by exact value across bool, int and float (True == 1 == 1.0, a
// NaN equals nothing), str by content, lists in order, dicts by key set and values (order ignored), None only None.
// Values must be acyclic.
bool eq(const Json& a, const Json& b);
// The KeyError Python raises for d[key] (its message is repr(key), ruling R4).
Err key_error(std::string_view key);
// d[key] on a JSON value: the value, a KeyError for a missing key, or the TypeError CPython 3.9 raises when d is not
// a dict ("'NoneType' object is not subscriptable", "list indices must be integers or slices, not str", ...).
Result<Json> get(const Json& d, std::string_view key);

}  // namespace scfg::py
