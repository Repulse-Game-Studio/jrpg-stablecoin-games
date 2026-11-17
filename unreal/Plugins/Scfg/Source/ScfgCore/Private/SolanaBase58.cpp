// Twin of scfg_core/solana/base58.py: base58 encode/decode and the fixed-length Solana forms.
#include "Scfg/SolanaBase58.h"

#include "Scfg/Err.h"
#include "Scfg/Py.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace scfg::solana::base58::detail {

// math.log(256) and math.log(58) as CPython computes them (bit for bit), so the limit is the same double whatever
// this platform's libm rounds log() to.
inline constexpr double kLog256 = 0x1.62e42fefa39efp+2;  // 5.545177444479562
inline constexpr double kLog58 = 0x1.03de4c5c6464dp+2;   // 4.060443010546419

// The alphabet index of a code point, or -1.
int digit_of(uint32_t cp) {
    if (cp >= 128) return -1;
    for (size_t i = 0; i < ALPHABET.size(); ++i) {
        if (static_cast<uint32_t>(static_cast<unsigned char>(ALPHABET[i])) == cp) return static_cast<int>(i);
    }
    return -1;
}

// float(n) for an int (py::to_float, correctly rounded as PyLong_AsDouble rounds it); nullopt for its OverflowError.
std::optional<double> to_double(const BigInt& n) {
    Result<double> f = py::to_float(py::Number{true, n, 0.0});
    if (!f.ok()) return std::nullopt;
    return f.value();
}

std::string decimal(size_t n) { return BigInt::from_uint64(static_cast<uint64_t>(n)).to_decimal(); }

// b58encode(decode_fixed(text.strip(), length)) for normalize_pubkey and normalize_signature.
Result<std::string> normalize(const Json& text, int64_t length, std::string_view what) {
    if (!text.is_str()) {
        SCFG_TRY(std::string shown, py::repr(text));
        return Err::value_error(std::string(what) + shown);
    }
    SCFG_TRY(std::vector<uint8_t> raw, decode_fixed(Json(py::strip(text.as_str())), Json(length)));
    return b58encode(raw);
}

}  // namespace scfg::solana::base58::detail

namespace scfg::solana::base58 {

std::string b58encode(std::span<const uint8_t> data) {
    size_t zeros = 0;
    while (zeros < data.size() && data[zeros] == 0) ++zeros;
    // The number's base58 digits, least significant first (divmod(n, 58) repeated, without a BigInt).
    std::vector<uint8_t> digits;
    digits.reserve(data.size() * 138 / 100 + 1);
    for (size_t i = zeros; i < data.size(); ++i) {
        uint32_t carry = data[i];
        for (uint8_t& d : digits) {
            carry += static_cast<uint32_t>(d) << 8;
            d = static_cast<uint8_t>(carry % 58);
            carry /= 58;
        }
        while (carry != 0) {
            digits.push_back(static_cast<uint8_t>(carry % 58));
            carry /= 58;
        }
    }
    std::string out(zeros, '1');
    out.reserve(zeros + digits.size());
    for (size_t i = digits.size(); i > 0; --i) out += ALPHABET[digits[i - 1]];
    return out;
}

Result<std::vector<uint8_t>> b58decode(const Json& text) {
    if (!text.is_str() || text.as_str().empty()) {
        SCFG_TRY(std::string shown, py::repr(text));
        return Err::value_error("not base58: " + shown);
    }
    std::string_view s = text.as_str();
    std::vector<uint8_t> values;
    values.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t start = i;
        int d = detail::digit_of(py::utf8_decode(s, i));
        if (d < 0) return Err::value_error("invalid base58 character " + py::repr_str(s.substr(start, i - start)));
        values.push_back(static_cast<uint8_t>(d));
    }
    size_t zeros = 0;
    while (zeros < values.size() && values[zeros] == 0) ++zeros;  // '1' is digit 0: text.lstrip("1")
    // n = n * 58 + digit, as big-endian bytes kept least significant first.
    std::vector<uint8_t> body;
    body.reserve(values.size() * 733 / 1000 + 1);
    for (uint8_t v : values) {
        uint32_t carry = v;
        for (uint8_t& b : body) {
            carry += static_cast<uint32_t>(b) * 58;
            b = static_cast<uint8_t>(carry & 0xFF);
            carry >>= 8;
        }
        while (carry != 0) {
            body.push_back(static_cast<uint8_t>(carry & 0xFF));
            carry >>= 8;
        }
    }
    std::vector<uint8_t> out(zeros, 0);
    out.reserve(zeros + body.size());
    for (size_t i = body.size(); i > 0; --i) out.push_back(body[i - 1]);
    return out;
}

std::optional<double> max_encoded_length(const BigInt& length) {
    std::optional<double> f = detail::to_double(length);
    if (!f) return std::nullopt;
    double x = *f * detail::kLog256 / detail::kLog58;
    if (std::isinf(x)) return std::nullopt;  // math.ceil(inf): OverflowError
    return std::ceil(x);
}

Result<std::vector<uint8_t>> decode_fixed(const Json& text, const Json& length) {
    if (!length.is_int()) {  // a bool is no int here
        SCFG_TRY(std::string shown, py::repr(length));
        return Err::value_error("not a byte count: " + shown);
    }
    BigInt n = length.as_bigint();
    std::optional<double> limit = max_encoded_length(n);
    if (!limit) {
        SCFG_TRY(std::string shown, py::repr(length));
        return Err::value_error("not a byte count: " + shown);
    }
    if (text.is_str()) {
        size_t chars = py::len(text.as_str());
        if (static_cast<double>(chars) > *limit) {
            // n converted to a finite float above, so it is below 2**1024: at most 309 digits
            return Err::value_error("too long for " + n.to_decimal() + " bytes: " + detail::decimal(chars) +
                                    " characters");
        }
    }
    SCFG_TRY(std::vector<uint8_t> raw, b58decode(text));
    if (BigInt::from_uint64(static_cast<uint64_t>(raw.size())) != n) {
        // n is below 2**1024 (see above): at most 309 digits; b58decode took text, so it is a str
        return Err::value_error("expected " + n.to_decimal() + " bytes, got " + detail::decimal(raw.size()) + ": " +
                                py::repr_str(text.as_str()));
    }
    return raw;
}

Result<std::string> normalize_pubkey(const Json& text) { return detail::normalize(text, 32, "not an address: "); }

Result<std::string> normalize_signature(const Json& text) { return detail::normalize(text, 64, "not a signature: "); }

}  // namespace scfg::solana::base58
