// No Python twin: Python's bytes.hex() and bytes.fromhex() (without its whitespace skipping), and
// base64.b64encode(b).decode("ascii").
#pragma once

#include "Scfg/Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scfg::bytes {

// b.hex(): two lowercase digits per byte.
std::string to_hex(std::span<const uint8_t> b);
// bytes.fromhex(s) for an even number of ASCII hex digits of either case and nothing else (no spaces, no 0x);
// anything else is a ValueError.
Result<std::vector<uint8_t>> from_hex(std::string_view s);

// base64.b64encode(b).decode("ascii"): RFC 4648's standard alphabet (+ and /), padded with = to a multiple of 4.
std::string b64encode(std::span<const uint8_t> b);

}  // namespace scfg::bytes
