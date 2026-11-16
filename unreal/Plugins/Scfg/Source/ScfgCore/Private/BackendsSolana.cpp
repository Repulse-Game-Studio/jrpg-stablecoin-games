// Twin of scfg_core/backends/solana.py (see BackendsSolana.h): family, make_rpc and normalize_tx; the network
// methods are stubs until task 12.
#include "Scfg/BackendsSolana.h"

#include "Scfg/Err.h"
#include "Scfg/Restore.h"
#include "Scfg/SolanaBase58.h"

#include <memory>
#include <utility>

namespace scfg::backends::solana::detail {

// A SolanaRpc and the RpcClient it wraps (by reference), kept alive together by one shared_ptr.
struct OwnedSolanaRpc {
    ::scfg::rpc::RpcClient client;
    ::scfg::solana::rpc::SolanaRpc rpc;
    explicit OwnedSolanaRpc(::scfg::rpc::RpcClient c) : client(std::move(c)), rpc(client) {}
    OwnedSolanaRpc(const OwnedSolanaRpc&) = delete;
    OwnedSolanaRpc& operator=(const OwnedSolanaRpc&) = delete;
};

Err stub(const char* method) { return Err::make("NotImplementedError", std::string("solana ") + method + ": task 12"); }

}  // namespace scfg::backends::solana::detail

namespace scfg::backends::solana {

Result<base::Rpc> SolanaBackend::make_rpc(const chains::Chain& chain, coio::Transport& transport) const {
    SCFG_TRY(::scfg::rpc::RpcClient client, ::scfg::rpc::RpcClient::make(chain.rpc, transport));
    auto owned = std::make_shared<detail::OwnedSolanaRpc>(std::move(client));
    return base::Rpc(std::shared_ptr<::scfg::solana::rpc::SolanaRpc>(owned, &owned->rpc));
}

Result<std::string> SolanaBackend::normalize_tx(const Json& text) const {
    return ::scfg::solana::base58::normalize_signature(text);
}

Co<Json> SolanaBackend::start(base::Rpc rpc, chains::Chain chain, std::optional<std::string> wallet,
                              std::string token, std::reference_wrapper<Entropy> entropy) const {
    (void)rpc, (void)chain, (void)wallet, (void)token, (void)entropy;
    co_return detail::stub("start");
}

Co<watcher::StepOutcome> SolanaBackend::advance(Json pending, Json receipts, std::string sid, base::Rpc rpc,
                                                chains::Chain chain, std::string wallet,
                                                OrderedMap<models::Item> items, Json now) const {
    (void)pending, (void)receipts, (void)sid, (void)rpc, (void)chain, (void)wallet, (void)items, (void)now;
    co_return detail::stub("advance");
}

Co<std::string> SolanaBackend::reverify(Json receipt, base::Rpc rpc, chains::Chain chain,
                                        std::vector<std::string> wallets) const {
    (void)receipt, (void)rpc, (void)chain, (void)wallets;
    co_return detail::stub("reverify");
}

Co<std::optional<std::vector<restore::RestoreCandidate>>> SolanaBackend::lookup(std::string tx, config::Config config,
                                                                                chains::Chain chain, base::Rpc rpc,
                                                                                Json receipts,
                                                                                base::ErrorSink on_error) const {
    (void)tx, (void)config, (void)chain, (void)rpc, (void)receipts, (void)on_error;
    co_return detail::stub("lookup");
}

Co<OrderedMap<bool>> SolanaBackend::check_ready(base::LazyRpc rpc, chains::Chain chain,
                                                std::optional<std::string> wallet) const {
    (void)rpc, (void)chain, (void)wallet;
    co_return detail::stub("check_ready");
}

Co<Json> SolanaBackend::build_payment(Json rec, base::Rpc rpc, chains::Chain chain, Json account) const {
    (void)rec, (void)rpc, (void)chain, (void)account;
    co_return detail::stub("build_payment");
}

}  // namespace scfg::backends::solana
