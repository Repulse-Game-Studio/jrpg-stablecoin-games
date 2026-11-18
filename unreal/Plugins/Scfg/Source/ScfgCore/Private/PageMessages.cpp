// Copyright 2026 Repulse
// Twin of scfg_core/page_messages.py (see PageMessages.h).
#include "Scfg/PageMessages.h"

#include "Scfg/Backends.h"
#include "Scfg/Err.h"
#include "Scfg/Evm.h"
#include "Scfg/Py.h"
#include "Scfg/SolanaBase58.h"

#include <utility>

namespace scfg::page_messages::detail {

// Python's `except (KeyError, TypeError, AttributeError) as e: raise ValueError(what) from e`.
Err as_value_error(Err e, const char* what) {
    if (e.is_a("KeyError") || e.is_a("TypeError") || e.is_a("AttributeError")) return Err::value_error(what);
    return e;
}

}  // namespace scfg::page_messages::detail

namespace scfg::page_messages {

Result<TxMessage> parse_tx_message(const Json& data) {
    if (!data.is_dict()) return Err::value_error("not an object");
    const Json* raw = data.as_dict().find("tx_hash");
    if (raw == nullptr) return Err::value_error("bad tx hash");  // data["tx_hash"]'s KeyError
    Result<std::string> tx_hash = backends::normalize_any_tx(*raw);
    if (!tx_hash.ok()) return detail::as_value_error(std::move(tx_hash).err(), "bad tx hash");
    TxMessage out{tx_hash.take(), std::nullopt};
    Json sender = data.as_dict().get("from");
    if (py::truthy(sender)) {
        Result<std::string> shown = py::str(sender);  // its ValueError is caught below, as Python's
        Result<std::string> normalized = !shown.ok()                     ? Result<std::string>(std::move(shown).err())
                                         : shown.value().starts_with("0x") ? evm::to_checksum_address(sender)
                                                                           : solana::base58::normalize_pubkey(sender);
        if (normalized.ok()) {
            out.sender = normalized.take();
        } else if (!normalized.err().is_a("ValueError") && !normalized.err().is_a("TypeError")) {
            return std::move(normalized).err();
        }
    }
    return out;
}

Result<std::string> parse_build_message(const Json& data) {
    if (!data.is_dict()) return Err::value_error("not an object");
    const Json* raw = data.as_dict().find("account");
    if (raw == nullptr) return Err::value_error("bad account");
    Result<std::string> account = solana::base58::normalize_pubkey(*raw);
    if (!account.ok()) return detail::as_value_error(std::move(account).err(), "bad account");
    return account;
}

}  // namespace scfg::page_messages
