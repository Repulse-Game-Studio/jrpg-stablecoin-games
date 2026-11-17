// No Python twin: a JSON value as Python's json module makes it (None, bool, int, float, str, list, dict), and
// json.loads / json.dumps.
//
// Lists and dicts are shared handles, as Python names are: copying a Json aliases the same list or dict, so an
// append through the copy shows in the original. deep_copy() copies (Python's copy.deepcopy). Scalars (strings too)
// are values. An int is an int64_t, or a BigInt beyond it, always normalised: a value that fits int64 is never
// stored as a BigInt. A bool is not an int here (is_int() is false for it), although Python's bool is one; py::eq
// and py::py_int treat it as Python does.
//
// Strings are UTF-8. A lone surrogate from a JSON \ud800 escape is kept as WTF-8 (its 3-byte encoding, ED A0 80),
// so nothing is lost, and dumps writes it back as \ud800.
//
// There is deliberately no operator== on Json: Python's == (True == 1 == 1.0) is py::eq, and the conformance rule
// (key order ignored, true is not 1) is the tests' json_eq.
//
// Wrong-type access (as_str() of an int, ...) is a programming error: it prints what and aborts, as Result's does.
// A list or dict that contains itself is one too for dumps and deep_copy (Python raises "Circular reference
// detected" and copies the cycle); py::repr prints it as Python does ([...]).
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Export.h"
#include "Scfg/Result.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace scfg {

class SCFGCORE_API Json {
public:
    enum class Type { Null, Bool, Int, Float, Str, List, Dict };
    using List = std::vector<Json>;
    class Dict;

    // None.
    Json() = default;
    Json(std::nullptr_t) {}
    // Copying aliases a list or dict (a shared handle); scalars are copied. Defined out of line: GCC 13 at -O2 sees
    // through an inlined std::variant move and reports the inactive shared_ptr alternative as maybe-uninitialized.
    Json(const Json& other);
    Json(Json&& other) noexcept;
    Json& operator=(const Json& other);
    Json& operator=(Json&& other) noexcept;
    ~Json();
    // Only a real bool (a pointer or an int does not become one).
    template <class B>
        requires std::same_as<B, bool>
    Json(B b) : v_(b) {}
    // Any integer type but bool and the character types; a uint64_t beyond int64 becomes a BigInt.
    template <std::integral I>
        requires(!std::same_as<I, bool> && !std::same_as<I, char> && !std::same_as<I, signed char> &&
                 !std::same_as<I, unsigned char> && !std::same_as<I, char8_t> && !std::same_as<I, char16_t> &&
                 !std::same_as<I, char32_t>)
    Json(I i) {
        if constexpr (std::is_signed_v<I>) {
            v_ = static_cast<int64_t>(i);
        } else if (static_cast<uint64_t>(i) <= static_cast<uint64_t>(INT64_MAX)) {
            v_ = static_cast<int64_t>(i);
        } else {
            v_ = BigInt::from_uint64(static_cast<uint64_t>(i));
        }
    }
    // Normalised: stored as int64 when it fits.
    Json(BigInt i);
    Json(double d) : v_(d) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(std::string_view s) : v_(std::string(s)) {}
    Json(const char* s) : v_(std::string(s)) {}
    // A new list or dict holding these (Python's [*items] / dict(items)).
    Json(List l);
    Json(Dict d);

    // Python's [] / [a, b] and {} / {"k": v}: a new list or dict.
    static Json list(std::initializer_list<Json> items = {});
    static Json dict(std::initializer_list<std::pair<std::string, Json>> items = {});

    Type type() const;
    // type(v).__name__: "NoneType", "bool", "int", "float", "str", "list", "dict".
    std::string_view type_name() const;

    bool is_null() const { return std::holds_alternative<std::monostate>(v_); }
    bool is_bool() const { return std::holds_alternative<bool>(v_); }
    // An int of any size (not a bool).
    bool is_int() const { return std::holds_alternative<int64_t>(v_) || std::holds_alternative<BigInt>(v_); }
    // An int that fits int64_t (normalisation makes that every int64 value).
    bool is_int64() const { return std::holds_alternative<int64_t>(v_); }
    bool is_float() const { return std::holds_alternative<double>(v_); }
    bool is_str() const { return std::holds_alternative<std::string>(v_); }
    bool is_list() const { return std::holds_alternative<std::shared_ptr<List>>(v_); }
    bool is_dict() const { return std::holds_alternative<std::shared_ptr<Dict>>(v_); }

    bool as_bool() const;
    int64_t as_int64() const;
    // Any int, as a BigInt.
    BigInt as_bigint() const;
    double as_float() const;
    const std::string& as_str() const;
    // The shared list or dict: a change shows through every Json that aliases it.
    List& as_list();
    const List& as_list() const;
    Dict& as_dict();
    const Dict& as_dict() const;

    // Python's `a is b` for a list or dict (the same shared object); false for anything else.
    bool same_object(const Json& other) const;

private:
    std::variant<std::monostate, bool, int64_t, BigInt, double, std::string, std::shared_ptr<List>, std::shared_ptr<Dict>>
        v_;
};

// A dict with str keys, in insertion order the Python way: assigning an existing key keeps its position, and a key
// that was erased and is assigned again goes last. Lookup is hashed; erase is O(n). Changing the key set while
// iterating invalidates the iteration (Python raises "dictionary changed size during iteration"); set() of a
// present key does not.
class SCFGCORE_API Json::Dict {
public:
    using Entry = std::pair<std::string, Json>;
    using const_iterator = std::vector<Entry>::const_iterator;

    Dict() = default;
    Dict(std::initializer_list<Entry> items);

    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    // key in d.
    bool contains(std::string_view key) const { return index_.find(key) != index_.end(); }
    // &d[key], or nullptr when the key is absent. The pointer is valid until the next set() of a new key or erase().
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);
    // d.get(key, fallback).
    Json get(std::string_view key, Json fallback = Json()) const;
    // d[key] = value.
    void set(std::string key, Json value);
    // del d[key] without the KeyError: true when the key was there.
    bool erase(std::string_view key);
    // list(d), in order.
    std::vector<std::string> keys() const;

    // (key, value) pairs in order.
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }

private:
    struct Hash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    std::vector<Entry> entries_;
    std::unordered_map<std::string, size_t, Hash, std::equal_to<>> index_;
};

// copy.deepcopy of a JSON value: every list and dict is copied (keys keep their order); scalars are values anyway.
// A list or dict met twice is copied twice (Python's memo would keep the two places aliased).
SCFGCORE_API Json deep_copy(const Json& v);

namespace json {

// The deepest nesting of lists and dicts loads reads. CPython raises RecursionError near its recursion limit
// (1000); here deeper nesting is a ValueError rather than a stack overflow.
inline constexpr size_t MAX_DEPTH = 1000;

// json_loads(raw.decode("utf-8")) as rpc.py reads a node's answer (integer literals through ints.int_from):
// - the bytes must be strict UTF-8: invalid UTF-8, an encoded surrogate (ED A0..BF xx, CESU-8/WTF-8) included, is a
//   ValueError with the strict decoder's UnicodeDecodeError message ("... byte 0xed in position 1: invalid
//   continuation byte"), which rpc.py turns into TransportError("invalid JSON response");
// - an integer literal stays an int (int64, or BigInt beyond); one of more than 4300 digits is a ValueError;
// - a fraction or an exponent makes a float (1e400 is inf, as float() makes it);
// - NaN, Infinity and -Infinity are read, as CPython reads them;
// - malformed text, trailing data and a BOM are ValueErrors with JSONDecodeError's message ("msg: line L column C
//   (char N)", counted in code points; a BOM is "Unexpected UTF-8 BOM", as for a str);
// - a JSON \ud800 escape decodes to WTF-8 (a lone surrogate is kept, as in a Python str).
// Use it for bytes from the network (an RPC response body).
SCFGCORE_API Result<Json> loads(std::string_view text);

// json.loads(s) of text that is already a Python str, which may hold lone surrogates (kept here as WTF-8): the same
// as loads, except that encoded surrogates (ED A0..BF xx) are accepted, each as one code point. Other invalid UTF-8
// is still a ValueError. Use it for text the core's own JSON produced or that arrived as a str, such as coio.py's
// envelope["body"].
SCFGCORE_API Result<Json> loads_text(std::string_view text);

struct DumpOptions {
    bool sort_keys = false;
    // None: one line with ", " and ": "; n: newlines, n spaces per level and "," (a negative n is 0, as in Python).
    std::optional<int> indent = std::nullopt;
    bool ensure_ascii = true;
};

// json.dumps(v, sort_keys=, indent=, ensure_ascii=): the text Python writes for every value it can write, floats as
// Python's repr (NaN, Infinity and -Infinity as Python writes them), keys sorted by code point, \uXXXX with
// surrogate pairs under ensure_ascii. An int of more than 4300 digits is the ValueError Python raises
// (py::int_str's, decided from the bit length first, so dumps stays linear in its output). A list or dict that
// contains itself is a programming error (Python's "Circular reference detected").
SCFGCORE_API Result<std::string> dumps(const Json& v, const DumpOptions& opts = {});

}  // namespace json
}  // namespace scfg
