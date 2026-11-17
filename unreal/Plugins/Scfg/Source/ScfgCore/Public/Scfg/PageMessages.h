// Twin of scfg_core/page_messages.py (and of unity/.../Core/PageMessages.cs): what the checkout page may tell the
// game, checked the same way wherever the page runs. A tx hash is only a hint: the watcher verifies it on-chain
// before anything is unlocked.
#pragma once

#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <cstdint>
#include <optional>
#include <string>

namespace scfg::page_messages {

// How long a page waits for the game to build a Solana transaction, in seconds.
inline constexpr int64_t BUILD_TIMEOUT_S = 15;

// parse_tx_message's (tx hash or signature, sender or None).
struct TxMessage {
    std::string tx_hash;
    std::optional<std::string> sender;
};

// {"tx_hash", "from"?} -> the normalized tx hash or signature (backends::normalize_any_tx) and the normalized
// sender. ValueError("not an object") for a non-dict, ValueError("bad tx hash") for a missing one, and
// normalize_any_tx's ValueError for a malformed one. A malformed sender is dropped: a truthy sender whose str()
// starts with "0x" is read as an EVM address (to_checksum_address), any other as a Solana account
// (normalize_pubkey), and a ValueError or TypeError there makes it None.
SCFGCORE_API Result<TxMessage> parse_tx_message(const Json& data);

// {"account"} -> the player's Solana account, normalized (normalize_pubkey). ValueError("not an object"),
// ValueError("bad account") for a missing one, or normalize_pubkey's ValueError.
SCFGCORE_API Result<std::string> parse_build_message(const Json& data);

}  // namespace scfg::page_messages
