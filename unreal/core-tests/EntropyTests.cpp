// No Python twin: tests of scfg::Entropy (Python's `secrets`): randbelow's ValueError, and that its rejection
// sampling is exactly uniform. A byte source that hands out every byte pattern once must give every value in [0, n)
// equally often, which modulo reduction cannot (256 % 6 != 0). Also SystemEntropy's sizes, hex and a loose
// statistical check.
#include "support/Test.h"

#include "Scfg/Entropy.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

using scfg::Result;

// Hands out the bytes of a counter: each draw of k bytes is the next counter value, big-endian.
class Counting final : public scfg::Entropy {
public:
    uint64_t next = 0;
    uint64_t bytes_drawn = 0;
    std::vector<uint8_t> token_bytes(int n) override {
        std::vector<uint8_t> out(static_cast<size_t>(n));
        uint64_t v = next++;
        for (int i = n - 1; i >= 0; --i) {
            out[static_cast<size_t>(i)] = static_cast<uint8_t>(v & 0xFF);
            v >>= 8;
        }
        bytes_drawn += static_cast<uint64_t>(n);
        return out;
    }
};

// Hands out the given draws in order.
class Fixed final : public scfg::Entropy {
public:
    std::vector<std::vector<uint8_t>> draws;
    size_t taken = 0;
    std::vector<uint8_t> token_bytes(int n) override {
        if (taken >= draws.size() || draws[taken].size() != static_cast<size_t>(n)) return {};
        return draws[taken++];
    }
};

template <class T>
static std::string outcome(const Result<T>& r) {
    if (r.ok()) return "ok";
    return r.err().kind + ": " + r.err().message;
}

SCFG_TEST(EntropyTests, RandbelowRefusesANonPositiveBound) {
    scfg::SystemEntropy e;
    SCFG_CHECK_EQ(t, outcome(e.randbelow(0)), "ValueError: Upper bound must be positive.");
    SCFG_CHECK_EQ(t, outcome(e.randbelow(-1)), "ValueError: Upper bound must be positive.");
    SCFG_CHECK_EQ(t, outcome(e.randbelow(INT64_MIN)), "ValueError: Upper bound must be positive.");
    Counting c;
    Result<int64_t> one = c.randbelow(1);
    SCFG_CHECK(t, one.ok() && one.value() == 0);
    SCFG_CHECK_EQ(t, c.bytes_drawn, 0);  // nothing to choose: no draw
}

SCFG_TEST(EntropyTests, RejectionSamplingIsExactlyUniformOnOneByte) {
    // n = 6: 3 bits (mask 7) of each byte; 6 and 7 are thrown away. The 256 byte values give 0..5 32 times each, after
    // 192 calls that consumed bytes 0..253 (254 and 255 would both be rejected).
    Counting c;
    std::map<int64_t, int> counts;
    for (int i = 0; i < 192; ++i) {
        Result<int64_t> r = c.randbelow(6);
        if (!SCFG_CHECK(t, r.ok())) return;
        counts[r.value()]++;
    }
    SCFG_CHECK_EQ(t, counts.size(), 6);
    for (const auto& [v, n] : counts) {
        SCFG_CHECK(t, v >= 0 && v < 6);
        SCFG_CHECK_EQ(t, n, 32);
    }
    SCFG_CHECK_EQ(t, c.bytes_drawn, 254);
}

SCFG_TEST(EntropyTests, RejectionSamplingIsExactlyUniformOnTwoBytes) {
    // n = 300: bit_length(299) = 9, two bytes read big-endian and masked with 0x1FF. Every 16-bit pattern once gives
    // each value 128 times.
    Counting c;
    std::vector<int> counts(300, 0);
    for (int i = 0; i < 300 * 128; ++i) {
        Result<int64_t> r = c.randbelow(300);
        if (!SCFG_CHECK(t, r.ok() && r.value() >= 0 && r.value() < 300)) return;
        counts[static_cast<size_t>(r.value())]++;
    }
    for (int n : counts) SCFG_CHECK_EQ(t, n, 128);
    SCFG_CHECK(t, c.next <= 65536);
}

SCFG_TEST(EntropyTests, RandbelowOfTheLargestBound) {
    // n = 2**63 - 1: 63 bits of 8 bytes. All ones masks to n itself and is rejected; the next draw is kept.
    Fixed f;
    f.draws = {std::vector<uint8_t>(8, 0xFF), {0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}};
    Result<int64_t> r = f.randbelow(INT64_MAX);
    SCFG_CHECK(t, r.ok() && r.value() == INT64_MAX - 1);
    SCFG_CHECK_EQ(t, f.taken, 2);
    // The high bit is masked off, not rejected: 0x80..01 gives 1.
    Fixed g;
    g.draws = {{0x80, 0, 0, 0, 0, 0, 0, 0x01}};
    Result<int64_t> s = g.randbelow(INT64_MAX);
    SCFG_CHECK(t, s.ok() && s.value() == 1);
}

SCFG_TEST(EntropyTests, TokenHexIsTheHexOfTokenBytes) {
    Fixed f;
    f.draws = {{0x00, 0x1f, 0xa0, 0xff}};
    SCFG_CHECK_EQ(t, f.token_hex(4), "001fa0ff");
}

// base64.urlsafe_b64encode(b).rstrip(b"=").decode() of the drawn bytes: Python's token_urlsafe.
SCFG_TEST(EntropyTests, TokenUrlsafeIsUnpaddedBase64url) {
    Fixed f;
    // 0xfb 0xff 0xbf is "+/+/" in the standard alphabet; 16 bytes leave two padding characters to strip
    f.draws = {{0xfb, 0xff, 0xbf}, std::vector<uint8_t>(16, 0xff), {0x00}};
    SCFG_CHECK_EQ(t, f.token_urlsafe(3), "-_-_");
    SCFG_CHECK_EQ(t, f.token_urlsafe(16), "_____________________w");
    SCFG_CHECK_EQ(t, f.token_urlsafe(1), "AA");
    scfg::SystemEntropy e;
    const std::string secret = e.token_urlsafe(16);
    SCFG_CHECK_EQ(t, secret.size(), 22);
    bool alphabet = true;
    for (char c : secret) {
        alphabet = alphabet && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '_');
    }
    SCFG_CHECK(t, alphabet);
    SCFG_CHECK(t, e.token_urlsafe(16) != secret);
}

SCFG_TEST(EntropyTests, SystemEntropySizes) {
    scfg::SystemEntropy e;
    SCFG_CHECK_EQ(t, e.token_bytes(32).size(), 32);
    SCFG_CHECK_EQ(t, e.token_bytes(1).size(), 1);
    SCFG_CHECK(t, e.token_bytes(0).empty());
    SCFG_CHECK(t, e.token_bytes(-1).empty());
    std::string hex = e.token_hex(8);
    SCFG_CHECK_EQ(t, hex.size(), 16);
    for (char ch : hex) SCFG_CHECK(t, (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
    // 64 random bytes repeating is a broken source.
    std::set<std::vector<uint8_t>> seen;
    for (int i = 0; i < 8; ++i) seen.insert(e.token_bytes(32));
    SCFG_CHECK_EQ(t, seen.size(), 8);
    // Two instances share one device and are both usable.
    scfg::SystemEntropy other;
    SCFG_CHECK(t, other.token_bytes(32) != e.token_bytes(32));
}

SCFG_TEST(EntropyTests, SystemRandbelowLooksUniform) {
    // 30000 draws of randbelow(3): each count is 10000 +- 82 (one sigma); 600 is over 7 sigma.
    scfg::SystemEntropy e;
    int counts[3] = {0, 0, 0};
    for (int i = 0; i < 30000; ++i) {
        Result<int64_t> r = e.randbelow(3);
        if (!SCFG_CHECK(t, r.ok() && r.value() >= 0 && r.value() < 3)) return;
        counts[r.value()]++;
    }
    for (int n : counts) SCFG_CHECK(t, n > 9400 && n < 10600);
    // A bound above 2**32 still stays below it.
    for (int i = 0; i < 1000; ++i) {
        Result<int64_t> r = e.randbelow(int64_t{5000000000});
        if (!SCFG_CHECK(t, r.ok() && r.value() >= 0 && r.value() < 5000000000)) return;
    }
}
