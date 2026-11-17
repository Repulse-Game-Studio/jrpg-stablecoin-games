// Twin of scfg_core/restore_flow.py (and of godot/addons/scfg/core/restore_flow.gd, its generator-free port): the
// state behind the restore screen, independent of any engine.
//
//   input -> checking -> (choose ->) saving -> done
//
// The screen shows the fields (state(), tx_input, error, candidates(), choices(), added()), reads message() and
// can_confirm(), calls check_tx(), choose() and confirm() from its buttons and poll() every frame. Engine work goes
// through the Submitter (Submitter.h): restore_lookup, then restore_apply.
//
// How Python's names map here:
// - Renamed: check is check_tx (Unreal defines a `check` macro, which the poisoned build refuses).
// - RestoreController(submit=) is the constructor. tx_input and error, which a screen writes, are public members;
//   submit, state, candidates, choices and added are read through const methods of the same name (only the
//   controller changes them); can_confirm, a property, is a const method.
// - choices maps a candidate index to an item id or restore::TIP_CHOICE ("tip").
//
// A command that failed or was cancelled sends the screen back to input with the RestoreError's code as error
// ("failed" for any other Err, as for Python's other exceptions; a CancelledError is "failed" too). A restore_apply
// that succeeded with something other than a list of dicts is "failed" as well, where Python would have stored it
// (the Engine always returns a list of dicts).
#pragma once

#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Restore.h"
#include "Scfg/Result.h"
#include "Scfg/Submitter.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace scfg::restore_flow {

// The restore screen's texts by message id, in Python's order.
SCFGCORE_API const OrderedMap<std::string>& MESSAGES();

class SCFGCORE_API RestoreController {
public:
    explicit RestoreController(std::shared_ptr<worker::Submitter> submit);
    RestoreController(const RestoreController&) = delete;
    RestoreController& operator=(const RestoreController&) = delete;

    std::string tx_input;
    std::optional<std::string> error;  // a message id (or a RestoreError's code)

    const std::shared_ptr<worker::Submitter>& submit() const { return submit_; }
    const std::string& state() const { return state_; }
    const std::vector<restore::RestoreCandidate>& candidates() const { return candidates_; }
    const std::map<int64_t, std::string>& choices() const { return choices_; }
    // The receipts the restore added (dicts).
    const std::vector<Json>& added() const { return added_; }

    // The text for error, or None (no error, or a code with no text).
    std::optional<std::string> message() const;
    // Indexes of the candidates that wait for the player's choice.
    std::vector<int64_t> pending_choices() const;
    // Python's check(text=None): look the text (default: tx_input) up. False, with error "invalid_hash", when it is
    // not a transaction id of any family; normalize_any_tx's other Errs are returned.
    Result<bool> check_tx(std::optional<std::string> text = std::nullopt);
    void choose(int64_t index, std::string choice);
    // Every candidate that needs the player's choice has one.
    bool can_confirm() const;
    void confirm();
    // Pick up the finished lookup or save (call often from the UI).
    void poll();
    // status "restored" / "nothing_found" once done, else "cancelled" (with the last error).
    models::RestoreResult result() const;

private:
    std::shared_ptr<worker::Submitter> submit_;
    std::string state_ = "input";
    std::vector<restore::RestoreCandidate> candidates_;
    std::map<int64_t, std::string> choices_;
    std::vector<Json> added_;
    // Python's _future and _phase: the lookup or the apply in flight (one at a time).
    std::shared_ptr<worker::Delivery<std::vector<restore::RestoreCandidate>>> lookup_;
    std::shared_ptr<worker::Delivery<Json>> apply_;
    std::optional<std::string> phase_;
};

}  // namespace scfg::restore_flow
