// Twin of scfg_core/backends/base.py (and of unity/.../Core/Backends/Base.cs): the interface every chain family
// implements (BackendsEvm.h, BackendsSolana.h).
//
// Python's cooperative generators (start, advance, reverify, lookup, check_ready, build_payment) are Co<...> here,
// awaiting the transport of the RPC object they are handed; make_rpc and normalize_tx are plain. The RPC object is
// whatever make_rpc built for the chain: an RpcClient for EVM, a SolanaRpc for Solana, held in a variant (the core
// has no RTTI); each backend takes its own family's alternative and refuses the other with a TypeError.
//
// Coroutine parameters are values (Co.h): records and stores are Json handles, so a backend mutates the caller's
// dict as Python's does. The Entropy is a std::reference_wrapper because it is an injected service that outlives
// every coroutine using it (as `this` does for a member coroutine), not caller data.
#pragma once

#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Coio.h"
#include "Scfg/Config.h"
#include "Scfg/Entropy.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"
#include "Scfg/SolanaRpc.h"
#include "Scfg/Watcher.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scfg::restore {
struct RestoreCandidate;  // Restore.h
}  // namespace scfg::restore

namespace scfg::backends::base {

// The RPC object of one chain, shared by the Engine's cache and the coroutines that use it.
using Rpc = std::variant<std::shared_ptr<::scfg::rpc::RpcClient>, std::shared_ptr<::scfg::solana::rpc::SolanaRpc>>;
// Python's rpc_for(key) (the Engine's rpc()): a chain's RPC object, built on first use. A KeyError for a chain the
// registry does not know; a ValueError from the client for a chain without RPC urls.
using RpcFor = std::function<Result<Rpc>(std::string key)>;
// _LazyRpc: the chain's RPC object, built only when a backend first needs it.
using LazyRpc = std::function<Result<Rpc>()>;
// Where a contained error goes (an Engine's on_error).
using ErrorSink = std::function<void(const Err&)>;

// The alternative of rpc a family's backend needs: TypeError "an EVM chain needs an RpcClient, not a SolanaRpc"
// (and the Solana twin) for the other one or a null pointer.
Result<std::shared_ptr<::scfg::rpc::RpcClient>> evm_client(const Rpc& rpc);
Result<std::shared_ptr<::scfg::solana::rpc::SolanaRpc>> solana_client(const Rpc& rpc);
// The failover client underneath either alternative (SolanaRpc wraps one), for a caller that sets its clock. An rpc
// holding a null pointer is a programming error.
::scfg::rpc::RpcClient& client_of(const Rpc& rpc);
// rpc holds a client (not a null pointer).
bool is_set(const Rpc& rpc);

class Backend {
public:
    virtual ~Backend() = default;

    // "evm" or "solana".
    virtual std::string_view family() const = 0;

    // The RPC object handed back to every other method for this chain, talking through transport (which must
    // outlive it). A ValueError for a chain without RPC urls.
    virtual Result<Rpc> make_rpc(const chains::Chain& chain, coio::Transport& transport) const = 0;

    // Validate a transaction id in this family's format; a ValueError otherwise (a non-str included).
    virtual Result<std::string> normalize_tx(const Json& text) const = 0;

    // Fields for a new session: a dict that must contain "start_block"; the rest is merged in. entropy stands in
    // for Python's `secrets` (Solana draws its reference from it).
    virtual Co<Json> start(Rpc rpc, chains::Chain chain, std::optional<std::string> wallet, std::string token,
                           std::reference_wrapper<Entropy> entropy) const = 0;

    // Move one pending session forward; returns a watcher::StepOutcome. pending and receipts are the Engine's
    // dicts (shared handles), now the Engine's clock value.
    virtual Co<watcher::StepOutcome> advance(Json pending, Json receipts, std::string sid, Rpc rpc,
                                             chains::Chain chain, std::string wallet,
                                             OrderedMap<models::Item> items, Json now) const = 0;

    // "keep" or "drop". Network trouble must answer "keep".
    virtual Co<std::string> reverify(Json receipt, Rpc rpc, chains::Chain chain,
                                     std::vector<std::string> wallets) const = 0;

    // Restore candidates for tx on this chain, or nullopt if the chain doesn't know it. RestoreError
    // "tx_reverted" | "tx_not_a_payment"; a transfer that cannot be classified is skipped and reported to on_error.
    virtual Co<std::optional<std::vector<restore::RestoreCandidate>>> lookup(std::string tx, config::Config config,
                                                                             chains::Chain chain, Rpc rpc,
                                                                             Json receipts,
                                                                             ErrorSink on_error) const = 0;

    // Token name -> can the developer receive it on this chain. rpc builds the chain's RPC object on first use
    // (Python's _LazyRpc), so a family that needs no network never builds one.
    virtual Co<OrderedMap<bool>> check_ready(LazyRpc rpc, chains::Chain chain,
                                             std::optional<std::string> wallet) const = 0;

    // Solana only: the unsigned transaction a browser wallet signs for this session ({"transaction",
    // "token_balance", "sol_balance"}). Python's EVM backend has no such method (AttributeError).
    virtual Co<Json> build_payment(Json rec, Rpc rpc, chains::Chain chain, Json account) const = 0;
};

}  // namespace scfg::backends::base
