// Copyright 2026 Repulse
// Twin of scfg_core/checkout.py (see Checkout.h), line by line, with godot/addons/scfg/core/checkout.gd as the
// generator-free template.
#include "Scfg/Checkout.h"

#include "Scfg/Amounts.h"
#include "Scfg/Backends.h"
#include "Scfg/BackendsBase.h"
#include "Scfg/Err.h"
#include "Scfg/Paylinks.h"
#include "Scfg/Py.h"
#include "Scfg/SolanaPaylinks.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace scfg::checkout {

namespace detail {

template <size_t N>
bool among(const std::array<std::string_view, N>& names, std::string_view s) {
    return std::find(names.begin(), names.end(), s) != names.end();
}

// STEP_ORDER[step]. Every step and method name the controller uses is in it.
int step_order(std::string_view step) {
    for (const auto& [name, place] : STEP_ORDER) {
        if (name == step) return place;
    }
    return -1;
}

bool among(const std::vector<std::string>& names, std::string_view s) {
    return std::find(names.begin(), names.end(), s) != names.end();
}

Json opt_json(const std::optional<std::string>& s) { return s ? Json(*s) : Json(); }

// A started session this controller can show (Checkout.h, "Divergences").
bool usable_session(const Json& rec, const chains::Chain* chain) {
    if (!rec.is_dict()) return false;
    const Json::Dict& d = rec.as_dict();
    const Json* id = d.find("id");
    const Json* units = d.find("units");
    if (id == nullptr || !id->is_str() || units == nullptr || !units->is_int()) return false;
    if (chain != nullptr && chain->family == "solana") {
        const Json* reference = d.find("reference");
        if (reference == nullptr || !reference->is_str()) return false;
    }
    return true;
}

}  // namespace detail

const OrderedMap<std::string>& MESSAGES() {
    static const OrderedMap<std::string> messages = {
        {"network_unavailable", "Can't reach the network. Retrying..."},
        {"tx_reverted", "That transaction failed on-chain; no funds were sent."},
        {"tx_not_a_payment", "That transaction isn't a payment for this purchase."},
        {"tx_not_landed", "That transaction didn't reach the network. You can pay again."},
        {"invalid_hash", "That doesn't look like a transaction hash or signature."},
        {"invalid_amount", "Enter an amount of at least " + payments::MIN_TIP().str() + " with up to 2 decimals."},
        {"browser_unavailable", "Couldn't start the browser checkout. Use another method."},
        {"start_failed", "Couldn't start the payment. Check your connection and try again."},
        {"start_unreachable", "Can't reach the network. Pick a network to try again."},
        {"solana_unavailable", "Payments on Solana aren't available right now."},
        {"internal_error", "Something went wrong while checking the payment. Retrying..."},
        {"paste_blocked", "Your browser blocked pasting. Type it instead: the game also finds the payment without it."},
    };
    return messages;
}

// --- construction ----------------------------------------------------------------------------------------------

CheckoutController::CheckoutController(CheckoutOptions options)
    : mobile(options.mobile),
      server_factory(std::move(options.server_factory)),
      unready(std::move(options.unready)),
      browser_wallet(std::move(options.browser_wallet)),
      game_name(options.game_name.empty() ? std::string("Game") : std::move(options.game_name)),
      config_(std::move(options.config)),
      spec_(std::move(options.spec)),
      submit_(std::move(options.submit)),
      amount_(spec_.price),
      step_(spec_.kind == "tip" ? "amount" : "network") {}

CheckoutController::~CheckoutController() { close(); }

Result<std::unique_ptr<CheckoutController>> CheckoutController::make(CheckoutOptions options) {
    if (!options.submit) return Err::type_error("CheckoutController needs a submit");
    SCFG_TRY(OrderedMap<chains::Chain> enabled, config::enabled_chains(options.config));
    const Json methods = options.config.methods;
    SCFG_TRY(Json browser, py::get(methods, "browser"));
    SCFG_TRY(Json browser_on, py::get(browser, "enabled"));
    SCFG_TRY(Json qr, py::get(methods, "qr"));
    SCFG_TRY(Json qr_on, py::get(qr, "enabled"));
    SCFG_TRY(Json qr_chains, py::get(qr, "chains"));
    SCFG_TRY(Json manual, py::get(methods, "manual"));
    SCFG_TRY(Json manual_on, py::get(manual, "enabled"));
    std::optional<std::vector<Json>> qr_list;
    if (!qr_chains.is_null()) {
        SCFG_TRY(std::vector<Json> items, py::iterate(qr_chains));
        qr_list = std::move(items);
    }
    SCFG_TRY(std::string on_accent, config::text_on(options.config.accent));

    std::unique_ptr<CheckoutController> ctl(new CheckoutController(std::move(options)));
    ctl->enabled_ = std::move(enabled);
    ctl->browser_enabled_ = py::truthy(browser_on);
    ctl->qr_enabled_ = py::truthy(qr_on);
    ctl->manual_enabled_ = py::truthy(manual_on);
    ctl->qr_chains_ = std::move(qr_list);
    ctl->on_accent_ = std::move(on_accent);
    if (ctl->step_ == "network") SCFG_TRY_VOID(ctl->enter_network());
    return ctl;
}

// --- read-only views -------------------------------------------------------------------------------------------

std::optional<std::string> CheckoutController::session_id() const {
    if (!session_.is_dict()) return std::nullopt;
    return session_.as_dict().get("id").as_str();
}

const chains::Chain* CheckoutController::chain() const {
    if (!chain_key_ || chain_key_->empty()) return nullptr;
    return enabled_.find(*chain_key_);
}

std::optional<std::string> CheckoutController::family() const {
    const chains::Chain* c = chain();
    return c != nullptr ? std::optional<std::string>(c->family) : std::nullopt;
}

std::vector<ChainOption> CheckoutController::chain_options() const {
    const Routes blocked = unready_routes();
    std::vector<ChainOption> out;
    for (const std::string& k : spec_.chains) {
        const chains::Chain* c = enabled_.find(k);
        if (c == nullptr) continue;
        if (std::find(blocked.begin(), blocked.end(), std::pair<std::string, std::string>(k, spec_.token)) !=
            blocked.end())
            continue;
        out.push_back(ChainOption{k, c->name, c->fee_hint});
    }
    return out;
}

const std::array<Dec, 4>& CheckoutController::tip_presets() const { return payments::TIP_PRESETS(); }

std::vector<std::string> CheckoutController::methods() const {
    std::vector<std::string> out;
    if (browser_enabled_ && !browser_failed_ && browser_possible()) out.push_back("browser");
    bool qr_here = true;
    if (qr_chains_) {
        const Json key = detail::opt_json(chain_key_);
        qr_here = std::any_of(qr_chains_->begin(), qr_chains_->end(), [&key](const Json& c) { return py::eq(c, key); });
    }
    if (qr_enabled_ && qr_here) out.push_back("qr");
    if (manual_enabled_) out.push_back("manual");
    return out;
}

// Desktop: a computer's browser can always open the page. Web build: only if a wallet that can pay on the chosen
// network is in the page (phones included: a wallet app's own browser injects one).
bool CheckoutController::browser_possible() const {
    if (browser_wallet) {
        const chains::Chain* c = chain();
        return c != nullptr && browser_wallet(*c);
    }
    return !mobile;
}

std::string CheckoutController::pay_amount_text() const {
    if (session_.is_dict()) return amounts::format_units(session_.as_dict().get("units").as_bigint());
    return amount_.str();
}

std::string CheckoutController::price_text() const { return amount_.str(); }

std::optional<std::string> CheckoutController::token_address() const {
    const chains::Chain* c = chain();
    if (c == nullptr) return std::nullopt;
    const chains::Token* t = c->tokens.find(spec_.token);
    return t != nullptr ? std::optional<std::string>(t->address) : std::nullopt;
}

std::optional<std::string> CheckoutController::recipient() const {
    const chains::Chain* c = chain();
    if (c != nullptr) return config_.wallet_for(*c);
    if (config_.wallet && !config_.wallet->empty()) return config_.wallet;
    return config_.solana_wallet;
}

Result<std::optional<std::string>> CheckoutController::payment_uri() const {
    if (!session_.is_dict()) return std::optional<std::string>();
    const Json::Dict& s = session_.as_dict();
    const BigInt units = s.get("units").as_bigint();
    const chains::Chain* c = chain();
    if (c == nullptr) return py::no_attribute(Json(), "chain_id");  // self.chain.chain_id of None
    // f"{None}" is "None"; solana_pay_uri leaves a falsy value out, and quote() refuses anything but a str.
    const std::string to = recipient().value_or("None");
    if (c->family == "solana") {
        std::string message;
        if (spec_.title.is_str()) {
            message = spec_.title.as_str();
        } else if (py::truthy(spec_.title)) {
            return Err::type_error("quote_from_bytes() expected bytes");
        }
        SCFG_TRY(std::string uri,
                 solana::paylinks::solana_pay_uri(to, units, token_address().value_or(""), s.get("reference").as_str(),
                                                  game_name, message));
        return std::optional<std::string>(std::move(uri));
    }
    SCFG_TRY(std::string uri, paylinks::eip681_transfer_uri(token_address().value_or("None"), c->chain_id, to, units));
    return std::optional<std::string>(std::move(uri));
}

Result<bool> card_offered(const config::Config& config, const std::optional<std::string>& chain_key) {
    const config::Onramp& opts = config.onramp;
    if (!opts.enabled || !chain_key || chain_key->empty()) return false;
    if (std::find(opts.chains.begin(), opts.chains.end(), *chain_key) == opts.chains.end()) return false;
    SCFG_TRY(OrderedMap<chains::Chain> enabled, config::enabled_chains(config));
    return enabled.contains(*chain_key);
}

Result<std::optional<std::string>> card_url(const config::Config& config, std::string_view token,
                                            const std::optional<std::string>& chain_key,
                                            const std::optional<BigInt>& units) {
    if (!units) return std::optional<std::string>();
    SCFG_TRY(bool offered, card_offered(config, chain_key));
    if (!offered) return std::optional<std::string>();
    SCFG_TRY(OrderedMap<chains::Chain> enabled, config::enabled_chains(config));
    const chains::Chain* c = enabled.find(*chain_key);
    const chains::Token* t = c->tokens.find(token);
    if (t == nullptr) return Err::key_error(py::repr_str(token));
    // f"{None}" is "None" where the family has no wallet (build_config refuses such a chain).
    SCFG_TRY(std::string url, paylinks::onramp_url(config.onramp.url, c->key, c->chain_id, token, t->address,
                                                   amounts::format_units(*units),
                                                   config.wallet_for(*c).value_or("None")));
    return std::optional<std::string>(std::move(url));
}

Result<NetworkView> network_view(const config::Config& config, const std::vector<std::string>& option_keys,
                                 const std::optional<std::string>& chain_key, bool expanded) {
    NetworkView view;
    std::size_t featured = 0;
    for (const std::string& k : option_keys) {
        if (detail::among(chains::MAIN_CHAINS, k)) ++featured;
    }
    std::size_t fill = featured < chains::MAIN_TILES ? chains::MAIN_TILES - featured : 0;
    for (const std::string& k : option_keys) {
        if (detail::among(chains::MAIN_CHAINS, k)) {
            view.main.push_back(k);
        } else if (fill > 0) {
            view.main.push_back(k);
            --fill;
        } else {
            view.more.push_back(k);
        }
    }
    view.expanded = !view.more.empty() &&
                    (expanded || (chain_key && std::find(view.more.begin(), view.more.end(), *chain_key) != view.more.end()));
    for (const std::string& k : option_keys) {
        SCFG_TRY(bool offered, card_offered(config, k));
        if (offered) view.card.push_back(k);
    }
    return view;
}

Result<NetworkView> CheckoutController::network_layout() const {
    std::vector<std::string> keys;
    for (const ChainOption& o : chain_options()) keys.push_back(o.key);
    return network_view(config_, keys, chain_key_, more_networks_);
}

bool CheckoutController::card_offered(const std::optional<std::string>& key) const {
    const config::Onramp& opts = config_.onramp;
    return opts.enabled && key && !key->empty() &&
           std::find(opts.chains.begin(), opts.chains.end(), *key) != opts.chains.end() && enabled_.contains(*key);
}

Result<std::optional<std::string>> CheckoutController::card_url() const {
    std::optional<BigInt> units;
    if (session_.is_dict()) units = session_.as_dict().get("units").as_bigint();
    return checkout::card_url(config_, spec_.token, chain_key_, units);
}

Result<std::optional<std::string>> CheckoutController::explorer_url() const {
    Json tx = detail::opt_json(hint_tx);
    if (result_ && py::truthy(result_->tx_hash)) tx = result_->tx_hash;
    const chains::Chain* c = chain();
    if (!py::truthy(tx) || c == nullptr) return std::optional<std::string>();
    SCFG_TRY(std::string text, py::str(tx));
    SCFG_TRY(std::string url, c->explorer_url(text));
    return std::optional<std::string>(std::move(url));
}

std::optional<std::string> CheckoutController::message() const {
    if (error && !error->empty()) {
        const std::string* text = MESSAGES().find(*error);
        return text != nullptr ? std::optional<std::string>(*text) : std::nullopt;
    }
    if (state_ == "choose" && !spec_.chains.empty() && chain_options().empty()) {
        return *MESSAGES().find("solana_unavailable");
    }
    return std::nullopt;
}

std::vector<std::string> CheckoutController::dev_warnings() const {
    const OrderedMap<chains::Chain> registry = config_.registry();
    Routes pairs = unready_routes();
    std::sort(pairs.begin(), pairs.end());  // sorted(): UTF-8 bytes order as code points do
    std::vector<std::string> out;
    for (const auto& [key, token] : pairs) {
        const chains::Chain* c = registry.find(key);
        if (c != nullptr && c->family == "solana") {
            out.push_back("Your Solana wallet can't receive " + token + " yet: receive any amount once.");
        }
    }
    return out;
}

Json CheckoutController::status_info() const {
    return Json::dict({{"state", state_},
                       {"confirmations", Json(confirmations_)},
                       {"required", Json(required_)},
                       {"tx_hash", detail::opt_json(hint_tx)},
                       {"error", detail::opt_json(error)},
                       {"dropped_tx", dropped_tx_}});
}

bool CheckoutController::tx_seen() const { return (hint_tx && !hint_tx->empty()) || state_ == "confirming"; }

std::vector<std::string> CheckoutController::step_path() const {
    std::vector<std::string> path;
    if (spec_.kind == "tip") path.push_back("amount");
    if (network_has_choice()) path.push_back("network");
    if (method_ == "card") {
        path.push_back("card");
        path.push_back("waiting");
        return path;
    }
    if (methods().size() != 1) path.push_back("method");
    path.push_back(method_ && !method_->empty() ? *method_ : "browser");
    path.push_back("waiting");
    return path;
}

std::pair<int64_t, int64_t> CheckoutController::step_dots() const {
    const std::vector<std::string> path = step_path();
    const int here = detail::step_order(step_);
    int64_t before = 0;
    for (const std::string& s : path) {
        if (detail::step_order(s) < here) ++before;
    }
    return {before, static_cast<int64_t>(path.size())};
}

bool CheckoutController::can_go_back() const {
    if (step_ == "done" || tx_seen()) return false;
    return previous_step().has_value();
}

// --- actions ---------------------------------------------------------------------------------------------------

Status CheckoutController::select_chain(const std::string& key) {
    if (!detail::among(spec_.chains, key)) {
        return Err::value_error("chain " + py::repr_str(key) + " not available for this payment");
    }
    const std::string chosen = key;  // key may be chain_key_'s own text
    discard_session();
    chain_key_ = chosen;
    state_ = "starting";
    error.reset();
    Json opts = Json::dict({{"item", spec_.item}, {"amount", amount_.str()}, {"token", spec_.token}, {"title", spec_.title}});
    std::shared_ptr<worker::Delivery<Json>> started = std::make_shared<worker::Delivery<Json>>();
    future_ = started;
    submit_->start_session(Json(spec_.kind), Json(chosen), std::move(opts), worker::Delivery<Json>::receiver(started),
                           started->abandon_flag());
    const std::vector<std::string> available = methods();
    if ((!method_ || !detail::among(available, *method_)) && !(method_ == "card" && card_offered(chosen))) {
        method_ = available.empty() ? std::nullopt : std::optional<std::string>(available[0]);
    }
    return ok();
}

Status CheckoutController::choose_network(const std::string& key) {
    if (method_ == "card") method_.reset();  // the network's own button: pick a method as usual
    return pick_network(key);
}

Status CheckoutController::pay_with_card(const std::string& key) {
    if (!card_offered(key)) return ok();
    method_ = "card";
    return pick_network(key);
}

void CheckoutController::toggle_more_networks() {
    const Result<NetworkView> layout = network_layout();
    more_networks_ = !(layout.ok() ? layout.value().expanded : more_networks_);
}

Status CheckoutController::pick_network(const std::string& key) {
    step_ = "network";
    nav_dir_ = 1;
    if (chain_key_ == key && session_.is_dict() && state_ == "waiting") {
        after_network();
        return ok();
    }
    picked_network_ = true;  // poll() moves on once the session has started
    if (chain_key_ != key || state_ != "starting") return select_chain(key);
    return ok();
}

void CheckoutController::choose_method(const std::string& name) {
    if (detail::among(methods(), name)) {
        method_ = name;
        error.reset();
        go(name, 1);
    }
}

Result<bool> CheckoutController::next_step() {
    if (step_ != "amount") return false;
    if (!py::strip(custom_amount).empty()) {
        SCFG_TRY(bool applied, apply_custom_amount());
        if (!applied) return false;
    }
    SCFG_TRY_VOID(enter_network());
    return true;
}

void CheckoutController::back() {
    if (can_go_back()) {
        error.reset();
        go(*previous_step(), -1);
    }
}

void CheckoutController::mark_sent() {
    if (step_ == "qr" || step_ == "manual") {
        error.reset();
        go("waiting", 1);
    }
}

Result<std::optional<std::string>> CheckoutController::open_card() {
    std::optional<std::string> url;
    if (step_ == "card") {
        SCFG_TRY(url, card_url());
    }
    if (url && !url->empty()) go("waiting", 1);
    return url;
}

std::optional<std::string> CheckoutController::open_browser() {
    std::optional<std::string> url = start_browser();
    if (url && !url->empty()) {
        go("waiting", 1);
    } else if (browser_failed_ && step_ == "browser") {
        const std::vector<std::string> available = methods();
        go(available.size() != 1 ? std::string("method") : available[0], -1);
    }
    return url;
}

Result<bool> CheckoutController::set_tip_amount(const Json& value) {
    if (spec_.kind != "tip") return false;
    Result<Dec> parsed = amounts::parse_amount(value);
    if (!parsed.ok()) {
        if (parsed.err().is_a("TypeError") || parsed.err().is_a("ValueError")) {
            error = "invalid_amount";
            return false;
        }
        return std::move(parsed).err();
    }
    Dec new_amount = parsed.take();
    if (new_amount < payments::MIN_TIP()) {
        error = "invalid_amount";
        return false;
    }
    error.reset();
    const bool started = chain_key_ && !chain_key_->empty();
    if (new_amount == amount_ && started) return true;  // same amount: keep the session already started
    amount_ = std::move(new_amount);
    if (started) {
        const std::string key = *chain_key_;
        SCFG_TRY_VOID(select_chain(key));
    }
    return true;
}

Result<bool> CheckoutController::apply_custom_amount() { return set_tip_amount(Json(py::strip(custom_amount))); }

Result<bool> CheckoutController::submit_hash(std::optional<std::string> text) {
    const chains::Chain* c = chain();
    if (!session_.is_dict() || c == nullptr) return false;
    Result<const backends::base::Backend*> backend = backends::backend_for(*c);
    Result<std::string> tx = backend.ok() ? backend.value()->normalize_tx(Json(text ? *text : tx_input))
                                          : Result<std::string>(backend.err());
    if (!tx.ok()) {
        if (tx.err().is_a("ValueError")) {
            error = "invalid_hash";
            return false;
        }
        return std::move(tx).err();
    }
    report_hint(tx.take());
    tx_input.clear();
    return true;
}

void CheckoutController::report_hint(std::string tx_hash, std::optional<std::string> sender) {
    (void)sender;
    const std::optional<std::string> sid = session_id();
    hint_tx = tx_hash;
    error.reset();
    dropped_tx_ = Json();
    submit_->report_hint(detail::opt_json(sid), Json(std::move(tx_hash)), {});
    if (step_ != "done") go("waiting", 1);
}

std::optional<std::string> CheckoutController::start_browser() {
    if (!session_.is_dict() || !server_factory) return std::nullopt;
    if (server_ == nullptr) {
        const chains::Chain* c = chain();
        if (c == nullptr) return std::nullopt;  // a session always has its chain
        const Json::Dict& s = session_.as_dict();
        const bool solana = c->family == "solana";
        Json info = Json::dict({
            {"title", spec_.title},
            {"kind", spec_.kind},
            {"token", spec_.token},
            {"family", c->family},
            {"decimals", 6},
            {"amount_units", s.get("units").as_bigint().to_decimal()},
            {"amount_text", pay_amount_text()},
            {"price_text", price_text()},
            {"recipient", detail::opt_json(recipient())},
            {"chain_name", c->name},
            {"native_symbol", c->native_symbol},
            {"explorer_tx", c->explorer_tx},
            {"testnet", config_.testnet},
            {"logo", config_.logo.has_value()},
            {"accent", config_.accent},
            {"on_accent", on_accent_},
        });
        Json::Dict& d = info.as_dict();
        if (solana) {
            d.set("wallet_chain", c->wallet_chain);
            d.set("mint", detail::opt_json(token_address()));
            d.set("reference", s.get("reference"));
        } else {
            d.set("chain_id", Json(c->chain_id));
            d.set("chain_id_hex", py::hex(c->chain_id));
            d.set("rpc_url", c->rpc.empty() ? Json() : Json(c->rpc.back()));
            d.set("token_address", detail::opt_json(token_address()));
            // Arc: the token is also the gas coin, with a fee floor.
            d.set("gas_is_token", c->native_token == spec_.token);
            d.set("min_fee_wei", py::dec(c->min_fee_wei));
        }
        ServerArgs args;
        args.session_info = std::move(info);
        args.on_tx = [this](std::string tx_hash, std::optional<std::string> sender) {
            report_hint(std::move(tx_hash), std::move(sender));
        };
        args.get_status = [this] { return status_info(); };
        if (solana) {
            args.build_tx = [this](Json account, worker::OnDone<Json> done) {
                submit_build(std::move(account), std::move(done));
            };
        }
        Result<std::unique_ptr<Server>> made = server_factory(std::move(args));
        std::unique_ptr<Server> server = made.ok() ? made.take() : nullptr;
        const bool started = server != nullptr && server->start().ok();
        if (!started) {
            server_.reset();
            browser_failed_ = true;
            error = "browser_unavailable";
            if (method_ == "browser") {
                const std::vector<std::string> available = methods();
                method_ = available.empty() ? std::nullopt : std::optional<std::string>(available[0]);
            }
            return std::nullopt;
        }
        server_ = std::move(server);
    } else {
        server_->reopen();
    }
    return server_->url();
}

// Ask the Worker for the unsigned Solana transfer. The desktop page's server waits for done.
void CheckoutController::submit_build(Json account, worker::OnDone<Json> done) {
    submit_->build_solana_payment(detail::opt_json(session_id()), std::move(account), std::move(done));
}

models::PaymentResult CheckoutController::cancel() {
    const std::optional<std::string> sid = session_id();
    if (sid && !sid->empty() && !detail::among(FINAL_STATES, state_)) {
        submit_->set_ui_status(Json(*sid), Json("cancelled"), {});
    }
    close();
    if (!result_) result_ = make_result("cancelled");
    return *result_;
}

models::PaymentResult CheckoutController::keep_playing() {
    const std::optional<std::string> sid = session_id();
    if (sid && !sid->empty()) submit_->set_ui_status(Json(*sid), Json("pending"), {});
    return make_result("pending");
}

models::PaymentResult CheckoutController::finish() {
    close();
    return result_ ? *result_ : make_result("cancelled");
}

void CheckoutController::close() {
    abandon_start();
    if (server_ != nullptr) {
        server_->stop();
        server_.reset();
    }
}

// --- updates ---------------------------------------------------------------------------------------------------

void CheckoutController::poll() {
    if (server_ != nullptr) server_->pump();
    if (future_ == nullptr || !future_->done()) return;
    const std::shared_ptr<worker::Delivery<Json>> fut = std::move(future_);
    future_.reset();
    const Result<Json>& started = fut->result();
    if (!started.ok() || !detail::usable_session(started.value(), chain())) {
        state_ = "choose";
        chain_key_.reset();
        const bool network = !started.ok() && started.err().is_a("RpcUnavailable");
        error = network ? "start_unreachable" : "start_failed";
        if (step_ != "amount" && step_ != "network") go("network", -1);
        return;
    }
    session_ = started.value();
    state_ = "waiting";
    const chains::Chain* c = chain();
    required_ = c != nullptr ? c->confirmations : BigInt(0);
    if (step_ == "network" && picked_network_) after_network();
}

void CheckoutController::on_outcome(const watcher::StepOutcome& outcome) {
    const std::optional<std::string> sid = session_id();
    if (!sid || outcome.session_id != *sid) return;
    const std::optional<std::string>& out_error = outcome.error;
    if (out_error && detail::among(HINT_VERDICTS, *out_error)) {
        hint_verdict(outcome);
    } else if (out_error && !out_error->empty()) {
        error = out_error;
    }
    const std::string& status = outcome.status;
    if (status == "confirming") {
        state_ = "confirming";
        confirmations_ = outcome.confirmations;
        required_ = outcome.required;
        if (!out_error) error.reset();
    } else if (status == "reorged") {
        state_ = "waiting";
        confirmations_ = 0;
    } else if (status == "paid" || status == "underpaid") {
        state_ = status;
        result_ = outcome.result;
        error.reset();
    } else if (status == "expired" && state_ != "paid" && state_ != "underpaid") {
        state_ = "expired";
        result_ = outcome.result;
    }
    if (detail::among(FINAL_STATES, state_)) {
        if (step_ != "done") go("done", 1);
    } else if (tx_seen() && step_ != "waiting") {
        go("waiting", 1);
    }
}

// --- internals -------------------------------------------------------------------------------------------------

void CheckoutController::go(std::string to, int direction) {
    step_ = std::move(to);
    nav_dir_ = direction;
    picked_network_ = false;
}

std::optional<std::string> CheckoutController::previous_step() const {
    const int here = detail::step_order(step_);
    std::optional<std::string> last;
    for (const std::string& s : step_path()) {
        if (detail::step_order(s) < here) last = s;
    }
    return last;
}

Status CheckoutController::enter_network() {
    const std::vector<ChainOption> options = chain_options();
    if (!network_has_choice()) return choose_network(options[0].key);
    go("network", 1);
    return ok();
}

bool CheckoutController::network_has_choice() const {
    const std::vector<ChainOption> options = chain_options();
    return options.size() != 1 || card_offered(options[0].key);
}

void CheckoutController::after_network() {
    if (method_ == "card") {
        go("card", 1);
        return;
    }
    const std::vector<std::string> available = methods();
    if (!method_ || !detail::among(available, *method_)) {
        method_ = available.empty() ? std::nullopt : std::optional<std::string>(available[0]);
    }
    go(available.size() == 1 ? *method_ : std::string("method"), 1);
}

void CheckoutController::hint_verdict(const watcher::StepOutcome& outcome) {
    const Json& about = outcome.hint_tx;
    const bool have_hint = hint_tx && !hint_tx->empty();
    if (py::truthy(about) && have_hint && !py::eq(about, Json(*hint_tx))) {
        return;  // judged an older hint; the newer one reported since is still pending
    }
    error = outcome.error;
    if (outcome.error == "tx_not_landed") {
        dropped_tx_ = py::truthy(about) ? about : detail::opt_json(hint_tx);
    } else {
        dropped_tx_ = Json();
    }
    hint_tx.reset();
}

void CheckoutController::abandon_start() {
    const std::shared_ptr<worker::Delivery<Json>> fut = std::move(future_);
    future_.reset();
    if (fut == nullptr) return;
    // worker.py's abandon(fut): cancelled if it has not run, refused its next request if it runs, so the next pick
    // does not wait behind it.
    fut->abandon();
    const std::weak_ptr<worker::Submitter> submit = submit_;
    // Future.add_done_callback: runs at once when the start is done, else when it finishes.
    fut->add_done_callback([submit](const Result<Json>& started) {
        if (!started.ok()) return;  // a failed or cancelled start has no session to discard
        const Json& rec = started.value();
        const Json* sid = rec.is_dict() ? rec.as_dict().find("id") : nullptr;
        if (sid == nullptr) return;  // Python's KeyError, which the Future logs and drops
        if (const std::shared_ptr<worker::Submitter> live = submit.lock()) live->discard_session(*sid, {});
    });
}

void CheckoutController::discard_session() {
    if (session_.is_dict() && (state_ == "waiting" || state_ == "starting")) {
        submit_->discard_session(detail::opt_json(session_id()), {});
    }
    close();
    session_ = Json();
    hint_tx.reset();
    dropped_tx_ = Json();
    confirmations_ = 0;
}

Routes CheckoutController::unready_routes() const { return unready ? unready() : Routes(); }

models::PaymentResult CheckoutController::make_result(std::string status) const {
    models::PaymentResult r;
    r.status = std::move(status);
    r.kind = Json(spec_.kind);
    r.item = spec_.item;
    r.token = Json(spec_.token);
    r.chain = detail::opt_json(chain_key_);
    r.session_id = detail::opt_json(session_id());
    return r;
}

}  // namespace scfg::checkout
