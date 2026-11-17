// Twin of scfg_core/checkout.py (and of godot/addons/scfg/core/checkout.gd, its generator-free port): the state
// behind the payment screen, independent of any engine.
//
// The screen only displays this object's fields and calls its methods from buttons. All Engine work goes through
// the Submitter (Submitter.h: the glue's worker), and results come back through poll() (the submitted commands'
// Deliveries) and on_outcome() (the watcher outcomes the worker drains, routed by session id). Nothing here touches
// the network, a clock or randomness.
//
// States: choose -> starting -> waiting -> confirming -> paid
//                                      \-> underpaid | expired
//
// The sheet shows one step at a time (step()):
// amount (tips only) -> network -> method -> browser | qr | manual -> waiting -> done.
// Steps with a single option are skipped both ways. back() works until a transaction is seen; a seen payment moves
// the sheet to waiting and a final state to done. The state, step and message ids are Python's (the UI keys its texts
// by conformance/vectors/messages.json, which MESSAGES() equals).
//
// How Python's names map here:
// - CheckoutController(config=, spec=, submit=, mobile=, server_factory=, unready=, game_name=, browser_wallet=) is
//   CheckoutController::make(CheckoutOptions): a constructor cannot return the Err of the first network step (an
//   unlock with one network starts its session at once) nor of enabled_chains().
// - Python's attributes that a screen writes (mobile, server_factory, unready, browser_wallet, game_name,
//   custom_amount, tx_input, error, hint_tx) are public members. The others are read through const methods of the
//   same name (config(), spec(), submit(), amount(), chain_key(), session(), method(), state(), confirmations(),
//   required(), dropped_tx(), result(), browser_failed(), step(), nav_dir()): only the controller moves them, so
//   STEP_ORDER always knows step() and method(), and session() is always a session poll() accepted. Python's
//   @property members are const methods too.
// - config() and spec() are the controller's own copies, which nothing changes: enabled_chains(config), the methods'
//   options and text_on(accent) are read once, in make().
// - Renamed: _report_hint is report_hint() (public: the page's on_tx, which the tests also call, as Python's do);
//   _submit_build is the server's build_tx; _result is make_result() (private: result() is the attribute's
//   accessor). Python's other private helpers keep their names without the underscore, and are private;
//   unready_routes() reads the unready callable (empty: nothing blocked, Python's `unready or frozenset`).
// - A method Python can raise from returns a Result (select_chain's ValueError, a parse_amount Err that is neither a
//   TypeError nor a ValueError, the paylinks' Errs).
//
// Divergences (README, "Known divergences from Python"), none reachable with the Engine's own results:
// - poll() accepts a started session only when it is a dict with a str "id", an int "units" and, on Solana, a str
//   "reference"; anything else is a failed start (start_failed), where Python would fail later reading it. So
//   session_id(), pay_amount_text() and start_browser() cannot fail. The session such a start may have created is
//   not discarded (there is no id to discard).
// - A cancelled start (CancelledError: the worker was stopped) is a failed start too (start_failed, back to the
//   network step); in Python fut.exception() raises CancelledError out of poll().
// - enabled_chains(config), the methods options (config.methods) and text_on(accent) are read once, in make(), so
//   their errors come from make(); Python re-reads the config on every call and raises where it is read. The
//   controller's config cannot change, so the values are the same.
// - Destroying the controller closes it (close(): an abandoned start is discarded once it finishes, the server is
//   stopped); Python has no such hook and relies on the screen calling cancel() or finish(). A start poll() already
//   picked up is not abandoned, and close() twice does nothing more.
// - A server factory's or start()'s Err of any kind makes the browser method unavailable (Python catches Exception;
//   a page has no BaseException to raise). The controller keeps only error = "browser_unavailable" and drops the
//   Err, so the factory and Server::start() report their own Err (log it once) before returning it.
// - token_address() is None for a chain without the payment's token (Python's KeyError; chain_key only ever names a
//   chain of spec().chains, which all carry it). rpc_url is None for a chain without RPC urls (Python's IndexError;
//   build_chain refuses one).
//
// Lifetime: the controller owns its server; its destructor stops and destroys the server first (close()), before any
// other member, and server_ is declared last so it would go first anyway. The callbacks it hands the server (on_tx,
// get_status, build_tx) are bound to the controller, so a server must not call them after it is stopped or
// destroyed. The OnDone a server passes to build_tx may run after that (the worker calls it later): it must not
// reach a dead server. The Submitter is shared: an abandoned session start keeps only a weak reference to it, to
// discard the session once the start finishes.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Config.h"
#include "Scfg/Dec.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Payments.h"
#include "Scfg/Result.h"
#include "Scfg/Submitter.h"
#include "Scfg/Watcher.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scfg::checkout {

inline constexpr std::array<std::string_view, 3> FINAL_STATES = {"paid", "underpaid", "expired"};
// Errors that judge the reported transaction (the hint) and drop it.
inline constexpr std::array<std::string_view, 3> HINT_VERDICTS = {"tx_reverted", "tx_not_a_payment", "tx_not_landed"};
// Order of the steps; the three method pages share a place.
inline constexpr std::array<std::pair<std::string_view, int>, 8> STEP_ORDER = {{
    {"amount", 0},
    {"network", 1},
    {"method", 2},
    {"browser", 3},
    {"qr", 3},
    {"manual", 3},
    {"waiting", 4},
    {"done", 5},
}};

// The screen's texts by message id, in Python's order (invalid_amount names MIN_TIP: "0.50").
SCFGCORE_API const OrderedMap<std::string>& MESSAGES();

// One entry of chain_options(): Python's (key, name, fee_hint) tuple.
struct ChainOption {
    std::string key;
    std::string name;
    std::string fee_hint;

    friend bool operator==(const ChainOption&, const ChainOption&) = default;
};

// The routes the developer cannot receive on, as (chain key, token) pairs (the worker snapshot's unready).
using Routes = std::vector<std::pair<std::string, std::string>>;

// The checkout page's server (what Python's server_factory returns: local_server.CheckoutServer on the desktop).
class Server {
public:
    virtual ~Server() = default;
    // Start serving the page. An Err (Python's raise) makes the browser method unavailable; the controller drops it,
    // so the server reports (logs) it itself before returning it.
    virtual Status start() = 0;
    virtual void stop() = 0;
    // The page's address.
    virtual std::string url() const = 0;
    // Optional in Python (getattr): reopen() when the page is opened again, pump() from every poll(). Both do nothing
    // unless overridden.
    virtual void reopen() {}
    virtual void pump() {}
};

// Python's server_factory keyword arguments.
struct ServerArgs {
    // The exact dict Python hands the page (same keys in the same order; amount_units and min_fee_wei as str,
    // chain_id_hex as Python's hex()).
    Json session_info;
    // report_hint(tx_hash, sender): the page reports a transaction it sent (a normalized hash or signature).
    std::function<void(std::string tx_hash, std::optional<std::string> sender)> on_tx;
    // status_info().
    std::function<Json()> get_status;
    // Solana only (empty otherwise): asks the worker for the unsigned transfer account signs (build_solana_payment).
    std::function<void(Json account, worker::OnDone<Json> done)> build_tx;
};

// An Err (or a null server) makes the browser method unavailable. The controller does not pass the Err on, so the
// factory reports (logs) its own Err before returning it.
using ServerFactory = std::function<Result<std::unique_ptr<Server>>(ServerArgs args)>;

// CheckoutController's keyword arguments.
struct CheckoutOptions {
    config::Config config;
    payments::PaymentSpec spec;  // payments::prepare_payment's
    std::shared_ptr<worker::Submitter> submit;
    bool mobile = false;
    ServerFactory server_factory;    // empty: None (no browser page)
    std::function<Routes()> unready;  // empty: nothing blocked
    std::string game_name = "Game";   // empty: "Game"
    // The web build's question "can a wallet in the page pay on this chain?"; empty: None (the desktop rule).
    std::function<bool(const chains::Chain&)> browser_wallet;
};

class SCFGCORE_API CheckoutController {
public:
    // A TypeError for an empty submit; enabled_chains()'s KeyError; a methods option that is missing or not
    // iterable; text_on's ValueError; the first network step's Err (for one network: select_chain's).
    static Result<std::unique_ptr<CheckoutController>> make(CheckoutOptions options);
    // close(): an abandoned start is discarded once it finishes, the server is stopped and destroyed first.
    ~CheckoutController();
    CheckoutController(const CheckoutController&) = delete;
    CheckoutController& operator=(const CheckoutController&) = delete;

    // --- what a screen writes ---------------------------------------------------------------------------------

    bool mobile = false;
    ServerFactory server_factory;
    std::function<Routes()> unready;
    std::function<bool(const chains::Chain&)> browser_wallet;
    std::string game_name;
    std::string custom_amount;
    std::string tx_input;
    std::optional<std::string> error;    // a message id
    std::optional<std::string> hint_tx;  // the transaction reported for this payment

    // --- read-only attributes ---------------------------------------------------------------------------------

    const config::Config& config() const { return config_; }
    const payments::PaymentSpec& spec() const { return spec_; }
    const std::shared_ptr<worker::Submitter>& submit() const { return submit_; }
    const Dec& amount() const { return amount_; }
    const std::optional<std::string>& chain_key() const { return chain_key_; }
    // The session record the Engine started (a dict), or None.
    const Json& session() const { return session_; }
    const std::optional<std::string>& method() const { return method_; }
    const std::string& state() const { return state_; }
    const BigInt& confirmations() const { return confirmations_; }
    const BigInt& required() const { return required_; }
    // The hint that never landed (tx_not_landed), as the verdict named it (a str), or None.
    const Json& dropped_tx() const { return dropped_tx_; }
    const std::optional<models::PaymentResult>& result() const { return result_; }
    bool browser_failed() const { return browser_failed_; }
    const std::string& step() const { return step_; }
    // 1: the step came in going forward, -1: going back.
    int nav_dir() const { return nav_dir_; }

    // --- read-only views --------------------------------------------------------------------------------------

    std::optional<std::string> session_id() const;
    // The enabled chain chain_key names, or nullptr (valid as long as the controller).
    const chains::Chain* chain() const;
    std::optional<std::string> family() const;
    std::vector<ChainOption> chain_options() const;
    const std::array<Dec, 4>& tip_presets() const;
    std::vector<std::string> methods() const;
    // The exact amount to send, with the item code and nonce: "4.993742" (the price before a session starts).
    std::string pay_amount_text() const;
    // The price as the player thinks of it: "4.99".
    std::string price_text() const;
    std::optional<std::string> token_address() const;
    std::optional<std::string> recipient() const;
    // The payment request a wallet scans or opens (EIP-681 or Solana Pay), or None before a session starts.
    Result<std::optional<std::string>> payment_uri() const;
    Result<std::optional<std::string>> explorer_url() const;
    std::optional<std::string> message() const;
    // Shown in DEV MODE only: setup problems the developer must fix.
    std::vector<std::string> dev_warnings() const;
    // Status for the localhost page: {"state", "confirmations", "required", "tx_hash", "error", "dropped_tx"}.
    // dropped_tx names the transaction a tx_not_landed is about, so the page offers Pay again only if it is the one
    // the page sent.
    Json status_info() const;
    // A transaction for this payment exists, so going back could start a second one.
    bool tx_seen() const;
    // The steps this payment walks through, without the single-option ones.
    std::vector<std::string> step_path() const;
    // (index of the current step, number of steps) for the progress dots.
    std::pair<int64_t, int64_t> step_dots() const;
    bool can_go_back() const;

    // --- actions ----------------------------------------------------------------------------------------------

    // Start a session on key, discarding the one running. ValueError for a chain the payment does not offer.
    Status select_chain(const std::string& key);
    // Network step: start a session on key, or keep the one already running.
    Status choose_network(const std::string& key);
    void choose_method(const std::string& name);
    // Leave the amount step (tips), applying a typed amount first. False when the step stays.
    Result<bool> next_step();
    void back();
    // The player says they sent the payment from the QR or manual page.
    void mark_sent();
    // Browser page: start the localhost page and move to waiting; returns its URL.
    std::optional<std::string> open_browser();
    // Change the tip. False (with error set) for an unusable amount, and for an unlock.
    Result<bool> set_tip_amount(const Json& value);
    Result<bool> apply_custom_amount();
    // The player pasted a transaction id (text, else tx_input). False (with error set) when it is not one in the
    // chain family's format, and before a session started.
    Result<bool> submit_hash(std::optional<std::string> text = std::nullopt);
    // Python's _report_hint: a transaction for this session (the page's on_tx).
    void report_hint(std::string tx_hash, std::optional<std::string> sender = std::nullopt);
    // Start the localhost page for this session; returns its URL or None.
    std::optional<std::string> start_browser();
    models::PaymentResult cancel();
    models::PaymentResult keep_playing();
    // Close after a final state (paid/underpaid/expired).
    models::PaymentResult finish();
    void close();

    // --- updates ----------------------------------------------------------------------------------------------

    // Pick up the result of start_session (call often from the UI).
    void poll();
    // A StepOutcome the worker drained; one for another session is ignored.
    void on_outcome(const watcher::StepOutcome& outcome);

private:
    explicit CheckoutController(CheckoutOptions options);

    void go(std::string to, int direction);
    std::optional<std::string> previous_step() const;
    // Go forward to the network step, or past it when there is only one network.
    Status enter_network();
    // The session is running: go to the method step, or straight to the only method.
    void after_network();
    void hint_verdict(const watcher::StepOutcome& outcome);
    // Stop waiting for a session start the player moved away from, and discard that session once it exists: it
    // would be watched for 24 h and reported expired.
    void abandon_start();
    void discard_session();
    bool browser_possible() const;
    Routes unready_routes() const;
    void submit_build(Json account, worker::OnDone<Json> done);
    models::PaymentResult make_result(std::string status) const;

    config::Config config_;
    payments::PaymentSpec spec_;
    std::shared_ptr<worker::Submitter> submit_;
    // Read once in make() from the config, which nothing changes.
    OrderedMap<chains::Chain> enabled_;
    bool browser_enabled_ = false;
    bool qr_enabled_ = false;
    bool manual_enabled_ = false;
    std::optional<std::vector<Json>> qr_chains_;  // None: every chain
    std::string on_accent_;

    Dec amount_;
    std::optional<std::string> chain_key_;
    Json session_;
    std::optional<std::string> method_;
    std::string state_ = "choose";
    BigInt confirmations_ = 0;
    BigInt required_ = 0;
    Json dropped_tx_;
    std::optional<models::PaymentResult> result_;
    bool browser_failed_ = false;
    std::shared_ptr<worker::Delivery<Json>> future_;
    std::string step_;
    int nav_dir_ = 1;
    bool picked_network_ = false;  // move on from the network step once the session starts
    // Last, so that it is destroyed first even without the destructor's close(): its callbacks reach every member.
    std::unique_ptr<Server> server_;
};

}  // namespace scfg::checkout
