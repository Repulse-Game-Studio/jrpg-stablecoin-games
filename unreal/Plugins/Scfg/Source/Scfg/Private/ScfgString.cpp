// No Python twin: see ScfgString.h.
#include "ScfgString.h"

#include "Containers/StringConv.h"

namespace ScfgString
{

std::string ToUtf8(FStringView S)
{
	const FTCHARToUTF8 Converted(S.GetData(), S.Len());
	return std::string(reinterpret_cast<const char*>(Converted.Get()), static_cast<size_t>(Converted.Length()));
}

FString FromUtf8(std::string_view S)
{
	const FUTF8ToTCHAR Converted(reinterpret_cast<const UTF8CHAR*>(S.data()), static_cast<int32>(S.size()));
	return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
}

}  // namespace ScfgString
