// Twin of scfg_core/restore.py (see Restore.h): stubs until task 13.
#include "Scfg/Restore.h"

#include "Scfg/Err.h"

namespace scfg::restore {

Co<std::vector<RestoreCandidate>> lookup(Json tx_hash, config::Config config, OrderedMap<chains::Chain> chains,
                                         backends::base::RpcFor rpc_for, Json receipts,
                                         backends::base::ErrorSink on_error) {
    (void)tx_hash, (void)config, (void)chains, (void)rpc_for, (void)receipts, (void)on_error;
    co_return Err::make("NotImplementedError", "restore lookup: task 13");
}

Result<Json> apply(Json::Dict& receipts, const std::vector<RestoreCandidate>& candidates,
                   const std::optional<std::map<int64_t, Json>>& choices) {
    (void)receipts, (void)candidates, (void)choices;
    return Err::make("NotImplementedError", "restore apply: task 13");
}

}  // namespace scfg::restore
