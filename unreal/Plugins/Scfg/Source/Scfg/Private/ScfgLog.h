// No Python twin: the glue's log category. Every Err that reaches the glue is logged once here
// ("LogScfg: Error: scfg: <Err::str()>"), never printed to stdout by the core's print_error.
#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

SCFG_API DECLARE_LOG_CATEGORY_EXTERN(LogScfg, Log, All);
