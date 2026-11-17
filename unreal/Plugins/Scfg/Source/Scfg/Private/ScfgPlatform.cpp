// No Python twin: see ScfgPlatform.h.
#include "ScfgPlatform.h"

#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"

bool FScfgDesktopPlatform::IsMobile() const
{
	return PLATFORM_IOS || PLATFORM_ANDROID;
}

void FScfgDesktopPlatform::OpenUrl(const FString& Url)
{
	FString Error;
	FPlatformProcess::LaunchURL(*Url, nullptr, &Error);
}

void FScfgDesktopPlatform::ClipboardSet(const FString& Text)
{
	FPlatformApplicationMisc::ClipboardCopy(*Text);
}

FString FScfgDesktopPlatform::ClipboardGet()
{
	FString Out;
	FPlatformApplicationMisc::ClipboardPaste(Out);
	return Out;
}
