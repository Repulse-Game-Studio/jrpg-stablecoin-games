// Copyright 2026 Repulse
// No Python twin: small helpers several *Tests.cpp files share. Unlike JsonEq and Tier2, this header depends on the
// test runner (same() records a failure), so the console driver and ScfgCoreTests do not include it.
#pragma once

#include "support/JsonEq.h"
#include "support/Test.h"
#include "support/Tier2.h"
#include "support/Vectors.h"

#include "Scfg/Bytes.h"
#include "Scfg/Co.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scfg_test {

// s * n, as Python repeats a str.
inline std::string repeat(std::string_view s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}

// A 32-byte digest as bytes.hex() prints it.
inline std::string hex(const std::array<uint8_t, 32>& d) { return scfg::bytes::to_hex(d); }

// bytes((i * 7 + 3) % 256 for i in range(n))
inline std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>((i * 7 + 3) % 256);
    return out;
}

// Checks that got equals want as conformance/README.md defines it (first_diff); the failure names the first
// difference and shows what came back.
inline bool same(Ctx& t, const scfg::Json& got, const scfg::Json& want, const std::string& what) {
    std::optional<std::string> diff = first_diff(got, want);
    return t.check(!diff, what + ": " + diff.value_or("") + " (got " + scfg::json::dumps(got).value() + ")");
}

// The Tier 2 vector tier2/<id>.json (id such as "smoke/evm_unlock_paid"), or nullopt when it cannot be read or parsed.
inline std::optional<scfg::Json> load_vector(const std::string& id) {
    std::optional<std::string> text = vectors::read_text("tier2/" + id + ".json");
    if (!text) return std::nullopt;
    scfg::Result<scfg::Json> v = tier2::parse(*text);
    if (!v.ok()) return std::nullopt;
    return v.take();
}

// The result of a coroutine that finished inside the call (the fakes answered at once), or nullopt when it is still
// suspended.
template <class T>
std::optional<scfg::Result<T>> run_now(scfg::Co<T> co) {
    if (!co.done()) return std::nullopt;
    return co.take();
}

}  // namespace scfg_test
