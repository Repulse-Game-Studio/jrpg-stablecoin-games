// Copyright 2026 Repulse
// No Python twin: the tier1/pda.json adapters (conformance/python/scfg_conformance/tier1.py's pda block;
// unity/core-tests/Tier1.cs). Seeds travel as hex, the program id as base58 (decode_fixed(program_id, 32)).
#include "support/Tier1.h"

#include "Scfg/Bytes.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/SolanaBase58.h"
#include "Scfg/SolanaPda.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using scfg::Err;
using scfg::Json;
using scfg::Result;
namespace base58 = scfg::solana::base58;
namespace pda = scfg::solana::pda;
namespace py = scfg::py;

namespace scfg_test::tier1::pda_adapters {

// [bytes.fromhex(s) for s in i["seeds_hex"]].
Result<pda::Seeds> seeds(const Json& in) {
    SCFG_TRY(Json list, py::get(in, "seeds_hex"));
    if (!list.is_list()) return Err::value_error("vector input seeds_hex must be a list");
    pda::Seeds out;
    for (const Json& s : list.as_list()) {
        if (!s.is_str()) return Err::type_error("fromhex() argument must be str, not " + std::string(s.type_name()));
        SCFG_TRY(std::vector<uint8_t> seed, scfg::bytes::from_hex(s.as_str()));
        out.push_back(std::move(seed));
    }
    return out;
}

// base58.decode_fixed(i["program_id"], 32).
Result<std::vector<uint8_t>> program_id(const Json& in) {
    SCFG_TRY(Json text, py::get(in, "program_id"));
    return base58::decode_fixed(text, 32);
}

}  // namespace scfg_test::tier1::pda_adapters

SCFG_TIER1_TOPIC(pda) {
    namespace a = scfg_test::tier1::pda_adapters;
    fn("create_program_address", [](const Json& in) -> Result<Json> {
        SCFG_TRY(pda::Seeds seeds, a::seeds(in));
        SCFG_TRY(std::vector<uint8_t> program, a::program_id(in));
        SCFG_TRY(std::optional<pda::Address> out, pda::create_program_address(seeds, program));
        return out ? Json(base58::b58encode(*out)) : Json();
    });
    fn("find_program_address", [](const Json& in) -> Result<Json> {
        SCFG_TRY(pda::Seeds seeds, a::seeds(in));
        SCFG_TRY(std::vector<uint8_t> program, a::program_id(in));
        SCFG_TRY(pda::ProgramAddress found, pda::find_program_address(seeds, program));
        return Json::dict({{"address", Json(base58::b58encode(found.address))}, {"bump", Json(found.bump)}});
    });
    fn("associated_token_address", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json owner, py::get(in, "owner"));
        SCFG_TRY(Json mint, py::get(in, "mint"));
        SCFG_TRY(std::string ata, pda::associated_token_address(owner, mint));
        return Json(ata);
    });
}
