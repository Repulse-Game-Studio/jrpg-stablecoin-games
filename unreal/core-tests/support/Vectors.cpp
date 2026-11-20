// Copyright 2026 Repulse
// No Python twin: locates and reads the shared conformance vectors.
#include "support/Vectors.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace scfg_test::vectors {

static std::string& fallback() {
    static std::string dir;
    return dir;
}

std::string root() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv: MSVC's "unsafe" deprecation; the value is only read here
#endif
    const char* env = std::getenv("SCFG_VECTORS");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (env != nullptr && *env != '\0') {
        std::error_code ec;
        fs::path abs = fs::absolute(fs::path(env), ec);
        return (ec ? fs::path(env) : abs).lexically_normal().generic_string();  // a bad path fails at listing
    }
    if (!fallback().empty()) return fallback();
#if defined(SCFG_REPO_ROOT)
    return (fs::path(SCFG_REPO_ROOT) / "conformance" / "vectors").lexically_normal().generic_string();
#else
    return "";
#endif
}

void set_fallback(std::string dir) { fallback() = std::move(dir); }

std::optional<std::string> find_above(std::string_view start) {
    fs::path dir = fs::path(std::string(start)).lexically_normal();
    while (true) {
        std::error_code ec;
        fs::path candidate = dir / "conformance" / "vectors";
        if (fs::is_directory(candidate, ec)) return candidate.generic_string();
        fs::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) return std::nullopt;
        dir = parent;
    }
}

// The paths of the *.json files under dir (recursively or not), relative to dir, without .json, sorted.
static std::vector<std::string> json_names(const fs::path& dir, bool recursive, std::string* error) {
    std::vector<std::string> out;
    std::error_code ec;
    auto report = [error](const std::string& what) {
        if (error != nullptr) *error += (error->empty() ? "" : "; ") + what;
    };
    auto take = [&](const fs::directory_entry& e) {
        std::error_code kind;
        bool file = e.is_regular_file(kind);
        if (kind) report(e.path().generic_string() + ": " + kind.message());
        if (!file || e.path().extension() != ".json") return;
        std::string rel = e.path().lexically_relative(dir).generic_string();
        out.push_back(rel.substr(0, rel.size() - 5));
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) take(*it);
    } else {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) take(*it);
    }
    if (ec) report(dir.generic_string() + ": " + ec.message());
    std::sort(out.begin(), out.end());  // std::string compares bytes as unsigned char
    return out;
}

std::vector<std::string> tier1_topics(std::string* error) {
    return json_names(fs::path(root()) / "tier1", false, error);
}

std::vector<std::string> tier2_ids(std::string* error) { return json_names(fs::path(root()) / "tier2", true, error); }

std::optional<std::string> read_text(std::string_view rel) {
    std::ifstream in(fs::path(root()) / fs::path(std::string(rel)), std::ios::binary);
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return text;
}

}  // namespace scfg_test::vectors
