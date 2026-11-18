// Copyright 2026 Repulse
// No Python twin: the tier1/page_messages.json adapters (conformance/python/scfg_conformance/tier1.py's
// parse_tx_message and parse_build_message; unity/core-tests/Tier1.cs's RegisterPageMessages).
#include "support/Tier1.h"

#include "Scfg/Json.h"
#include "Scfg/PageMessages.h"
#include "Scfg/Py.h"

using scfg::Json;
using scfg::Result;
namespace py = scfg::py;
namespace page_messages = scfg::page_messages;

SCFG_TIER1_TOPIC(page_messages) {
    fn("parse_tx_message", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json data, py::get(in, "data"));
        SCFG_TRY(page_messages::TxMessage m, page_messages::parse_tx_message(data));
        return Json::list({Json(m.tx_hash), m.sender ? Json(*m.sender) : Json()});
    });
    fn("parse_build_message", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json data, py::get(in, "data"));
        SCFG_TRY(std::string account, page_messages::parse_build_message(data));
        return Json(account);
    });
}
