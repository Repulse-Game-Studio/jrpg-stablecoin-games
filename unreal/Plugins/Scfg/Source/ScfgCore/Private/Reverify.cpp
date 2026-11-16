// Twin of scfg_core/reverify.py (see Reverify.h): a stub until task 13, which answers an empty store.
#include "Scfg/Reverify.h"

#include "Scfg/Err.h"

namespace scfg::reverify {

Co<Json> reverify_receipts(Json receipts, backends::base::RpcFor rpc_for, OrderedMap<chains::Chain> chains,
                           OrderedMap<std::vector<std::string>> wallets, backends::base::ErrorSink on_error) {
    (void)rpc_for, (void)chains, (void)wallets, (void)on_error;
    // `for key, r in list(receipts.items())` never runs: nothing removed, nothing asked, nothing reported.
    if (receipts.is_dict() && receipts.as_dict().empty()) co_return Json::list();
    co_return Err::make("NotImplementedError", "reverify: task 13");
}

}  // namespace scfg::reverify
