// Copyright 2026 Repulse
// Twin of scfg_core/payments.py (and of unity/.../Core/Payments.cs, rpgmaker/src/core/payments.ts).
// Validation of a pay() request, independent of chain and network.
//
// Used synchronously by scfg.pay() (so programmer errors raise immediately in the developer's script) and again by
// Engine.start_session.
#pragma once

#include "Scfg/Config.h"
#include "Scfg/Dec.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scfg::payments {

// Decimal("0.50").
SCFGCORE_API const Dec& MIN_TIP();
// (Decimal("1.00"), Decimal("3.00"), Decimal("5.00"), Decimal("10.00")).
SCFGCORE_API const std::array<Dec, 4>& TIP_PRESETS();

// Python's frozen PaymentSpec dataclass. item and title are Json because Python carries what it was given: an
// unlock's item is the item id (a str), a tip's is any value or None, and a title is any truthy value or the
// default (the item's title, "Tip").
struct PaymentSpec {
    std::string kind;  // "unlock" | "tip"
    Json item;
    Dec price;
    std::string token;
    int64_t code = 0;  // 0 for tips
    Json title;
    std::vector<std::string> chains;  // enabled chains that carry the token, in display order
};

// prepare_payment(config, kind, *, item=None, amount=None, token=None, title=None): kwargs holds the keyword
// arguments (an unknown one is a TypeError); kind is any value ("unlock" or "tip", else payment.bad_kind).
SCFGCORE_API Result<PaymentSpec> prepare_payment(const config::Config& config, const Json& kind, const Json& kwargs);

}  // namespace scfg::payments
