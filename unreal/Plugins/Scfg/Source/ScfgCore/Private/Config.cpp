// Twin of scfg_core/config.py: build_chain, build_config, enabled_chains and add_item, check for check in Python's
// order.
#include "Scfg/Config.h"

#include "Scfg/Amounts.h"
#include "Scfg/Dec.h"
#include "Scfg/Err.h"
#include "Scfg/Evm.h"
#include "Scfg/Py.h"
#include "Scfg/Rpc.h"
#include "Scfg/SolanaBase58.h"
#include "Scfg/SolanaEd25519.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scfg::config::detail {

// ---- Python's argument binding and built-ins ----

struct Param {
    std::string_view name;
    bool required = false;
    bool positional = false;  // positional-or-keyword (build_chain's key, build_config's wallet)
};

std::string type_name(const Json& v) { return std::string(v.type_name()); }

// "'a'", "'a' and 'b'", "'a', 'b', and 'c'": CPython's list of missing argument names.
std::string name_list(const std::vector<std::string_view>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += names.size() == 2 ? " and " : i + 1 == names.size() ? ", and " : ", ";
        out += "'" + std::string(names[i]) + "'";
    }
    return out;
}

std::string decimal(size_t n) { return BigInt::from_uint64(static_cast<uint64_t>(n)).to_decimal(); }

// fn(**kwargs) bound to fn's signature: a non-dict, an unexpected keyword and a missing required argument are the
// TypeErrors CPython raises, in that order (the missing positional ones before the keyword-only ones).
Result<const Json::Dict*> bind(const Json& kwargs, std::string_view fn, std::span<const Param> params) {
    std::string f(fn);
    if (!kwargs.is_dict()) {
        return Err::type_error(f + "() argument after ** must be a mapping, not " + type_name(kwargs));
    }
    const Json::Dict& d = kwargs.as_dict();
    for (const auto& [key, value] : d) {
        bool known = false;
        for (const Param& p : params) known = known || p.name == key;
        if (!known) return Err::type_error(f + "() got an unexpected keyword argument '" + key + "'");
    }
    for (bool positional : {true, false}) {
        std::vector<std::string_view> missing;
        for (const Param& p : params) {
            if (p.required && p.positional == positional && !d.contains(p.name)) missing.push_back(p.name);
        }
        if (missing.empty()) continue;
        std::string what = positional ? "positional" : "keyword-only";
        return Err::type_error(f + "() missing " + decimal(missing.size()) + " required " + what + " argument" +
                               (missing.size() == 1 ? "" : "s") + ": " + name_list(missing));
    }
    return &d;
}

// The value of an argument, or its default when it was not passed.
Json arg(const Json::Dict& d, std::string_view name, Json fallback = Json()) {
    const Json* v = d.find(name);
    return v != nullptr ? *v : fallback;
}

// `v in some_set` / `v in some_dict` hashes v: a list or dict is the TypeError.
Status hashable(const Json& v) {
    if (v.is_list() || v.is_dict()) return Err::type_error("unhashable type: '" + type_name(v) + "'");
    return ok();
}

// (v or {}).items(): nullptr for a falsy v, the dict for a dict, else the AttributeError.
Result<const Json::Dict*> items_or_empty(const Json& v) {
    if (!py::truthy(v)) return static_cast<const Json::Dict*>(nullptr);
    if (!v.is_dict()) return Err::make("AttributeError", "'" + type_name(v) + "' object has no attribute 'items'");
    return &v.as_dict();
}

// v.items(): the dict, else the AttributeError (even for a falsy v).
Result<const Json::Dict*> items_of(const Json& v) {
    if (!v.is_dict()) return Err::make("AttributeError", "'" + type_name(v) + "' object has no attribute 'items'");
    return &v.as_dict();
}

bool contains(const std::vector<std::string>& xs, std::string_view x) {
    for (const std::string& s : xs) {
        if (s == x) return true;
    }
    return false;
}

template <size_t N>
bool contains(const std::array<std::string_view, N>& xs, const Json& x) {
    if (!x.is_str()) return false;  // `x in TOKENS`: only a str equals a str
    for (std::string_view s : xs) {
        if (s == x.as_str()) return true;
    }
    return false;
}

// isinstance(v, int) and not isinstance(v, bool): Json keeps the two apart already.
bool is_int(const Json& v) { return v.is_int(); }

// re.fullmatch(r"[a-z0-9_-]{1,max}", v) (ASCII only, so bytes are code points; no trailing newline).
bool is_id(const Json& v, size_t max) {
    if (!v.is_str()) return false;
    std::string_view s = v.as_str();
    if (s.empty() || s.size() > max) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

// re.fullmatch(r"[A-Za-z0-9]{1,11}", v).
bool is_native_symbol(const Json& v) {
    if (!v.is_str()) return false;
    std::string_view s = v.as_str();
    if (s.empty() || s.size() > 11) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// s.count("{}") (non-overlapping).
size_t count_braces(std::string_view s) {
    size_t n = 0;
    for (size_t i = s.find("{}"); i != std::string_view::npos; i = s.find("{}", i + 2)) ++n;
    return n;
}

// ---- addresses ----

Result<std::string> address(const Json& value, std::string_view what) {
    Result<std::string> r = evm::normalize_address(value);
    if (!r.ok() && r.err().is_a("ValueError")) {
        return Err::config_error("address.invalid", std::string(what) + ": " + r.err().message);
    }
    return r;
}

Result<std::string> solana_address(const Json& value, std::string_view what) {
    Result<std::string> normalized = solana::base58::normalize_pubkey(value);
    if (!normalized.ok()) {
        if (!normalized.err().is_a("ValueError")) return normalized.err();
        return Err::config_error("solana_address.invalid",
                                 std::string(what) + ": not a Solana address: " + py::repr(value));
    }
    std::string a = normalized.take();
    SCFG_TRY(std::vector<uint8_t> raw, solana::base58::b58decode(Json(a)));
    if (!solana::ed25519::is_on_curve(raw)) {
        return Err::config_error("solana_address.not_on_curve",
                                 std::string(what) + ": " + a +
                                     " looks like a token account or program address; use your wallet address");
    }
    return a;
}

struct Wallet {
    std::string family;
    std::string address;
};

// (family, normalized address): 0x addresses are EVM, anything else Solana.
Result<Wallet> any_wallet(const Json& value, std::string_view what) {
    if (value.is_str()) {
        std::string stripped = py::strip(value.as_str());
        if (stripped.rfind("0x", 0) == 0) {
            SCFG_TRY(std::string a, address(Json(stripped), what));
            return Wallet{"evm", std::move(a)};
        }
    }
    SCFG_TRY(std::string a, solana_address(value, what));
    return Wallet{"solana", std::move(a)};
}

// ---- rpc urls and numbers ----

// re.match(r"^http://(127\.0\.0\.1|localhost)(:\d{1,5})?(/|\Z)", u), with \d the Unicode digits of CPython 3.9
// (py::is_decimal_digit) and \Z the very end of the string (no trailing newline).
bool is_local_http(std::string_view u) {
    constexpr std::string_view scheme = "http://";
    if (u.substr(0, scheme.size()) != scheme) return false;
    size_t p = scheme.size();
    std::string_view host = u.substr(p, 9);
    if (host != "127.0.0.1" && host != "localhost") return false;
    p += 9;
    if (p < u.size() && u[p] == ':') {
        size_t q = p + 1;
        size_t digits = 0;
        while (q < u.size()) {
            size_t next = q;
            if (!py::is_decimal_digit(py::utf8_decode(u, next))) break;
            ++digits;
            q = next;
        }
        // More than five digits cannot backtrack into a match: the character after any five is a digit, not "/".
        // Backtracking out of the optional group leaves ":", which is not "/" or the end either.
        if (digits < 1 || digits > 5) return false;
        return q == u.size() || u[q] == '/';
    }
    return p == u.size() || u[p] == '/';
}

Result<std::vector<std::string>> rpc_urls(const Json& value, std::string_view what) {
    // urls = [value] if isinstance(value, str) else value
    std::vector<Json> urls;
    if (value.is_str()) {
        urls.push_back(value);
    } else if (value.is_list()) {
        urls = value.as_list();
    }
    if (urls.empty()) {  // not a list or tuple, or empty
        return Err::config_error("rpc.bad_list", std::string(what) + " must be a URL or a non-empty list of URLs");
    }
    std::vector<std::string> out;
    out.reserve(urls.size());
    for (const Json& u : urls) {
        if (!u.is_str() || !(u.as_str().rfind("https://", 0) == 0 || is_local_http(u.as_str()))) {
            return Err::config_error("rpc.not_https", "rpc url must start with https://: " + py::repr(u));
        }
        out.push_back(u.as_str());
    }
    return out;
}

Result<BigInt> positive_int(const Json& value, std::string_view what, int64_t minimum = 1) {
    if (!is_int(value) || value.as_bigint() < BigInt(minimum)) {
        return Err::config_error("int.below_minimum",
                                 std::string(what) + " must be an int >= " + BigInt(minimum).to_decimal());
    }
    return value.as_bigint();
}

// ---- custom chains, logo, accent ----

struct Custom {
    OrderedMap<chains::Chain> mine;  // chains for this network keyed by key
    std::vector<std::string> other;  // keys only defined for the other network
};

Result<Custom> custom_chains(const std::vector<CustomChain>& custom, bool testnet) {
    const OrderedMap<chains::Chain>& builtin = chains::get_chains(testnet);
    Custom out;
    for (const CustomChain& entry : custom) {
        const chains::Chain* chain = std::get_if<chains::Chain>(&entry);
        if (chain == nullptr) {
            return Err::config_error("chain.not_from_add_chain", "custom chains must be created with scfg.add_chain()");
        }
        std::string key = py::repr_str(chain->key);
        if (chains::get_chains(false).contains(chain->key) || chains::get_chains(true).contains(chain->key)) {
            return Err::config_error("chain.is_builtin",
                                     "chain " + key +
                                         " is built in; use rpc_urls/confirmations to change it, or pick another key");
        }
        if (chain->testnet != testnet) {
            if (!contains(out.other, chain->key)) out.other.push_back(chain->key);
            continue;
        }
        if (out.mine.contains(chain->key)) return Err::config_error("chain.duplicate", "chain " + key + " added twice");
        const OrderedMap<chains::Chain>& mine = out.mine;
        for (const OrderedMap<chains::Chain>* group : {&builtin, &mine}) {
            for (const auto& [k, c] : *group) {
                if (c.chain_id == chain->chain_id) {
                    return Err::config_error("chain.duplicate_chain_id", "chain " + key + " has the same chain_id (" +
                                                                             chain->chain_id.to_decimal() + ") as " +
                                                                             py::repr_str(c.key));
                }
            }
        }
        out.mine.set(chain->key, *chain);
    }
    // other - set(mine)
    std::vector<std::string> other;
    for (const std::string& k : out.other) {
        if (!out.mine.contains(k)) other.push_back(k);
    }
    out.other = std::move(other);
    return out;
}

Result<std::optional<std::string>> logo(const Json& value) {
    if (value.is_null()) return std::optional<std::string>();
    bool bad = !value.is_str();
    if (!bad) {
        std::string_view s = value.as_str();
        bad = s.empty() || s.find('\\') != std::string_view::npos || s.find(':') != std::string_view::npos ||
              s[0] == '/';
        // ".." in value.split("/")
        for (size_t start = 0; !bad;) {
            size_t slash = s.find('/', start);
            std::string_view part = s.substr(start, slash == std::string_view::npos ? s.npos : slash - start);
            bad = part == "..";
            if (slash == std::string_view::npos) break;
            start = slash + 1;
        }
    }
    if (bad) {
        return Err::config_error("logo.bad_path",
                                 "logo must be a path inside the game folder, like 'images/logo.png': " +
                                     py::repr(value));
    }
    SCFG_TRY_VOID(logo_content_type(value.as_str()));
    return std::optional<std::string>(value.as_str());
}

Result<std::string> accent(const Json& value) {
    if (value.is_null()) return std::string(DEFAULT_ACCENT);
    // re.fullmatch(r"#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})", value): ASCII only.
    bool good = value.is_str();
    if (good) {
        std::string_view s = value.as_str();
        good = !s.empty() && s[0] == '#' && (s.size() == 4 || s.size() == 7 || s.size() == 9);
        for (size_t i = 1; good && i < s.size(); ++i) good = is_hex(s[i]);
    }
    if (!good) return Err::config_error("accent.invalid", "accent must be a color like '#0e7c74': " + py::repr(value));
    std::string digits = value.as_str().substr(1);
    if (digits.size() == 3) digits = {digits[0], digits[0], digits[1], digits[1], digits[2], digits[2]};
    return "#" + py::lower_ascii(std::string_view(digits).substr(0, 6));  // alpha, if any, is dropped
}

// color[i:i + 2], by code points.
std::string slice2(std::string_view color, size_t i) {
    size_t at = 0;
    for (size_t n = 0; n < i && at < color.size(); ++n) py::utf8_decode(color, at);
    size_t end = at;
    for (size_t n = 0; n < 2 && end < color.size(); ++n) py::utf8_decode(color, end);
    return std::string(color.substr(at, end - at));
}

// WCAG relative luminance of a #rrggbb color.
Result<double> luminance(std::string_view color) {
    double ch[3] = {0, 0, 0};
    for (size_t k = 0; k < 3; ++k) {
        SCFG_TRY(BigInt v, py::py_int_hex(slice2(color, 1 + 2 * k)));  // two characters: |v| <= 255
        double c = static_cast<double>(v.to_int64().value()) / 255;
        ch[k] = c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }
    return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
}

// ---- methods ----

Result<Json> merge_methods(const Json& methods, const std::vector<std::string>& chains) {
    Json merged = DEFAULT_METHODS();
    Json::Dict& m = merged.as_dict();
    SCFG_TRY(const Json::Dict* given, items_or_empty(methods));
    if (given != nullptr) {
        for (const auto& [name, opts] : *given) {
            Json* target = m.find(name);
            if (target == nullptr) {
                return Err::config_error("methods.unknown_method", "unknown method: " + py::repr_str(name));
            }
            SCFG_TRY(const Json::Dict* options, items_of(opts));
            for (const auto& [key, value] : *options) {
                if (!target->as_dict().contains(key)) {
                    return Err::config_error("methods.unknown_option", "unknown option " + py::repr_str(key) +
                                                                           " for method " + py::repr_str(name));
                }
                target->as_dict().set(key, value);
            }
        }
    }
    for (const auto& [name, opts] : m) {
        if (!opts.as_dict().get("enabled").is_bool()) {
            return Err::config_error("methods.enabled_not_bool", "methods." + name + ".enabled must be True or False");
        }
    }
    Json timeout = m.find("browser")->as_dict().get("timeout_min");
    // isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not timeout > 0 (a NaN is not > 0).
    bool good = (timeout.is_int() && timeout.as_bigint().sign() > 0) || (timeout.is_float() && timeout.as_float() > 0);
    if (!good) {
        return Err::config_error("methods.bad_timeout",
                                 "methods.browser.timeout_min must be a number of minutes above 0");
    }
    bool any = false;
    for (const auto& [name, opts] : m) any = any || opts.as_dict().get("enabled").as_bool();
    if (!any) return Err::config_error("methods.none_enabled", "at least one payment method must be enabled");
    Json qr_chains = m.find("qr")->as_dict().get("chains");
    if (!qr_chains.is_null()) {
        SCFG_TRY(std::vector<Json> entries, py::iterate(qr_chains));
        for (const Json& c : entries) {
            if (!(c.is_str() && contains(chains, c.as_str()))) {  // `c not in chains`: a tuple, compared with ==
                return Err::config_error("methods.qr_disabled_chain",
                                         "methods.qr.chains contains disabled chain " + py::repr(c));
            }
        }
    }
    return merged;
}

// ---- build_config ----

constexpr Param kConfigParams[] = {
    {"wallet", false, true},        {"testnet"},  {"chains"}, {"tokens"}, {"methods"},
    {"rpc_urls"},                   {"confirmations"}, {"previous_wallets"}, {"logo"}, {"accent"},
    {"custom_chains"},              {"solana_wallet"},
};

Result<Config> build(const Json& kwargs, const std::vector<CustomChain>* typed) {
    SCFG_TRY(const Json::Dict* kw, bind(kwargs, "build_config", kConfigParams));
    bool testnet = py::truthy(arg(*kw, "testnet", false));
    std::vector<CustomChain> raw;
    if (typed == nullptr) {
        Json given = arg(*kw, "custom_chains");
        if (py::truthy(given)) {  // `for chain in custom or ()`
            SCFG_TRY(std::vector<Json> entries, py::iterate(given));
            for (Json& e : entries) raw.emplace_back(std::move(e));
        }
        typed = &raw;
    }
    SCFG_TRY(Custom custom, custom_chains(*typed, testnet));
    OrderedMap<chains::Chain> registry = chains::get_chains(testnet);
    for (const auto& [k, c] : custom.mine) registry.set(k, c);

    Json wallet_arg = arg(*kw, "wallet");
    Json solana_arg = arg(*kw, "solana_wallet");
    if (wallet_arg.is_null() && solana_arg.is_null()) {
        return Err::config_error("wallet.missing",
                                 "configure() needs wallet= (an EVM 0x address) and/or solana_wallet=");
    }
    std::optional<std::string> wallet;
    if (!wallet_arg.is_null()) {
        SCFG_TRY(wallet, address(wallet_arg, "wallet"));
    }
    std::optional<std::string> sol;
    if (!solana_arg.is_null()) {
        SCFG_TRY(sol, solana_address(solana_arg, "solana_wallet"));
    }
    std::vector<std::string> prev;
    std::vector<std::string> prev_sol;
    Json previous = arg(*kw, "previous_wallets");
    if (py::truthy(previous)) {  // `for w in previous_wallets or ()`
        SCFG_TRY(std::vector<Json> entries, py::iterate(previous));
        for (const Json& w : entries) {
            SCFG_TRY(Wallet found, any_wallet(w, "previous_wallets"));
            bool is_sol = found.family == "solana";
            const std::optional<std::string>& current = is_sol ? sol : wallet;
            std::vector<std::string>& seen = is_sol ? prev_sol : prev;
            if (found.address != current && !contains(seen, found.address)) seen.push_back(found.address);
        }
    }

    // chains = (DISPLAY_ORDER + tuple(custom) if wallet else ()) + (solana_keys if sol else ()) if chains is None
    // else tuple(chains); every entry is then checked, the default ones too.
    std::vector<Json> chain_values;
    Json chains_arg = arg(*kw, "chains");
    if (chains_arg.is_null()) {
        if (wallet) {
            for (std::string_view k : chains::DISPLAY_ORDER) chain_values.emplace_back(k);
            for (const auto& [k, c] : custom.mine) chain_values.emplace_back(k);
        }
        if (sol) {  // solana_keys
            for (const auto& [k, c] : registry) {
                if (c.family == "solana") chain_values.emplace_back(k);
            }
        }
    } else {
        SCFG_TRY(chain_values, py::iterate(chains_arg));
    }
    if (chain_values.empty()) return Err::config_error("chains.empty", "chains must not be empty");
    std::vector<std::string> chain_keys;
    for (const Json& c : chain_values) {
        SCFG_TRY_VOID(hashable(c));  // `c in other_network` hashes c
        if (c.is_str() && contains(custom.other, c.as_str())) {
            std::string net = testnet ? "mainnet" : "testnet";
            return Err::config_error("chains.wrong_network", "chain " + py::repr(c) + " was only added for " + net +
                                                                 "; add it again with testnet=" +
                                                                 (testnet ? "True" : "False") + " to use it here");
        }
        const chains::Chain* chain = c.is_str() ? registry.find(c.as_str()) : nullptr;
        if (chain == nullptr) return Err::config_error("chains.unknown", "unknown chain: " + py::repr(c));
        if (chain->family == "solana" && !sol) {
            return Err::config_error("chain.needs_solana_wallet", "chain " + py::repr(c) + " needs solana_wallet=");
        }
        if (chain->family == "evm" && !wallet) {
            return Err::config_error("chain.needs_evm_wallet",
                                     "chain " + py::repr(c) + " needs wallet= (an EVM 0x address)");
        }
        chain_keys.push_back(c.as_str());
    }
    for (size_t i = 0; i < chain_keys.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (chain_keys[i] == chain_keys[j]) {
                return Err::config_error("chains.duplicate", "duplicate chain in chains");
            }
        }
    }

    std::vector<std::string> tokens;
    Json tokens_arg = arg(*kw, "tokens");
    std::vector<Json> token_values;
    if (tokens_arg.is_null()) {
        for (std::string_view t : TOKENS) token_values.emplace_back(t);
    } else {
        SCFG_TRY(token_values, py::iterate(tokens_arg));
    }
    if (token_values.empty()) return Err::config_error("tokens.empty", "tokens must not be empty");
    for (const Json& t : token_values) {
        if (!contains(TOKENS, t)) return Err::config_error("tokens.unknown", "unknown token: " + py::repr(t));
        tokens.push_back(t.as_str());
    }

    SCFG_TRY(Json merged, merge_methods(arg(*kw, "methods"), chain_keys));

    OrderedMap<std::vector<std::string>> urls;
    SCFG_TRY(const Json::Dict* rpc_arg, items_or_empty(arg(*kw, "rpc_urls")));
    if (rpc_arg != nullptr) {
        for (const auto& [c, lst] : *rpc_arg) {
            if (!contains(chain_keys, c)) {
                return Err::config_error("rpc_urls.unknown_chain",
                                         "rpc_urls for unknown or disabled chain " + py::repr_str(c));
            }
            SCFG_TRY(std::vector<std::string> list, rpc_urls(lst, "rpc_urls[" + py::repr_str(c) + "]"));
            urls.set(c, std::move(list));
        }
    }

    OrderedMap<BigInt> confs;
    SCFG_TRY(const Json::Dict* conf_arg, items_or_empty(arg(*kw, "confirmations")));
    if (conf_arg != nullptr) {
        for (const auto& [c, n] : *conf_arg) {
            if (!contains(chain_keys, c)) {
                return Err::config_error("confirmations.unknown_chain",
                                         "confirmations for unknown or disabled chain " + py::repr_str(c));
            }
            if (registry.find(c)->family == "solana") {
                return Err::config_error("confirmations.solana_fixed",
                                         "Solana payments wait for finalization; confirmations can't be set for " +
                                             py::repr_str(c));
            }
            if (!is_int(n) || n.as_bigint() < BigInt(1)) {
                return Err::config_error("confirmations.invalid", "confirmations must be an int >= 1");
            }
            confs.set(c, n.as_bigint());
        }
    }

    Config cfg;
    cfg.wallet = std::move(wallet);
    cfg.testnet = testnet;
    cfg.chains = std::move(chain_keys);
    cfg.tokens = std::move(tokens);
    cfg.methods = std::move(merged);
    cfg.rpc_urls = std::move(urls);
    cfg.confirmations = std::move(confs);
    cfg.previous_wallets = std::move(prev);
    cfg.solana_wallet = std::move(sol);
    cfg.previous_solana_wallets = std::move(prev_sol);
    SCFG_TRY(cfg.logo, logo(arg(*kw, "logo")));
    SCFG_TRY(cfg.accent, accent(arg(*kw, "accent")));
    cfg.custom_chains = std::move(custom.mine);
    SCFG_TRY(OrderedMap<chains::Chain> usable, enabled_chains(cfg));
    if (usable.empty()) {
        return Err::config_error("config.no_usable_token", "no enabled chain supports any enabled token");
    }
    return cfg;
}

}  // namespace scfg::config::detail

namespace scfg::config {

Json DEFAULT_METHODS() {
    return Json::dict({
        {"browser", Json::dict({{"enabled", true}, {"timeout_min", 30}})},
        {"qr", Json::dict({{"enabled", true}, {"wallet_button", true}, {"chains", Json()}})},
        {"manual", Json::dict({{"enabled", true}, {"allow_tx_paste", true}})},
    });
}

// ---- Config ----

OrderedMap<chains::Chain> Config::registry() const {
    OrderedMap<chains::Chain> r = chains::get_chains(testnet);
    for (const auto& [k, c] : custom_chains) r.set(k, c);
    return r;
}

std::vector<std::string> Config::accepted_wallets() const {
    std::vector<std::string> out;
    if (wallet && !wallet->empty()) out.push_back(*wallet);
    out.insert(out.end(), previous_wallets.begin(), previous_wallets.end());
    return out;
}

std::vector<std::string> Config::accepted_wallets_for(std::string_view family) const {
    if (family != "solana") return accepted_wallets();
    std::vector<std::string> out;
    if (solana_wallet && !solana_wallet->empty()) out.push_back(*solana_wallet);
    out.insert(out.end(), previous_solana_wallets.begin(), previous_solana_wallets.end());
    return out;
}

std::optional<std::string> Config::wallet_for(const chains::Chain& chain) const {
    return chain.family == "solana" ? solana_wallet : wallet;
}

const models::Item* Config::item_by_code(int64_t code) const {
    for (const auto& [id, item] : items) {
        if (item.code == code) return &item;
    }
    return nullptr;
}

// ---- build_chain ----

Result<chains::Chain> build_chain(const Json& kwargs) {
    static constexpr detail::Param params[] = {
        {"key", true, true},       {"name", true},         {"chain_id", true},      {"rpc", true},
        {"tokens", true},          {"confirmations", true}, {"explorer_tx", true},  {"native_symbol"},
        {"fee_hint"},              {"max_log_range"},       {"testnet"},
    };
    SCFG_TRY(const Json::Dict* kw, detail::bind(kwargs, "build_chain", params));
    Json key = detail::arg(*kw, "key");
    Json name = detail::arg(*kw, "name");
    Json explorer_tx = detail::arg(*kw, "explorer_tx");
    Json native_symbol = detail::arg(*kw, "native_symbol", "ETH");
    Json fee_hint = detail::arg(*kw, "fee_hint", "low");

    if (!detail::is_id(key, 32)) {
        return Err::config_error("chain.invalid_key",
                                 "invalid chain key " + py::repr(key) + " (use a-z, 0-9, _ and -)");
    }
    std::string what = "chain " + py::repr(key);
    if (!name.is_str() || py::strip(name.as_str()).empty() || py::len(name.as_str()) > 32) {
        return Err::config_error("chain.invalid_name",
                                 what + ": name must be a non-empty string of at most 32 characters");
    }
    SCFG_TRY(BigInt chain_id, detail::positive_int(detail::arg(*kw, "chain_id"), what + ": chain_id"));
    SCFG_TRY(std::vector<std::string> rpc, detail::rpc_urls(detail::arg(*kw, "rpc"), what + ": rpc"));
    Json tokens = detail::arg(*kw, "tokens");
    if (!tokens.is_dict() || tokens.as_dict().empty()) {
        return Err::config_error("chain.tokens_invalid", what + ": tokens must be a dict like {'USDC': '0x...'}");
    }
    OrderedMap<chains::Token> parsed;
    for (const auto& [symbol, token_address] : tokens.as_dict()) {
        if (!detail::contains(TOKENS, Json(symbol))) {
            return Err::config_error("chain.unknown_token",
                                     what + ": unknown token " + py::repr_str(symbol) + " (supported: USDC, USDT)");
        }
        SCFG_TRY(std::string a, detail::address(token_address, what + ": " + symbol + " address"));
        SCFG_TRY(chains::Token t, chains::Token::make(a));
        parsed.set(symbol, std::move(t));
    }
    SCFG_TRY(BigInt confirmations, detail::positive_int(detail::arg(*kw, "confirmations"), what + ": confirmations"));
    if (!explorer_tx.is_str() || explorer_tx.as_str().rfind("https://", 0) != 0 ||
        detail::count_braces(explorer_tx.as_str()) != 1) {
        return Err::config_error("chain.bad_explorer_tx",
                                 what + ": explorer_tx must be an https:// URL with {} where the transaction hash "
                                        "goes, like 'https://snowtrace.io/tx/{}'");
    }
    Result<std::string> formatted = py::format1(explorer_tx.as_str(), "0x");
    if (!formatted.ok()) {
        const Err& e = formatted.err();
        if (!(e.is_a("IndexError") || e.is_a("KeyError") || e.is_a("ValueError"))) return e;
        return Err::config_error("chain.bad_explorer_tx_braces",
                                 what + ": explorer_tx may contain no braces besides {}");
    }
    if (!detail::is_native_symbol(native_symbol)) {
        return Err::config_error("chain.bad_native_symbol",
                                 what + ": native_symbol must be letters/digits, like 'AVAX'");
    }
    if (!detail::contains(FEE_HINTS, fee_hint)) {
        return Err::config_error("chain.bad_fee_hint", what + ": fee_hint must be one of lowest, low, high");
    }
    SCFG_TRY(BigInt max_log_range, detail::positive_int(detail::arg(*kw, "max_log_range", 1000),
                                                         what + ": max_log_range", rpc::MIN_LOG_RANGE));
    chains::Chain c;
    c.key = key.as_str();
    c.chain_id = std::move(chain_id);
    c.name = py::strip(name.as_str());
    c.rpc = std::move(rpc);
    c.confirmations = std::move(confirmations);
    c.max_log_range = std::move(max_log_range);
    c.explorer_tx = explorer_tx.as_str();
    c.native_symbol = native_symbol.as_str();
    c.fee_hint = fee_hint.as_str();
    c.tokens = std::move(parsed);
    c.testnet = py::truthy(detail::arg(*kw, "testnet", false));
    return c;
}

// ---- build_config ----

Result<Config> build_config(const Json& kwargs) { return detail::build(kwargs, nullptr); }

Result<Config> build_config(const Json& kwargs, std::vector<CustomChain> custom_chains) {
    if (kwargs.is_dict() && kwargs.as_dict().contains("custom_chains")) {
        return Err::type_error("build_config() got multiple values for keyword argument 'custom_chains'");
    }
    return detail::build(kwargs, &custom_chains);
}

Result<std::string> logo_content_type(std::string_view path) {
    // path[path.rfind("."):].lower() if "." in path else "" ('.' is ASCII, so the byte index is the character's).
    size_t dot = path.rfind('.');
    std::string ext = dot == std::string_view::npos ? std::string() : py::lower_for_ascii(path.substr(dot));
    for (const auto& [suffix, type] : LOGO_TYPES) {
        if (ext == suffix) return std::string(type);
    }
    return Err::config_error("logo.bad_type", "logo must be a .png, .jpg or .webp image: " + py::repr_str(path));
}

Result<std::string> text_on(std::string_view color) {
    SCFG_TRY(double lum, detail::luminance(color));
    SCFG_TRY(double ink_lum, detail::luminance(INK));
    double white = 1.05 / (lum + 0.05);
    double ink = (lum + 0.05) / (ink_lum + 0.05);
    return white >= ink ? std::string("#ffffff") : std::string(INK);
}

Result<OrderedMap<chains::Chain>> enabled_chains(const Config& config) {
    OrderedMap<chains::Chain> registry = config.registry();
    OrderedMap<chains::Chain> out;
    for (const std::string& key : config.chains) {
        const chains::Chain* base = registry.find(key);
        if (base == nullptr) return py::key_error(key);
        OrderedMap<chains::Token> tokens;
        for (const auto& [t, v] : base->tokens) {
            if (detail::contains(config.tokens, t)) tokens.set(t, v);
        }
        if (tokens.empty()) continue;
        std::vector<std::string> rpc;
        if (const std::vector<std::string>* mine = config.rpc_urls.find(key)) rpc = *mine;
        rpc.insert(rpc.end(), base->rpc.begin(), base->rpc.end());
        const BigInt* confirmations = config.confirmations.find(key);
        out.set(key, base->with(std::move(rpc), confirmations != nullptr ? *confirmations : base->confirmations,
                                std::move(tokens)));
    }
    return out;
}

Result<std::vector<std::string>> chains_for_token(const Config& config, std::string_view token) {
    SCFG_TRY(OrderedMap<chains::Chain> enabled, enabled_chains(config));
    std::vector<std::string> out;
    for (const auto& [k, c] : enabled) {
        if (c.tokens.contains(token)) out.push_back(k);
    }
    return out;
}

// ---- add_item ----

Result<models::Item> add_item(Config& config, const Json& item_id, const Json& kwargs) {
    static constexpr detail::Param params[] = {
        {"code", true}, {"price", true}, {"token", true}, {"title", true}, {"min_price"},
    };
    SCFG_TRY(const Json::Dict* kw, detail::bind(kwargs, "add_item", params));
    Json code = detail::arg(*kw, "code");
    Json price = detail::arg(*kw, "price");
    Json token = detail::arg(*kw, "token");
    Json title = detail::arg(*kw, "title");
    Json min_price = detail::arg(*kw, "min_price");

    if (!detail::is_id(item_id, 64)) {
        return Err::config_error("item.invalid_id",
                                 "invalid item id " + py::repr(item_id) + " (use a-z, 0-9, _ and -)");
    }
    const std::string& id = item_id.as_str();
    if (config.items.contains(id)) {
        return Err::config_error("item.duplicate_id", "duplicate item id " + py::repr_str(id));
    }
    if (!detail::is_int(code) || code.as_bigint() < BigInt(1) || code.as_bigint() > BigInt(99)) {
        return Err::config_error("item.invalid_code", "item code must be an int in 1..99");
    }
    int64_t item_code = code.as_int64();
    if (config.item_by_code(item_code) != nullptr) {
        return Err::config_error("item.duplicate_code", "duplicate item code " + code.as_bigint().to_decimal());
    }
    // except (TypeError, ValueError) as e: ConfigError(f"item {item_id!r}: {e}")
    auto amount = [&id](const Json& v) -> Result<Dec> {
        Result<Dec> r = amounts::parse_amount(v);
        if (!r.ok() && (r.err().is_a("TypeError") || r.err().is_a("ValueError"))) {
            return Err::config_error("item.invalid_price", "item " + py::repr_str(id) + ": " + r.err().message);
        }
        return r;
    };
    SCFG_TRY(Dec p, amount(price));
    Dec mp = p;
    if (!min_price.is_null()) {
        SCFG_TRY(mp, amount(min_price));
    }
    if (mp > p) return Err::config_error("item.min_price_above_price", "min_price must not exceed price");
    bool available = token.is_str() && detail::contains(config.tokens, token.as_str());
    if (available) {
        SCFG_TRY(std::vector<std::string> carrying, chains_for_token(config, token.as_str()));
        available = !carrying.empty();
    }
    if (!available) {
        return Err::config_error("token.unavailable",
                                 "token " + py::repr(token) + " is not available on any enabled chain");
    }
    if (!title.is_str() || py::strip(title.as_str()).empty() || py::len(title.as_str()) > 80) {
        return Err::config_error("item.invalid_title", "title must be a non-empty string of at most 80 characters");
    }
    models::Item item{id, item_code, p, token.as_str(), title.as_str(), mp};
    config.items.set(id, item);
    return item;
}

}  // namespace scfg::config
