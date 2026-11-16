// Twin of scfg_core/solana/paylinks.py (and of unity/.../Core/Solana/Paylinks.cs, rpgmaker/.../solana/paylinks.ts).
// Solana Pay transfer-request links (https://docs.solanapay.com/spec).
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Result.h"

#include <string>
#include <string_view>

namespace scfg::solana::paylinks {

// "solana:<recipient>?amount=..&spl-token=..&reference=..&label=..&message=..". The amount is format_units(units),
// so it always carries 6 decimals and the item code in it survives. Each value is urllib.parse.quote(v, safe=''):
// UTF-8 percent-encoded, only A-Za-z0-9_.-~ kept, so "/", "&", "=", "?" and spaces are encoded too. An empty value
// is left out with its key. The recipient is inserted as it is. A lone surrogate in a quoted value is
// UnicodeEncodeError's ValueError, as in Python.
Result<std::string> solana_pay_uri(std::string_view recipient, const BigInt& units, std::string_view mint,
                                   std::string_view reference, std::string_view label, std::string_view message);

}  // namespace scfg::solana::paylinks
