// Twin of scfg_core/rpc.py (and of unity/.../Core/Rpc.cs, rpgmaker/src/core/rpc.ts).
// Ethereum JSON-RPC over the injected transport. Task 6 lands only the constants config.py imports; task 10 adds the
// client, its errors and Private/Rpc.cpp.
#pragma once

#include <cstdint>

namespace scfg::rpc {

// The smallest eth_getLogs block range the client narrows to, and the smallest max_log_range a custom chain may
// declare (config.build_chain).
inline constexpr int64_t MIN_LOG_RANGE = 10;

}  // namespace scfg::rpc
