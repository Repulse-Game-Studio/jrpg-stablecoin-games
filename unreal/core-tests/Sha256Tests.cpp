// Copyright 2026 Repulse
// No Python twin: tests of scfg::sha256 against hashlib.sha256: the FIPS 180-4 (and NIST CAVP) messages "", "abc",
// the 448-bit and the 896-bit message and one million 'a', plus the padding boundaries around one and two blocks
// (55/56 and 119/120 bytes, where the length no longer fits the last block). Every digest is what python3's
// hashlib.sha256(m).hexdigest() prints.
#include "support/Test.h"

#include "Scfg/Bytes.h"
#include "Scfg/Sha256.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sha256 = scfg::sha256;

static std::string hex(const std::array<uint8_t, 32>& d) { return scfg::bytes::to_hex(d); }

// bytes((i * 7 + 3) % 256 for i in range(n))
static std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>((i * 7 + 3) % 256);
    return out;
}

SCFG_TEST(Sha256Tests, Fips180Vectors) {
    SCFG_CHECK_EQ(t, hex(sha256::digest(std::string_view(""))),
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    SCFG_CHECK_EQ(t, hex(sha256::digest(std::string_view("abc"))),
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    SCFG_CHECK_EQ(t, hex(sha256::digest(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    SCFG_CHECK_EQ(t,
                  hex(sha256::digest(std::string_view("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                                                      "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))),
                  "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

SCFG_TEST(Sha256Tests, OneMillionA) {
    std::string a(1000000, 'a');
    SCFG_CHECK_EQ(t, hex(sha256::digest(a)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

SCFG_TEST(Sha256Tests, PaddingBoundaries) {
    struct Row {
        size_t n;
        const char* digest;
    };
    const Row rows[] = {
        {55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
        {56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
        {63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
        {64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
        {65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
        {119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
        {120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
        {128, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6"},
    };
    for (const Row& r : rows) {
        t.check(hex(sha256::digest(pattern(r.n))) == r.digest, "sha256 of the " + std::to_string(r.n) + "-byte pattern");
    }
}

SCFG_TEST(Sha256Tests, BytesAndTextAgree) {
    const std::string text("\xff\0\x80 bytes", 9);
    std::vector<uint8_t> bytes(text.begin(), text.end());
    SCFG_CHECK_EQ(t, hex(sha256::digest(text)), hex(sha256::digest(bytes)));
}
