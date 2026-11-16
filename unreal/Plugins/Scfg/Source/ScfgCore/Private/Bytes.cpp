// No Python twin: Python's bytes.hex() and bytes.fromhex().
#include "Scfg/Bytes.h"

#include <charconv>

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
            char pos[24];
            std::to_chars_result r = std::to_chars(pos, pos + sizeof pos, i);
            return Err::value_error("non-hexadecimal number found in fromhex() arg at position " +
                                    std::string(pos, r.ptr));
        }
        out[i / 2] = uint8_t(out[i / 2] << 4 | d);
    }
    return out;
}

}  // namespace scfg::bytes
