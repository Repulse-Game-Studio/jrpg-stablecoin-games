// Copyright 2026 Repulse
// No Python twin: Python's `secrets` module as the payment core uses it (token_hex, randbelow, token_bytes, and
// token_urlsafe for the checkout page's secret).
//
// Randomness is injected, never global: sessions draw randbelow(100) for a nonce and then token_hex(8) for the session
// id, and the Solana backend adds token_bytes(32) for its reference, in that order, so the Tier 2 runner can script
// the draws with an Entropy of its own. SystemEntropy is the OS source; the Unreal glue may supply another.
#pragma once

#include "Scfg/Export.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace scfg {

class SCFGCORE_API Entropy {
public:
    virtual ~Entropy() = default;

    // secrets.token_bytes(n): n random bytes. Every caller passes a constant (8 or 32); a negative n gives no bytes,
    // where Python raises ValueError("negative argument not allowed").
    //
    // The contract every implementation keeps (a scripted one included): exactly n bytes for n >= 0. randbelow's
    // default relies on it: it masks the draw to the bits it needs and draws again while the value is >= n, so a
    // short answer would bias the draw and a constant one >= n would loop forever.
    virtual std::vector<uint8_t> token_bytes(int n) = 0;
    // secrets.token_hex(n): 2n lowercase hex digits. By default the hex of token_bytes(n).
    virtual std::string token_hex(int n);
    // secrets.token_urlsafe(n): token_bytes(n) in base64url (- and _ for + and /) without the = padding, so 16 bytes
    // (local_server.py's 128-bit secret) are 22 characters. By default made from token_bytes(n).
    virtual std::string token_urlsafe(int n);
    // secrets.randbelow(n): a uniform integer in [0, n), or ValueError("Upper bound must be positive.") for n <= 0. By
    // default rejection sampling on token_bytes (CPython's _randbelow): the bit length of n - 1 is drawn, and a
    // draw >= n is thrown away and drawn again, so there is no modulo bias.
    // It relies on token_bytes' contract above.
    virtual Result<int64_t> randbelow(int64_t n);
};

// The operating system's random source: one device for the whole program, opened on first use.
class SCFGCORE_API SystemEntropy final : public Entropy {
public:
    std::vector<uint8_t> token_bytes(int n) override;
};

}  // namespace scfg
