// Twin of scfg_core/transfers.py (and of unity/.../Core/Transfers.cs, which also holds evm.py's TransferLog; here
// that is scfg::evm::TransferLog in Evm.h, beside its Python home).
// A token transfer to the developer, in a form shared by every chain family. Receipts and restore candidates are
// built from it. EVM fills it from an ERC-20 Transfer log; Solana from a transaction's token-balance changes.
#pragma once

#include "Scfg/BigInt.h"

#include <cstdint>
#include <optional>
#include <string>

namespace scfg::transfers {

// Python's frozen Transfer dataclass: a value, equal field by field.
struct Transfer {
    std::string token;                  // token contract (EVM) or mint (Solana)
    std::optional<std::string> sender;  // payer; Solana: None when no account lost the token and there is no fee payer
    std::string recipient;              // developer wallet (the owner, never a token account)
    BigInt value;                       // base units
    std::string tx_hash;                // EVM: lowercase 0x hash; Solana: base58 signature, case preserved
    int64_t log_index = 0;              // EVM: log index; Solana: the developer token account's accountIndex
    int64_t block_number = 0;           // EVM: block number; Solana: slot

    friend bool operator==(const Transfer&, const Transfer&) = default;
};

}  // namespace scfg::transfers
