// Copyright 2026 Repulse
// No Python twin: SCFGCORE_API, the symbol export macro of the ScfgCore module.
//
// UBT defines SCFGCORE_API for every module it builds: DLLEXPORT while it compiles ScfgCore itself, DLLIMPORT in a
// module that depends on it (a modular build, such as the editor's), empty in a monolithic build (a packaged game).
// Anywhere else (the CMake build, any plain C++ build) it is empty. It goes on every class or struct with a member
// defined in a .cpp, every non-inline free function and friend, and every extern variable of a public header; never
// on a template, an inline function or a constexpr variable, which every module compiles for itself
// (LintTests.ExportsMarked).
#pragma once

#ifdef SCFGCORE_API
// Under UBT. DLLEXPORT and DLLIMPORT come from Unreal's platform headers, which UBT normally force-includes through
// the shared PCH. A core file compiled without one (-NoPCH -NoSharedPCH, UBT's include-what-you-use builds) has not
// seen them, so they are defined here as UnixPlatform.h, ApplePlatform.h and WindowsPlatform.h define them: the
// same tokens, so the engine's own definition, if it follows, is not a conflicting redefinition.
#ifndef DLLEXPORT
#if defined(_WIN32)
#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)
#else
#define DLLEXPORT __attribute__((visibility("default")))
#define DLLIMPORT __attribute__((visibility("default")))
#endif
#endif
#else
#define SCFGCORE_API
#endif
