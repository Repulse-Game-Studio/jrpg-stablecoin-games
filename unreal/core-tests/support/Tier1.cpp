// No Python twin: the Tier 1 runner (see Tier1.h).
#include "support/Tier1.h"

#include "support/JsonEq.h"
#include "support/Vectors.h"

#include "Scfg/Err.h"
#include "Scfg/Py.h"
#include "Scfg/PyFloat.h"

#include <cmath>
#include <optional>
#include <utility>

using scfg::Err;
using scfg::Json;

namespace scfg_test::tier1 {

Registry& registry() {
    static Registry r;
    return r;
}

void Topic::operator()(std::string fn, Adapter adapter) {
    auto at = r_.fn_topic.find(fn);
    if (at != r_.fn_topic.end()) {
        r_.errors.push_back("fn " + fn + " is registered by both tier1/" + at->second + " and tier1/" + name_);
        return;
    }
    r_.fn_topic.emplace(fn, name_);
    r_.fns.emplace(std::move(fn), std::move(adapter));
}

namespace detail {

TopicRegistrar::TopicRegistrar(const char* name, void (*body)(Topic& fn)) {
    Registry& r = registry();
    for (const std::string& t : r.topics) {
        if (t == name) r.errors.push_back(std::string("topic ") + name + " is registered twice");
    }
    r.topics.emplace_back(name);
    Topic topic(r, name);
    body(topic);
}

// json.dumps(v, ensure_ascii=False), cut to about 300 bytes on a code point boundary.
std::string show(const Json& v) {
    std::string s = scfg::json::dumps(v, {.ensure_ascii = false});
    if (s.size() <= 300) return s;
    size_t cut = 297;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

std::string number(size_t n) {
    std::string s;
    do {
        s.insert(s.begin(), static_cast<char>('0' + n % 10));
        n /= 10;
    } while (n != 0);
    return s;
}

}  // namespace detail

std::optional<std::string> run_case(const Json& c, const Registry& r) {
    if (!c.is_dict()) return "the case is not an object: " + detail::show(c);
    const Json::Dict& d = c.as_dict();
    const Json* fn_value = d.find("fn");
    if (fn_value == nullptr || !fn_value->is_str()) return "the case has no str fn: " + detail::show(c);
    const std::string& fn = fn_value->as_str();
    const Json* input = d.find("in");
    if (input == nullptr || !input->is_dict()) return fn + ": the case's in is not an object";
    auto adapter = r.fns.find(fn);
    if (adapter == r.fns.end()) return "no C++ adapter for " + fn;

    Json got = Json::dict();
    scfg::Result<Json> out = adapter->second(*input);
    if (out.ok()) {
        got.as_dict().set("out", out.value());
    } else {
        const Err& e = out.err();
        if (e.is_a("ConfigError")) {
            got.as_dict().set("error", e.code);
        } else if (e.is_a("ValueError") || e.is_a("TypeError")) {
            got.as_dict().set("error", "invalid");
        } else {
            return fn + "(" + detail::show(*input) + "): raised " + e.str() +
                   " (only ValueError, TypeError and ConfigError are rejections)";
        }
    }

    Json want = Json::dict();
    for (const char* key : {"out", "error"}) {
        if (const Json* v = d.find(key)) want.as_dict().set(key, *v);
    }
    std::optional<std::string> diff = first_diff(got, want);
    if (!diff) return std::nullopt;
    return fn + "(" + detail::show(*input) + "): " + *diff + " (got " + detail::show(got) + ", vector says " +
           detail::show(want) + ")";
}

std::vector<std::string> run_topic(std::string_view topic, const Registry& r) {
    std::vector<std::string> failures;
    std::string file = "tier1/" + std::string(topic) + ".json";
    bool registered = false;
    for (const std::string& t : r.topics) registered = registered || t == topic;
    if (!registered) {
        failures.push_back("no adapters are registered for this topic (no SCFG_TIER1_TOPIC(" + std::string(topic) + "))");
    }
    std::optional<std::string> text = vectors::read_text(file);
    if (!text) {
        failures.push_back(file + " cannot be read under " + vectors::root());
        return failures;
    }
    scfg::Result<Json> doc = scfg::json::loads(*text);
    if (!doc.ok()) {
        failures.push_back(file + " is not JSON: " + doc.err().str());
        return failures;
    }
    if (!doc.value().is_dict()) {
        failures.push_back(file + " is not an object");
        return failures;
    }
    const Json::Dict& d = doc.value().as_dict();
    const Json* named = d.find("topic");
    if (named == nullptr || !named->is_str() || named->as_str() != topic) {
        failures.push_back(file + " names topic " + (named == nullptr ? "(none)" : detail::show(*named)));
    }
    const Json* cases = d.find("cases");
    if (cases == nullptr || !cases->is_list()) {
        failures.push_back(file + " has no cases list");
        return failures;
    }
    if (cases->as_list().empty()) failures.push_back(file + " has no cases: an empty topic never passes");
    const Json::List& list = cases->as_list();
    for (size_t n = 0; n < list.size(); ++n) {
        if (std::optional<std::string> diff = run_case(list[n], r)) failures.push_back("#" + detail::number(n) + " " + *diff);
    }
    return failures;
}

scfg::Result<Json> decode_value(const Json& v) {
    if (!v.is_dict() || v.as_dict().size() != 1 || !v.as_dict().contains("float")) return v;
    const Json& x = *v.as_dict().find("float");
    if (x.is_float()) return x;
    if (x.is_bool()) return Json(x.as_bool() ? 1.0 : 0.0);
    if (x.is_int()) {  // float(int): correctly rounded, an OverflowError beyond the doubles
        std::optional<double> f = scfg::pyfloat::parse(x.as_bigint().to_decimal());
        if (!f || std::isinf(*f)) return Err::overflow_error("int too large to convert to float");
        return Json(*f);
    }
    if (!x.is_str()) {
        return Err::type_error("float() argument must be a string or a number, not '" + std::string(x.type_name()) +
                               "'");
    }
    std::optional<double> f = scfg::pyfloat::parse(x.as_str());
    if (!f) return Err::value_error("could not convert string to float: " + scfg::py::repr_str(x.as_str()));
    return Json(*f);
}

std::string report(std::string_view what, const std::vector<std::string>& failures, size_t shown) {
    std::string out = std::string(what) + ": " + detail::number(failures.size()) + " failing case(s)";
    size_t n = 0;
    for (; n < failures.size() && n < shown; ++n) out += "\n    " + failures[n];
    if (n < failures.size()) out += "\n    ... and " + detail::number(failures.size() - n) + " more";
    return out;
}

}  // namespace scfg_test::tier1
