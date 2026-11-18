// Copyright 2026 Repulse
// No Python twin: the glue's one bridge between the core's strings (std::string, UTF-8) and Unreal's FString
// (TCHAR: UTF-16 on every platform the plugin builds for).
//
// Conversions go through the engine's FTCHARToUTF8 and FUTF8ToTCHAR, so a code point outside the BMP (an emoji, a
// CJK extension character) crosses intact as a surrogate pair on the FString side and four bytes on the UTF-8 side.
//
// Lone surrogate policy: what cannot be represented becomes '?' (the engine's UNICODE_BOGUS_CHAR_CODEPOINT,
// GenericPlatformString.h:55), one '?' for each unpaired surrogate and each invalid UTF-8 sequence. An FString with an
// unpaired surrogate is not valid Unicode, and UTF-8 may not encode a surrogate (StringConv.h:255), so the core never
// receives one from the glue. The core keeps a lone surrogate it read from a node's JSON "\ud800" escape as its
// generalized UTF-8 bytes (ED A0 80, Json.h), and those become a '?' on the way to the screen. Nothing else is
// replaced: an embedded NUL crosses both ways, since lengths are passed, never NUL-terminated pointers.
// ScfgStringTests (Scfg.Glue.String.*) pins all of this.
#pragma once

#include "CoreMinimal.h"

#include <string>
#include <string_view>

namespace ScfgString
{

// The UTF-8 bytes of S (see the policy above).
SCFG_API std::string ToUtf8(FStringView S);
// The FString of UTF-8 bytes (see the policy above).
SCFG_API FString FromUtf8(std::string_view S);

}  // namespace ScfgString
