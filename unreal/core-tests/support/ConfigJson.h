// No Python twin: the configuration adapters of conformance/python/scfg_conformance/tier1_config.py (unity/
// core-tests/Tier1.cs's RegisterConfig), shared by tier1/{Chains,Config,CustomChains,Items,Payments}.cpp and, later,
// the Tier 2 runner and the driver. Does not depend on the test runner (ruling R6).
//
// Inputs: a Config is always the kwargs of build_config; a custom chain inside it is the kwargs of build_chain (a
// non-dict entry is passed through untouched, which is how chain.not_from_add_chain is reached); items are lists of
// add_item kwargs (item_id among them), every value through decode_value ({"float": "4.99"} is a float).
// Outputs are canonical.py's to_jsonable of the dataclasses: Decimals as str(), ints as JSON ints, tuples as lists.
#pragma once

#include "Scfg/Chains.h"
#include "Scfg/Config.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/OrderedMap.h"
#include "Scfg/Payments.h"
#include "Scfg/Result.h"

#include <utility>

namespace scfg_test::config_json {

scfg::Json token_json(const scfg::chains::Token& t);
scfg::Json chain_json(const scfg::chains::Chain& c);
scfg::Json chains_json(const scfg::OrderedMap<scfg::chains::Chain>& chains);
scfg::Json item_json(const scfg::models::Item& it);
scfg::Json config_json(const scfg::config::Config& c);
scfg::Json spec_json(const scfg::payments::PaymentSpec& s);

// _config(kwargs): build_config(**kw) with every dict in kw["custom_chains"] built by build_chain(**entry).
scfg::Result<scfg::config::Config> config_from(const scfg::Json& kwargs);

// _item_kwargs(d) split as add_item is called: (item_id, the other kwargs), every value decoded; no item_id is the
// KeyError of kw.pop("item_id").
scfg::Result<std::pair<scfg::Json, scfg::Json>> item_kwargs(const scfg::Json& d);

// _with_items(config_kw, items): the config, then add_item for each entry in order.
scfg::Result<scfg::config::Config> with_items(const scfg::Json& config_kw, const scfg::Json& items);

}  // namespace scfg_test::config_json
