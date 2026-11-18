// Copyright 2026 Repulse
// Twin of scfg_core/backends/base.py (see BackendsBase.h): the RPC object's alternatives.
#include "Scfg/BackendsBase.h"

#include "Scfg/Err.h"

#include <memory>
#include <variant>

namespace scfg::backends::base {

Result<std::shared_ptr<::scfg::rpc::RpcClient>> evm_client(const Rpc& rpc) {
    if (const auto* c = std::get_if<std::shared_ptr<::scfg::rpc::RpcClient>>(&rpc); c && *c) return *c;
    return Err::type_error("an EVM chain needs an RpcClient, not a SolanaRpc");
}

Result<std::shared_ptr<::scfg::solana::rpc::SolanaRpc>> solana_client(const Rpc& rpc) {
    if (const auto* c = std::get_if<std::shared_ptr<::scfg::solana::rpc::SolanaRpc>>(&rpc); c && *c) return *c;
    return Err::type_error("a Solana chain needs a SolanaRpc, not an RpcClient");
}

::scfg::rpc::RpcClient& client_of(const Rpc& rpc) {
    if (!is_set(rpc)) ::scfg::detail::programming_error("an RPC object that holds no client");
    if (const auto* c = std::get_if<std::shared_ptr<::scfg::rpc::RpcClient>>(&rpc)) return **c;
    return (*std::get_if<std::shared_ptr<::scfg::solana::rpc::SolanaRpc>>(&rpc))->client();
}

bool is_set(const Rpc& rpc) {
    return std::visit([](const auto& p) { return p != nullptr; }, rpc);
}

}  // namespace scfg::backends::base
