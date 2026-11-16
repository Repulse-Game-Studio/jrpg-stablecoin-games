// No Python twin: force-included (-include) before every core file in the scfg_core_poisoned target.
//
// Unreal and the Windows headers it pulls in #define plain words as macros. In the core they would silently turn a
// name into something else, so here each one expands to SCFG_UE_POISON_<name>::unreal_macro, a member of a
// namespace that does not exist: an expression, a declaration or an enumerator that spells the name fails to compile
// ("'SCFG_UE_POISON_ERROR' has not been declared"), as it would (or worse, would not) inside Unreal. A bare
// undeclared identifier is not enough: `enum class Kind { ERROR };` or a member `bool check(int);` would quietly
// declare SCFG_UE_POISON_ERROR or SCFG_UE_POISON_check and every use would match it. Function-like macros are
// poisoned function-like, so a member or variable that merely shares the name and is never called stays legal, as
// it does under Unreal. Not poisoned: min and max (Unreal builds with NOMINMAX) and anything the C or C++ standard
// defines.
#pragma once

// Unreal's Core module (AssertionMacros.h, Platform.h, UnrealMathUtility.h, CoreMiscDefines.h).
#define check(...) SCFG_UE_POISON_check::unreal_macro
#define checkf(...) SCFG_UE_POISON_checkf::unreal_macro
#define checkSlow(...) SCFG_UE_POISON_checkSlow::unreal_macro
#define checkNoEntry(...) SCFG_UE_POISON_checkNoEntry::unreal_macro
#define verify(...) SCFG_UE_POISON_verify::unreal_macro
#define verifyf(...) SCFG_UE_POISON_verifyf::unreal_macro
#define ensure(...) SCFG_UE_POISON_ensure::unreal_macro
#define ensureMsgf(...) SCFG_UE_POISON_ensureMsgf::unreal_macro
#define ensureAlways(...) SCFG_UE_POISON_ensureAlways::unreal_macro
#define TEXT(...) SCFG_UE_POISON_TEXT::unreal_macro
#define PI SCFG_UE_POISON_PI::unreal_macro
#define INDEX_NONE SCFG_UE_POISON_INDEX_NONE::unreal_macro
#define FORCEINLINE SCFG_UE_POISON_FORCEINLINE::unreal_macro

// windows.h (minwindef.h, winnt.h, wingdi.h, winerror.h, winbase.h, rpcndr.h), which Unreal includes on Windows.
// BOOL, INT, UINT, DWORD, CHAR and TCHAR are typedefs there (TCHAR is Unreal's too), poisoned so the core never
// borrows the name.
#define TRUE SCFG_UE_POISON_TRUE::unreal_macro
#define FALSE SCFG_UE_POISON_FALSE::unreal_macro
#define BOOL SCFG_UE_POISON_BOOL::unreal_macro
#define INT SCFG_UE_POISON_INT::unreal_macro
#define UINT SCFG_UE_POISON_UINT::unreal_macro
#define DWORD SCFG_UE_POISON_DWORD::unreal_macro
#define CHAR SCFG_UE_POISON_CHAR::unreal_macro
#define TCHAR SCFG_UE_POISON_TCHAR::unreal_macro
#define ERROR SCFG_UE_POISON_ERROR::unreal_macro
#define DELETE SCFG_UE_POISON_DELETE::unreal_macro
#define IN SCFG_UE_POISON_IN::unreal_macro
#define OUT SCFG_UE_POISON_OUT::unreal_macro
#define OPTIONAL SCFG_UE_POISON_OPTIONAL::unreal_macro
#define CONST SCFG_UE_POISON_CONST::unreal_macro
#define VOID SCFG_UE_POISON_VOID::unreal_macro
#define interface SCFG_UE_POISON_interface::unreal_macro
#define small SCFG_UE_POISON_small::unreal_macro
#define hyper SCFG_UE_POISON_hyper::unreal_macro
#define near SCFG_UE_POISON_near::unreal_macro
#define far SCFG_UE_POISON_far::unreal_macro
#define NO_ERROR SCFG_UE_POISON_NO_ERROR::unreal_macro
#define INFINITE SCFG_UE_POISON_INFINITE::unreal_macro
#define ABSOLUTE SCFG_UE_POISON_ABSOLUTE::unreal_macro
#define RELATIVE SCFG_UE_POISON_RELATIVE::unreal_macro
#define TRANSPARENT SCFG_UE_POISON_TRANSPARENT::unreal_macro
#define OPAQUE SCFG_UE_POISON_OPAQUE::unreal_macro
#define Yield(...) SCFG_UE_POISON_Yield::unreal_macro
#define GetCurrentTime(...) SCFG_UE_POISON_GetCurrentTime::unreal_macro

// windows.h's A/W aliases (GetObject -> GetObjectW, ...).
#define GetObject SCFG_UE_POISON_GetObject::unreal_macro
#define SendMessage SCFG_UE_POISON_SendMessage::unreal_macro
#define PostMessage SCFG_UE_POISON_PostMessage::unreal_macro
#define GetMessage SCFG_UE_POISON_GetMessage::unreal_macro
#define LoadString SCFG_UE_POISON_LoadString::unreal_macro
#define DrawText SCFG_UE_POISON_DrawText::unreal_macro
#define CreateFile SCFG_UE_POISON_CreateFile::unreal_macro
#define DeleteFile SCFG_UE_POISON_DeleteFile::unreal_macro
#define CopyFile SCFG_UE_POISON_CopyFile::unreal_macro
#define MoveFile SCFG_UE_POISON_MoveFile::unreal_macro
#define GetUserName SCFG_UE_POISON_GetUserName::unreal_macro
#define GetClassName SCFG_UE_POISON_GetClassName::unreal_macro
#define FormatMessage SCFG_UE_POISON_FormatMessage::unreal_macro
#define GetCommandLine SCFG_UE_POISON_GetCommandLine::unreal_macro
#define LoadLibrary SCFG_UE_POISON_LoadLibrary::unreal_macro
#define MessageBox SCFG_UE_POISON_MessageBox::unreal_macro
#define CreateEvent SCFG_UE_POISON_CreateEvent::unreal_macro
#define CreateMutex SCFG_UE_POISON_CreateMutex::unreal_macro

// MSVC <math.h> oldnames (the SVID matherr codes, defined unless _CRT_INTERNAL_NONSTDC_NAMES is 0).
#define DOMAIN SCFG_UE_POISON_DOMAIN::unreal_macro
#define SING SCFG_UE_POISON_SING::unreal_macro
#define OVERFLOW SCFG_UE_POISON_OVERFLOW::unreal_macro
#define UNDERFLOW SCFG_UE_POISON_UNDERFLOW::unreal_macro
#define TLOSS SCFG_UE_POISON_TLOSS::unreal_macro
#define PLOSS SCFG_UE_POISON_PLOSS::unreal_macro

// glibc <sys/sysmacros.h> (reached through <sys/types.h> on older glibc, as on some Linux toolchains).
#define major(...) SCFG_UE_POISON_major::unreal_macro
#define minor(...) SCFG_UE_POISON_minor::unreal_macro

// GNU predefines (-std=gnu++20, the default without CMAKE_CXX_EXTENSIONS OFF): linux and unix are 1.
#define linux SCFG_UE_POISON_linux::unreal_macro
#define unix SCFG_UE_POISON_unix::unreal_macro
