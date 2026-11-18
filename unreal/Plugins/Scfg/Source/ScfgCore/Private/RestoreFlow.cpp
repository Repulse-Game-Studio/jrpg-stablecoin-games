// Copyright 2026 Repulse
// Twin of scfg_core/restore_flow.py (see RestoreFlow.h), with godot/addons/scfg/core/restore_flow.gd as the
// generator-free template.
#include "Scfg/RestoreFlow.h"

#include "Scfg/Backends.h"
#include "Scfg/Err.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace scfg::restore_flow {

namespace detail {

bool is_list_of_dicts(const Json& value) {
    if (!value.is_list()) return false;
    const Json::List& items = value.as_list();
    return std::all_of(items.begin(), items.end(), [](const Json& item) { return item.is_dict(); });
}

// The error a failed command leaves: the RestoreError's code, else "failed".
std::string failure_code(const Err& e) { return e.is_a("RestoreError") ? e.code : std::string("failed"); }

}  // namespace detail

const OrderedMap<std::string>& MESSAGES() {
    static const OrderedMap<std::string> messages = {
        {"invalid_hash", "That doesn't look like a transaction hash or signature."},
        {"tx_not_found", "Transaction not found. Check the hash, or try again later."},
        {"tx_reverted", "That transaction failed on-chain; nothing was paid."},
        {"tx_not_a_payment", "This transaction isn't a payment to this game."},
        {"network_unavailable", "Can't reach the network. Try again in a moment."},
        {"paste_blocked", "Your browser blocked pasting. Type the hash instead."},
        {"failed", "Something went wrong. Please try again."},
    };
    return messages;
}

RestoreController::RestoreController(std::shared_ptr<worker::Submitter> submit) : submit_(std::move(submit)) {}

std::optional<std::string> RestoreController::message() const {
    if (!error || error->empty()) return std::nullopt;
    const std::string* text = MESSAGES().find(*error);
    return text != nullptr ? std::optional<std::string>(*text) : std::nullopt;
}

std::vector<int64_t> RestoreController::pending_choices() const {
    std::vector<int64_t> out;
    for (size_t i = 0; i < candidates_.size(); ++i) {
        const restore::RestoreCandidate& c = candidates_[i];
        if (!c.kind && !c.already_restored) out.push_back(static_cast<int64_t>(i));
    }
    return out;
}

Result<bool> RestoreController::check_tx(std::optional<std::string> text) {
    Result<std::string> tx = backends::normalize_any_tx(Json(text ? *text : tx_input));
    if (!tx.ok()) {
        if (tx.err().is_a("ValueError")) {
            error = "invalid_hash";
            return false;
        }
        return std::move(tx).err();
    }
    error.reset();
    state_ = "checking";
    phase_ = "lookup";
    apply_.reset();
    std::shared_ptr<worker::Delivery<std::vector<restore::RestoreCandidate>>> lookup =
        std::make_shared<worker::Delivery<std::vector<restore::RestoreCandidate>>>();
    lookup_ = lookup;
    submit_->restore_lookup(Json(tx.take()),
                            worker::Delivery<std::vector<restore::RestoreCandidate>>::receiver(std::move(lookup)));
    return true;
}

void RestoreController::choose(int64_t index, std::string choice) { choices_[index] = std::move(choice); }

bool RestoreController::can_confirm() const {
    const std::vector<int64_t> pending = pending_choices();
    return std::all_of(pending.begin(), pending.end(), [this](int64_t i) { return choices_.count(i) != 0; });
}

void RestoreController::confirm() {
    if ((state_ != "checking" && state_ != "choose") || !can_confirm()) return;
    std::vector<restore::RestoreCandidate> cands;
    std::map<int64_t, int64_t> index_map;  // {self.candidates.index(c): i for i, c in enumerate(cands)}
    for (const restore::RestoreCandidate& c : candidates_) {
        if (c.already_restored) continue;
        const auto first = std::find(candidates_.begin(), candidates_.end(), c);  // list.index: the first equal one
        index_map[static_cast<int64_t>(first - candidates_.begin())] = static_cast<int64_t>(cands.size());
        cands.push_back(c);
    }
    std::map<int64_t, Json> picked;
    for (const auto& [i, v] : choices_) {
        const auto at = index_map.find(i);
        if (at != index_map.end()) picked[at->second] = Json(v);
    }
    state_ = "saving";
    phase_ = "apply";
    lookup_.reset();
    std::shared_ptr<worker::Delivery<Json>> apply = std::make_shared<worker::Delivery<Json>>();
    apply_ = apply;
    submit_->restore_apply(std::move(cands), std::move(picked), worker::Delivery<Json>::receiver(std::move(apply)));
}

void RestoreController::poll() {
    if (phase_ == "lookup") {
        if (lookup_ == nullptr || !lookup_->done()) return;
        const std::shared_ptr<worker::Delivery<std::vector<restore::RestoreCandidate>>> fut = std::move(lookup_);
        lookup_.reset();
        const Result<std::vector<restore::RestoreCandidate>>& found = fut->result();
        if (!found.ok()) {
            state_ = "input";
            error = detail::failure_code(found.err());
            return;
        }
        candidates_ = found.value();
        const bool all_restored = std::all_of(candidates_.begin(), candidates_.end(),
                                              [](const restore::RestoreCandidate& c) { return c.already_restored; });
        if (all_restored) {
            state_ = "done";
        } else if (!pending_choices().empty()) {
            state_ = "choose";
        } else {
            confirm();
        }
        return;
    }
    if (apply_ == nullptr || !apply_->done()) return;
    const std::shared_ptr<worker::Delivery<Json>> fut = std::move(apply_);
    apply_.reset();
    const Result<Json>& saved = fut->result();
    if (!saved.ok() || !detail::is_list_of_dicts(saved.value())) {
        state_ = "input";
        error = saved.ok() ? std::string("failed") : detail::failure_code(saved.err());
        return;
    }
    added_ = saved.value().as_list();
    state_ = "done";
}

models::RestoreResult RestoreController::result() const {
    models::RestoreResult r;
    if (state_ == "done") {
        r.status = added_.empty() ? "nothing_found" : "restored";
        r.restored = added_;
    } else {
        r.status = "cancelled";
        r.error = error;
    }
    return r;
}

}  // namespace scfg::restore_flow
