// Twin of scfg_core/paylinks.py (and of unity/.../Core/Paylinks.cs, rpgmaker/src/core/paylinks.ts).
// Payment links (EIP-681) and QR code geometry. The QR matrix comes from QrCodeGen.h (Project Nayuki's generator).
// Rendering is left to the engine: qr_rects merges each row's dark modules into horizontal runs, so the screen draws
// a few hundred rectangles instead of thousands of single squares.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scfg::paylinks {

// ERC-20 transfer request that mobile wallets understand when scanned or opened:
// "ethereum:<token_address>@<chain_id>/transfer?address=<recipient>&uint256=<units>", the text inserted as it is and
// the ints in decimal. An int of more than 4300 digits is str()'s ValueError (py::int_str), as in Python.
Result<std::string> eip681_transfer_uri(std::string_view token_address, const BigInt& chain_id,
                                        std::string_view recipient, const BigInt& units);

// Square matrix of modules, matrix[y][x], true = dark. Medium error correction (boosted when the version does not
// grow), the best mask. DataTooLongError (a ValueError) when the text fits no version; a lone surrogate is
// UnicodeEncodeError's ValueError.
Result<std::vector<std::vector<bool>>> qr_matrix(std::string_view text);

// One dark run of qr_rects: Python's (x, y, width) tuple, the border already added. An aggregate, so
// `for (auto [x, y, w] : rects.runs)` reads as Python's `for x, y, w in runs`.
struct QrRun {
    int64_t x = 0;
    int64_t y = 0;
    int64_t width = 0;
    friend bool operator==(const QrRun&, const QrRun&) = default;
};

// Python's (size, runs) tuple: `auto [size, runs] = rects.value();` reads as `size, runs = qr_rects(text)`.
struct QrRects {
    int64_t size = 0;         // modules per side, the quiet-zone border on both sides included
    std::vector<QrRun> runs;  // row by row, left to right
};

// The dark runs of qr_matrix(text), each row's adjacent dark modules merged into one, shifted by border; qr_matrix's
// errors.
Result<QrRects> qr_rects(std::string_view text, int64_t border = 2);

}  // namespace scfg::paylinks
