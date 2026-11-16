// Twin of scfg_core/transfers.py: the Transfer dataclass is all of the module (Transfers.h); this is its ==.
#include "Scfg/Transfers.h"

#include "Scfg/Py.h"

namespace scfg::transfers {

bool operator==(const Transfer& a, const Transfer& b) {
    return a.token == b.token && py::eq(a.sender, b.sender) && a.recipient == b.recipient && a.value == b.value &&
           py::eq(a.tx_hash, b.tx_hash) && py::eq(a.log_index, b.log_index) && a.block_number == b.block_number;
}

}  // namespace scfg::transfers
