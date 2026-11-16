// Twin of scfg_core/evm.py (and of unity/.../Core/Evm.cs, rpgmaker/src/core/evm.ts).
// EVM helpers: addresses (EIP-55) and event topics, and the ERC-20 Transfer log.
//
// Addresses are loosely typed, as in Python: a Json that is not a str is "not a 20-byte hex address" (ValueError).
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/Transfers.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scfg::evm {

// topic0 of the ERC-20/721 Transfer(address,address,uint256) event: "0x" + keccak256(b"Transfer(...)").hex(), which
// EvmTests checks.
inline constexpr std::string_view TRANSFER_TOPIC = "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef";

// The EIP-55 mixed-case form of a 20-byte hex address (any input case): "0x" and exactly 40 ASCII hex digits, else
// ValueError("not a 20-byte hex address: <repr>").
Result<std::string> to_checksum_address(const Json& address);
// True only if the address is already exactly in EIP-55 form. Never fails.
bool is_checksum_address(const Json& address);
// Validates a user-supplied address and returns its checksummed form. Single-case input carries no checksum, so it is
// accepted and converted. Mixed-case input claims a checksum, so it must be correct (else ValueError).
Result<std::string> normalize_address(const Json& address);
// The address left-padded to 32 bytes (lowercase), as it appears in indexed event topics.
Result<std::string> address_topic(const Json& address);

// One ERC-20 Transfer event, normalized (Python's frozen TransferLog dataclass).
struct TransferLog {
    std::string token;      // checksummed contract address
    std::string sender;     // checksummed
    std::string recipient;  // checksummed
    BigInt value;           // base units
    std::string tx_hash;    // lowercase
    BigInt log_index;       // int(logIndex, 16): any size, as Python's
    BigInt block_number;    // int(blockNumber, 16)
    std::string block_hash;  // lowercase

    transfers::Transfer to_transfer() const;
    friend bool operator==(const TransferLog&, const TransferLog&) = default;
};

// Parses a raw JSON-RPC log into a TransferLog, or nullopt if it isn't an ERC-20 Transfer. ERC-721 Transfer events
// share topic0 but have 4 topics (the token id is indexed), so requiring exactly 3 topics filters them out. The data
// must be exactly the 32-byte value ("0x" and 64 hex digits): an empty, shorter, longer or malformed one is not an
// ERC-20 Transfer either.
//
// A malformed log fails as Python's does, with the same class: a non-dict log or a non-str hash or topic0 an
// AttributeError, a missing field a KeyError, a bad address a ValueError, logIndex/blockNumber through int(x, 16)
// (unbounded, as Python's int). Port limit: str.lower() of the hashes is py::lower_for_ascii (exact for ASCII text and
// for any comparison with ASCII text; other non-ASCII letters keep their case).
Result<std::optional<TransferLog>> parse_transfer_log(const Json& log);

}  // namespace scfg::evm
