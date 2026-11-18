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
//   test_the_method_step_offers_the_buy_page_for_the_chosen_network        -> the same name
//   test_on_arc_the_buy_amount_keeps_a_cent_for_the_fee                    -> the same name
//   test_the_buy_link_moves_to_the_only_payment_page                       -> the same name
//   test_the_buy_link_follows_the_onramp_option                            -> the same name
//   test_before_a_session_the_buy_link_suggests_the_most_the_payment_can_be -> the same name
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
    std::optional<Setup> s = setup(t, {.chains = Json::list({"base"})});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.state() == "starting" && show(ctl.chain_key()) == "base");
    ctl.poll();
    SCFG_CHECK(t, ctl.step() == "method" && !ctl.can_go_back());
    SCFG_CHECK(t, ctl.step_path() == std::vector<std::string>({"method", "browser", "waiting"}));
}

SCFG_TEST(CheckoutStepsTests, test_single_network_is_skipped_for_tips_both_ways) {
    std::optional<Setup> s = setup(t, {.kind = "tip", .chains = Json::list({"base"})});
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
    std::optional<Setup> s = setup(t, {.kind = "tip", .chains = Json::list({"base"})});
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

// --- the buy link (configure's onramp) ------------------------------------------------------------------------------

SCFG_TEST(CheckoutStepsTests, test_the_method_step_offers_the_buy_page_for_the_chosen_network) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.onramp_url().ok() && !ctl.onramp_url().value() && !ctl.shows_onramp());  // no network yet
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    ctl.poll();
    SCFG_CHECK(t, ctl.step() == "method" && ctl.shows_onramp());
    const scfg::BigInt units = ctl.session().as_dict().get("units").as_bigint();
    SCFG_CHECK(t, scfg::BigInt(4'990'000) <= units && units < scfg::BigInt(5'000'000));
    SCFG_CHECK_EQ(t, show(ctl.onramp_url().value()),
                  "https://thirdweb.com/bridge/buy-widget?chain=42161&tokenAddress=" + usdc_arb() +
                      "&amount=5.00&tokenEditable=false&paymentMethods=card");
}

SCFG_TEST(CheckoutStepsTests, test_on_arc_the_buy_amount_keeps_a_cent_for_the_fee) {
    std::optional<Setup> s = setup(t, {.chains = Json::list({"base", "arc"})});
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.choose_network("arc").ok());
    ctl.poll();
    const std::string url = show(ctl.onramp_url().value());
    SCFG_CHECK(t, url.ends_with("&amount=5.01&tokenEditable=false&paymentMethods=card"));
}

SCFG_TEST(CheckoutStepsTests, test_the_buy_link_moves_to_the_only_payment_page) {
    std::optional<Setup> s = on_method_step(t, {.methods = only_manual()});
    if (!s) return;
    SCFG_CHECK(t, s->ctl->step() == "manual" && s->ctl->shows_onramp());
    s.reset();
    s = on_method_step(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    ctl.choose_method("qr");
    SCFG_CHECK(t, ctl.step() == "qr" && !ctl.shows_onramp());  // the method step showed it
    ctl.mark_sent();
    SCFG_CHECK(t, !ctl.shows_onramp());
}

SCFG_TEST(CheckoutStepsTests, test_the_buy_link_follows_the_onramp_option) {
    std::optional<Setup> s = on_method_step(t, {.onramp = Json::dict({{"enabled", false}})});
    if (!s) return;
    SCFG_CHECK(t, !s->ctl->onramp_url().value() && !s->ctl->shows_onramp());
    s.reset();
    s = on_method_step(t, {.onramp = Json::dict({{"chains", Json::list({"base"})}})});
    if (!s) return;
    SCFG_CHECK(t, show(s->ctl->chain_key()) == "arbitrum" && !s->ctl->onramp_url().value());
    s.reset();
    s = on_method_step(t, {.onramp = Json::dict({{"url", "https://buy.example/{chain}/{token}?a={amount}"}})});
    if (!s) return;
    SCFG_CHECK_EQ(t, show(s->ctl->onramp_url().value()), "https://buy.example/arbitrum/USDC?a=5.00");
}

SCFG_TEST(CheckoutStepsTests, test_before_a_session_the_buy_link_suggests_the_most_the_payment_can_be) {
    Result<scfg::config::Config> cfg =
        scfg::config::build_config(Json::dict({{"wallet", DEV}, {"chains", Json::list({"arbitrum", "arc"})}}));
    if (!t.check(cfg.ok(), "build_config")) return;
    const scfg::Dec price = *scfg::Dec::parse("4.99").value();
    const std::vector<std::string> path = {"network", "method", "browser", "waiting"};
    const std::vector<std::string> keys = {"arbitrum", "arc"};
    auto view = [&](std::optional<std::string> key) {
        return scfg::checkout::onramp_view(cfg.value(), "USDC", key, std::nullopt, price, "method", path, true, keys);
    };
    // 4.99 plus at most 0.009999 of item code and nonce: the session's 4.993742 also gives 5.00
    SCFG_CHECK(t, show(view("arbitrum").value().url).find("&amount=5.00&") != std::string::npos);
    SCFG_CHECK(t, show(view("arc").value().url).find("&amount=5.01&") != std::string::npos);
    const scfg::checkout::OnrampView none = view(std::nullopt).value();
    SCFG_CHECK(t, !none.url && !none.shows_onramp && none.shows_onramp_hint);
}
