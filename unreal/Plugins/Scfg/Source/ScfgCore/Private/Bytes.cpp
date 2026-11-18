// Copyright 2026 Repulse
// No Python twin: Python's bytes.hex(), bytes.fromhex() and base64.b64encode().
#include "Scfg/Bytes.h"

#include "Scfg/Py.h"

namespace scfg::bytes {
namespace detail {

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace detail

std::string to_hex(std::span<const uint8_t> b) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (uint8_t v : b) {
        out += kDigits[v >> 4];
        out += kDigits[v & 0xf];
    }
    return out;
}

Result<std::vector<uint8_t>> from_hex(std::string_view s) {
    if (s.size() % 2 != 0) {
        return Err::value_error("fromhex() arg must contain an even number of hexadecimal digits");
    }
    std::vector<uint8_t> out(s.size() / 2);
    for (size_t i = 0; i < s.size(); ++i) {
        int d = detail::nibble(s[i]);
        if (d < 0) {
            return Err::value_error("non-hexadecimal number found in fromhex() arg at position " + py::dec(i));
        }
        out[i / 2] = uint8_t(out[i / 2] << 4 | d);
    }
    return out;
}

std::string b64encode(std::span<const uint8_t> b) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((b.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= b.size(); i += 3) {
        const uint32_t n = uint32_t(b[i]) << 16 | uint32_t(b[i + 1]) << 8 | uint32_t(b[i + 2]);
        out += kAlphabet[n >> 18 & 63];
        out += kAlphabet[n >> 12 & 63];
        out += kAlphabet[n >> 6 & 63];
        out += kAlphabet[n & 63];
    }
    const size_t rest = b.size() - i;
    if (rest > 0) {
        const uint32_t n = uint32_t(b[i]) << 16 | (rest == 2 ? uint32_t(b[i + 1]) << 8 : 0u);
        out += kAlphabet[n >> 18 & 63];
        out += kAlphabet[n >> 12 & 63];
        out += rest == 2 ? kAlphabet[n >> 6 & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace scfg::bytes
