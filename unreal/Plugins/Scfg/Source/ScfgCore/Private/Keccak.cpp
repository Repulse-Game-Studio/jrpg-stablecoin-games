// Twin of scfg_core/keccak.py and scfg_core/_xkcp.py: keccak256 and the Keccak-f[1600] permutation.
#include "Scfg/Keccak.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace scfg::keccak::detail {

constexpr size_t RATE_BYTES = 1088 / 8;  // _RATE_BITS; the capacity is 512 bits
constexpr uint8_t KECCAK_SUFFIX = 0x01;  // NIST SHA3 uses 0x06; Ethereum uses the original Keccak 0x01

using Lanes = std::array<uint64_t, 25>;  // lane (x, y) is a[x + 5 * y], as _xkcp.py's state[8*(x+5*y)...]

uint64_t rol64(uint64_t a, unsigned n) {
    n %= 64;
    return n == 0 ? a : (a << n) | (a >> (64 - n));
}

// _xkcp.py's KeccakF1600onLanes, with the round constants and rotation offsets computed as it computes them.
void keccak_f1600(Lanes& a) {
    unsigned r = 1;  // the LFSR of the iota step
    for (int round = 0; round < 24; ++round) {
        // theta
        uint64_t c[5];
        uint64_t d[5];
        for (int x = 0; x < 5; ++x) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (int x = 0; x < 5; ++x) d[x] = c[(x + 4) % 5] ^ rol64(c[(x + 1) % 5], 1);
        for (int i = 0; i < 25; ++i) a[i] ^= d[i % 5];
        // rho and pi
        int x = 1;
        int y = 0;
        uint64_t current = a[x + 5 * y];
        for (int t = 0; t < 24; ++t) {
            int nx = y;
            int ny = (2 * x + 3 * y) % 5;
            x = nx;
            y = ny;
            uint64_t next = a[x + 5 * y];
            a[x + 5 * y] = rol64(current, static_cast<unsigned>((t + 1) * (t + 2) / 2));
            current = next;
        }
        // chi
        for (int row = 0; row < 5; ++row) {
            uint64_t tt[5];
            for (int k = 0; k < 5; ++k) tt[k] = a[k + 5 * row];
            for (int k = 0; k < 5; ++k) a[k + 5 * row] = tt[k] ^ (~tt[(k + 1) % 5] & tt[(k + 2) % 5]);
        }
        // iota
        for (int j = 0; j < 7; ++j) {
            r = ((r << 1) ^ ((r >> 7) * 0x71)) % 256;
            if ((r & 2) != 0) a[0] ^= uint64_t(1) << ((1u << j) - 1);
        }
    }
}

// XORs one RATE_BYTES block into the state's lanes, little-endian (load64).
void absorb(Lanes& a, const uint8_t* block) {
    for (size_t i = 0; i < RATE_BYTES / 8; ++i) {
        uint64_t lane = 0;
        for (size_t k = 8; k-- > 0;) lane = (lane << 8) | block[8 * i + k];
        a[i] ^= lane;
    }
}

}  // namespace scfg::keccak::detail

namespace scfg::keccak {

std::array<uint8_t, 32> keccak256(std::span<const uint8_t> data) {
    using namespace detail;
    Lanes a{};
    size_t offset = 0;
    // Absorb all whole blocks.
    for (; data.size() - offset >= RATE_BYTES; offset += RATE_BYTES) {
        absorb(a, data.data() + offset);
        keccak_f1600(a);
    }
    // Pad the last (possibly empty) partial block: the suffix after the data, 0x80 on the last rate byte. The suffix
    // has no 0x80 bit, so the padding always fits this block (_xkcp.py's extra permutation never happens).
    std::array<uint8_t, RATE_BYTES> last{};
    size_t rest = data.size() - offset;
    for (size_t i = 0; i < rest; ++i) last[i] = data[offset + i];
    last[rest] ^= KECCAK_SUFFIX;
    last[RATE_BYTES - 1] ^= 0x80;
    absorb(a, last.data());
    keccak_f1600(a);
    // Squeeze 32 bytes, less than one rate block (store64).
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(a[i / 8] >> (8 * (i % 8)));
    return out;
}

std::array<uint8_t, 32> keccak256(std::string_view data) {
    std::vector<uint8_t> bytes(data.begin(), data.end());  // a copy: no cast between char and uint8_t pointers
    return keccak256(std::span<const uint8_t>(bytes));
}

}  // namespace scfg::keccak
