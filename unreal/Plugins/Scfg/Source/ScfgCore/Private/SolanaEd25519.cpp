// Twin of scfg_core/solana/ed25519.py: point decompression on BigInt mod 2**255 - 19.
#include "Scfg/SolanaEd25519.h"

#include <array>
#include <cstddef>

namespace scfg::solana::ed25519::detail {

// x % P(), which never fails (P() is not zero).
BigInt mod_p(const BigInt& x) { return x.mod(P()).value(); }

// pow(b, e, P()).
BigInt pow_p(const BigInt& b, const BigInt& e) { return b.pow_mod(e, P()).value(); }

}  // namespace scfg::solana::ed25519::detail

namespace scfg::solana::ed25519 {

const BigInt& P() {
    static const BigInt p = (BigInt(1) << 255) - BigInt(19);
    return p;
}

const BigInt& D() {
    // D = (-121665 * pow(121666, P - 2, P)) % P
    static const BigInt d = detail::mod_p(BigInt(-121665) * detail::pow_p(BigInt(121666), P() - BigInt(2)));
    return d;
}

bool is_on_curve(std::span<const uint8_t> point) {
    if (point.size() != 32) return false;
    // int.from_bytes(point, "little") & ((1 << 255) - 1): the sign bit is the top bit of the last byte.
    std::array<uint8_t, 32> be{};
    for (size_t i = 0; i < 32; ++i) be[i] = point[31 - i];
    be[0] &= 0x7F;
    BigInt y = detail::mod_p(BigInt::from_bytes_be(be));
    BigInt yy = detail::mod_p(y * y);
    BigInt u = detail::mod_p(yy - BigInt(1));
    BigInt v = detail::mod_p(D() * yy + BigInt(1));  // never 0: d is not a square mod p
    BigInt x2 = detail::mod_p(u * detail::pow_p(v, P() - BigInt(2)));
    if (x2.is_zero()) return true;
    static const BigInt half = (P() - BigInt(1)) >> 1;  // (P - 1) // 2
    return detail::pow_p(x2, half) == BigInt(1);
}

}  // namespace scfg::solana::ed25519
