// No Python twin: the tier1/payments.json adapter (tier1_config.py's _prepare_payment; unity/core-tests/Tier1.cs).
// item, amount, token and title are passed only when the case has them, each through decode_value.
#include "support/ConfigJson.h"
#include "support/Tier1.h"

#include "Scfg/Config.h"
#include "Scfg/Json.h"
#include "Scfg/Payments.h"
#include "Scfg/Py.h"

using scfg::Json;
using scfg::Result;
namespace cj = scfg_test::config_json;
namespace py = scfg::py;

SCFG_TIER1_TOPIC(payments) {
    fn("prepare_payment", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json config_kw, py::get(in, "config"));
        Json items = in.as_dict().get("items", Json::list());
        SCFG_TRY(scfg::config::Config config, cj::with_items(config_kw, items));
        Json kw = Json::dict();
        for (const char* key : {"item", "amount", "token", "title"}) {
            if (const Json* v = in.as_dict().find(key)) {
                SCFG_TRY(Json decoded, scfg_test::tier1::decode_value(*v));
                kw.as_dict().set(key, decoded);
            }
        }
        SCFG_TRY(Json kind, py::get(in, "kind"));
        SCFG_TRY(scfg::payments::PaymentSpec spec, scfg::payments::prepare_payment(config, kind, kw));
        return cj::spec_json(spec);
    });
}
