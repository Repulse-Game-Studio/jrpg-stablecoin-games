// Twin of scfg_core/backends/evm.py (and of unity/.../Core/Backends/Evm.cs): EVM chains, the watcher,
// re-verification and restore code behind the Backend interface.
//
// reverify is reverify::check_evm_receipt and lookup is restore::lookup_evm, on the chain's RpcClient.
#pragma once

#include "Scfg/BackendsBase.h"

#include <cstdint>
#include <string_view>

namespace scfg::backends::evm {

// Blocks before the head a new session starts scanning from.
inline constexpr int64_t START_BLOCK_MARGIN = 5;

class EvmBackend final : public base::Backend {
public:
    std::string_view family() const override { return "evm"; }
    // RpcClient(chain.rpc, log_range=chain.max_log_range).
    Result<base::Rpc> make_rpc(const chains::Chain& chain, coio::Transport& transport) const override;
    // sessions::normalize_tx_hash.
    Result<std::string> normalize_tx(const Json& text) const override;
    // {"start_block": max(0, head - START_BLOCK_MARGIN)}.
    Co<Json> start(base::Rpc rpc, chains::Chain chain, std::optional<std::string> wallet, std::string token,
                   std::reference_wrapper<Entropy> entropy) const override;
    // watcher::advance.
    Co<watcher::StepOutcome> advance(Json pending, Json receipts, std::string sid, base::Rpc rpc, chains::Chain chain,
                                     std::string wallet, OrderedMap<models::Item> items, Json now) const override;
    Co<std::string> reverify(Json receipt, base::Rpc rpc, chains::Chain chain,
                             std::vector<std::string> wallets) const override;
    Co<std::optional<std::vector<restore::RestoreCandidate>>> lookup(std::string tx, config::Config config,
                                                                     chains::Chain chain, base::Rpc rpc,
                                                                     Json receipts,
                                                                     base::ErrorSink on_error) const override;
    // Every token can be received on an EVM chain: no network needed, so rpc is never called.
    Co<OrderedMap<bool>> check_ready(base::LazyRpc rpc, chains::Chain chain,
                                     std::optional<std::string> wallet) const override;
    // Python's EvmBackend has none: the AttributeError of calling it (the Engine refuses a non-Solana session first).
    Co<Json> build_payment(Json rec, base::Rpc rpc, chains::Chain chain, Json account) const override;
};

}  // namespace scfg::backends::evm
