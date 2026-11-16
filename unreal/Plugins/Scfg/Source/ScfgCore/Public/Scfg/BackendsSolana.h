// Twin of scfg_core/backends/solana.py (and of unity/.../Core/Backends/Solana.cs): Solana behind the Backend
// interface.
//
// Task 11 ports what the family-neutral code needs (family, make_rpc and normalize_tx, which normalize_any_tx and
// report_hint use); every network method is a stub returning Err NotImplementedError ("solana <method>: task 12"),
// which never satisfies a vector (the runner refuses NotImplementedError as a recorded "other" or as a contained
// error). Task 12 replaces this file.
#pragma once

#include "Scfg/BackendsBase.h"

#include <string_view>

namespace scfg::backends::solana {

class SolanaBackend final : public base::Backend {
public:
    std::string_view family() const override { return "solana"; }
    // SolanaRpc(RpcClient(chain.rpc)): the client's default log range; the SolanaRpc owns its client.
    Result<base::Rpc> make_rpc(const chains::Chain& chain, coio::Transport& transport) const override;
    // solana::base58::normalize_signature.
    Result<std::string> normalize_tx(const Json& text) const override;
    Co<Json> start(base::Rpc rpc, chains::Chain chain, std::optional<std::string> wallet, std::string token,
                   std::reference_wrapper<Entropy> entropy) const override;
    Co<watcher::StepOutcome> advance(Json pending, Json receipts, std::string sid, base::Rpc rpc, chains::Chain chain,
                                     std::string wallet, OrderedMap<models::Item> items, Json now) const override;
    Co<std::string> reverify(Json receipt, base::Rpc rpc, chains::Chain chain,
                             std::vector<std::string> wallets) const override;
    Co<std::optional<std::vector<restore::RestoreCandidate>>> lookup(std::string tx, config::Config config,
                                                                     chains::Chain chain, base::Rpc rpc,
                                                                     Json receipts,
                                                                     base::ErrorSink on_error) const override;
    Co<OrderedMap<bool>> check_ready(base::LazyRpc rpc, chains::Chain chain,
                                     std::optional<std::string> wallet) const override;
    Co<Json> build_payment(Json rec, base::Rpc rpc, chains::Chain chain, Json account) const override;
};

}  // namespace scfg::backends::solana
