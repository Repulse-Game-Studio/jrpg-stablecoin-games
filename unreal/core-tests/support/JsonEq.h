// No Python twin: JSON value equality as conformance/README.md defines it for params and outputs (does not depend
// on the test runner; the *Tests.cpp files assert on what it returns, ruling R6).
//
// Objects ignore key order, arrays are ordered, strings compare byte for byte, numbers compare by exact value (1
// equals 1.0, 2**70 equals 2.0**70, but 2**53 + 1 is not 2.0**53: no lossy conversion), a boolean never equals a
// number (true is not 1, unlike Python's ==, which is scfg::py::eq), and a NaN equals a NaN (so a recorded NaN
// matches). Values must be acyclic.
#pragma once

#include "Scfg/Json.h"

#include <optional>
#include <string>

namespace scfg_test {

bool json_eq(const scfg::Json& a, const scfg::Json& b);
// nullopt when equal, otherwise "path: left != right" for the first difference: "a.b[1]: 2 != 3",
// "c: missing on the left", "length 1 != 2". Object keys are visited in byte order, left's keys first.
std::optional<std::string> first_diff(const scfg::Json& a, const scfg::Json& b);

}  // namespace scfg_test
