// Copyright 2026 Repulse
// No Python twin: the stepped payment sheet (CheckoutController::step() and its navigation), the twins of
// tests/unit/test_checkout_steps.py one for one (same names, same assertions), over a real Engine on the fake nodes
// of support/CheckoutFakes.h with the immediate submitter.
//
// Python test (tests/unit/test_checkout_steps.py)                          -> C++ test (CheckoutStepsTests.<name>)
//   test_unlock_starts_on_network_and_tip_on_amount                        -> the same name
//   test_step_path_counts_the_steps_shown                                  -> the same name
//   test_network_waits_for_the_session_then_moves_to_method                -> the same name
//   test_choosing_a_method_opens_its_page                                  -> the same name
//   test_unknown_method_is_ignored                                         -> the same name
//   test_opening_the_browser_moves_to_waiting                              -> the same name
//   test_browser_failure_returns_to_method                                 -> the same name
//   test_ive_sent_it_moves_qr_and_manual_to_waiting                        -> the same name
//   test_ive_sent_it_clears_a_bad_hash_error                               -> the same name
//   test_tip_amount_step_applies_a_typed_amount                            -> the same name
//   test_a_typed_amount_is_stripped_as_str_strip_does                      -> the same name
//   test_single_network_is_skipped_for_unlocks                             -> the same name
//   test_single_network_is_skipped_for_tips_both_ways                      -> the same name
//   test_single_method_is_skipped_both_ways                                -> the same name
//   test_back_walks_the_path_and_keeps_the_session                         -> the same name
//   test_choosing_the_same_network_again_keeps_the_session                 -> the same name
//   test_choosing_another_network_restarts_the_session                     -> the same name
//   test_back_to_amount_and_same_amount_keeps_the_session                  -> the same name
//   test_back_clears_the_message                                           -> the same name
//   test_a_seen_payment_moves_to_waiting_and_hides_back                    -> the same name
//   test_a_pasted_hash_moves_to_waiting_and_hides_back                     -> the same name
//   test_a_hash_from_the_browser_page_moves_to_waiting                     -> the same name
//   test_a_dropped_transaction_gives_back_the_arrow                        -> the same name
//   test_final_states_move_to_done                                         -> the same name
//   test_start_failure_returns_to_network_with_the_message                 -> the same name
//   test_start_failure_on_a_skipped_network_shows_the_network_step         -> the same name
//   test_a_session_restarted_on_the_amount_step_waits_for_a_network_pick   -> the same name
//   test_leaving_the_network_step_while_starting_stays_put                 -> the same name
//   test_web_offers_browser_wallet_only_when_the_page_has_one              -> the same name
//   test_web_browser_wallet_ignores_the_mobile_flag                        -> the same name
//   test_the_controller_pumps_and_reopens_an_overlay_page                  -> the same name
//   test_the_main_networks_are_tiles_and_the_rest_sit_behind_more_networks -> the same name
//   test_more_networks_stays_open_when_the_chosen_network_is_behind_it     -> the same name
//   test_without_a_main_network_every_network_is_a_tile                    -> the same name
//   test_pay_with_card_skips_the_method_step_for_the_card_page             -> the same name
//   test_the_networks_own_tile_still_goes_to_the_method_step               -> the same name
//   test_pay_with_card_follows_the_onramp_option                           -> the same name
//   test_a_single_network_with_a_card_button_keeps_the_network_step        -> the same name
//   test_open_card_waits_for_the_session                                   -> the same name
// Python's FakeServer has no pump() or reopen() and PageWithPump adds them; the one FakeServer here has both (a
// server without them is the Server base's no-ops, which nothing can observe). ctl._server is the test's own
// record of the server it made (support/CheckoutFakes.h, ServerLog).
#include "support/CheckoutFakes.h"
#include "support/Test.h"

#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Checkout.h"
#include "Scfg/Config.h"
#include "Scfg/Dec.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/Watcher.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::Json;
using scfg::Result;
using scfg::checkout::CheckoutController;
using scfg::watcher::StepOutcome;
namespace fakes = scfg_test::fakes;

namespace scfg_test::checkout_steps_tests {

constexpr const char* DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
constexpr const char* PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const std::string TX = "0xabababababababababababababababababababababababababababababababab";

std::string usdc_arb() { return scfg::chains::MAINNET().find("arbitrum")->tokens.find("USDC")->address; }
std::string show(const std::optional<std::string>& s) { return s ? *s : std::string("None"); }
// No "Pay with card" button: a single network is skipped.
Json no_card() { return Json::dict({{"enabled", false}}); }
Json only_manual() {
    return Json::dict({{"browser", Json::dict({{"enabled", false}})}, {"qr", Json::dict({{"enabled", false}})}});
}

using Setup = fakes::Rig;

struct Opts {
    std::string kind = "unlock";
    Json chains{};
    Json methods{};
    std::function<bool(const scfg::chains::Chain&)> browser_wallet{};
    Json onramp{};
};

// setup(kind="unlock", chains=None, methods=None, browser_wallet=None): (ctl, engine, fakes, clock).
std::optional<Setup> setup(Ctx& t, const Opts& o = {}) {
    fakes::RigOptions r;
    r.kind = o.kind;
    r.config = Json::dict({{"wallet", DEV}});
    if (!o.chains.is_null()) r.config.as_dict().set("chains", o.chains);
    if (!o.methods.is_null()) r.config.as_dict().set("methods", o.methods);
    if (!o.onramp.is_null()) r.config.as_dict().set("onramp", o.onramp);
    r.browser_wallet = o.browser_wallet;
    Result<Setup> rig = fakes::make_rig(std::move(r));
    if (!t.check(rig.ok(), rig.ok() ? "" : "setup: " + rig.err().str())) return std::nullopt;
    return rig.take();
}

// on_method_step(**kw): choose_network("arbitrum"), then poll().
std::optional<Setup> on_method_step(Ctx& t, const Opts& o = {}) {
    std::optional<Setup> s = setup(t, o);
    if (!s) return std::nullopt;
    if (!t.check(s->ctl->choose_network("arbitrum").ok(), "choose_network")) return std::nullopt;
    s->ctl->poll();
    return s;
}

bool deliver(Ctx& t, Setup& s) {
    scfg::Status ticked = s.deliver();
    if (!t.check(ticked.ok(), ticked.ok() ? "" : "tick: " + ticked.err().str())) return false;
    for (const std::string& p : s.nodes->problems) t.fail("fake node: " + p);
    return s.nodes->problems.empty();
}

bool is_true(const Result<bool>& r) { return r.ok() && r.value(); }
bool is_false(const Result<bool>& r) { return r.ok() && !r.value(); }

StepOutcome outcome(std::string sid, std::string status) {
    StepOutcome o;
    o.session_id = std::move(sid);
    o.status = std::move(status);
    return o;
}

}  // namespace scfg_test::checkout_steps_tests

using namespace scfg_test::checkout_steps_tests;

// --- where the sheet starts ---------------------------------------------------------------------------------------

SCFG_TEST(CheckoutStepsTests, test_unlock_starts_on_network_and_tip_on_amount) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    SCFG_CHECK(t, s->ctl->step() == "network" && !s->ctl->can_go_back());
    s.reset();  // the old rig goes first, controller before nodes
    s = setup(t, {.kind = "tip"});
    if (!s) return;
    SCFG_CHECK(t, s->ctl->step() == "amount" && !s->ctl->can_go_back());
}

SCFG_TEST(CheckoutStepsTests, test_step_path_counts_the_steps_shown) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    SCFG_CHECK(t, s->ctl->step_path() == std::vector<std::string>({"network", "method", "browser", "waiting"}));
    SCFG_CHECK(t, (s->ctl->step_dots() == std::pair<int64_t, int64_t>(0, 4)));
    s.reset();  // the old rig goes first, controller before nodes
    s = setup(t, {.kind = "tip"});
    if (!s) return;
    SCFG_CHECK(t, s->ctl->step_path() ==
                      std::vector<std::string>({"amount", "network", "method", "browser", "waiting"}));
}

// --- forward ------------------------------------------------------------------------------------------------------

SCFG_TEST(CheckoutStepsTests, test_network_waits_for_the_session_then_moves_to_method) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    SCFG_CHECK(t, ctl.step() == "network" && ctl.state() == "starting");
    ctl.poll();
    SCFG_CHECK(t, ctl.step() == "method" && ctl.nav_dir() == 1);
    SCFG_CHECK(t, !s->pending(ctl.session_id()).is_null());
    SCFG_CHECK(t, (ctl.step_dots() == std::pair<int64_t, int64_t>(1, 4)));
}

SCFG_TEST(CheckoutStepsTests, test_choosing_a_method_opens_its_page) {
    for (const char* name : {"browser", "qr", "manual"}) {
        std::optional<Setup> s = on_method_step(t);
        if (!s) return;
        s->ctl->choose_method(name);
        SCFG_CHECK(t, s->ctl->step() == name && show(s->ctl->method()) == name);
    }
}

SCFG_TEST(CheckoutStepsTests, test_unknown_method_is_ignored) {
    std::optional<Setup> s = on_method_step(t, {.methods = Json::dict({{"qr", Json::dict({{"enabled", false}})}})});
    if (!s) return;
    s->ctl->choose_method("qr");
    SCFG_CHECK_EQ(t, s->ctl->step(), "method");
}

SCFG_TEST(CheckoutStepsTests, test_opening_the_browser_moves_to_waiting) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("browser");
    SCFG_CHECK_EQ(t, show(ctl.open_browser()), fakes::SERVER_URL);
    SCFG_CHECK(t, ctl.step() == "waiting" && ctl.can_go_back());
}

SCFG_TEST(CheckoutStepsTests, test_browser_failure_returns_to_method) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("browser");
    ctl.server_factory = fakes::broken_factory();
    SCFG_CHECK(t, !ctl.open_browser().has_value());
    SCFG_CHECK(t, ctl.step() == "method" && ctl.nav_dir() == -1);
    SCFG_CHECK(t, show(ctl.error) == "browser_unavailable" && show(ctl.method()) == "qr");
}

SCFG_TEST(CheckoutStepsTests, test_ive_sent_it_moves_qr_and_manual_to_waiting) {
    for (const char* name : {"qr", "manual"}) {
        std::optional<Setup> s = on_method_step(t);
        if (!s) return;
        s->ctl->choose_method(name);
        s->ctl->mark_sent();
        SCFG_CHECK(t, s->ctl->step() == "waiting" && s->ctl->can_go_back());
    }
}

SCFG_TEST(CheckoutStepsTests, test_ive_sent_it_clears_a_bad_hash_error) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("manual");
    (void)ctl.submit_hash("garbage");
    SCFG_CHECK_EQ(t, show(ctl.error), "invalid_hash");
    ctl.mark_sent();
    SCFG_CHECK(t, ctl.step() == "waiting" && !ctl.error);
}

SCFG_TEST(CheckoutStepsTests, test_tip_amount_step_applies_a_typed_amount) {
    std::optional<Setup> s = setup(t, {.kind = "tip"});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.custom_amount = "0.10";
    SCFG_CHECK(t, is_false(ctl.next_step()));
    SCFG_CHECK(t, ctl.step() == "amount" && show(ctl.error) == "invalid_amount");
    ctl.custom_amount = "7.50";
    SCFG_CHECK(t, is_true(ctl.next_step()));
    SCFG_CHECK(t, ctl.step() == "network" && ctl.amount().str() == "7.50");
}

// The typed amount is read through str.strip(): \x85 and \x1c are whitespace (an amount of only those is no
// amount), U+FEFF is not (the amount is invalid).
SCFG_TEST(CheckoutStepsTests, test_a_typed_amount_is_stripped_as_str_strip_does) {
    std::optional<Setup> s = setup(t, {.kind = "tip"});
    if (!s) return;
    s->ctl->custom_amount = "\xC2\x85\x1C";
    SCFG_CHECK(t, is_true(s->ctl->next_step()) && s->ctl->step() == "network");
    s.reset();  // the old rig goes first, controller before nodes
    s = setup(t, {.kind = "tip"});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.custom_amount = "\xEF\xBB\xBF" "7.50";
    SCFG_CHECK(t, is_false(ctl.next_step()) && show(ctl.error) == "invalid_amount");
    SCFG_CHECK(t, is_false(ctl.apply_custom_amount()));
    ctl.custom_amount = "\xC2\x85" "7.50\x1C";
    SCFG_CHECK(t, is_true(ctl.apply_custom_amount()) && ctl.amount().str() == "7.50");
}

// --- skipping single-option steps ---------------------------------------------------------------------------------

SCFG_TEST(CheckoutStepsTests, test_single_network_is_skipped_for_unlocks) {
    std::optional<Setup> s = setup(t, {.chains = Json::list({"base"}), .onramp = no_card()});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.state() == "starting" && show(ctl.chain_key()) == "base");
    ctl.poll();
    SCFG_CHECK(t, ctl.step() == "method" && !ctl.can_go_back());
    SCFG_CHECK(t, ctl.step_path() == std::vector<std::string>({"method", "browser", "waiting"}));
}

SCFG_TEST(CheckoutStepsTests, test_single_network_is_skipped_for_tips_both_ways) {
    std::optional<Setup> s = setup(t, {.kind = "tip", .chains = Json::list({"base"}), .onramp = no_card()});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    (void)ctl.next_step();
    ctl.poll();
    SCFG_CHECK_EQ(t, ctl.step(), "method");
    ctl.back();
    SCFG_CHECK(t, ctl.step() == "amount" && ctl.nav_dir() == -1);
}

SCFG_TEST(CheckoutStepsTests, test_single_method_is_skipped_both_ways) {
    std::optional<Setup> s = on_method_step(t, {.methods = only_manual()});
    if (!s) return;
    SCFG_CHECK_EQ(t, s->ctl->step(), "manual");
    s->ctl->back();
    SCFG_CHECK_EQ(t, s->ctl->step(), "network");
}

// --- back ---------------------------------------------------------------------------------------------------------

SCFG_TEST(CheckoutStepsTests, test_back_walks_the_path_and_keeps_the_session) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    const std::optional<std::string> sid = ctl.session_id();
    ctl.choose_method("qr");
    ctl.mark_sent();
    ctl.back();
    SCFG_CHECK(t, ctl.step() == "qr" && ctl.nav_dir() == -1);
    ctl.back();
    SCFG_CHECK_EQ(t, ctl.step(), "method");
    ctl.back();
    SCFG_CHECK(t, ctl.step() == "network" && !ctl.can_go_back());
    ctl.back();  // nothing before network on an unlock
    SCFG_CHECK_EQ(t, ctl.step(), "network");
    SCFG_CHECK(t, ctl.session_id() == sid && !s->pending(sid).is_null());
}

SCFG_TEST(CheckoutStepsTests, test_choosing_the_same_network_again_keeps_the_session) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    const std::optional<std::string> sid = ctl.session_id();
    ctl.back();
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    SCFG_CHECK(t, ctl.step() == "method" && ctl.session_id() == sid && ctl.nav_dir() == 1);
}

SCFG_TEST(CheckoutStepsTests, test_choosing_another_network_restarts_the_session) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    const std::optional<std::string> sid = ctl.session_id();
    ctl.back();
    SCFG_CHECK(t, ctl.choose_network("base").ok());
    ctl.poll();
    SCFG_CHECK(t, ctl.step() == "method" && ctl.session_id() != sid);
    SCFG_CHECK(t, s->pending(sid).is_null());
}

SCFG_TEST(CheckoutStepsTests, test_back_to_amount_and_same_amount_keeps_the_session) {
    std::optional<Setup> s = on_method_step(t, {.kind = "tip"});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    const std::optional<std::string> sid = ctl.session_id();
    ctl.back();
    ctl.back();
    SCFG_CHECK_EQ(t, ctl.step(), "amount");
    SCFG_CHECK(t, is_true(ctl.set_tip_amount("3")) && ctl.session_id() == sid);
    SCFG_CHECK(t, is_true(ctl.set_tip_amount("5")));
    ctl.poll();
    SCFG_CHECK(t, ctl.session_id() != sid && ctl.step() == "amount");
    (void)ctl.next_step();
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    SCFG_CHECK_EQ(t, ctl.step(), "method");
}

SCFG_TEST(CheckoutStepsTests, test_back_clears_the_message) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("manual");
    (void)ctl.submit_hash("garbage");
    SCFG_CHECK_EQ(t, show(ctl.error), "invalid_hash");
    ctl.back();
    SCFG_CHECK(t, !ctl.error);
}

// --- automatic moves ----------------------------------------------------------------------------------------------

SCFG_TEST(CheckoutStepsTests, test_a_seen_payment_moves_to_waiting_and_hides_back) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("qr");
    s->fake("arbitrum").logs.push_back(fakes::transfer_log(
        usdc_arb(), PAYER, DEV, ctl.session().as_dict().get("units").as_int64(), 999, TX));
    if (!deliver(t, *s)) return;
    SCFG_CHECK_EQ(t, ctl.state(), "confirming");
    SCFG_CHECK(t, ctl.step() == "waiting" && ctl.nav_dir() == 1 && !ctl.can_go_back());
}

SCFG_TEST(CheckoutStepsTests, test_a_pasted_hash_moves_to_waiting_and_hides_back) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("manual");
    SCFG_CHECK(t, is_true(ctl.submit_hash(TX)));
    SCFG_CHECK(t, ctl.step() == "waiting" && !ctl.can_go_back());
}

SCFG_TEST(CheckoutStepsTests, test_a_hash_from_the_browser_page_moves_to_waiting) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("browser");
    (void)ctl.open_browser();
    if (!t.check(!s->servers->empty(), "a server")) return;
    s->servers->front()->args.on_tx(TX, std::string(PAYER));
    SCFG_CHECK(t, ctl.step() == "waiting" && !ctl.can_go_back());
}

SCFG_TEST(CheckoutStepsTests, test_a_dropped_transaction_gives_back_the_arrow) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("manual");
    (void)ctl.submit_hash(TX);
    StepOutcome dropped = outcome(*ctl.session_id(), "waiting");
    dropped.error = "tx_not_landed";
    dropped.hint_tx = TX;
    ctl.on_outcome(dropped);
    SCFG_CHECK(t, ctl.step() == "waiting" && ctl.can_go_back());
    ctl.back();
    SCFG_CHECK_EQ(t, ctl.step(), "manual");
}

SCFG_TEST(CheckoutStepsTests, test_final_states_move_to_done) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("qr");
    s->fake("arbitrum").logs.push_back(fakes::transfer_log(
        usdc_arb(), PAYER, DEV, ctl.session().as_dict().get("units").as_int64(), 999, TX));
    if (!deliver(t, *s)) return;
    s->fake("arbitrum").head = 1100;
    if (!deliver(t, *s)) return;
    SCFG_CHECK(t, ctl.state() == "paid" && ctl.step() == "done" && !ctl.can_go_back());

    std::optional<Setup> s2 = on_method_step(t);
    if (!s2) return;
    s2->ctl->choose_method("browser");
    s2->clock.advance(1800);
    if (!deliver(t, *s2)) return;
    SCFG_CHECK(t, s2->ctl->state() == "expired" && s2->ctl->step() == "done");
}

SCFG_TEST(CheckoutStepsTests, test_start_failure_returns_to_network_with_the_message) {
    std::optional<Setup> s = setup(t, {.chains = Json::list({"base"})});  // replaced at once, as in Python
    s.reset();  // the old rig goes first, controller before nodes
    s = setup(t);
    if (!s) return;
    s->fake("arbitrum") = fakes::FakeChain();
    s->fake("arbitrum").down = true;
    SCFG_CHECK(t, s->ctl->choose_network("arbitrum").ok());
    s->ctl->poll();
    SCFG_CHECK(t, s->ctl->step() == "network" && show(s->ctl->error) == "start_unreachable");
}

SCFG_TEST(CheckoutStepsTests, test_start_failure_on_a_skipped_network_shows_the_network_step) {
    std::optional<Setup> s = setup(t, {.kind = "tip", .chains = Json::list({"base"}), .onramp = no_card()});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    s->fake("base") = fakes::FakeChain();
    s->fake("base").down = true;
    (void)ctl.next_step();
    ctl.poll();
    SCFG_CHECK(t, ctl.step() == "network" && show(ctl.error) == "start_unreachable");
    SCFG_CHECK(t, ctl.can_go_back());
    s->fake("base").down = false;
    SCFG_CHECK(t, ctl.choose_network("base").ok());
    ctl.poll();
    SCFG_CHECK_EQ(t, ctl.step(), "method");
}

SCFG_TEST(CheckoutStepsTests, test_a_session_restarted_on_the_amount_step_waits_for_a_network_pick) {
    std::optional<Setup> s = setup(t, {.kind = "tip"});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    (void)ctl.next_step();
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    ctl.poll();
    ctl.back();
    ctl.back();
    SCFG_CHECK_EQ(t, ctl.step(), "amount");
    (void)ctl.set_tip_amount("5");  // restarts the session on arbitrum
    (void)ctl.next_step();
    ctl.poll();                           // ... which is ready now
    SCFG_CHECK_EQ(t, ctl.step(), "network");  // but the player hasn't picked yet
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    SCFG_CHECK_EQ(t, ctl.step(), "method");
}

SCFG_TEST(CheckoutStepsTests, test_leaving_the_network_step_while_starting_stays_put) {
    std::optional<Setup> s = setup(t, {.kind = "tip"});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    (void)ctl.next_step();
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    ctl.back();
    ctl.poll();
    SCFG_CHECK(t, ctl.step() == "amount" && ctl.state() == "waiting");
}

// --- web build ----------------------------------------------------------------------------------------------------

SCFG_TEST(CheckoutStepsTests, test_web_offers_browser_wallet_only_when_the_page_has_one) {
    std::optional<Setup> s = on_method_step(t, {.browser_wallet = [](const scfg::chains::Chain&) { return false; }});
    if (!s) return;
    SCFG_CHECK(t, s->ctl->methods() == std::vector<std::string>({"qr", "manual"}));
    s.reset();  // the old rig goes first, controller before nodes
    s = on_method_step(t, {.browser_wallet = [](const scfg::chains::Chain& c) { return c.key == "arbitrum"; }});
    if (!s) return;
    SCFG_CHECK(t, s->ctl->methods() == std::vector<std::string>({"browser", "qr", "manual"}));
}

SCFG_TEST(CheckoutStepsTests, test_web_browser_wallet_ignores_the_mobile_flag) {
    std::optional<Setup> s = setup(t, {.browser_wallet = [](const scfg::chains::Chain&) { return true; }});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.mobile = true;
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    ctl.poll();
    const std::vector<std::string> methods = ctl.methods();
    SCFG_CHECK(t, std::find(methods.begin(), methods.end(), "browser") != methods.end());
}

SCFG_TEST(CheckoutStepsTests, test_the_controller_pumps_and_reopens_an_overlay_page) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.server_factory = fakes::server_factory(s->servers);  // PageWithPump
    ctl.choose_method("browser");
    (void)ctl.open_browser();
    ctl.poll();
    if (!t.check(!s->servers->empty(), "a server")) return;
    SCFG_CHECK_EQ(t, s->servers->front()->pumps, 1);
    (void)ctl.open_browser();
    SCFG_CHECK_EQ(t, s->servers->front()->reopens, 1);
}

// --- the network step: main tiles, "More networks", "Pay with card" --------------------------------------------------

namespace scfg_test::checkout_steps_tests {
std::vector<std::string> keys(std::initializer_list<const char*> names) { return {names.begin(), names.end()}; }
scfg::checkout::NetworkView layout_of(Ctx& t, const CheckoutController& ctl) {
    Result<scfg::checkout::NetworkView> v = ctl.network_layout();
    t.check(v.ok(), v.ok() ? "" : "network_layout: " + v.err().str());
    return v.ok() ? v.value() : scfg::checkout::NetworkView{};
}
}  // namespace scfg_test::checkout_steps_tests

SCFG_TEST(CheckoutStepsTests, test_the_main_networks_are_tiles_and_the_rest_sit_behind_more_networks) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, layout_of(t, ctl) == (scfg::checkout::NetworkView{
                                           keys({"ethereum"}), keys({"arc", "base", "arbitrum", "polygon"}), false,
                                           keys({"ethereum", "arc", "base", "arbitrum", "polygon"})}));
    ctl.toggle_more_networks();
    SCFG_CHECK(t, layout_of(t, ctl).expanded);
    ctl.toggle_more_networks();
    SCFG_CHECK(t, !layout_of(t, ctl).expanded);
}

SCFG_TEST(CheckoutStepsTests, test_more_networks_stays_open_when_the_chosen_network_is_behind_it) {
    std::optional<Setup> s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.back();
    SCFG_CHECK(t, ctl.step() == "network" && layout_of(t, ctl).expanded);
    ctl.toggle_more_networks();               // closing it hides the chosen network...
    SCFG_CHECK(t, layout_of(t, ctl).expanded);  // ...so it stays open while it's chosen
}

SCFG_TEST(CheckoutStepsTests, test_without_a_main_network_every_network_is_a_tile) {
    std::optional<Setup> s = setup(t, {.chains = Json::list({"base", "arc"})});
    if (!s) return;
    SCFG_CHECK(t, layout_of(t, *s->ctl) ==
                      (scfg::checkout::NetworkView{keys({"base", "arc"}), {}, false, keys({"base", "arc"})}));
}

SCFG_TEST(CheckoutStepsTests, test_pay_with_card_skips_the_method_step_for_the_card_page) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.card_url().ok() && !ctl.card_url().value());
    SCFG_CHECK(t, ctl.pay_with_card("arbitrum").ok());
    ctl.poll();
    SCFG_CHECK(t, ctl.step() == "card" && show(ctl.method()) == "card");
    SCFG_CHECK(t, ctl.step_path() == keys({"network", "card", "waiting"}));
    SCFG_CHECK(t, (ctl.step_dots() == std::pair<int64_t, int64_t>(1, 3)));
    const int64_t units = ctl.session().as_dict().get("units").as_int64();
    SCFG_CHECK(t, 4'990'000 <= units && units < 5'000'000);
    std::string nonce = std::to_string(units % 10'000);
    nonce.insert(0, 4 - nonce.size(), '0');
    const std::string amount = "4.99" + nonce;
    SCFG_CHECK_EQ(t, show(ctl.card_url().value()),
                  "https://thirdweb.com/bridge/checkout-widget?chain=42161&tokenAddress=" + usdc_arb() +
                      "&amount=" + amount + "&seller=" + DEV + "&paymentMethods=card");
    const std::string expected = show(ctl.card_url().value());
    SCFG_CHECK_EQ(t, show(ctl.open_card().value()), expected);
    SCFG_CHECK_EQ(t, ctl.step(), "waiting");
    ctl.back();
    SCFG_CHECK_EQ(t, ctl.step(), "card");
    ctl.back();
    SCFG_CHECK(t, ctl.step() == "network" && !ctl.can_go_back());
}

SCFG_TEST(CheckoutStepsTests, test_the_networks_own_tile_still_goes_to_the_method_step) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.pay_with_card("arbitrum").ok());
    ctl.poll();
    ctl.back();
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());  // same network: the session is kept
    SCFG_CHECK(t, ctl.step() == "method" && show(ctl.method()) == "browser");
    const std::optional<std::string> sid = ctl.session_id();
    ctl.back();
    SCFG_CHECK(t, ctl.pay_with_card("arbitrum").ok());
    SCFG_CHECK(t, ctl.step() == "card" && ctl.session_id() == sid);
}

SCFG_TEST(CheckoutStepsTests, test_pay_with_card_follows_the_onramp_option) {
    std::optional<Setup> s = setup(t, {.onramp = Json::dict({{"enabled", false}})});
    if (!s) return;
    SCFG_CHECK(t, layout_of(t, *s->ctl).card.empty());
    SCFG_CHECK(t, s->ctl->pay_with_card("arbitrum").ok());  // no button: nothing happens
    SCFG_CHECK(t, s->ctl->step() == "network" && s->ctl->session().is_null());
    s.reset();
    s = setup(t, {.onramp = Json::dict({{"chains", Json::list({"base"})}})});
    if (!s) return;
    SCFG_CHECK(t, layout_of(t, *s->ctl).card == keys({"base"}));
    s.reset();
    s = setup(t, {.onramp = Json::dict({{"url", "https://pay.example/{chain}/{token}?a={amount}&to={recipient}"}})});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.pay_with_card("base").ok());
    ctl.poll();
    SCFG_CHECK_EQ(t, show(ctl.card_url().value()),
                  "https://pay.example/base/USDC?a=" + ctl.pay_amount_text() + "&to=" + DEV);
}

SCFG_TEST(CheckoutStepsTests, test_a_single_network_with_a_card_button_keeps_the_network_step) {
    std::optional<Setup> s = setup(t, {.chains = Json::list({"base"})});
    if (!s) return;
    SCFG_CHECK(t, s->ctl->step() == "network" && s->ctl->step_path()[0] == "network");
    s.reset();
    s = setup(t, {.chains = Json::list({"base"}), .onramp = no_card()});
    if (!s) return;
    s->ctl->poll();
    SCFG_CHECK_EQ(t, s->ctl->step(), "method");  // nothing to choose: skipped as before
}

SCFG_TEST(CheckoutStepsTests, test_open_card_waits_for_the_session) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.open_card().ok() && !ctl.open_card().value());  // not on the card page
    SCFG_CHECK(t, ctl.pay_with_card("base").ok());
    SCFG_CHECK(t, ctl.step() == "network" && !ctl.open_card().value());
    ctl.poll();
    SCFG_CHECK_EQ(t, ctl.step(), "card");
    SCFG_CHECK(t, ctl.open_card().value().has_value() && ctl.step() == "waiting");
}
