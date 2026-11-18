// Copyright 2026 Repulse
// No Python twin: the build_config adapter of tier1/config.json and tier1/custom_chains.json (tier1_config.py's
// _build_config; unity/core-tests/Tier1.cs). Adapters are keyed by fn across topics, so the custom_chains topic's
// build_config cases run through this one.
#include "support/ConfigJson.h"
#include "support/Tier1.h"

#include "Scfg/Config.h"
#include "Scfg/Json.h"

using scfg::Json;
using scfg::Result;

SCFG_TIER1_TOPIC(config) {
    fn("build_config", [](const Json& in) -> Result<Json> {
        SCFG_TRY(scfg::config::Config c, scfg_test::config_json::config_from(in));
        return scfg_test::config_json::config_json(c);
    });
}
