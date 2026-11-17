// No Python twin: a session start the player moved away from (another network, another tip amount, Cancel) must not
// leave a session behind: it would be watched for 24 h and reported expired to on_result 30 minutes later, for a
// payment the player never saw. The twins of tests/unit/test_checkout_abandoned_start.py one for one (same names,
// same assertions), over a real Engine on the fake nodes of support/CheckoutFakes.h with the deferred submitter
// (commands run only when the test calls run()).
//
// Python test (tests/unit/test_checkout_abandoned_start.py)              -> C++ test (CheckoutAbandonedStartTests.<name>)
//   test_switching_network_while_starting_discards_the_first_session    -> the same name
//   test_changing_the_tip_while_starting_discards_the_first_session     -> the same name
//   test_cancel_while_starting_leaves_no_session_to_expire              -> the same name
//   test_a_failed_abandoned_start_needs_no_discard                      -> the same name
// C++ only (as godot/tests/test_checkout_abandoned_start.gd): test_abandoning_an_already_finished_start_discards_at_once
// (Python's add_done_callback runs at once on a done future), and test_an_abandoned_start_outlives_its_controller (the
// discard still goes out when the start finishes after the controller is gone).
// `fut.set_exception(RuntimeError("rpc down"))` is the queued command's fail() with that Err.
#include "support/CheckoutFakes.h"
#include "support/Test.h"

#include "Scfg/Checkout.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/Watcher.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

using scfg::Json;
using scfg::Result;
using scfg::checkout::CheckoutController;
namespace fakes = scfg_test::fakes;

namespace scfg_test::checkout_abandoned_start_tests {

constexpr const char* DEV = "0x52908400098527886E0F7030069857D2E4169EE7";

using Setup = fakes::Rig;

// setup(kind="unlock"): (ctl, engine, submit, clock).
std::optional<Setup> setup(Ctx& t, const std::string& kind = "unlock") {
    fakes::RigOptions r;
    r.kind = kind;
    r.config = Json::dict({{"wallet", DEV}});
    r.server = false;
    r.deferred = true;
    Result<Setup> rig = fakes::make_rig(std::move(r));
    if (!t.check(rig.ok(), rig.ok() ? "" : "setup: " + rig.err().str())) return std::nullopt;
    return rig.take();
}

// [r["chain"] for r in engine.pending.values()]
std::vector<std::string> pending_chains(const Setup& s) {
    std::vector<std::string> out;
    for (const auto& [sid, rec] : s.engine->pending().as_dict()) {
        (void)sid;
        out.push_back(rec.as_dict().get("chain").as_str());
    }
    return out;
}

}  // namespace scfg_test::checkout_abandoned_start_tests

using namespace scfg_test::checkout_abandoned_start_tests;

SCFG_TEST(CheckoutAbandonedStartTests, test_switching_network_while_starting_discards_the_first_session) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    SCFG_CHECK(t, ctl.choose_network("base").ok());
    s->deferred->run();
    ctl.poll();
    SCFG_CHECK(t, ctl.state() == "waiting" && ctl.chain_key() == "base");
    s->deferred->run();  // the discard queued once the abandoned start finished
    SCFG_CHECK(t, pending_chains(*s) == std::vector<std::string>({"base"}));
}

SCFG_TEST(CheckoutAbandonedStartTests, test_changing_the_tip_while_starting_discards_the_first_session) {
    std::optional<Setup> s = setup(t, "tip");
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    (void)ctl.next_step();
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    Result<bool> changed = ctl.set_tip_amount("5");
    SCFG_CHECK(t, changed.ok() && changed.value());
    s->deferred->run();
    ctl.poll();
    s->deferred->run();
    const Json::Dict& pending = s->engine->pending().as_dict();
    if (!t.check(pending.size() == 1, "one session left")) return;
    SCFG_CHECK_EQ(t, pending.begin()->second.as_dict().get("price").as_str(), "5.00");
}

SCFG_TEST(CheckoutAbandonedStartTests, test_cancel_while_starting_leaves_no_session_to_expire) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    SCFG_CHECK_EQ(t, ctl.cancel().status, "cancelled");
    s->deferred->run();
    s->deferred->run();
    SCFG_CHECK(t, s->engine->pending().as_dict().empty());
    s->clock.advance(31 * 60);
    std::vector<std::string> expired;
    scfg::Status ticked = fakes::deliver(*s->engine, [&expired](const scfg::watcher::StepOutcome& o) {
        if (o.status == "expired") expired.push_back(o.session_id);
    });
    SCFG_CHECK(t, ticked.ok() && expired.empty());
}

SCFG_TEST(CheckoutAbandonedStartTests, test_a_failed_abandoned_start_needs_no_discard) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    (void)ctl.cancel();
    if (!t.check(!s->deferred->queue.empty(), "the start is queued")) return;
    fakes::DeferredSubmitter::Queued start = std::move(s->deferred->queue.front());
    s->deferred->queue.pop_front();
    start.fail(scfg::Err::make("RuntimeError", "rpc down"));
    s->deferred->run();
    SCFG_CHECK(t, s->engine->pending().as_dict().empty());
    SCFG_CHECK(t, s->deferred->queue.empty());
}

SCFG_TEST(CheckoutAbandonedStartTests, test_abandoning_an_already_finished_start_discards_at_once) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.choose_network("arbitrum").ok());
    s->deferred->run();  // the start finished, but poll() has not picked it up
    SCFG_CHECK(t, ctl.state() == "starting" && s->engine->pending().as_dict().size() == 1);
    (void)ctl.cancel();
    SCFG_CHECK(t, s->deferred->queue.size() == 1 && s->deferred->queue.front().name == "discard_session");
    s->deferred->run();
    SCFG_CHECK(t, s->engine->pending().as_dict().empty());
}

SCFG_TEST(CheckoutAbandonedStartTests, test_an_abandoned_start_outlives_its_controller) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    SCFG_CHECK(t, s->ctl->choose_network("arbitrum").ok());
    (void)s->ctl->cancel();
    s->ctl.reset();  // the sheet is gone; the start is still queued
    s->deferred->run();
    SCFG_CHECK(t, s->engine->pending().as_dict().empty());
}
