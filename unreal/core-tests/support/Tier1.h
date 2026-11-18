// Copyright 2026 Repulse
// No Python twin: the Tier 1 runner, replaying conformance/vectors/tier1/*.json against the C++ core (as
// conformance/python/scfg_conformance/tier1.py's run_case, unity/core-tests/Tier1.cs and conformance/js/tier1.ts).
// Does not depend on the test runner (ruling R6): it returns failure messages and Tier1Tests.cpp asserts on them.
//
// Each topic registers its adapters from its own file, core-tests/tier1/<Topic>.cpp:
//
//   SCFG_TIER1_TOPIC(amounts) {
//       fn("parse_amount", [](const Json& in) -> Result<Json> { ... });
//   }
//
// An adapter maps a case's JSON "in" to the JSON "out" the vector records, or returns the Err the Python function
// raises. Adapters are keyed by the Python function name ("fn") across all topics, as tier1.py's FUNCS.
#pragma once

#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scfg_test::tier1 {

using Adapter = std::function<scfg::Result<scfg::Json>(const scfg::Json& in)>;

struct Registry {
    // Python fn name -> adapter.
    std::map<std::string, Adapter, std::less<>> fns;
    // fn name -> the topic file that registered it.
    std::map<std::string, std::string, std::less<>> fn_topic;
    // The topics that registered adapters (SCFG_TIER1_TOPIC), in registration order.
    std::vector<std::string> topics;
    // Registration mistakes (an fn or a topic registered twice); Tier1Tests fails while this is non-empty.
    std::vector<std::string> errors;
};

// The registry SCFG_TIER1_TOPIC fills.
Registry& registry();

// The `fn` a topic's registration body calls: fn("name", adapter).
class Topic {
public:
    Topic(Registry& r, std::string name) : r_(r), name_(std::move(name)) {}
    void operator()(std::string fn, Adapter adapter);

private:
    Registry& r_;
    std::string name_;
};

// Recomputes one case through the adapters: nullopt when it agrees with the vector, otherwise a message naming the
// first difference. The vector's expectation is whichever of "out" and "error" the case has (key presence, so an
// explicit "error": null expects an error null and fails). An "error" with a dot is a ConfigError with that code;
// "invalid" is an Err that is_a ValueError or TypeError and is not a ConfigError. Any other Err fails the case
// (stricter than Python on purpose), and so does an fn with no adapter.
std::optional<std::string> run_case(const scfg::Json& c, const Registry& r);
inline std::optional<std::string> run_case(const scfg::Json& c) { return run_case(c, registry()); }

// Every failure of a topic, in file order (empty when it passes): one per failing case ("#<n> <diff>"), and one for
// a file that cannot be read or parsed, that names another topic or has no cases, and for a topic no
// SCFG_TIER1_TOPIC registered (its cases still run, so their diffs show).
std::vector<std::string> run_topic(std::string_view topic, const Registry& r);
inline std::vector<std::string> run_topic(std::string_view topic) { return run_topic(topic, registry()); }

// replay.py's decode_value: {"float": x} (exactly that one key) is float(x), anything else is the value itself.
// x is the float's repr, as the vectors write it (an int, a float or a bool is converted too, as float() converts
// them); malformed text is float()'s ValueError, a list, a dict or None its TypeError.
scfg::Result<scfg::Json> decode_value(const scfg::Json& v);

// "<what>: <n> failing case(s)" and the first `shown` messages, then how many more there were.
std::string report(std::string_view what, const std::vector<std::string>& failures, size_t shown = 20);

namespace detail {
struct TopicRegistrar {
    TopicRegistrar(const char* name, void (*body)(Topic& fn));
};
}  // namespace detail

}  // namespace scfg_test::tier1

#define SCFG_TIER1_TOPIC(Name)                                                                              \
    static void scfg_tier1_topic_##Name(::scfg_test::tier1::Topic& fn);                                     \
    static const ::scfg_test::tier1::detail::TopicRegistrar scfg_tier1_reg_##Name(#Name,                    \
                                                                                  &scfg_tier1_topic_##Name); \
    static void scfg_tier1_topic_##Name([[maybe_unused]] ::scfg_test::tier1::Topic& fn)
