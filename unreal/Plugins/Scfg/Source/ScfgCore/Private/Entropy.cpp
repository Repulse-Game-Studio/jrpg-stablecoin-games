// Copyright 2026 Repulse
// No Python twin: Python's `secrets` (see Entropy.h). The one file of the core that may use <random> (LintTests'
// WAIVERS).
#include "Scfg/Entropy.h"

#include "Scfg/Bytes.h"
#include "Scfg/Err.h"

#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace scfg::entropy_detail {

// The OS source, opened once for the whole program (the C# port opened a generator per call).
std::random_device& device() {
    static std::random_device d;
    return d;
}

}  // namespace scfg::entropy_detail

namespace scfg {

std::string Entropy::token_hex(int n) { return bytes::to_hex(token_bytes(n)); }

std::string Entropy::token_urlsafe(int n) {
    std::string out = bytes::b64encode(token_bytes(n));
    while (!out.empty() && out.back() == '=') out.pop_back();
    for (char& c : out) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    return out;
}

Result<int64_t> Entropy::randbelow(int64_t n) {
    if (n <= 0) return Err::value_error("Upper bound must be positive.");
    const uint64_t limit = static_cast<uint64_t>(n);
    const int bits = std::bit_width(limit - 1);  // values 0..n-1 need this many bits
    if (bits == 0) return int64_t{0};
    const int len = (bits + 7) / 8;
    const uint64_t mask = bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
    // Rejection sampling, as CPython's _randbelow: a uniform draw of `bits` bits is uniform on [0, 2**bits), so
    // keeping only the draws below n is uniform on [0, n). Each draw is kept with probability above 1/2.
    for (;;) {
        std::vector<uint8_t> b = token_bytes(len);
        uint64_t x = 0;
        for (uint8_t byte : b) x = (x << 8) | byte;
        x &= mask;
        if (x < limit) return static_cast<int64_t>(x);
    }
}

std::vector<uint8_t> SystemEntropy::token_bytes(int n) {
    if (n <= 0) return {};
    std::vector<uint8_t> out(static_cast<size_t>(n));
    std::random_device& d = entropy_detail::device();
    size_t i = 0;
    while (i < out.size()) {
        uint32_t word = static_cast<uint32_t>(d());  // random_device yields at least 32 random bits per call
        for (int k = 0; k < 4 && i < out.size(); ++k, ++i) {
            out[i] = static_cast<uint8_t>(word & 0xFF);
            word >>= 8;
        }
    }
    return out;
}

}  // namespace scfg
