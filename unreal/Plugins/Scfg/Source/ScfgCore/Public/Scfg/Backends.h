// Twin of scfg_core/backends/__init__.py (and of unity/.../Core/Backends/Index.cs): chain-family backends,
// everything that differs between EVM chains and Solana.
//
// The engine, sessions, receipts and screens are family-neutral. Anything that touches a chain's RPC,
// transaction-id format or payment matching goes through backend_for(chain). One stateless backend per family, for
// the whole program, registered EVM first, then Solana: normalize_any_tx tries the families in that order, as
// Python does.
#pragma once

#include "Scfg/BackendsBase.h"
#include "Scfg/Chains.h"
#include "Scfg/Export.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <array>
#include <string>
#include <string_view>

namespace scfg::backends {

// The backends in registration order (EVM, then Solana).
SCFGCORE_API const std::array<const base::Backend*, 2>& registered();

// The backend of a family; ValueError("unknown chain family <repr>") for any other.
SCFGCORE_API Result<const base::Backend*> backend_for_family(std::string_view family);
SCFGCORE_API Result<const base::Backend*> backend_for(const chains::Chain& chain);

// A transaction id in any supported family's format, normalized: each family's normalize_tx in registration order
// (EVM's normalize_tx_hash, then Solana's normalize_signature). ValueError("not a transaction hash or signature:
// <repr>") when none reads it; any other Err passes through, as in Python.
SCFGCORE_API Result<std::string> normalize_any_tx(const Json& text);

}  // namespace scfg::backends
