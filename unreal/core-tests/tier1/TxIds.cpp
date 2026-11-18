// Copyright 2026 Repulse
// No Python twin: the tier1/tx_ids.json adapters (conformance/python/scfg_conformance/tier1.py's normalize_tx_hash
// and normalize_any_tx; unity/core-tests/Tier1.cs's RegisterTxIds). normalize_signature, the third fn of the file, is
// registered with the base58 topic.
#include "support/Tier1.h"

#include "Scfg/Backends.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"
#include "Scfg/Sessions.h"

using scfg::Json;
using scfg::Result;
namespace py = scfg::py;

SCFG_TIER1_TOPIC(tx_ids) {
    fn("normalize_tx_hash", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json text, py::get(in, "text"));
        SCFG_TRY(std::string tx, scfg::sessions::normalize_tx_hash(text));
        return Json(tx);
    });
    fn("normalize_any_tx", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json text, py::get(in, "text"));
        SCFG_TRY(std::string tx, scfg::backends::normalize_any_tx(text));
        return Json(tx);
    });
}
