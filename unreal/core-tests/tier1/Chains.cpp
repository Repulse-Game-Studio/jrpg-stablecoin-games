// Copyright 2026 Repulse
// No Python twin: the tier1/chains.json adapter (tier1_config.py's get_chains; unity/core-tests/Tier1.cs).
#include "support/ConfigJson.h"
#include "support/Tier1.h"

#include "Scfg/Chains.h"
#include "Scfg/Json.h"
#include "Scfg/Py.h"

using scfg::Json;
using scfg::Result;
namespace py = scfg::py;

SCFG_TIER1_TOPIC(chains) {
    fn("get_chains", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json testnet, py::get(in, "testnet"));
        return scfg_test::config_json::chains_json(scfg::chains::get_chains(py::truthy(testnet)));
    });
}
