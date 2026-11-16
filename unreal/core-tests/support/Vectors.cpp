// No Python twin: locates and reads the shared conformance vectors.
#include "support/Vectors.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace scfg_test::vectors {

std::string root() {
    const char* env = std::getenv("SCFG_VECTORS");
    if (env != nullptr && *env != '\0') return fs::absolute(fs::path(env)).lexically_normal().generic_string();
    return (fs::path(SCFG_REPO_ROOT) / "conformance" / "vectors").lexically_normal().generic_string();
}

// The paths of the *.json files under dir (recursively or not), relative to dir, without .json, sorted.
static std::vector<std::string> json_names(const fs::path& dir, bool recursive) {
    std::vector<std::string> out;
    std::error_code ec;
    auto take = [&](const fs::directory_entry& e) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json") return;
        std::string rel = e.path().lexically_relative(dir).generic_string();
        out.push_back(rel.substr(0, rel.size() - 5));
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) take(*it);
    } else {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) take(*it);
    }
    std::sort(out.begin(), out.end());  // std::string compares bytes as unsigned char
    return out;
}

std::vector<std::string> tier1_topics() { return json_names(fs::path(root()) / "tier1", false); }

std::vector<std::string> tier2_ids() { return json_names(fs::path(root()) / "tier2", true); }

std::optional<std::string> read_text(std::string_view rel) {
    std::ifstream in(fs::path(root()) / fs::path(std::string(rel)), std::ios::binary);
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return text;
}

}  // namespace scfg_test::vectors
