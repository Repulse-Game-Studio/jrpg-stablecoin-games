// Twin of scfg_core/solana/paylinks.py (see SolanaPaylinks.h).
#include "Scfg/SolanaPaylinks.h"

#include "Scfg/Amounts.h"
#include "Scfg/Py.h"

#include <array>
#include <utility>

namespace scfg::solana::paylinks {

Result<std::string> solana_pay_uri(std::string_view recipient, const BigInt& units, std::string_view mint,
                                   std::string_view reference, std::string_view label, std::string_view message) {
    const std::string amount = amounts::format_units(units);
    const std::array<std::pair<std::string_view, std::string_view>, 5> params = {{
        {"amount", amount},
        {"spl-token", mint},
        {"reference", reference},
        {"label", label},
        {"message", message},
    }};
    std::string query;
    for (const auto& [k, v] : params) {
        if (v.empty()) continue;
        SCFG_TRY(std::string quoted, py::quote(v, ""));
        if (!query.empty()) query += '&';
        query += k;
        query += '=';
        query += quoted;
    }
    std::string uri = "solana:";
    uri += recipient;
    uri += '?';
    uri += query;
    return uri;
}

}  // namespace scfg::solana::paylinks
