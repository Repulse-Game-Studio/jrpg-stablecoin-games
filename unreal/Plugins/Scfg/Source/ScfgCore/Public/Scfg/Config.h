// Copyright 2026 Repulse
// Twin of scfg_core/config.py (and of unity/.../Core/Config.cs, rpgmaker/src/core/config.ts).
// Developer configuration: validation of configure()/add_item() arguments.
//
// Every mistake raises ConfigError at startup, so it surfaces while the developer is testing instead of when a
// player tries to pay.
//
// Python's keyword arguments arrive as a Json dict (kwargs), its values loosely typed as Python's are: an unknown key
// is a TypeError, as Python's unexpected keyword argument is, and so is a missing required one. Every check runs in
// config.py's order, because the first failing check decides the ConfigError code, and the non-ConfigError failures
// are Python's own classes: a truthy non-dict methods, rpc_urls or confirmations is the AttributeError of its
// .items(), a list where a set or dict key is needed the TypeError "unhashable type", a non-iterable chains/tokens/
// previous_wallets the TypeError "'int' object is not iterable". Text lengths count code points.
//
// Port limit: py::format1 refuses format specs, indexing and attribute access, so an explorer_tx whose only "{}" is
// inside an escaped "{{}}" and that also holds a field such as "{:>5}" is chain.bad_explorer_tx_braces here, where
// Python accepts it (or raises AttributeError for "{.x}").
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Paylinks.h"
#include "Scfg/Result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scfg::config {

inline constexpr std::array<std::string_view, 2> TOKENS = {"USDC", "USDT"};
inline constexpr std::array<std::string_view, 3> FEE_HINTS = {"lowest", "low", "high"};
inline constexpr std::string_view DEFAULT_ACCENT = "#0e7c74";
inline constexpr std::string_view INK = "#17202e";
// Raster formats only: an SVG served from the checkout page's origin could run script.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4> LOGO_TYPES = {{
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".webp", "image/webp"},
}};

// copy.deepcopy(DEFAULT_METHODS): a new dict each call,
// {"browser": {"enabled": True, "timeout_min": 30}, "qr": {"enabled": True, "wallet_button": True, "chains": None},
//  "manual": {"enabled": True, "allow_tx_paste": True}}.
SCFGCORE_API Json DEFAULT_METHODS();

// The longest onramp url template (config.py's ONRAMP_URL_LIMIT; Config.cpp's onramp.bad_url message spells it).
inline constexpr size_t ONRAMP_URL_LIMIT = 2048;

// configure's onramp, merged and checked: the "No USDC yet? Buy some with a card" link. Python's Config.onramp dict
// {"enabled", "url", "chains"}; a Config made without build_config has it off (the dataclass default).
struct SCFGCORE_API Onramp {
    bool enabled = false;
    std::string url = std::string(paylinks::ONRAMP_URL);
    std::vector<std::string> chains;  // the chain keys the buy link is offered on, in order
};

// Python's Config dataclass (mutable: add_item adds to items).
struct SCFGCORE_API Config {
    std::optional<std::string> wallet;  // EVM payout address; None for a Solana-only game
    bool testnet = false;
    std::vector<std::string> chains;
    std::vector<std::string> tokens;
    Json methods;  // dict[str, dict]: the merged method options, values as the developer gave them
    OrderedMap<std::vector<std::string>> rpc_urls;
    OrderedMap<BigInt> confirmations;
    std::vector<std::string> previous_wallets;
    std::optional<std::string> solana_wallet;
    std::vector<std::string> previous_solana_wallets;
    std::optional<std::string> logo;  // image path, shown on the screens and the checkout page
    std::string accent = std::string(DEFAULT_ACCENT);  // #rrggbb used for selected options
    OrderedMap<chains::Chain> custom_chains;           // add_chain(), this network
    Onramp onramp;                                     // the buy link (build_config resolves it)
    OrderedMap<models::Item> items;

    // Every chain this game knows on its network: built-in, then the developer's own (a new dict).
    OrderedMap<chains::Chain> registry() const;
    // EVM: current wallet first, then wallets used by earlier versions of the game.
    std::vector<std::string> accepted_wallets() const;
    std::vector<std::string> accepted_wallets_for(std::string_view family) const;
    std::optional<std::string> wallet_for(const chains::Chain& chain) const;
    // The item with this code, or nullptr (valid until items changes).
    const models::Item* item_by_code(int64_t code) const;
};

// A developer-defined EVM chain (scfg.add_chain), from build_chain's keyword arguments, key included: key, name,
// chain_id, rpc, tokens, confirmations, explorer_tx (required), native_symbol="ETH", fee_hint="low",
// max_log_range=1000, testnet=False. Validated like the rest of the configuration, so a typo fails at startup
// rather than when a player pays.
//
// Only EVM chains work: payments are found through ERC-20 Transfer logs over Ethereum JSON-RPC. The tokens must have
// 6 decimals, because the item code and nonce live in the last four decimals of the amount.
SCFGCORE_API Result<chains::Chain> build_chain(const Json& kwargs);

// One entry of build_config's custom_chains: a Chain from build_chain, or any other value, which is the ConfigError
// chain.not_from_add_chain (Python's `not isinstance(chain, Chain)`).
using CustomChain = std::variant<chains::Chain, Json>;

// build_config's keyword arguments: wallet, testnet, chains, tokens, methods, rpc_urls, confirmations,
// previous_wallets, logo, accent, custom_chains, solana_wallet and onramp (a dict of enabled, url and chains: the buy
// link), all optional. A Json cannot hold a Chain, so
// kwargs' own custom_chains (if any) is iterated as Python iterates it and every entry is not a Chain; the second
// overload passes the list beside kwargs, which must not also name it (a TypeError, Python's "got multiple values").
// Custom chains of the other network are ignored unless chains names them.
SCFGCORE_API Result<Config> build_config(const Json& kwargs);
SCFGCORE_API Result<Config> build_config(const Json& kwargs, std::vector<CustomChain> custom_chains);

// The content type of a logo path by its extension (lowercased): a ConfigError logo.bad_type for anything but
// .png, .jpg, .jpeg and .webp.
SCFGCORE_API Result<std::string> logo_content_type(std::string_view path);

// White or ink, whichever has the higher WCAG contrast on `color` (#rrggbb). Each channel is int(color[i:i+2], 16),
// so a color that is not one is that int()'s ValueError.
SCFGCORE_API Result<std::string> text_on(std::string_view color);

// Chains the player may pay on, in display order, filtered to enabled tokens, with developer RPC URLs tried first
// and confirmation overrides applied. A chain missing from the registry (the driver removed it after build_config)
// is Python's KeyError.
SCFGCORE_API Result<OrderedMap<chains::Chain>> enabled_chains(const Config& config);

// The enabled chains that carry token, in display order.
SCFGCORE_API Result<std::vector<std::string>> chains_for_token(const Config& config, std::string_view token);

// add_item(config, item_id, *, code, price, token, title, min_price=None): validates, adds the item to config.items
// and returns it. item_id is the positional argument, any value (a non-str is item.invalid_id); kwargs holds the
// keyword arguments.
SCFGCORE_API Result<models::Item> add_item(Config& config, const Json& item_id, const Json& kwargs);

}  // namespace scfg::config
