// Copyright 2026 Repulse
// No Python twin: the build_chain adapter of tier1/custom_chains.json (tier1_config.py's _build_chain; unity/
// core-tests/Tier1.cs). The topic's build_config cases use tier1/Config.cpp's adapter.
#include "support/ConfigJson.h"
#include "support/Tier1.h"

#include "Scfg/Chains.h"
#include "Scfg/Config.h"
#include "Scfg/Json.h"

using scfg::Json;
using scfg::Result;

SCFG_TIER1_TOPIC(custom_chains) {
    fn("build_chain", [](const Json& in) -> Result<Json> {
        SCFG_TRY(scfg::chains::Chain c, scfg::config::build_chain(in));
        return scfg_test::config_json::chain_json(c);
    });
}
