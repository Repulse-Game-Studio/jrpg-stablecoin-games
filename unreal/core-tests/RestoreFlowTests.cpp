// No Python twin: tests of scfg::restore_flow::RestoreController, the twins of tests/unit/test_restore_flow.py one
// for one (same names, same assertions), over a real Engine on the fake nodes of support/CheckoutFakes.h with the
// immediate submitter.
//
// Python test (tests/unit/test_restore_flow.py)        -> C++ test (RestoreFlowTests.<name>)
//   test_automatic_restore                              -> test_automatic_restore
//   test_already_restored_is_nothing_found              -> test_already_restored_is_nothing_found
//   test_ambiguous_amount_needs_choice                  -> test_ambiguous_amount_needs_choice
//   test_restore_waits_until_every_choice_is_answered   -> test_restore_waits_until_every_choice_is_answered
//   test_errors_return_to_input                         -> test_errors_return_to_input
//   test_reverted                                       -> test_reverted
//   test_cancel_result                                  -> test_cancel_result
//   test_a_blocked_paste_shows_the_restore_wording      -> test_a_blocked_paste_shows_the_restore_wording
// (test_checkout_solana.py's test_restore_accepts_signatures is in CheckoutSolanaTests.)
// C++ only (the command edges godot/tests/test_restore_flow.gd adds): test_messages_and_result_shapes,
// test_a_cancelled_command_lands_in_failed, test_a_failure_that_is_not_a_restore_error_is_failed,
// test_a_restore_error_code_is_the_error, test_a_save_that_is_not_a_list_of_dicts_is_failed,
// test_choices_follow_the_candidates_left_to_save, test_equal_candidates_map_to_the_first (list.index finds the
// first equal candidate).
// Python's Engine here runs on time.time(); this one on a fixed clock (restore reads no time).
#include "support/CheckoutFakes.h"
#include "support/Test.h"

#include "Scfg/Chains.h"
#include "Scfg/Config.h"
#include "Scfg/Engine.h"
#include "Scfg/Err.h"
#include "Scfg/Evm.h"
#include "Scfg/Json.h"
#include "Scfg/Restore.h"
#include "Scfg/RestoreFlow.h"
#include "Scfg/Result.h"
#include "Scfg/Transfers.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::Json;
using scfg::Result;
using scfg::restore::RestoreCandidate;
using scfg::restore_flow::RestoreController;
namespace fakes = scfg_test::fakes;

namespace scfg_test::restore_flow_tests {

constexpr const char* DEV = "0x52908400098527886E0F7030069857D2E4169EE7";
constexpr const char* PAYER = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const std::string TX = "0xabababababababababababababababababababababababababababababababab";

std::string usdc_arb() { return scfg::chains::MAINNET().find("arbitrum")->tokens.find("USDC")->address; }
std::string show(const std::optional<std::string>& s) { return s ? *s : std::string("None"); }

// setup()'s (RestoreController, engine), in teardown order.
struct Setup {
    std::unique_ptr<fakes::FakeNodes> nodes = std::make_unique<fakes::FakeNodes>();
    std::unique_ptr<scfg::engine::Engine> engine;
    std::shared_ptr<fakes::ImmediateSubmitter> submit;
    std::unique_ptr<RestoreController> ctl;
};

// setup(*logs, status="0x1").
std::optional<Setup> setup(Ctx& t, std::vector<Json> logs = {}, const std::string& status = "0x1") {
    Setup s;
    Result<scfg::config::Config> cfg = scfg::config::build_config(Json::dict({{"wallet", DEV}}));
    if (!t.check(cfg.ok(), "build_config")) return std::nullopt;
    Result<scfg::models::Item> item = scfg::config::add_item(
        cfg.value(), "chapter2", Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}));
    if (!t.check(item.ok(), "add_item")) return std::nullopt;
    if (!logs.empty() || status == "0x0") {
        s.nodes->evm["arbitrum"].receipts[TX] = Json::dict({{"status", status}, {"logs", Json(std::move(logs))}});
    }
    s.engine = fakes::make_engine(cfg.take(), *s.nodes, [] { return int64_t{1700000000}; });
    s.submit = std::make_shared<fakes::ImmediateSubmitter>(*s.engine);
    s.ctl = std::make_unique<RestoreController>(s.submit);
    return s;
}

// run(ctl, text=TX).
void run(Ctx& t, RestoreController& ctl, const std::string& text = TX) {
    ctl.tx_input = text;
    Result<bool> checked = ctl.check_tx();
    t.check(checked.ok(), "check_tx");
    ctl.poll();
    if (ctl.state() == "saving") ctl.poll();
}

std::vector<std::string> owned(Ctx& t, const scfg::engine::Engine& engine) {
    Result<scfg::engine::Snapshot> snap = engine.snapshot();
    if (!t.check(snap.ok(), "snapshot")) return {};
    return snap.value().owned;
}

Json paid(int64_t units) { return fakes::transfer_log(usdc_arb(), PAYER, DEV, units, 5, TX); }

// A submitter the test answers.
struct HeldSubmitter final : scfg::worker::Submitter {
    std::vector<scfg::worker::OnDone<std::vector<RestoreCandidate>>> lookups;
    std::vector<scfg::worker::OnDone<Json>> applies;
    std::vector<std::vector<RestoreCandidate>> applied;
    std::vector<std::optional<std::map<int64_t, Json>>> applied_choices;
    void start_session(Json, Json, Json, scfg::worker::OnDone<Json>) override {}
    void build_solana_payment(Json, Json, scfg::worker::OnDone<Json>) override {}
    void report_hint(Json, Json, scfg::worker::OnDone<void>) override {}
    void set_ui_status(Json, Json, scfg::worker::OnDone<void>) override {}
    void discard_session(Json, scfg::worker::OnDone<void>) override {}
    void restore_lookup(Json, scfg::worker::OnDone<std::vector<RestoreCandidate>> done) override {
        lookups.push_back(std::move(done));
    }
    void restore_apply(std::vector<RestoreCandidate> candidates, std::optional<std::map<int64_t, Json>> choices,
                       scfg::worker::OnDone<Json> done) override {
        applied.push_back(std::move(candidates));
        applied_choices.push_back(std::move(choices));
        applies.push_back(std::move(done));
    }
};

RestoreCandidate candidate(std::string chain, std::optional<std::string> kind, bool already_restored = false) {
    RestoreCandidate c;
    c.chain = std::move(chain);
    c.token = "USDC";
    c.transfer = scfg::transfers::Transfer{};
    c.kind = std::move(kind);
    if (!c.kind) c.choices = {"chapter2", std::string(scfg::restore::TIP_CHOICE)};
    c.already_restored = already_restored;
    return c;
}

}  // namespace scfg_test::restore_flow_tests

using namespace scfg_test::restore_flow_tests;

SCFG_TEST(RestoreFlowTests, test_automatic_restore) {
    std::optional<Setup> s = setup(t, {paid(4'993'742)});
    if (!s) return;
    run(t, *s->ctl);
    SCFG_CHECK_EQ(t, s->ctl->state(), "done");
    scfg::models::RestoreResult r = s->ctl->result();
    SCFG_CHECK(t, r.status == "restored" && !r.restored.empty() &&
                      r.restored[0].as_dict().get("item").as_str() == "chapter2");
    SCFG_CHECK(t, owned(t, *s->engine) == std::vector<std::string>({"chapter2"}));
    for (const std::string& p : s->nodes->problems) t.fail("fake node: " + p);
}

SCFG_TEST(RestoreFlowTests, test_already_restored_is_nothing_found) {
    std::optional<Setup> s = setup(t, {paid(4'993'742)});
    if (!s) return;
    run(t, *s->ctl);
    RestoreController ctl2(s->ctl->submit());
    run(t, ctl2);
    SCFG_CHECK(t, ctl2.state() == "done" && ctl2.result().status == "nothing_found");
}

SCFG_TEST(RestoreFlowTests, test_ambiguous_amount_needs_choice) {
    std::optional<Setup> s = setup(t, {paid(5'000'000)});
    if (!s) return;
    RestoreController& ctl = *s->ctl;
    run(t, ctl);
    SCFG_CHECK(t, ctl.state() == "choose" && ctl.pending_choices() == std::vector<int64_t>({0}));
    if (!t.check(!ctl.candidates().empty(), "a candidate")) return;
    SCFG_CHECK(t, ctl.candidates()[0].choices ==
                      std::vector<std::string>({"chapter2", std::string(scfg::restore::TIP_CHOICE)}));
    ctl.choose(0, "chapter2");
    ctl.confirm();
    ctl.poll();
    SCFG_CHECK(t, ctl.state() == "done" && owned(t, *s->engine) == std::vector<std::string>({"chapter2"}));
}

SCFG_TEST(RestoreFlowTests, test_restore_waits_until_every_choice_is_answered) {
    std::optional<Setup> s = setup(t, {paid(5'000'000)});
    if (!s) return;
    RestoreController& ctl = *s->ctl;
    run(t, ctl);
    SCFG_CHECK(t, !ctl.can_confirm());
    ctl.confirm();  // e.g. a click that raced the screen: nothing happens
    SCFG_CHECK_EQ(t, ctl.state(), "choose");
    ctl.choose(0, std::string(scfg::restore::TIP_CHOICE));
    SCFG_CHECK(t, ctl.can_confirm());
}

SCFG_TEST(RestoreFlowTests, test_errors_return_to_input) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    RestoreController& ctl = *s->ctl;
    run(t, ctl, "nonsense");
    SCFG_CHECK(t, ctl.state() == "input" && show(ctl.error) == "invalid_hash");
    run(t, ctl);
    SCFG_CHECK(t, ctl.state() == "input" && show(ctl.error) == "tx_not_found" && ctl.message());
}

SCFG_TEST(RestoreFlowTests, test_reverted) {
    std::optional<Setup> s = setup(t, {}, "0x0");
    if (!s) return;
    run(t, *s->ctl);
    SCFG_CHECK_EQ(t, show(s->ctl->error), "tx_reverted");
}

SCFG_TEST(RestoreFlowTests, test_cancel_result) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    SCFG_CHECK_EQ(t, s->ctl->result().status, "cancelled");
}

SCFG_TEST(RestoreFlowTests, test_a_blocked_paste_shows_the_restore_wording) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    s->ctl->error = "paste_blocked";
    SCFG_CHECK(t, s->ctl->message() && s->ctl->message()->find("Type the hash instead") != std::string::npos);
}

// --- C++ only -----------------------------------------------------------------------------------------------------

SCFG_TEST(RestoreFlowTests, test_messages_and_result_shapes) {
    SCFG_CHECK(t, scfg::restore_flow::MESSAGES().keys() ==
                      std::vector<std::string>({"invalid_hash", "tx_not_found", "tx_reverted", "tx_not_a_payment",
                                                "network_unavailable", "paste_blocked", "failed"}));
    SCFG_CHECK_EQ(t, *scfg::restore_flow::MESSAGES().find("failed"), "Something went wrong. Please try again.");
    RestoreController ctl(std::make_shared<HeldSubmitter>());
    SCFG_CHECK(t, !ctl.message());
    ctl.error = "no_such_id";
    SCFG_CHECK(t, !ctl.message());
    scfg::models::RestoreResult r = ctl.result();
    SCFG_CHECK(t, r.status == "cancelled" && show(r.error) == "no_such_id" && r.restored.empty());
}

SCFG_TEST(RestoreFlowTests, test_a_cancelled_command_lands_in_failed) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    RestoreController ctl(sub);
    Result<bool> checked = ctl.check_tx(TX);
    SCFG_CHECK(t, checked.ok() && checked.value() && ctl.state() == "checking");
    ctl.poll();  // nothing yet
    SCFG_CHECK_EQ(t, ctl.state(), "checking");
    if (!t.check(sub->lookups.size() == 1, "one lookup")) return;
    sub->lookups[0](scfg::Err::make("CancelledError", "stopped"));
    ctl.poll();
    SCFG_CHECK(t, ctl.state() == "input" && show(ctl.error) == "failed");
}

SCFG_TEST(RestoreFlowTests, test_a_failure_that_is_not_a_restore_error_is_failed) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    RestoreController ctl(sub);
    (void)ctl.check_tx(TX);
    if (!t.check(sub->lookups.size() == 1, "one lookup")) return;
    sub->lookups[0](scfg::Err::make("RpcUnavailable", "all endpoints failed"));
    ctl.poll();
    SCFG_CHECK(t, ctl.state() == "input" && show(ctl.error) == "failed");
}

SCFG_TEST(RestoreFlowTests, test_a_restore_error_code_is_the_error) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    RestoreController ctl(sub);
    (void)ctl.check_tx(TX);
    if (!t.check(sub->lookups.size() == 1, "one lookup")) return;
    sub->lookups[0](scfg::Err::restore_error("network_unavailable", "no chain answered"));
    ctl.poll();
    SCFG_CHECK(t, ctl.state() == "input" && show(ctl.error) == "network_unavailable" && ctl.message());
}

SCFG_TEST(RestoreFlowTests, test_a_save_that_is_not_a_list_of_dicts_is_failed) {
    for (const Json& saved : {Json(), Json::dict(), Json::list({1})}) {
        std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
        RestoreController ctl(sub);
        (void)ctl.check_tx(TX);
        if (!t.check(sub->lookups.size() == 1, "one lookup")) return;
        sub->lookups[0](std::vector<RestoreCandidate>{candidate("arbitrum", "unlock")});
        ctl.poll();  // nothing to choose: confirm() at once
        SCFG_CHECK_EQ(t, ctl.state(), "saving");
        if (!t.check(sub->applies.size() == 1, "one apply")) return;
        sub->applies[0](saved);
        ctl.poll();
        SCFG_CHECK(t, ctl.state() == "input" && show(ctl.error) == "failed");
    }
}

// cands holds the not-yet-restored candidates; a choice made for index i of candidates() goes to cands' index of
// candidates().index(c), the first equal candidate, as Python's index_map.
SCFG_TEST(RestoreFlowTests, test_choices_follow_the_candidates_left_to_save) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    RestoreController ctl(sub);
    (void)ctl.check_tx(TX);
    if (!t.check(sub->lookups.size() == 1, "one lookup")) return;
    sub->lookups[0](std::vector<RestoreCandidate>{candidate("polygon", "unlock", /*already_restored=*/true),
                                                  candidate("arbitrum", std::nullopt), candidate("base", std::nullopt)});
    ctl.poll();
    SCFG_CHECK(t, ctl.state() == "choose" && ctl.pending_choices() == std::vector<int64_t>({1, 2}));
    ctl.choose(1, "chapter2");
    SCFG_CHECK(t, !ctl.can_confirm());
    ctl.choose(2, "tip");
    ctl.confirm();
    if (!t.check(sub->applied.size() == 1 && sub->applied_choices[0].has_value(), "one apply with choices")) return;
    SCFG_CHECK(t, sub->applied[0].size() == 2 && sub->applied[0][0].chain == "arbitrum");
    const std::map<int64_t, Json>& picked = *sub->applied_choices[0];  // {0: "chapter2", 1: "tip"}
    const auto first = picked.find(0);
    const auto second = picked.find(1);
    SCFG_CHECK(t, picked.size() == 2 && first != picked.end() && second != picked.end() &&
                      first->second.as_str() == "chapter2" && second->second.as_str() == "tip");
    sub->applies[0](Json::list({Json::dict({{"item", "chapter2"}})}));
    ctl.poll();
    SCFG_CHECK(t, ctl.state() == "done" && ctl.result().status == "restored" && ctl.added().size() == 1);
}

SCFG_TEST(RestoreFlowTests, test_equal_candidates_map_to_the_first) {
    std::shared_ptr<HeldSubmitter> sub = std::make_shared<HeldSubmitter>();
    RestoreController ctl(sub);
    (void)ctl.check_tx(TX);
    if (!t.check(sub->lookups.size() == 1, "one lookup")) return;
    const RestoreCandidate open = candidate("arbitrum", std::nullopt);
    sub->lookups[0](std::vector<RestoreCandidate>{candidate("polygon", "unlock", true), open, open});
    ctl.poll();
    ctl.choose(1, "chapter2");
    ctl.choose(2, "tip");
    ctl.confirm();
    if (!t.check(sub->applied.size() == 1 && sub->applied_choices[0].has_value(), "one apply with choices")) return;
    SCFG_CHECK_EQ(t, sub->applied[0].size(), 2);
    // index_map = {1: 1} (both equal candidates are index 1 of candidates()): of the choices {1: "chapter2",
    // 2: "tip"} only index 1's is kept, under cands index 1.
    const std::map<int64_t, Json>& picked = *sub->applied_choices[0];
    const auto kept = picked.find(1);
    SCFG_CHECK(t, picked.size() == 1 && kept != picked.end() && kept->second.as_str() == "chapter2");
}
