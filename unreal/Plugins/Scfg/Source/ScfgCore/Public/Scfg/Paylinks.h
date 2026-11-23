// Copyright 2026 Repulse
// Twin of scfg_core/paylinks.py (and of unity/.../Core/Paylinks.cs, rpgmaker/src/core/paylinks.ts).
// Payment links (EIP-681), the "Pay with card" page link (onramp) and QR code geometry. The QR matrix comes from QrCodeGen.h
// (Project Nayuki's generator). Rendering is left to the engine: qr_rects merges each row's dark modules into
// horizontal runs, so the screen draws a few hundred rectangles instead of thousands of single squares.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Export.h"
#include "Scfg/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scfg::paylinks {

// The "Pay with card" page (configure's onramp): a template whose {placeholders} are filled in for the network the
// player picked and the payment's session. The default is thirdweb's hosted Checkout page, which needs no account or
// key: the player pays by card and thirdweb sends the token to `seller`, the developer's wallet, on that network.
// {amount} is the session's exact amount with all 6 decimals, so the game finds the payment as it finds any other
// (amounts::encode). paymentMethods=card offers card payment only (paylinks.py). NOT YET CHECKED with a real card:
// that thirdweb delivers the amount exactly, to 6 decimals.
inline constexpr std::string_view ONRAMP_URL =
    "https://thirdweb.com/bridge/checkout-widget?chain={chain_id}&tokenAddress={token_address}&amount={amount}"
    "&seller={recipient}&paymentMethods=card";
inline constexpr std::array<std::string_view, 6> ONRAMP_FIELDS = {"amount",    "chain", "chain_id",
                                                                  "recipient", "token", "token_address"};

// One match of Python's ONRAMP_PLACEHOLDER, \{([^{}]*)\}: [start, end) in the template and the name between the
// braces.
struct OnrampPlaceholder {
    size_t start = 0;
    size_t end = 0;
    std::string name;
};

// The matches of ONRAMP_PLACEHOLDER, left to right without overlaps: a '{' whose next brace is a '}'.
SCFGCORE_API std::vector<OnrampPlaceholder> onramp_placeholders(std::string_view tmpl);

// Fill a validated onramp template (config's onramp url check). Every value is made of URL-safe characters (a chain
// key, digits, a 0x address, a token symbol, an amount), so none is escaped. A placeholder outside ONRAMP_FIELDS is
// Python's KeyError.
SCFGCORE_API Result<std::string> onramp_url(std::string_view tmpl, std::string_view chain, const BigInt& chain_id,
                                            std::string_view token, std::string_view token_address,
                                            std::string_view amount, std::string_view recipient);

// ERC-20 transfer request that mobile wallets understand when scanned or opened:
// "ethereum:<token_address>@<chain_id>/transfer?address=<recipient>&uint256=<units>", the text inserted as it is and
// the ints in decimal. An int of more than 4300 digits is str()'s ValueError (py::int_str), as in Python.
SCFGCORE_API Result<std::string> eip681_transfer_uri(std::string_view token_address, const BigInt& chain_id,
                                                     std::string_view recipient, const BigInt& units);

// Square matrix of modules, matrix[y][x], true = dark. Medium error correction (boosted when the version does not
// grow), the best mask. DataTooLongError (a ValueError) when the text fits no version; a lone surrogate is
// UnicodeEncodeError's ValueError.
SCFGCORE_API Result<std::vector<std::vector<bool>>> qr_matrix(std::string_view text);

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
SCFGCORE_API Result<QrRects> qr_rects(std::string_view text, int64_t border = 2);

}  // namespace scfg::paylinks
