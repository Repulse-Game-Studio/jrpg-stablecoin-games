// No Python twin: locates and reads the shared conformance vectors (does not depend on the test runner).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scfg_test::vectors {

// Absolute path of conformance/vectors: env SCFG_VECTORS if set, else the fallback set_fallback() gave, else
// SCFG_REPO_ROOT/conformance/vectors when the build defines SCFG_REPO_ROOT (CMake does), else "" (listing fails).
std::string root();
// Where root() looks when SCFG_VECTORS is unset (the Unreal test module passes find_above(the plugin's folder)).
void set_fallback(std::string dir);
// The conformance/vectors directory of the nearest folder at or above `start` (an absolute path) that has one, or
// nullopt.
std::optional<std::string> find_above(std::string_view start);
// Topic names (file names without .json) under tier1, sorted byte-wise. A directory error is appended to *error.
std::vector<std::string> tier1_topics(std::string* error = nullptr);
// Scenario ids such as "smoke/evm_unlock_paid" (paths under tier2 without .json), sorted byte-wise.
std::vector<std::string> tier2_ids(std::string* error = nullptr);
// The bytes of root()/rel, or nullopt if it cannot be read.
std::optional<std::string> read_text(std::string_view rel);

}  // namespace scfg_test::vectors
