// Copyright 2026 Repulse
// No Python twin: hashlib.sha256 (FIPS 180-4, section 6.2).
#include "Scfg/Sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace scfg::sha256::detail {

// The first 32 bits of the fractional parts of the cube roots of the first 64 primes (FIPS 180-4, 4.2.2).
constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// The initial hash value H(0) (FIPS 180-4, 5.3.3).
constexpr uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

// One 64-byte block into the hash state (FIPS 180-4, 6.2.2).
void compress(std::array<uint32_t, 8>& h, const uint8_t* block) {
    uint32_t w[64];
    for (size_t t = 0; t < 16; ++t) {
        w[t] = (uint32_t(block[4 * t]) << 24) | (uint32_t(block[4 * t + 1]) << 16) | (uint32_t(block[4 * t + 2]) << 8) |
               uint32_t(block[4 * t + 3]);
    }
    for (size_t t = 16; t < 64; ++t) {
        uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (size_t t = 0; t < 64; ++t) {
        uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

}  // namespace scfg::sha256::detail

namespace scfg::sha256 {

std::array<uint8_t, 32> digest(std::span<const uint8_t> data) {
    std::array<uint32_t, 8> h;
    for (size_t i = 0; i < 8; ++i) h[i] = detail::H0[i];
    size_t offset = 0;
    for (; data.size() - offset >= 64; offset += 64) detail::compress(h, data.data() + offset);
    // Padding (5.1.1): 0x80, zeros, then the message length in bits as a 64-bit big-endian number; one block, or
    // two when fewer than 9 bytes are left after the data.
    std::array<uint8_t, 128> tail{};
    size_t rest = data.size() - offset;
    for (size_t i = 0; i < rest; ++i) tail[i] = data[offset + i];
    tail[rest] = 0x80;
    size_t blocks = rest + 9 <= 64 ? 1 : 2;
    uint64_t bits = uint64_t(data.size()) * 8;
    for (size_t i = 0; i < 8; ++i) tail[64 * blocks - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    for (size_t b = 0; b < blocks; ++b) detail::compress(h, tail.data() + 64 * b);
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < 32; ++i) out[i] = static_cast<uint8_t>(h[i / 4] >> (24 - 8 * (i % 4)));
    return out;
}

std::array<uint8_t, 32> digest(std::string_view data) {
    std::vector<uint8_t> bytes(data.begin(), data.end());  // a copy: no cast between char and uint8_t pointers
    return digest(std::span<const uint8_t>(bytes));
}

}  // namespace scfg::sha256
