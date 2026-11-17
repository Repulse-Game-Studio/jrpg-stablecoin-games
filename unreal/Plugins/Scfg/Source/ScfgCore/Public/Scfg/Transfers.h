// Twin of scfg_core/transfers.py (and of unity/.../Core/Transfers.cs, which also holds evm.py's TransferLog; here
// that is scfg::evm::TransferLog in Evm.h, beside its Python home).
// A token transfer to the developer, in a form shared by every chain family. Receipts and restore candidates are
// built from it. EVM fills it from an ERC-20 Transfer log; Solana from a transaction's token-balance changes.
#pragma once

#include "Scfg/BigInt.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"

#include <string>

namespace scfg::transfers {

// Python's frozen Transfer dataclass, which holds whatever it is given. Solana's transfer_to fills it from a node's
// reply, and the values Python only carries along are carried as the Json they are: a sender that is a str, None or
// any other hashable owner the node named; a tx_hash that is the signature transfer_to was asked about (a stored
// hint as it was stored); a log_index that is the accountIndex as the node sent it (an int of any size, or a str, a
// float or a bool). value and block_number are always ints (a balance difference, int() of the slot), of any size.
// make_receipt stores the carried values as they are, as Python's does.
struct SCFGCORE_API Transfer {
    std::string token;      // token contract (EVM) or mint (Solana)
    Json sender;            // payer; Solana: None when no account lost the token and there is no fee payer
    std::string recipient;  // developer wallet (the owner, never a token account)
    BigInt value;           // base units
    Json tx_hash;           // EVM: lowercase 0x hash; Solana: base58 signature, case preserved
    Json log_index;         // EVM: log index; Solana: the developer token account's accountIndex
    BigInt block_number;    // EVM: block number; Solana: slot

    // The dataclass's ==: field by field, the carried values with Python's == (py::eq: 1 == 1.0 == True).
    friend SCFGCORE_API bool operator==(const Transfer& a, const Transfer& b);
};

}  // namespace scfg::transfers
