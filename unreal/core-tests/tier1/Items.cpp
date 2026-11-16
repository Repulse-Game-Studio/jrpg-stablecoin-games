// No Python twin: the tier1/items.json adapter (tier1_config.py's _add_item; unity/core-tests/Tier1.cs).
#include "support/ConfigJson.h"
#include "support/Tier1.h"

#include "Scfg/Config.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/Py.h"

using scfg::Json;
using scfg::Result;
namespace cj = scfg_test::config_json;
namespace py = scfg::py;

SCFG_TIER1_TOPIC(items) {
    fn("add_item", [](const Json& in) -> Result<Json> {
        SCFG_TRY(Json config_kw, py::get(in, "config"));
        Json existing = in.as_dict().get("existing", Json::list());
        SCFG_TRY(scfg::config::Config config, cj::with_items(config_kw, existing));
        SCFG_TRY(Json item, py::get(in, "item"));
        SCFG_TRY(auto kw, cj::item_kwargs(item));
        SCFG_TRY(scfg::models::Item added, scfg::config::add_item(config, kw.first, kw.second));
        return cj::item_json(added);
    });
}
