// Copyright 2026 Repulse
// No Python twin: see ScfgString.h.
#include "ScfgString.h"

#include "Containers/StringConv.h"

#include <limits>

// The policy in ScfgString.h is written for UTF-16 TCHAR (a USE_UTF8_TCHARS build would need another bridge).
static_assert(sizeof(TCHAR) == 2, "ScfgString assumes a UTF-16 TCHAR");

namespace ScfgString
{

std::string ToUtf8(FStringView S)
{
	const FTCHARToUTF8 Converted(S.GetData(), S.Len());
	return std::string(reinterpret_cast<const char*>(Converted.Get()), static_cast<size_t>(Converted.Length()));
}

FString FromUtf8(std::string_view S)
{
	// The engine's conversions take int32 lengths. The core's strings are far smaller (a node's answer is capped at
	// 16 MiB), so a longer one is a programming error, not text to cut short.
	checkf(S.size() <= static_cast<size_t>(std::numeric_limits<int32>::max()), TEXT("a string of %llu bytes"),
		static_cast<unsigned long long>(S.size()));
	const FUTF8ToTCHAR Converted(reinterpret_cast<const UTF8CHAR*>(S.data()), static_cast<int32>(S.size()));
	return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
}

}  // namespace ScfgString
