// Twin of scfg_core/models.py (and of unity/.../Core/Models.cs, rpgmaker/src/core/models.ts).
// Value objects shared with the developer's script.
#pragma once

#include "Scfg/Dec.h"
#include "Scfg/Json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scfg::models {

// A paid unlock declared with scfg.add_item() (Python's frozen Item dataclass; config::add_item validates it).
struct Item {
    std::string item_id;
    int64_t code = 0;  // 1..99
    Dec price;
    std::string token;
    std::string title;
    Dec min_price;
};

// What pay(), callbacks and on_result receive (Python's PaymentResult dataclass).
//
// status: "paid" | "already_owned" | "cancelled" | "expired" | "underpaid" | "failed" | "pending". kind, item, token,
// chain, tx_hash, payer and session_id are Json because Python carries whatever value a stored session or match
// held (None for absent): a hand-edited store must not make building the result fail. error is text the core itself
// writes, or None.
struct PaymentResult {
    std::string status;
    Json kind;
    Json item;
    std::optional<Dec> amount;
    Json token;
    Json chain;
    Json tx_hash;
    Json payer;
    Json session_id;
    std::optional<std::string> error;
};

// What restore() returns. status: "restored" | "nothing_found" | "cancelled" | "failed".
struct RestoreResult {
    std::string status;
    std::vector<Json> restored;  // list[dict]
    std::optional<std::string> error;
};

}  // namespace scfg::models
