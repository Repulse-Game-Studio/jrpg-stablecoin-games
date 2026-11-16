// No Python twin: locates and reads the shared conformance vectors (does not depend on the test runner).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scfg_test::vectors {

// Absolute path of conformance/vectors: env SCFG_VECTORS if set, else SCFG_REPO_ROOT/conformance/vectors.
std::string root();
// Topic names (file names without .json) under tier1, sorted byte-wise. A directory error is appended to *error.
std::vector<std::string> tier1_topics(std::string* error = nullptr);
// Scenario ids such as "smoke/evm_unlock_paid" (paths under tier2 without .json), sorted byte-wise.
std::vector<std::string> tier2_ids(std::string* error = nullptr);
// The bytes of root()/rel, or nullopt if it cannot be read.
std::optional<std::string> read_text(std::string_view rel);

}  // namespace scfg_test::vectors
