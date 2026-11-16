// No Python twin: the tier1/solana_tx.json adapters (conformance/python/scfg_conformance/tier1.py's solana_tx block;
// unity/core-tests/Tier1.cs). Bytes travel as hex; compact_u16's n is passed as the vector gives it, with Python's
// errors for a value that is not an int.
#include "support/Tier1.h"

#include "Scfg/BigInt.h"
#include "Scfg/Bytes.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/PyFloat.h"
#include "Scfg/SolanaTx.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace py = scfg::py;
namespace tx = scfg::solana::tx;

namespace scfg_test::tier1::solana_tx_adapters {

Result<std::string> str_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_str()) return Err::value_error("vector input " + std::string(key) + " must be a str");
    return v.as_str();
}

Result<bool> bool_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_bool()) return Err::value_error("vector input " + std::string(key) + " must be a bool");
    return v.as_bool();
}

// bytes.fromhex(d[key]).
Result<std::vector<uint8_t>> hex_field(const Json& d, std::string_view key) {
    SCFG_TRY(Json v, py::get(d, key));
    if (!v.is_str()) return Err::type_error("fromhex() argument must be str, not " + std::string(v.type_name()));
    return scfg::bytes::from_hex(v.as_str());
}

// compact_u16(n) for any JSON n, as Python's `0 <= n <= 0xFFFF` and `n & 0x7F` treat it.
Result<std::vector<uint8_t>> compact_u16(const Json& n) {
    if (n.is_bool()) return tx::compact_u16(n.as_bool() ? 1 : 0);
    if (n.is_int64()) return tx::compact_u16(n.as_int64());
    if (n.is_int()) return Err::value_error("compact-u16 out of range: " + n.as_bigint().to_decimal());
    if (n.is_float()) {
        const double f = n.as_float();
        if (f >= 0 && f <= 65535) return Err::type_error("unsupported operand type(s) for &: 'float' and 'int'");
        return Err::value_error("compact-u16 out of range: " + scfg::pyfloat::repr(f));
    }
    return Err::type_error("'<=' not supported between instances of 'int' and '" + std::string(n.type_name()) + "'");
}

// tier1.py's _compile_message.
Result<Json> compile_message(const Json& in) {
    SCFG_TRY(Json list, py::get(in, "instructions"));
    if (!list.is_list()) return Err::value_error("vector input instructions must be a list");
    std::vector<tx::Instruction> ixs;
    for (const Json& x : list.as_list()) {
        tx::Instruction ix;
        SCFG_TRY(ix.program_id, str_field(x, "program_id"));
        SCFG_TRY(Json accounts, py::get(x, "accounts"));
        if (!accounts.is_list()) return Err::value_error("vector input accounts must be a list");
        for (const Json& m : accounts.as_list()) {
            tx::AccountMeta meta;
            SCFG_TRY(meta.pubkey, str_field(m, "pubkey"));
            SCFG_TRY(meta.is_signer, bool_field(m, "is_signer"));
            SCFG_TRY(meta.is_writable, bool_field(m, "is_writable"));
            ix.accounts.push_back(std::move(meta));
        }
        SCFG_TRY(ix.data, hex_field(x, "data_hex"));
        ixs.push_back(std::move(ix));
    }
    SCFG_TRY(std::string payer, str_field(in, "payer"));
    SCFG_TRY(std::string blockhash, str_field(in, "recent_blockhash"));
    SCFG_TRY(std::vector<uint8_t> message, tx::compile_message(payer, ixs, blockhash));
    return Json(scfg::bytes::to_hex(message));
}

}  // namespace scfg_test::tier1::solana_tx_adapters

SCFG_TIER1_TOPIC(solana_tx) {
    namespace a = scfg_test::tier1::solana_tx_adapters;
    fn("compact_u16", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json n, py::get(in, "n"));
        SCFG_TRY(std::vector<uint8_t> out, a::compact_u16(n));
        return Json(scfg::bytes::to_hex(out));
    });
    fn("compile_message", [](const Json& in) -> Result<Json> { return a::compile_message(in); });
    fn("unsigned_transaction", [](const Json& in) -> Result<Json> {
        SCFG_TRY(std::vector<uint8_t> message, a::hex_field(in, "message_hex"));
        SCFG_TRY(std::vector<uint8_t> out, tx::unsigned_transaction(message));
        return Json(scfg::bytes::to_hex(out));
    });
}
