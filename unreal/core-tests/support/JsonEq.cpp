// No Python twin: JSON value equality as conformance/README.md defines it (see JsonEq.h).
#include "support/JsonEq.h"

#include "Scfg/Py.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

using scfg::Json;

namespace scfg_test::json_eq_detail {

std::string here(const std::string& path, const std::string& detail) {
    return path.empty() ? detail : path + ": " + detail;
}

// json.dumps of v, cut to 120 bytes at a code point boundary (or the error dumps raised, in <>).
std::string show(const Json& v) {
    scfg::Result<std::string> text = scfg::json::dumps(v);
    if (!text.ok()) return "<" + text.err().str() + ">";
    std::string s = text.take();
    if (s.size() <= 120) return s;
    size_t cut = 117;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

std::string differ(const std::string& path, const Json& a, const Json& b) {
    return here(path, show(a) + " != " + show(b));
}

bool is_number(const Json& v) { return v.is_int() || v.is_float(); }

// Exact numeric equality of two ints or floats (neither a bool).
bool same_number(const Json& a, const Json& b) {
    if (a.is_float() && b.is_float()) {
        double x = a.as_float(), y = b.as_float();
        return x == y || (std::isnan(x) && std::isnan(y));
    }
    if (a.is_int() && b.is_int()) return a.as_bigint() == b.as_bigint();
    const Json& i = a.is_int() ? a : b;
    double f = (a.is_int() ? b : a).as_float();
    if (!std::isfinite(f) || std::trunc(f) != f) return false;
    // f is integral: compare exactly through its own int value (Python's int(f)).
    scfg::Result<scfg::BigInt> fi = scfg::py::py_int(Json(f));
    return fi.ok() && fi.value() == i.as_bigint();
}

std::optional<std::string> diff(const Json& a, const Json& b, const std::string& path) {
    if (a.is_list() || b.is_list()) {
        if (!a.is_list() || !b.is_list()) return differ(path, a, b);
        const Json::List& l = a.as_list();
        const Json::List& r = b.as_list();
        size_t n = std::min(l.size(), r.size());
        for (size_t i = 0; i < n; ++i) {
            if (auto d = diff(l[i], r[i], path + "[" + std::to_string(i) + "]")) return d;
        }
        if (l.size() != r.size()) return here(path, "length " + std::to_string(l.size()) + " != " + std::to_string(r.size()));
        return std::nullopt;
    }
    if (a.is_dict() || b.is_dict()) {
        if (!a.is_dict() || !b.is_dict()) return differ(path, a, b);
        const Json::Dict& l = a.as_dict();
        const Json::Dict& r = b.as_dict();
        auto sub = [&path](const std::string& k) { return path.empty() ? k : path + "." + k; };
        std::vector<std::string> lk = l.keys(), rk = r.keys();
        std::sort(lk.begin(), lk.end());
        std::sort(rk.begin(), rk.end());
        for (const std::string& k : lk) {
            const Json* rv = r.find(k);
            if (rv == nullptr) return sub(k) + ": missing on the right";
            if (auto d = diff(*l.find(k), *rv, sub(k))) return d;
        }
        for (const std::string& k : rk) {
            if (!l.contains(k)) return sub(k) + ": missing on the left";
        }
        return std::nullopt;
    }
    bool same = false;
    if (a.is_null() || b.is_null()) {
        same = a.is_null() && b.is_null();
    } else if (a.is_bool() || b.is_bool()) {
        same = a.is_bool() && b.is_bool() && a.as_bool() == b.as_bool();
    } else if (a.is_str() || b.is_str()) {
        same = a.is_str() && b.is_str() && a.as_str() == b.as_str();
    } else if (is_number(a) && is_number(b)) {
        same = same_number(a, b);
    }
    if (same) return std::nullopt;
    return differ(path, a, b);
}

}  // namespace scfg_test::json_eq_detail

namespace scfg_test {

bool json_eq(const Json& a, const Json& b) { return !first_diff(a, b).has_value(); }

std::optional<std::string> first_diff(const Json& a, const Json& b) { return json_eq_detail::diff(a, b, ""); }

}  // namespace scfg_test
