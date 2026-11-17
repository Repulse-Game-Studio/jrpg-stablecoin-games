// No Python twin: see ScfgPlatform.h.
#include "ScfgPlatform.h"

#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

const TCHAR* const FScfgEnginePlatform::MobileSwitch = TEXT("scfg-mobile");

bool FScfgEnginePlatform::MobileOverride(const TCHAR* CommandLine)
{
#if UE_BUILD_SHIPPING
	(void)CommandLine;
	return false;
#else
	return CommandLine != nullptr && FParse::Param(CommandLine, MobileSwitch);
#endif
}

bool FScfgEnginePlatform::DefaultIsMobile()
{
	return bMobileTarget || MobileOverride(FCommandLine::Get());
}

void FScfgEnginePlatform::OpenUrl(const FString& Url)
{
	// Android: an ACTION_VIEW intent (CATEGORY_BROWSABLE) to the app registered for the scheme; iOS: UIApplication
	// openURL. A link nothing handles is logged by the engine (engine-facts.md, "Mobile").
	FString Error;
	FPlatformProcess::LaunchURL(*Url, nullptr, &Error);
}

void FScfgEnginePlatform::ClipboardSet(const FString& Text)
{
	FPlatformApplicationMisc::ClipboardCopy(*Text);
}

FString FScfgEnginePlatform::ClipboardGet()
{
	FString Out;
	FPlatformApplicationMisc::ClipboardPaste(Out);
	return Out;
}
