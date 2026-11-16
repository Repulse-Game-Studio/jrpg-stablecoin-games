// Twin of scfg_core/payments.py: prepare_payment, check for check in Python's order.
#include "Scfg/Payments.h"

#include "Scfg/Amounts.h"
#include "Scfg/BigInt.h"
#include "Scfg/Err.h"
#include "Scfg/Py.h"

#include <string>
#include <utility>
#include <vector>

namespace scfg::payments::detail {

Dec dec(int64_t cents) { return Dec(BigInt(cents), -2, false); }

bool is_str(const Json& v, std::string_view s) { return v.is_str() && v.as_str() == s; }

}  // namespace scfg::payments::detail

namespace scfg::payments {

const Dec& MIN_TIP() {
    static const Dec d = detail::dec(50);
    return d;
}

const std::array<Dec, 4>& TIP_PRESETS() {
    static const std::array<Dec, 4> presets = {detail::dec(100), detail::dec(300), detail::dec(500), detail::dec(1000)};
    return presets;
}

Result<PaymentSpec> prepare_payment(const config::Config& config, const Json& kind, const Json& kwargs) {
    if (!kwargs.is_dict()) {
        return Err::type_error("prepare_payment() argument after ** must be a mapping, not " +
                               std::string(kwargs.type_name()));
    }
    for (const auto& [k, v] : kwargs.as_dict()) {
        if (k != "item" && k != "amount" && k != "token" && k != "title") {
            return Err::type_error("prepare_payment() got an unexpected keyword argument '" + k + "'");
        }
    }
    const Json::Dict& kw = kwargs.as_dict();
    Json item = kw.get("item");
    Json amount = kw.get("amount");
    Json token = kw.get("token");
    Json title = kw.get("title");

    PaymentSpec spec;
    if (detail::is_str(kind, "unlock")) {
        // `item not in config.items` hashes item: a list or dict is the TypeError.
        if (item.is_list() || item.is_dict()) {
            return Err::type_error("unhashable type: '" + std::string(item.type_name()) + "'");
        }
        const models::Item* it = item.is_str() ? config.items.find(item.as_str()) : nullptr;
        if (it == nullptr) {
            return Err::config_error("item.unknown",
                                     "unknown item " + py::repr(item) + "; declare it with scfg.add_item()");
        }
        Dec price = it->price;
        if (!amount.is_null()) {
            SCFG_TRY(price, amounts::parse_amount(amount));
        }
        if (price < it->min_price) {
            return Err::config_error("payment.below_min_price", "amount " + price.str() +
                                                                    " is below the item's min_price " +
                                                                    it->min_price.str());
        }
        if (!py::truthy(token)) token = Json(it->token);  // token or it.token
        spec.price = std::move(price);
        spec.code = it->code;
        spec.title = py::truthy(title) ? title : Json(it->title);
    } else if (detail::is_str(kind, "tip")) {
        if (amount.is_null() || token.is_null()) {
            return Err::config_error("tip.needs_amount_and_token", "tips need an amount and a token");
        }
        SCFG_TRY(Dec price, amounts::parse_amount(amount));
        if (price < MIN_TIP()) {
            return Err::config_error("tip.below_minimum", "tips must be at least " + MIN_TIP().str());
        }
        spec.price = std::move(price);
        spec.code = 0;
        spec.title = py::truthy(title) ? title : Json("Tip");
    } else {
        return Err::config_error("payment.bad_kind", "kind must be 'unlock' or 'tip', not " + py::repr(kind));
    }
    bool enabled = false;
    if (token.is_str()) {
        for (const std::string& t : config.tokens) enabled = enabled || t == token.as_str();
    }
    if (!enabled) return Err::config_error("token.not_enabled", "token " + py::repr(token) + " is not enabled");
    SCFG_TRY(std::vector<std::string> chains, config::chains_for_token(config, token.as_str()));
    if (chains.empty()) {
        return Err::config_error("token.unavailable", token.as_str() + " is not available on any enabled chain");
    }
    spec.kind = kind.as_str();
    spec.item = item;
    spec.token = token.as_str();
    spec.chains = std::move(chains);
    return spec;
}

}  // namespace scfg::payments
