// Twin of scfg_core/restore.py (see Restore.h): classify and classify_or_skip; lookup and apply are stubs until
// task 13.
#include "Scfg/Restore.h"

#include "Scfg/Amounts.h"
#include "Scfg/Dec.h"
#include "Scfg/Err.h"

#include <utility>

namespace scfg::restore {

Result<Classified> classify(const config::Config& config, const BigInt& value) {
    SCFG_TRY(amounts::Decoded decoded, amounts::decode(value));
    const Dec amount = amounts::units_to_decimal(value);
    std::vector<std::string> affordable;
    for (const auto& [id, item] : config.items) {
        (void)id;
        if (item.min_price <= amount) affordable.push_back(item.item_id);
    }
    if (decoded.code == 0 && decoded.nonce == 0) {
        // A whole-cent amount carries no code at all: could be an exchange-rounded purchase or a plain tip. Let the
        // player say which.
        affordable.emplace_back(TIP_CHOICE);
        return Classified{std::nullopt, std::nullopt, std::move(affordable)};
    }
    if (decoded.code == 0) return Classified{std::string("tip"), std::nullopt, {}};
    const models::Item* item = config.item_by_code(decoded.code);
    if (item != nullptr && amount >= item->min_price) return Classified{std::string("unlock"), item->item_id, {}};
    affordable.emplace_back(TIP_CHOICE);
    return Classified{std::nullopt, std::nullopt, std::move(affordable)};
}

Result<std::optional<Classified>> classify_or_skip(const config::Config& config, const BigInt& value,
                                                   const backends::base::ErrorSink& on_error) {
    Result<Classified> c = classify(config, value);
    if (c.ok()) return std::optional<Classified>(c.take());
    if (!c.err().is_a("InvalidOperation")) return std::move(c).err();
    if (on_error) {
        on_error(c.err());
    } else {
        errors::print_error(c.err());
    }
    return std::optional<Classified>();
}

Co<std::vector<RestoreCandidate>> lookup(Json tx_hash, config::Config config, OrderedMap<chains::Chain> chains,
                                         backends::base::RpcFor rpc_for, Json receipts,
                                         backends::base::ErrorSink on_error) {
    (void)tx_hash, (void)config, (void)chains, (void)rpc_for, (void)receipts, (void)on_error;
    co_return Err::make("NotImplementedError", "restore lookup: task 13");
}

Result<Json> apply(Json::Dict& receipts, const std::vector<RestoreCandidate>& candidates,
                   const std::optional<std::map<int64_t, Json>>& choices) {
    (void)receipts, (void)candidates, (void)choices;
    return Err::make("NotImplementedError", "restore apply: task 13");
}

}  // namespace scfg::restore
