// Twin of scfg_core/backends/evm.py (see BackendsEvm.h).
#include "Scfg/BackendsEvm.h"

#include "Scfg/Err.h"
#include "Scfg/Restore.h"
#include "Scfg/Reverify.h"
#include "Scfg/Sessions.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace scfg::backends::evm {

Result<base::Rpc> EvmBackend::make_rpc(const chains::Chain& chain, coio::Transport& transport) const {
    rpc::RpcClientOptions options;
    options.log_range = chain.max_log_range;
    SCFG_TRY(rpc::RpcClient client, rpc::RpcClient::make(chain.rpc, transport, std::move(options)));
    return base::Rpc(std::make_shared<rpc::RpcClient>(std::move(client)));
}

Result<std::string> EvmBackend::normalize_tx(const Json& text) const { return sessions::normalize_tx_hash(text); }

Co<Json> EvmBackend::start(base::Rpc rpc, chains::Chain chain, std::optional<std::string> wallet, std::string token,
                           std::reference_wrapper<Entropy> entropy) const {
    (void)chain, (void)wallet, (void)token, (void)entropy;
    SCFG_CO_TRY(std::shared_ptr<rpc::RpcClient> client, base::evm_client(rpc));
    SCFG_CO_TRY(BigInt head, co_await client->block_number());
    co_return Json::dict({{"start_block", Json(std::max(BigInt(0), head - BigInt(START_BLOCK_MARGIN)))}});
}

Co<watcher::StepOutcome> EvmBackend::advance(Json pending, Json receipts, std::string sid, base::Rpc rpc,
                                             chains::Chain chain, std::string wallet, OrderedMap<models::Item> items,
                                             Json now) const {
    SCFG_CO_TRY(std::shared_ptr<rpc::RpcClient> client, base::evm_client(rpc));
    co_return co_await watcher::advance(std::move(pending), std::move(receipts), std::move(sid), std::move(client),
                                        std::move(chain), std::move(wallet), std::move(items), std::move(now));
}

Co<std::string> EvmBackend::reverify(Json receipt, base::Rpc rpc, chains::Chain chain,
                                     std::vector<std::string> wallets) const {
    SCFG_CO_TRY(std::shared_ptr<rpc::RpcClient> client, base::evm_client(rpc));
    co_return co_await ::scfg::reverify::check_evm_receipt(std::move(receipt), std::move(client), std::move(chain),
                                                           std::move(wallets));
}

Co<std::optional<std::vector<restore::RestoreCandidate>>> EvmBackend::lookup(std::string tx, config::Config config,
                                                                             chains::Chain chain, base::Rpc rpc,
                                                                             Json receipts,
                                                                             base::ErrorSink on_error) const {
    SCFG_CO_TRY(std::shared_ptr<rpc::RpcClient> client, base::evm_client(rpc));
    co_return co_await restore::lookup_evm(std::move(tx), std::move(config), std::move(chain), std::move(client),
                                           std::move(receipts), std::move(on_error));
}

Co<OrderedMap<bool>> EvmBackend::check_ready(base::LazyRpc rpc, chains::Chain chain,
                                             std::optional<std::string> wallet) const {
    (void)rpc, (void)wallet;
    OrderedMap<bool> ready;
    for (const auto& [name, token] : chain.tokens) {
        (void)token;
        ready.set(name, true);
    }
    co_return ready;
}

Co<Json> EvmBackend::build_payment(Json rec, base::Rpc rpc, chains::Chain chain, Json account) const {
    (void)rec, (void)rpc, (void)chain, (void)account;
    co_return Err::make("AttributeError", "'EvmBackend' object has no attribute 'build_payment'");
}

}  // namespace scfg::backends::evm
