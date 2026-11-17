// Twin of scfg_core/backends/__init__.py (see Backends.h).
#include "Scfg/Backends.h"

#include "Scfg/BackendsEvm.h"
#include "Scfg/BackendsSolana.h"
#include "Scfg/Err.h"
#include "Scfg/Py.h"

#include <array>

namespace scfg::backends {

const std::array<const base::Backend*, 2>& registered() {
    static const evm::EvmBackend evm_backend;
    static const solana::SolanaBackend solana_backend;
    static const std::array<const base::Backend*, 2> all = {&evm_backend, &solana_backend};
    return all;
}

Result<const base::Backend*> backend_for_family(std::string_view family) {
    for (const base::Backend* b : registered()) {
        if (b->family() == family) return b;
    }
    return Err::value_error("unknown chain family " + py::repr_str(family));
}

Result<const base::Backend*> backend_for(const chains::Chain& chain) { return backend_for_family(chain.family); }

Result<std::string> normalize_any_tx(const Json& text) {
    for (const base::Backend* b : registered()) {
        Result<std::string> r = b->normalize_tx(text);
        if (r.ok() || !r.err().is_a("ValueError")) return r;
    }
    SCFG_TRY(std::string shown, py::repr(text));
    return Err::value_error("not a transaction hash or signature: " + shown);
}

}  // namespace scfg::backends
