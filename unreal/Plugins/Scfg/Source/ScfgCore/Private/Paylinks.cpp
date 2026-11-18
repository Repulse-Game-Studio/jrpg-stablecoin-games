// Copyright 2026 Repulse
// Twin of scfg_core/paylinks.py (see Paylinks.h).
#include "Scfg/Paylinks.h"

#include "Scfg/Err.h"
#include "Scfg/Py.h"
#include "Scfg/QrCodeGen.h"

namespace scfg::paylinks {

Result<std::string> eip681_transfer_uri(std::string_view token_address, const BigInt& chain_id,
                                        std::string_view recipient, const BigInt& units) {
    SCFG_TRY(std::string chain, py::int_str(chain_id));
    SCFG_TRY(std::string amount, py::int_str(units));
    std::string uri = "ethereum:";
    uri += token_address;
    uri += "@" + chain + "/transfer?address=";
    uri += recipient;
    uri += "&uint256=" + amount;
    return uri;
}

std::vector<OnrampPlaceholder> onramp_placeholders(std::string_view tmpl) {
    std::vector<OnrampPlaceholder> found;
    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] != '{') {
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < tmpl.size() && tmpl[j] != '{' && tmpl[j] != '}') ++j;
        if (j < tmpl.size() && tmpl[j] == '}') {
            found.push_back({i, j + 1, std::string(tmpl.substr(i + 1, j - i - 1))});
            i = j + 1;
        } else {
            i = j;  // no match at i; the next try is the next brace (nothing between can start one)
        }
    }
    return found;
}

Result<std::string> onramp_amount(const BigInt& units, bool gas_is_token) {
    const BigInt total = units + BigInt(gas_is_token ? ONRAMP_GAS_UNITS : 0);
    SCFG_TRY(BigInt down, (-total).floordiv(BigInt(10'000)));  // -(-total // 10_000): the ceiling
    const BigInt cents = -down;
    SCFG_TRY(BigInt whole, cents.floordiv(BigInt(100)));
    SCFG_TRY(BigInt rest, cents.mod(BigInt(100)));
    SCFG_TRY(std::string head, py::int_str(whole));
    SCFG_TRY(int64_t r, rest.to_int64());
    std::string out = head + ".";
    out += static_cast<char>('0' + r / 10);
    out += static_cast<char>('0' + r % 10);
    return out;
}

Result<std::string> onramp_url(std::string_view tmpl, std::string_view chain, const BigInt& chain_id,
                               std::string_view token, std::string_view token_address, std::string_view amount) {
    SCFG_TRY(std::string id, py::int_str(chain_id));
    std::string out;
    out.reserve(tmpl.size() + 64);
    size_t at = 0;
    for (const OnrampPlaceholder& p : onramp_placeholders(tmpl)) {
        out += tmpl.substr(at, p.start - at);
        if (p.name == "amount") {
            out += amount;
        } else if (p.name == "chain") {
            out += chain;
        } else if (p.name == "chain_id") {
            out += id;
        } else if (p.name == "token") {
            out += token;
        } else if (p.name == "token_address") {
            out += token_address;
        } else {
            return Err::key_error(py::repr_str(p.name));
        }
        at = p.end;
    }
    out += tmpl.substr(at);
    return out;
}

Result<std::vector<std::vector<bool>>> qr_matrix(std::string_view text) {
    using qrcodegen::QrCode;
    SCFG_TRY(QrCode qr, QrCode::encode_text(text, QrCode::Ecc::MEDIUM));
    const int n = qr.get_size();
    std::vector<std::vector<bool>> matrix(static_cast<size_t>(n), std::vector<bool>(static_cast<size_t>(n)));
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) matrix[static_cast<size_t>(y)][static_cast<size_t>(x)] = qr.get_module(x, y);
    }
    return matrix;
}

Result<QrRects> qr_rects(std::string_view text, int64_t border) {
    SCFG_TRY(std::vector<std::vector<bool>> matrix, qr_matrix(text));
    const size_t n = matrix.size();
    QrRects out;
    for (size_t y = 0; y < n; ++y) {
        const std::vector<bool>& row = matrix[y];
        size_t x = 0;
        while (x < n) {
            if (row[x]) {
                const size_t start = x;
                while (x < n && row[x]) ++x;
                out.runs.push_back({static_cast<int64_t>(start) + border, static_cast<int64_t>(y) + border,
                                    static_cast<int64_t>(x - start)});
            } else {
                ++x;
            }
        }
    }
    out.size = static_cast<int64_t>(n) + 2 * border;
    return out;
}

}  // namespace scfg::paylinks
