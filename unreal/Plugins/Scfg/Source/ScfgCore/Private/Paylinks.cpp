// Copyright 2026 Repulse
// Twin of scfg_core/paylinks.py (see Paylinks.h).
#include "Scfg/Paylinks.h"

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
