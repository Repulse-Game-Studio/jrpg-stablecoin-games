// Copyright 2026 Repulse
// No Python twin: tests of scfg::checkout::CheckoutController, the twins of tests/unit/test_checkout.py one for one
// (same names, same assertions), over a real Engine whose chains are the fakes of support/CheckoutFakes.h and the
// immediate submitter (each command runs at once).
//
// Python test (tests/unit/test_checkout.py)                    -> C++ test (CheckoutTests.<name>)
//   test_initial_state_and_chain_options                       -> test_initial_state_and_chain_options
//   test_select_chain_starts_session                           -> test_select_chain_starts_session
//   test_select_unknown_chain_raises                           -> test_select_unknown_chain_raises
//   test_switching_chain_discards_old_session                  -> test_switching_chain_discards_old_session
//   test_start_failure_returns_to_choose_with_error            -> test_start_failure_returns_to_choose_with_error
//   test_methods_on_mobile_and_config                          -> test_methods_on_mobile_and_config
//   test_payment_uri                                           -> test_payment_uri
//   test_submit_hash_validation_and_hint                       -> test_submit_hash_validation_and_hint
//   test_outcomes_drive_states_to_paid                         -> test_outcomes_drive_states_to_paid
//   test_outcomes_for_other_sessions_are_ignored               -> test_outcomes_for_other_sessions_are_ignored
//   test_reorg_and_hint_errors                                 -> test_reorg_and_hint_errors
//   test_expiry_shows_expired_state                            -> test_expiry_shows_expired_state
//   test_cancel_marks_session_and_returns_result               -> test_cancel_marks_session_and_returns_result
//   test_keep_playing                                          -> test_keep_playing
//   test_cancel_before_choosing_chain                          -> test_cancel_before_choosing_chain
//   test_tip_amount_changes_restart_session                    -> test_tip_amount_changes_restart_session
//   test_tip_amount_ignored_for_unlocks                        -> test_tip_amount_ignored_for_unlocks
//   test_browser_server_lifecycle_and_hints                    -> test_browser_server_lifecycle_and_hints
//   test_browser_failure_falls_back                            -> test_browser_failure_falls_back
//   test_browser_session_info_carries_appearance               -> test_browser_session_info_carries_appearance
//   test_browser_info_tells_the_page_how_gas_is_paid[arc-...]  -> test_browser_info_tells_the_page_how_gas_is_paid
//   test_browser_info_tells_the_page_how_gas_is_paid[base-...]    (one test, both parameter sets)
// C++ only (as godot/tests/test_checkout.gd adds them): test_session_info_is_the_exact_python_dict,
// test_messages_match_catalogue (MESSAGES() is conformance/vectors/messages.json), and the Submitter contract:
// test_a_cancelled_start_is_a_failed_start, test_a_start_that_is_not_a_session_is_a_failed_start,
// test_a_start_delivered_later_is_picked_up_by_poll; and test_a_confirming_outcome_clears_the_message (no Python test
// reaches that branch).
// C++ only, review R1/R3 (guards no Python test pins): test_destroying_the_controller_stops_its_server,
// test_finish_stops_the_server, test_cancel_after_paid_keeps_the_paid_result, test_expired_after_paid_keeps_paid,
// test_a_reverted_verdict_drops_no_transaction, test_a_network_switch_forgets_the_old_payment,
// test_the_same_network_while_starting_does_not_skip_ahead, test_the_same_network_after_a_seen_payment_restarts,
// test_a_delivery_keeps_its_first_result.
// The broken server factory raises RuntimeError("no sockets") where Python raises OSError (not in the Err table).
#include "support/CheckoutFakes.h"
#include "support/JsonEq.h"
#include "support/Test.h"
#include "support/Vectors.h"

#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Checkout.h"
#include "Scfg/Config.h"
#include "Scfg/Dec.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Payments.h"
#include "Scfg/Result.h"
#include "Scfg/Watcher.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::Json;
using scfg::Result;
using scfg::checkout::CheckoutController;
using scfg::watcher::StepOutcome;
namespace fakes = scfg_test::fakes;

namespace scfg_test::checkout_tests {

constexpr const char* DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
constexpr const char* PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const std::string TX = "0x" + [] {
    std::string s;
    for (int i = 0; i < 32; ++i) s += "ab";
    return s;
}();

std::string usdc_arb() { return scfg::chains::MAINNET().find("arbitrum")->tokens.find("USDC")->address; }

std::string show(const std::optional<std::string>& s) { return s ? *s : std::string("None"); }

scfg::Dec dec(const char* s) { return *scfg::Dec::parse(s).value(); }

using Setup = fakes::Rig;

struct Opts {
    std::string kind = "unlock";
    bool mobile = false;
    Json methods{};     // None
    Json appearance{};  // extra build_config keyword arguments
};

// setup(kind="unlock", mobile=False, methods=None, appearance=None): (ctl, engine, fakes, clock, servers). nullopt
// (and a failure) if anything failed.
std::optional<Setup> setup(Ctx& t, const Opts& o = {}) {
    fakes::RigOptions r;
    r.kind = o.kind;
    r.mobile = o.mobile;
    r.config = Json::dict({{"wallet", DEV}});
    if (!o.methods.is_null()) r.config.as_dict().set("methods", o.methods);
    if (o.appearance.is_dict()) {
        for (const auto& [k, v] : o.appearance.as_dict()) r.config.as_dict().set(k, v);
    }
    Result<Setup> rig = fakes::make_rig(std::move(r));
    if (!t.check(rig.ok(), rig.ok() ? "" : "setup: " + rig.err().str())) return std::nullopt;
    return rig.take();
}

// started(): select_chain("arbitrum"), then poll().
std::optional<Setup> started(Ctx& t, const Opts& o = {}) {
    std::optional<Setup> s = setup(t, o);
    if (!s) return std::nullopt;
    if (!t.check(s->ctl->select_chain("arbitrum").ok(), "select_chain")) return std::nullopt;
    s->ctl->poll();
    return s;
}

std::vector<std::string> keys(const std::vector<scfg::checkout::ChainOption>& options) {
    std::vector<std::string> out;
    for (const auto& o : options) out.push_back(o.key);
    return out;
}

// for o in engine.tick(): ctl.on_outcome(o); the fake nodes answered every request and nothing crashed.
bool deliver(Ctx& t, Setup& s) {
    scfg::Status ticked = s.deliver();
    if (!t.check(ticked.ok(), ticked.ok() ? "" : "tick: " + ticked.err().str())) return false;
    for (const std::string& p : s.nodes->problems) t.fail("fake node: " + p);
    for (const std::string& e : *s.errors) t.fail("contained: " + e);
    return s.nodes->problems.empty() && s.errors->empty();
}

StepOutcome outcome(std::string sid, std::string status) {
    StepOutcome o;
    o.session_id = std::move(sid);
    o.status = std::move(status);
    return o;
}

const Json& info_of(const Setup& s) { return s.servers->front()->args.session_info; }

// A submitter whose start_session is answered by the test; it records every command by name.
struct HeldSubmitter final : scfg::worker::Submitter {
    std::vector<scfg::worker::OnDone<Json>> starts;
    std::vector<std::string> names;
    void start_session(Json, Json, Json, scfg::worker::OnDone<Json> done, scfg::worker::AbandonFlag) override {
        names.push_back("start_session");
        starts.push_back(std::move(done));
    }
    void build_solana_payment(Json, Json, scfg::worker::OnDone<Json>) override { names.push_back("build_solana_payment"); }
    void report_hint(Json, Json, scfg::worker::OnDone<void>) override { names.push_back("report_hint"); }
    void set_ui_status(Json, Json status, scfg::worker::OnDone<void>) override {
        names.push_back("set_ui_status " + (status.is_str() ? status.as_str() : std::string("?")));
    }
    void discard_session(Json, scfg::worker::OnDone<void>) override { names.push_back("discard_session"); }
    void restore_lookup(Json, scfg::worker::OnDone<std::vector<scfg::restore::RestoreCandidate>>) override {}
    void restore_apply(std::vector<scfg::restore::RestoreCandidate>, std::optional<std::map<int64_t, Json>>,
                       scfg::worker::OnDone<Json>) override {}
};

std::unique_ptr<CheckoutController> held_controller(Ctx& t, std::shared_ptr<HeldSubmitter> sub) {
    scfg::checkout::CheckoutOptions options;
    options.config = scfg::config::build_config(Json::dict({{"wallet", DEV}})).value();
    (void)scfg::config::add_item(options.config, "chapter2",
                                 Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}))
        .value();
    options.spec = scfg::payments::prepare_payment(options.config, "unlock", Json::dict({{"item", "chapter2"}})).value();
    options.submit = std::move(sub);
    Result<std::unique_ptr<CheckoutController>> ctl = CheckoutController::make(std::move(options));
    if (!t.check(ctl.ok(), "make")) return nullptr;
    return ctl.take();
}

}  // namespace scfg_test::checkout_tests

using namespace scfg_test::checkout_tests;

SCFG_TEST(CheckoutTests, test_initial_state_and_chain_options) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.state() == "choose" && ctl.session().is_null());
    SCFG_CHECK(t, keys(ctl.chain_options()) ==
                      std::vector<std::string>({"arbitrum", "base", "arc", "polygon", "ethereum"}));
    SCFG_CHECK_EQ(t, ctl.pay_amount_text(), "4.99");
}

SCFG_TEST(CheckoutTests, test_select_chain_starts_session) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK_EQ(t, ctl.state(), "waiting");
    SCFG_CHECK(t, !s->pending(ctl.session_id()).is_null());
    SCFG_CHECK_EQ(t, show(ctl.method()), "browser");
    SCFG_CHECK(t, ctl.required() == scfg::BigInt(20));
    SCFG_CHECK(t, ctl.pay_amount_text().rfind("4.9937", 0) == 0);
    SCFG_CHECK_EQ(t, ctl.price_text(), "4.99");
}

SCFG_TEST(CheckoutTests, test_select_unknown_chain_raises) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    scfg::Status r = s->ctl->select_chain("solana");
    SCFG_CHECK(t, !r.ok() && r.err().is_a("ValueError"));
}

SCFG_TEST(CheckoutTests, test_switching_chain_discards_old_session) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    const std::optional<std::string> old = ctl.session_id();
    SCFG_CHECK(t, ctl.select_chain("base").ok());
    ctl.poll();
    SCFG_CHECK(t, s->pending(old).is_null() && !s->pending(ctl.session_id()).is_null());
    SCFG_CHECK_EQ(t, ctl.session().as_dict().get("chain").as_str(), "base");
}

SCFG_TEST(CheckoutTests, test_start_failure_returns_to_choose_with_error) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    s->fake("arbitrum") = fakes::FakeChain();
    s->fake("arbitrum").down = true;
    SCFG_CHECK(t, ctl.select_chain("arbitrum").ok());
    ctl.poll();
    SCFG_CHECK(t, ctl.state() == "choose" && show(ctl.error) == "start_unreachable");
    SCFG_CHECK(t, ctl.message().has_value());
}

SCFG_TEST(CheckoutTests, test_methods_on_mobile_and_config) {
    std::optional<Setup> s = started(t, {.mobile = true});
    if (!s) return;
    SCFG_CHECK(t, s->ctl->methods() == std::vector<std::string>({"qr", "manual"}));
    s.reset();  // the old rig goes first, controller before nodes
    s = started(t, {.methods = Json::dict({{"qr", Json::dict({{"chains", Json::list({"base"})}})}})});
    if (!s) return;
    SCFG_CHECK(t, s->ctl->methods() == std::vector<std::string>({"browser", "manual"}));  // QR limited to base
}

SCFG_TEST(CheckoutTests, test_payment_uri) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    Result<std::optional<std::string>> uri = ctl.payment_uri();
    if (!t.check(uri.ok() && uri.value(), "payment_uri")) return;
    SCFG_CHECK_EQ(t, *uri.value(),
                  "ethereum:" + usdc_arb() + "@42161/transfer?address=" + DEV +
                      "&uint256=" + ctl.session().as_dict().get("units").as_bigint().to_decimal());
}

SCFG_TEST(CheckoutTests, test_submit_hash_validation_and_hint) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    Result<bool> bad = ctl.submit_hash("garbage");
    SCFG_CHECK(t, bad.ok() && !bad.value() && show(ctl.error) == "invalid_hash");
    ctl.tx_input = TX;
    Result<bool> good = ctl.submit_hash();
    SCFG_CHECK(t, good.ok() && good.value());
    SCFG_CHECK_EQ(t, s->pending(ctl.session_id()).as_dict().get("hint_tx").as_str(), TX);
    SCFG_CHECK(t, show(ctl.hint_tx) == TX && ctl.tx_input.empty() && !ctl.error);
}

SCFG_TEST(CheckoutTests, test_outcomes_drive_states_to_paid) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    s->fake("arbitrum").logs.push_back(fakes::transfer_log(
        usdc_arb(), PAYER, DEV, ctl.session().as_dict().get("units").as_int64(), 999, TX));
    if (!deliver(t, *s)) return;
    SCFG_CHECK(t, ctl.state() == "confirming" && ctl.confirmations() == scfg::BigInt(2));
    s->fake("arbitrum").head = 1100;
    if (!deliver(t, *s)) return;
    SCFG_CHECK(t, ctl.state() == "paid" && ctl.result() && ctl.result()->status == "paid");
    Result<std::optional<std::string>> url = ctl.explorer_url();
    SCFG_CHECK(t, url.ok() && url.value() && *url.value() == "https://arbiscan.io/tx/" + TX);
    SCFG_CHECK_EQ(t, ctl.finish().status, "paid");
}

SCFG_TEST(CheckoutTests, test_outcomes_for_other_sessions_are_ignored) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    s->ctl->on_outcome(outcome("ps_other", "paid"));
    SCFG_CHECK_EQ(t, s->ctl->state(), "waiting");
}

SCFG_TEST(CheckoutTests, test_reorg_and_hint_errors) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    const std::string sid = *ctl.session_id();
    ctl.hint_tx = TX;
    StepOutcome confirming = outcome(sid, "confirming");
    confirming.confirmations = 3;
    confirming.required = 20;
    ctl.on_outcome(confirming);
    ctl.on_outcome(outcome(sid, "reorged"));
    SCFG_CHECK(t, ctl.state() == "waiting" && ctl.confirmations() == scfg::BigInt(0));
    StepOutcome verdict = outcome(sid, "waiting");
    verdict.error = "tx_not_a_payment";
    ctl.on_outcome(verdict);
    SCFG_CHECK(t, show(ctl.error) == "tx_not_a_payment" && !ctl.hint_tx);
}

SCFG_TEST(CheckoutTests, test_expiry_shows_expired_state) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    s->clock.advance(1800);
    if (!deliver(t, *s)) return;
    SCFG_CHECK(t, s->ctl->state() == "expired" && s->ctl->finish().status == "expired");
}

SCFG_TEST(CheckoutTests, test_cancel_marks_session_and_returns_result) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    scfg::models::PaymentResult result = ctl.cancel();
    SCFG_CHECK(t, result.status == "cancelled" && result.session_id.is_str() &&
                      result.session_id.as_str() == show(ctl.session_id()));
    SCFG_CHECK_EQ(t, s->pending(ctl.session_id()).as_dict().get("ui_status").as_str(), "cancelled");
}

SCFG_TEST(CheckoutTests, test_keep_playing) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK_EQ(t, ctl.keep_playing().status, "pending");
    SCFG_CHECK_EQ(t, s->pending(ctl.session_id()).as_dict().get("ui_status").as_str(), "pending");
}

SCFG_TEST(CheckoutTests, test_cancel_before_choosing_chain) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    SCFG_CHECK_EQ(t, s->ctl->cancel().status, "cancelled");
}

SCFG_TEST(CheckoutTests, test_tip_amount_changes_restart_session) {
    std::optional<Setup> s = started(t, {.kind = "tip"});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    const std::optional<std::string> first = ctl.session_id();
    Result<bool> five = ctl.set_tip_amount("5");
    SCFG_CHECK(t, five.ok() && five.value());
    ctl.poll();
    SCFG_CHECK(t, ctl.amount() == dec("5.00") && ctl.session().as_dict().get("price").as_str() == "5.00");
    SCFG_CHECK(t, s->pending(first).is_null());
    Result<bool> low = ctl.set_tip_amount("0.10");
    SCFG_CHECK(t, low.ok() && !low.value() && show(ctl.error) == "invalid_amount");
    ctl.custom_amount = " 7.50 ";
    Result<bool> typed = ctl.apply_custom_amount();
    SCFG_CHECK(t, typed.ok() && typed.value() && ctl.amount() == dec("7.50"));
}

SCFG_TEST(CheckoutTests, test_tip_amount_ignored_for_unlocks) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    Result<bool> r = s->ctl->set_tip_amount("1");
    SCFG_CHECK(t, r.ok() && !r.value());
}

SCFG_TEST(CheckoutTests, test_browser_server_lifecycle_and_hints) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    std::optional<std::string> url = ctl.start_browser();
    SCFG_CHECK_EQ(t, show(url), fakes::kServerUrl);
    SCFG_CHECK(t, ctl.start_browser() == url && s->servers->size() == 1);  // reused
    if (s->servers->empty()) return;
    fakes::ServerLog& srv = *s->servers->front();
    const Json::Dict& info = srv.args.session_info.as_dict();
    SCFG_CHECK(t, srv.started);
    SCFG_CHECK_EQ(t, info.get("amount_units").as_str(), ctl.session().as_dict().get("units").as_bigint().to_decimal());
    SCFG_CHECK_EQ(t, info.get("chain_id_hex").as_str(), "0xa4b1");
    SCFG_CHECK(t, info.get("logo").is_bool() && !info.get("logo").as_bool());
    SCFG_CHECK(t, info.get("accent").as_str() == "#0e7c74" && info.get("on_accent").as_str() == "#ffffff");
    srv.args.on_tx(TX, std::string(PAYER));  // page reports the tx
    SCFG_CHECK_EQ(t, s->pending(ctl.session_id()).as_dict().get("hint_tx").as_str(), TX);
    SCFG_CHECK_EQ(t, srv.args.get_status().as_dict().get("tx_hash").as_str(), TX);
    (void)ctl.cancel();
    SCFG_CHECK(t, srv.stopped);
}

SCFG_TEST(CheckoutTests, test_browser_failure_falls_back) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.server_factory = fakes::broken_factory();
    SCFG_CHECK(t, !ctl.start_browser().has_value());
    SCFG_CHECK_EQ(t, show(ctl.error), "browser_unavailable");
    const std::vector<std::string> methods = ctl.methods();
    SCFG_CHECK(t, std::find(methods.begin(), methods.end(), "browser") == methods.end() && show(ctl.method()) == "qr");
}

SCFG_TEST(CheckoutTests, test_browser_session_info_carries_appearance) {
    std::optional<Setup> s = started(t, {.appearance = Json::dict({{"logo", "gui/logo.png"}, {"accent", "#ffcc00"}})});
    if (!s) return;
    (void)s->ctl->start_browser();
    if (!t.check(!s->servers->empty(), "a server")) return;
    const Json::Dict& info = info_of(*s).as_dict();
    SCFG_CHECK(t, info.get("logo").is_bool() && info.get("logo").as_bool());
    SCFG_CHECK(t, info.get("accent").as_str() == "#ffcc00" && info.get("on_accent").as_str() == "#17202e");
}

SCFG_TEST(CheckoutTests, test_browser_info_tells_the_page_how_gas_is_paid) {
    struct Case {
        const char* chain;
        bool gas_is_token;
        const char* min_fee;
        const char* native;
    };
    const Case cases[] = {
        {"arc", true, "20000000000", "USDC"},  // USDC is Arc's gas coin; 20 gwei mempool floor
        {"base", false, "0", "ETH"},
    };
    for (const Case& c : cases) {
        std::optional<Setup> s = setup(t);
        if (!s) return;
        if (!t.check(s->ctl->select_chain(c.chain).ok(), c.chain)) continue;
        s->ctl->poll();
        (void)s->ctl->start_browser();
        if (!t.check(!s->servers->empty(), std::string("a server on ") + c.chain)) continue;
        const Json::Dict& info = info_of(*s).as_dict();
        SCFG_CHECK(t, info.get("gas_is_token").is_bool() && info.get("gas_is_token").as_bool() == c.gas_is_token);
        SCFG_CHECK_EQ(t, info.get("min_fee_wei").as_str(), c.min_fee);
        SCFG_CHECK_EQ(t, info.get("native_symbol").as_str(), c.native);
        SCFG_CHECK(t, scfg_test::json_eq(info.get("chain_id"), Json(scfg::chains::MAINNET().find(c.chain)->chain_id)));
    }
}

// godot/tests/test_checkout.gd: what the EVM page is handed, key for key and in Python's order; no build_tx off
// Solana.
SCFG_TEST(CheckoutTests, test_session_info_is_the_exact_python_dict) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    (void)ctl.start_browser();
    if (!t.check(!s->servers->empty(), "a server")) return;
    const fakes::ServerLog& srv = *s->servers->front();
    SCFG_CHECK(t, srv.args.on_tx && srv.args.get_status && !srv.args.build_tx);
    const scfg::chains::Chain& arb = *scfg::chains::MAINNET().find("arbitrum");
    Json expected = Json::dict({
        {"title", "Chapter 2"},
        {"kind", "unlock"},
        {"token", "USDC"},
        {"family", "evm"},
        {"decimals", 6},
        {"amount_units", ctl.session().as_dict().get("units").as_bigint().to_decimal()},
        {"amount_text", ctl.pay_amount_text()},
        {"price_text", "4.99"},
        {"recipient", DEV},
        {"chain_name", "Arbitrum"},
        {"native_symbol", "ETH"},
        {"explorer_tx", "https://arbiscan.io/tx/{}"},
        {"testnet", false},
        {"logo", false},
        {"accent", "#0e7c74"},
        {"on_accent", "#ffffff"},
        {"chain_id", 42161},
        {"chain_id_hex", "0xa4b1"},
        {"rpc_url", arb.rpc.back()},
        {"token_address", usdc_arb()},
        {"gas_is_token", false},
        {"min_fee_wei", "0"},
    });
    const Json& info = srv.args.session_info;
    std::optional<std::string> diff = scfg_test::first_diff(info, expected);
    SCFG_CHECK_EQ(t, diff.value_or("same"), "same");
    SCFG_CHECK(t, info.as_dict().keys() == expected.as_dict().keys());
}

SCFG_TEST(CheckoutTests, test_messages_match_catalogue) {
    std::optional<std::string> text = scfg_test::vectors::read_text("messages.json");
    if (!t.check(text.has_value(), "conformance/vectors/messages.json")) return;
    Result<Json> doc = scfg::json::loads(*text);
    if (!t.check(doc.ok() && doc.value().is_dict(), "messages.json parses")) return;
    Json catalogue = doc.value().as_dict().get("messages");
    Json ours = Json::dict();
    for (const auto& [id, message] : scfg::checkout::MESSAGES()) ours.as_dict().set(id, message);
    std::optional<std::string> diff = scfg_test::first_diff(ours, catalogue);
    SCFG_CHECK_EQ(t, diff.value_or("same"), "same");
    SCFG_CHECK(t, ours.as_dict().keys() == std::vector<std::string>({"network_unavailable", "tx_reverted",
                                                                     "tx_not_a_payment", "tx_not_landed", "invalid_hash",
                                                                     "invalid_amount", "browser_unavailable",
                                                                     "start_failed", "start_unreachable",
                                                                     "solana_unavailable", "internal_error",
                                                                     "paste_blocked"}));
}

// A worker Stop() cancels the start: start_failed (not network_unavailable), back to choose.
SCFG_TEST(CheckoutTests, test_a_cancelled_start_is_a_failed_start) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    std::unique_ptr<CheckoutController> ctl = held_controller(t, sub);
    if (!ctl) return;
    SCFG_CHECK(t, ctl->choose_network("arbitrum").ok());
    if (!t.check(sub->starts.size() == 1, "one start")) return;
    sub->starts[0](scfg::Err::make("CancelledError", "stopped"));
    ctl->poll();
    SCFG_CHECK(t, ctl->state() == "choose" && show(ctl->error) == "start_failed" && !ctl->chain_key());
    SCFG_CHECK_EQ(t, ctl->step(), "network");
}

// Python would keep anything the future held; a record the controller cannot show is a failed start.
SCFG_TEST(CheckoutTests, test_a_start_that_is_not_a_session_is_a_failed_start) {
    const Json bad[] = {Json(), Json::list(), Json::dict({{"id", "ps_a"}}), Json::dict({{"id", 1}, {"units", 5}}),
                        Json::dict({{"id", "ps_a"}, {"units", "5"}})};
    for (const Json& rec : bad) {
        std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
        std::unique_ptr<CheckoutController> ctl = held_controller(t, sub);
        if (!ctl) return;
        SCFG_CHECK(t, ctl->select_chain("base").ok());
        if (!t.check(sub->starts.size() == 1, "one start")) return;
        sub->starts[0](rec);
        ctl->poll();
        SCFG_CHECK(t, ctl->state() == "choose" && show(ctl->error) == "start_failed" && ctl->session().is_null());
    }
}

// The worker calls back on a later frame: poll() before then changes nothing.
SCFG_TEST(CheckoutTests, test_a_start_delivered_later_is_picked_up_by_poll) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    std::unique_ptr<CheckoutController> ctl = held_controller(t, sub);
    if (!ctl) return;
    SCFG_CHECK(t, ctl->choose_network("base").ok());
    ctl->poll();
    SCFG_CHECK(t, ctl->state() == "starting" && ctl->step() == "network");
    if (!t.check(sub->starts.size() == 1, "one start")) return;
    sub->starts[0](Json::dict({{"id", "ps_a"}, {"units", 4993742}, {"chain", "base"}}));
    SCFG_CHECK_EQ(t, ctl->state(), "starting");  // nothing moves until poll()
    ctl->poll();
    SCFG_CHECK(t, ctl->state() == "waiting" && show(ctl->session_id()) == "ps_a" && ctl->step() == "method");
    SCFG_CHECK_EQ(t, ctl->pay_amount_text(), "4.993742");
}

// A confirming outcome without an error clears the message an earlier one left (checkout.py's `if outcome.error is
// None`), and one with an error keeps it.
SCFG_TEST(CheckoutTests, test_a_confirming_outcome_clears_the_message) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    const std::string sid = *ctl.session_id();
    StepOutcome down = outcome(sid, "waiting");
    down.error = "network_unavailable";
    ctl.on_outcome(down);
    SCFG_CHECK_EQ(t, show(ctl.error), "network_unavailable");
    StepOutcome still = outcome(sid, "confirming");
    still.error = "internal_error";
    ctl.on_outcome(still);
    SCFG_CHECK(t, show(ctl.error) == "internal_error" && ctl.state() == "confirming" && ctl.step() == "waiting");
    ctl.on_outcome(outcome(sid, "confirming"));
    SCFG_CHECK(t, !ctl.error && !ctl.message());
}

// --- review R1 / R3: what the Python tests leave unpinned ------------------------------------------------------------

// A paid outcome for the controller's own session, with its PaymentResult.
static StepOutcome final_outcome(const CheckoutController& ctl, std::string status, std::string tx) {
    StepOutcome o = outcome(*ctl.session_id(), status);
    scfg::models::PaymentResult r;
    r.status = std::move(status);
    r.tx_hash = Json(std::move(tx));
    r.session_id = Json(*ctl.session_id());
    o.result = r;
    return o;
}

// held_controller() with its start answered by a session on base.
static std::unique_ptr<CheckoutController> held_started(scfg_test::Ctx& t, const std::shared_ptr<HeldSubmitter>& sub) {
    std::unique_ptr<CheckoutController> ctl = held_controller(t, sub);
    if (!ctl) return nullptr;
    if (!t.check(ctl->choose_network("base").ok() && sub->starts.size() == 1, "one start")) return nullptr;
    sub->starts[0](Json::dict({{"id", "ps_a"}, {"units", 4993742}, {"chain", "base"}}));
    ctl->poll();
    if (!t.check(ctl->state() == "waiting", "started")) return nullptr;
    return ctl;
}

// R1: a sheet torn down without cancel() or finish() still stops its page.
SCFG_TEST(CheckoutTests, test_destroying_the_controller_stops_its_server) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    SCFG_CHECK_EQ(t, show(s->ctl->start_browser()), fakes::kServerUrl);
    if (!t.check(s->servers->size() == 1, "a server")) return;
    std::shared_ptr<fakes::ServerLog> srv = s->servers->front();
    SCFG_CHECK(t, srv->started && !srv->stopped);
    s->ctl.reset();
    SCFG_CHECK(t, srv->stopped);
}

SCFG_TEST(CheckoutTests, test_finish_stops_the_server) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    (void)s->ctl->start_browser();
    if (!t.check(s->servers->size() == 1, "a server")) return;
    (void)s->ctl->finish();
    SCFG_CHECK(t, s->servers->front()->stopped);
}

// Cancel after a final state marks nothing cancelled and returns the paid result.
SCFG_TEST(CheckoutTests, test_cancel_after_paid_keeps_the_paid_result) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    std::unique_ptr<CheckoutController> ctl = held_started(t, sub);
    if (!ctl) return;
    ctl->on_outcome(final_outcome(*ctl, "paid", TX));
    sub->names.clear();
    scfg::models::PaymentResult r = ctl->cancel();
    SCFG_CHECK(t, r.status == "paid" && r.tx_hash.is_str() && r.tx_hash.as_str() == TX);
    SCFG_CHECK(t, sub->names.empty());
}

// The 30-minute window closing after the payment landed changes nothing.
SCFG_TEST(CheckoutTests, test_expired_after_paid_keeps_paid) {
    for (const char* landed : {"paid", "underpaid"}) {
        std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
        std::unique_ptr<CheckoutController> ctl = held_started(t, sub);
        if (!ctl) return;
        ctl->on_outcome(final_outcome(*ctl, landed, TX));
        ctl->on_outcome(final_outcome(*ctl, "expired", "0x" + std::string(64, 'c')));
        SCFG_CHECK(t, ctl->state() == landed && ctl->result() && ctl->result()->status == landed);
        SCFG_CHECK(t, ctl->result() && ctl->result()->tx_hash.is_str() && ctl->result()->tx_hash.as_str() == TX);
        SCFG_CHECK_EQ(t, ctl->step(), "done");
    }
}

// Only tx_not_landed names a dropped transaction (the page offers Pay again for it).
SCFG_TEST(CheckoutTests, test_a_reverted_verdict_drops_no_transaction) {
    for (const char* verdict : {"tx_reverted", "tx_not_a_payment"}) {
        std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
        std::unique_ptr<CheckoutController> ctl = held_started(t, sub);
        if (!ctl) return;
        ctl->report_hint(TX);
        StepOutcome o = outcome(*ctl->session_id(), "waiting");
        o.error = verdict;
        o.hint_tx = TX;
        ctl->on_outcome(o);
        SCFG_CHECK(t, ctl->dropped_tx().is_null() && !ctl->hint_tx && show(ctl->error) == verdict);
    }
}

// Starting on another network forgets the hint, the dropped transaction and the confirmations of the old one.
SCFG_TEST(CheckoutTests, test_a_network_switch_forgets_the_old_payment) {
    std::optional<Setup> s = started(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    const std::string sid = *ctl.session_id();
    StepOutcome confirming = outcome(sid, "confirming");
    confirming.confirmations = 3;
    confirming.required = 20;
    ctl.on_outcome(confirming);
    StepOutcome dropped = outcome(sid, "waiting");
    dropped.error = "tx_not_landed";
    dropped.hint_tx = TX;
    ctl.on_outcome(dropped);
    ctl.hint_tx = "0x" + std::string(64, 'c');
    SCFG_CHECK(t, ctl.confirmations() == scfg::BigInt(3) && ctl.dropped_tx().is_str() && ctl.hint_tx);
    SCFG_CHECK(t, ctl.select_chain("base").ok());
    SCFG_CHECK(t, !ctl.hint_tx && ctl.dropped_tx().is_null() && ctl.confirmations() == scfg::BigInt(0));
}

// Picking the network that is still starting waits for that start: no second start, no skip to the method step.
SCFG_TEST(CheckoutTests, test_the_same_network_while_starting_does_not_skip_ahead) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    std::unique_ptr<CheckoutController> ctl = held_controller(t, sub);
    if (!ctl) return;
    SCFG_CHECK(t, ctl->choose_network("base").ok());
    SCFG_CHECK(t, ctl->choose_network("base").ok());
    SCFG_CHECK(t, ctl->step() == "network" && ctl->state() == "starting" && sub->starts.size() == 1);
}

// Only a waiting session is kept by picking its network again; a confirming one starts over (Python's rule).
SCFG_TEST(CheckoutTests, test_the_same_network_after_a_seen_payment_restarts) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    std::unique_ptr<CheckoutController> ctl = held_started(t, sub);
    if (!ctl) return;
    ctl->on_outcome(outcome(*ctl->session_id(), "confirming"));
    SCFG_CHECK_EQ(t, ctl->state(), "confirming");
    SCFG_CHECK(t, ctl->choose_network("base").ok());
    SCFG_CHECK(t, ctl->state() == "starting" && ctl->session().is_null() && sub->starts.size() == 2);
    SCFG_CHECK_EQ(t, ctl->step(), "network");
}

// Submitter.h: done is called at most once; a second Result is ignored and each callback runs once.
SCFG_TEST(CheckoutTests, test_a_delivery_keeps_its_first_result) {
    std::shared_ptr<scfg::worker::Delivery<Json>> d = std::make_shared<scfg::worker::Delivery<Json>>();
    std::vector<std::string> seen;
    d->add_done_callback([&seen](const Result<Json>& r) { seen.push_back(r.ok() ? r.value().as_str() : "err"); });
    scfg::worker::OnDone<Json> done = scfg::worker::Delivery<Json>::receiver(d);
    SCFG_CHECK(t, !d->done());
    done(Json("first"));
    done(Json("second"));
    d->deliver(scfg::Err::make("RuntimeError", "late"));
    SCFG_CHECK(t, d->done() && d->result().ok() && d->result().value().as_str() == "first");
    SCFG_CHECK(t, seen == std::vector<std::string>({"first"}));
    d->add_done_callback([&seen](const Result<Json>& r) { seen.push_back(r.ok() ? "late " + r.value().as_str() : "late err"); });
    SCFG_CHECK(t, seen == std::vector<std::string>({"first", "late first"}));  // at once on a done delivery
}
