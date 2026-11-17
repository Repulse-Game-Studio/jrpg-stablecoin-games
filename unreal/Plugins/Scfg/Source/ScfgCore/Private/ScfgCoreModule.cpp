// No Python twin: the ScfgCore module's registration with Unreal, the module's only Unreal code. UBT compiles it; the
// CMake build leaves it out, and LintTests.ModuleFileIsMinimal keeps it to these two lines.
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, ScfgCore)
