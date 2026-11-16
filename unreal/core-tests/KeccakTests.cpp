// No Python twin: tests of scfg::keccak beyond tier1/keccak.json: the empty-string digest every Ethereum library
// prints, the Transfer event signature, the rate boundaries (one and two 136-byte blocks, where the padding needs
// a block of its own) and one million 'a'. Every digest is what scfg_core.keccak.keccak256(m).hex() prints.
#include "support/Test.h"

#include "Scfg/Bytes.h"
#include "Scfg/Keccak.h"

#include <cstdint>
#include <string>
#include <vector>

namespace keccak = scfg::keccak;

static std::string hex(const std::array<uint8_t, 32>& d) { return scfg::bytes::to_hex(d); }

// bytes((i * 7 + 3) % 256 for i in range(n))
static std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>((i * 7 + 3) % 256);
    return out;
}

SCFG_TEST(KeccakTests, EmptyStringAndTransferSignature) {
    SCFG_CHECK_EQ(t, hex(keccak::keccak256(std::vector<uint8_t>())),
                  "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
    SCFG_CHECK_EQ(t, hex(keccak::keccak256(std::string_view(""))),
                  "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
    SCFG_CHECK_EQ(t, hex(keccak::keccak256(std::string_view("Transfer(address,address,uint256)"))),
                  "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef");
}

SCFG_TEST(KeccakTests, RateBoundaries) {
    struct Row {
        size_t n;
        const char* digest;
    };
    const Row rows[] = {
        {135, "00ef96af9cf4b24c7f269d922294444a197d0a33638c2e56634c57e892103a8f"},
        {136, "742061bcad767ed4c4f5883b1dcb1aad11afdcc140dc469d953759b127b9f9ed"},
        {137, "e3371f61e770abf254c34239c3b0099ad90594507415bc81dd0a10b9692bbf2a"},
        {271, "4401c4afbe16ff911bdbf2d38e556e5b861f3fdf0f9d4306b1c46f6ae4f73584"},
        {272, "ac141fd7b0a0ffcd2e967254d508da3ec616596493c36fa304425647d90e6de5"},
        {273, "16192ea86793083e47731cb3c970600f04768414d92bc0540e54ce8607a0fce0"},
    };
    for (const Row& r : rows) {
        t.check(hex(keccak::keccak256(pattern(r.n))) == r.digest, "keccak256 of the " + std::to_string(r.n) + "-byte pattern");
    }
}

SCFG_TEST(KeccakTests, OneMillionA) {
    SCFG_CHECK_EQ(t, hex(keccak::keccak256(std::string(1000000, 'a'))),
                  "fadae6b49f129bbb812be8407b7b2894f34aecf6dbd1f9b0f0c7e9853098fc96");
}
