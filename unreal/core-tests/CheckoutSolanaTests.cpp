// Copyright 2026 Repulse
// No Python twin: CheckoutController (and RestoreController) on Solana, the twins of tests/unit/test_checkout_solana.py
// one for one (same names, same assertions), over a real Engine on the fake nodes of support/CheckoutFakes.h with
// the immediate submitter.
//
// Python test (tests/unit/test_checkout_solana.py)                     -> C++ test (CheckoutSolanaTests.<name>)
//   test_solana_is_offered_and_unready_tokens_are_hidden               -> the same name
//   test_only_unready_routes_shows_the_unavailable_message             -> the same name
//   test_solana_recipient_and_payment_link                             -> the same name
//   test_evm_chain_still_uses_eip681                                   -> the same name
//   test_pasting_the_wrong_family_is_an_invalid_hash                   -> the same name
//   test_restore_accepts_signatures                                    -> the same name
//   test_local_server_accepts_a_signature_hint                         -> the same name: the core's
//       local_server::CheckoutServer over the in-memory sockets of support/FakeSockets.h (the shared suite
//       tests/server_security runs it again over real loopback sockets). Its message half is
//       test_a_signature_hint_message_parses below.
//   test_transaction_that_never_landed_lets_the_player_pay_again       -> the same name
//   test_status_names_the_signature_that_never_landed                  -> the same name
//   test_a_new_hint_clears_the_old_not_landed_status                   -> the same name
//   test_a_verdict_on_an_older_hint_does_not_touch_the_newer_one[...]  -> the same name (one test, the three errors)
//   test_solana_points_to_a_network_with_the_buy_link                  -> the same name
//   test_the_hint_needs_a_network_with_the_link_that_the_player_can_pick -> the same name
//   test_the_hint_shows_where_the_link_would_and_not_once_a_payment_is_seen -> the same name
// C++ only: test_a_signature_hint_message_parses, and test_solana_session_info_key_order (as
// godot/tests/test_checkout_solana.gd: the Solana page's keys in Python's order, and build_tx submits
// build_solana_payment for this session).
// DEV is a fixed Solana account where Python draws a fresh Keypair.
#include "support/CheckoutFakes.h"
#include "support/FakeSockets.h"
#include "support/Test.h"

#include "Scfg/Chains.h"
#include "Scfg/Checkout.h"
#include "Scfg/Config.h"
#include "Scfg/Json.h"
#include "Scfg/LocalServer.h"
#include "Scfg/PageMessages.h"
#include "Scfg/Payments.h"
#include "Scfg/RestoreFlow.h"
#include "Scfg/Result.h"
#include "Scfg/SolanaBase58.h"
#include "Scfg/SolanaPda.h"
#include "Scfg/Watcher.h"

#include <algorithm>
#include <cstdint>
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

namespace scfg_test::checkout_solana_tests {

constexpr const char* DEV_EVM = "0x52908400098527886E0F7030069857D2E4169EE7";
constexpr const char* DEV = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM";
const std::string EVM_TX = "0xabababababababababababababababababababababababababababababababab";

std::string mint() { return scfg::chains::MAINNET().find("solana")->tokens.find("USDC")->address; }

// b58encode(n.to_bytes(64, "big")).
std::string sig_of(uint8_t n) {
    std::vector<uint8_t> bytes(64, 0);
    bytes.back() = n;
    return scfg::solana::base58::b58encode(bytes);
}
const std::string SIG = sig_of(7);
const std::string SIG2 = sig_of(8);

std::string show(const std::optional<std::string>& s) { return s ? *s : std::string("None"); }
bool is_true(const Result<bool>& r) { return r.ok() && r.value(); }
bool is_false(const Result<bool>& r) { return r.ok() && !r.value(); }
bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

using Setup = fakes::Rig;

// setup(unready=frozenset()): (ctl, e).
std::optional<Setup> setup(Ctx& t, scfg::checkout::Routes unready = {}, bool server = false, bool deferred = false) {
    fakes::RigOptions r;
    r.config = Json::dict({{"wallet", DEV_EVM}, {"solana_wallet", DEV}});
    r.now = 0;
    r.server = server;
    r.deferred = deferred;
    r.unready = [unready] { return unready; };
    r.game_name = "Ruby Dreams";
    Result<Setup> rig = fakes::make_rig(std::move(r));
    if (!t.check(rig.ok(), rig.ok() ? "" : "setup: " + rig.err().str())) return std::nullopt;
    return rig.take();
}

// setup(**config): the same, with build_config's extra keyword arguments.
std::optional<Setup> setup_with(Ctx& t, const Json& extra) {
    fakes::RigOptions r;
    r.config = Json::dict({{"wallet", DEV_EVM}, {"solana_wallet", DEV}});
    for (const auto& [k, v] : extra.as_dict()) r.config.as_dict().set(k, v);
    r.now = 0;
    r.server = false;
    r.unready = [] { return scfg::checkout::Routes{}; };
    r.game_name = "Ruby Dreams";
    Result<Setup> rig = fakes::make_rig(std::move(r));
    if (!t.check(rig.ok(), rig.ok() ? "" : "setup: " + rig.err().str())) return std::nullopt;
    return rig.take();
}

// on_method(chain, **config): choose_network(chain), then poll().
std::optional<Setup> on_method(Ctx& t, const std::string& chain, const Json& extra = Json::dict()) {
    std::optional<Setup> s = setup_with(t, extra);
    if (!s) return std::nullopt;
    if (!t.check(s->ctl->choose_network(chain).ok(), "choose_network")) return std::nullopt;
    s->ctl->poll();
    return s;
}

Json only_manual_methods() {
    return Json::dict({{"methods", Json::dict({{"browser", Json::dict({{"enabled", false}})},
                                               {"qr", Json::dict({{"enabled", false}})}})}});
}

// started(chain): select_chain(chain), then poll().
std::optional<Setup> started(Ctx& t, const std::string& chain) {
    std::optional<Setup> s = setup(t);
    if (!s) return std::nullopt;
    if (!t.check(s->ctl->select_chain(chain).ok(), "select_chain")) return std::nullopt;
    s->ctl->poll();
    if (!t.check(s->ctl->state() == "waiting", "the session started on " + chain)) return std::nullopt;
    return s;
}

StepOutcome outcome(std::string sid, std::string status, std::string error, Json hint_tx = Json()) {
    StepOutcome o;
    o.session_id = std::move(sid);
    o.status = std::move(status);
    o.error = std::move(error);
    o.hint_tx = std::move(hint_tx);
    return o;
}

// (info["error"], info["tx_hash"], info["dropped_tx"]) as text, None for None.
std::string triple(const Json& info) {
    auto one = [&info](const char* key) {
        const Json v = info.as_dict().get(key);
        return v.is_str() ? v.as_str() : v.is_null() ? std::string("None") : std::string("?");
    };
    return one("error") + "," + one("tx_hash") + "," + one("dropped_tx");
}

}  // namespace scfg_test::checkout_solana_tests

using namespace scfg_test::checkout_solana_tests;

SCFG_TEST(CheckoutSolanaTests, test_solana_is_offered_and_unready_tokens_are_hidden) {
    std::optional<Setup> s = setup(t);
    if (!s) return;
    const std::vector<scfg::checkout::ChainOption> options = s->ctl->chain_options();
    const scfg::checkout::ChainOption solana{"solana", "Solana", "lowest"};
    SCFG_CHECK(t, std::find(options.begin(), options.end(), solana) != options.end());
    s.reset();
    s = setup(t, {{"solana", "USDC"}});
    if (!s) return;
    for (const auto& o : s->ctl->chain_options()) SCFG_CHECK(t, o.key != "solana");
    SCFG_CHECK(t, s->ctl->dev_warnings() ==
                      std::vector<std::string>({"Your Solana wallet can't receive USDC yet: receive any amount once."}));
}

SCFG_TEST(CheckoutSolanaTests, test_only_unready_routes_shows_the_unavailable_message) {
    Result<scfg::config::Config> cfg = scfg::config::build_config(Json::dict({{"solana_wallet", DEV}}));
    if (!t.check(cfg.ok(), cfg.ok() ? "" : cfg.err().str())) return;
    Result<scfg::models::Item> item = scfg::config::add_item(
        cfg.value(), "chapter2", Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}));
    Result<scfg::payments::PaymentSpec> spec =
        scfg::payments::prepare_payment(cfg.value(), "unlock", Json::dict({{"item", "chapter2"}}));
    if (!t.check(item.ok() && spec.ok(), "add_item, prepare_payment")) return;
    scfg::checkout::CheckoutOptions o;
    o.config = cfg.take();
    o.spec = spec.take();
    o.submit = std::make_shared<fakes::RecordingSubmitter>();  // lambda fn: None
    o.unready = [] { return scfg::checkout::Routes{{"solana", "USDC"}}; };
    Result<std::unique_ptr<CheckoutController>> ctl = CheckoutController::make(std::move(o));
    if (!t.check(ctl.ok(), "make")) return;
    SCFG_CHECK(t, ctl.value()->chain_options().empty() &&
                      ctl.value()->message() == *scfg::checkout::MESSAGES().find("solana_unavailable"));
}

SCFG_TEST(CheckoutSolanaTests, test_solana_recipient_and_payment_link) {
    std::optional<Setup> s = started(t, "solana");
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, show(ctl.family()) == "solana" && show(ctl.recipient()) == DEV && show(ctl.token_address()) == mint());
    Result<std::optional<std::string>> uri = ctl.payment_uri();
    if (!t.check(uri.ok() && uri.value(), "payment_uri")) return;
    const std::string& u = *uri.value();
    SCFG_CHECK(t, u.rfind(std::string("solana:") + DEV + "?amount=4.9937", 0) == 0 && contains(u, "spl-token=" + mint()));
    SCFG_CHECK(t, contains(u, "reference=" + ctl.session().as_dict().get("reference").as_str()) &&
                      contains(u, "label=Ruby%20Dreams"));
    Result<std::string> dest = scfg::solana::pda::associated_token_address(Json(DEV), Json(mint()));
    SCFG_CHECK(t, dest.ok() && ctl.session().as_dict().get("dest").as_str() == dest.value());
}

SCFG_TEST(CheckoutSolanaTests, test_evm_chain_still_uses_eip681) {
    std::optional<Setup> s = started(t, "base");
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, show(ctl.family()) == "evm" && show(ctl.recipient()) == DEV_EVM);
    Result<std::optional<std::string>> uri = ctl.payment_uri();
    SCFG_CHECK(t, uri.ok() && uri.value() && uri.value()->rfind("ethereum:", 0) == 0);
}

SCFG_TEST(CheckoutSolanaTests, test_pasting_the_wrong_family_is_an_invalid_hash) {
    std::optional<Setup> s = started(t, "solana");
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, is_false(ctl.submit_hash(EVM_TX)) && show(ctl.error) == "invalid_hash");
    SCFG_CHECK(t, is_true(ctl.submit_hash(SIG)) && s->pending(ctl.session_id()).as_dict().get("hint_tx").as_str() == SIG);
    s.reset();
    s = started(t, "base");
    if (!s) return;
    SCFG_CHECK(t, is_false(s->ctl->submit_hash(SIG)) && show(s->ctl->error) == "invalid_hash");
    SCFG_CHECK(t, contains(*scfg::checkout::MESSAGES().find("invalid_hash"), "signature"));
}

SCFG_TEST(CheckoutSolanaTests, test_restore_accepts_signatures) {
    std::shared_ptr<fakes::RecordingSubmitter> submitted = std::make_shared<fakes::RecordingSubmitter>();
    scfg::restore_flow::RestoreController ctl(submitted);
    SCFG_CHECK(t, is_true(ctl.check_tx(SIG)) && !submitted->submitted.empty());
    SCFG_CHECK(t, is_false(ctl.check_tx("nonsense")) && show(ctl.error) == "invalid_hash");
}

SCFG_TEST(CheckoutSolanaTests, test_local_server_accepts_a_signature_hint) {
    namespace ls = scfg::local_server;
    namespace sk = scfg_test::sockets;
    auto got = std::make_shared<std::vector<std::pair<std::string, std::optional<std::string>>>>();
    auto net = std::make_shared<sk::Net>();
    ls::Options o;
    o.load_asset = [](const std::string&) -> std::optional<std::string> { return std::string(); };
    o.session_info = Json::dict();
    o.get_status = [] { return Json::dict(); };
    o.on_tx = [got](std::string tx, std::optional<std::string> sender) { got->emplace_back(tx, sender); };
    o.clock = [] { return 0.0; };
    o.listen = sk::listen_on(net);
    Result<std::unique_ptr<ls::CheckoutServer>> made = ls::CheckoutServer::make(std::move(o));
    if (!t.check(made.ok(), "made")) return;
    std::unique_ptr<ls::CheckoutServer> srv = made.take();
    if (!t.check(srv->start().ok(), "started")) return;
    std::shared_ptr<sk::Pipe> conn = net->connect();
    const Result<std::string> body = scfg::json::dumps(Json::dict({{"tx_hash", SIG}, {"from", DEV}}));
    conn->send(sk::head(*srv, "POST", "tx", body.value().size()) + body.value());
    for (int i = 0; i < 5 && !sk::answer(conn->to_client); ++i) srv->pump();
    const std::optional<sk::Answer> answer = sk::answer(conn->to_client);
    SCFG_CHECK(t, answer && answer->status == 202);
    srv->stop();
    SCFG_CHECK(t, got->size() == 1 && got->front().first == SIG && got->front().second == std::optional<std::string>(DEV));
}

// The core's half of test_local_server_accepts_a_signature_hint: the page's {"tx_hash": SIG, "from": DEV}.
SCFG_TEST(CheckoutSolanaTests, test_a_signature_hint_message_parses) {
    Result<scfg::page_messages::TxMessage> m =
        scfg::page_messages::parse_tx_message(Json::dict({{"tx_hash", SIG}, {"from", DEV}}));
    SCFG_CHECK(t, m.ok() && m.value().tx_hash == SIG && show(m.value().sender) == DEV);
}

SCFG_TEST(CheckoutSolanaTests, test_transaction_that_never_landed_lets_the_player_pay_again) {
    std::optional<Setup> s = started(t, "solana");
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, is_true(ctl.submit_hash(SIG)) && show(ctl.hint_tx) == SIG);
    ctl.on_outcome(outcome(*ctl.session_id(), "waiting", "tx_not_landed"));
    SCFG_CHECK(t, !ctl.hint_tx && ctl.state() == "waiting");
    SCFG_CHECK_EQ(t, show(ctl.message()), "That transaction didn't reach the network. You can pay again.");
    SCFG_CHECK(t, ctl.status_info().as_dict().get("tx_hash").is_null());
    SCFG_CHECK_EQ(t, ctl.status_info().as_dict().get("error").as_str(), "tx_not_landed");
}

SCFG_TEST(CheckoutSolanaTests, test_status_names_the_signature_that_never_landed) {
    std::optional<Setup> s = started(t, "solana");
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.status_info().as_dict().get("dropped_tx").is_null());
    (void)ctl.submit_hash(SIG);
    ctl.on_outcome(outcome(*ctl.session_id(), "waiting", "tx_not_landed", SIG));
    SCFG_CHECK_EQ(t, triple(ctl.status_info()), "tx_not_landed,None," + SIG);
}

SCFG_TEST(CheckoutSolanaTests, test_a_new_hint_clears_the_old_not_landed_status) {
    std::optional<Setup> s = started(t, "solana");
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    (void)ctl.submit_hash(SIG);
    ctl.on_outcome(outcome(*ctl.session_id(), "waiting", "tx_not_landed", SIG));
    ctl.report_hint(SIG2, std::string(DEV));  // the page's POST /tx for the next send
    SCFG_CHECK_EQ(t, triple(ctl.status_info()), "None," + SIG2 + ",None");
    SCFG_CHECK(t, !ctl.message() && s->pending(ctl.session_id()).as_dict().get("hint_tx").as_str() == SIG2);
}

// The worker may judge the old hint just before the page reports the next one.
SCFG_TEST(CheckoutSolanaTests, test_a_verdict_on_an_older_hint_does_not_touch_the_newer_one) {
    for (const char* error : {"tx_not_landed", "tx_reverted", "tx_not_a_payment"}) {
        std::optional<Setup> s = started(t, "solana");
        if (!s) return;
        CheckoutController& ctl = *s->ctl;
        (void)ctl.submit_hash(SIG);
        ctl.report_hint(SIG2, std::string(DEV));
        ctl.on_outcome(outcome(*ctl.session_id(), "waiting", error, SIG));
        SCFG_CHECK_EQ(t, triple(ctl.status_info()), "None," + SIG2 + ",None");
    }
}

// godot/tests/test_checkout_solana.gd: what the Solana page is handed (the shared keys in Python's order, then
// wallet_chain, mint and reference), and build_tx asks the worker for this session's transfer.
SCFG_TEST(CheckoutSolanaTests, test_solana_session_info_key_order) {
    std::optional<Setup> s = setup(t, {}, /*server=*/true, /*deferred=*/true);
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK(t, ctl.select_chain("solana").ok());
    s->deferred->run();
    ctl.poll();
    if (!t.check(ctl.state() == "waiting", "started")) return;
    (void)ctl.start_browser();
    if (!t.check(!s->servers->empty(), "a server")) return;
    const scfg::checkout::ServerArgs& args = s->servers->front()->args;
    SCFG_CHECK(t, args.session_info.as_dict().keys() ==
                      std::vector<std::string>({"title", "kind", "token", "family", "decimals", "amount_units",
                                                "amount_text", "price_text", "recipient", "chain_name", "native_symbol",
                                                "explorer_tx", "testnet", "logo", "accent", "on_accent",
                                                "wallet_chain", "mint", "reference"}));
    const Json::Dict& info = args.session_info.as_dict();
    SCFG_CHECK(t, info.get("mint").as_str() == mint() &&
                      info.get("reference").as_str() == ctl.session().as_dict().get("reference").as_str());
    SCFG_CHECK_EQ(t, info.get("wallet_chain").as_str(), scfg::chains::MAINNET().find("solana")->wallet_chain);
    if (!t.check(static_cast<bool>(args.build_tx), "build_tx on Solana")) return;
    args.build_tx(Json(DEV), {});
    SCFG_CHECK(t, s->deferred->queue.size() == 1 && s->deferred->queue.back().name == "build_solana_payment");
}

// --- the buy link's hint: Solana has no buy page, another network does ------------------------------------------

SCFG_TEST(CheckoutSolanaTests, test_solana_points_to_a_network_with_the_buy_link) {
    std::optional<Setup> s = on_method(t, "solana");
    if (!s) return;
    CheckoutController& ctl = *s->ctl;
    SCFG_CHECK_EQ(t, ctl.step(), "method");
    SCFG_CHECK(t, !ctl.onramp_url().value() && !ctl.shows_onramp());
    SCFG_CHECK(t, ctl.shows_onramp_hint());
    ctl.back();
    SCFG_CHECK_EQ(t, ctl.step(), "network");
    SCFG_CHECK(t, ctl.choose_network("base").ok());
    ctl.poll();
    SCFG_CHECK(t, ctl.shows_onramp() && !ctl.shows_onramp_hint());
}

SCFG_TEST(CheckoutSolanaTests, test_the_hint_needs_a_network_with_the_link_that_the_player_can_pick) {
    std::optional<Setup> s = on_method(t, "solana", Json::dict({{"onramp", Json::dict({{"enabled", false}})}}));
    if (!s) return;
    SCFG_CHECK(t, !s->ctl->shows_onramp_hint());
    s.reset();
    s = on_method(t, "solana", Json::dict({{"testnet", true}}));  // testnet: no network has the link
    if (!s) return;
    SCFG_CHECK(t, !s->ctl->shows_onramp_hint());
    s.reset();
    s = on_method(t, "solana", Json::dict({{"onramp", Json::dict({{"chains", Json::list({"base"})}})},
                                           {"chains", Json::list({"solana", "base"})}}));
    if (!s) return;
    SCFG_CHECK(t, s->ctl->shows_onramp_hint());
    s.reset();
    s = on_method(t, "solana",
                  Json::dict({{"testnet", true}, {"onramp", Json::dict({{"chains", Json::list({"base"})}})}}));
    if (!s) return;
    SCFG_CHECK(t, s->ctl->shows_onramp_hint());
}

SCFG_TEST(CheckoutSolanaTests, test_the_hint_shows_where_the_link_would_and_not_once_a_payment_is_seen) {
    std::optional<Setup> s = on_method(t, "solana", only_manual_methods());
    if (!s) return;
    SCFG_CHECK(t, s->ctl->step() == "manual" && s->ctl->shows_onramp_hint());  // the only payment page
    s.reset();
    s = on_method(t, "solana");
    if (!s) return;
    s->ctl->choose_method("qr");
    SCFG_CHECK(t, !s->ctl->shows_onramp_hint());  // the method step had it
    s.reset();
    s = on_method(t, "solana", only_manual_methods());
    if (!s) return;
    SCFG_CHECK(t, is_true(s->ctl->submit_hash(SIG)));
    SCFG_CHECK(t, s->ctl->step() == "waiting" && !s->ctl->shows_onramp_hint());
}
