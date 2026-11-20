// Copyright 2026 Repulse
// No Python twin: see ScfgPlatform.h.
#include "ScfgPlatform.h"

#include "ScfgLog.h"

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
	// openURL. A link nothing handles is logged by the engine.
	FString Error;
	FPlatformProcess::LaunchURL(*Url, nullptr, &Error);
	if (!Error.IsEmpty())
	{
		// Such as "LaunchURL cancelled by delegate" (a game's FCoreDelegates::ShouldLaunchUrl, a parental gate): one
		// line with the link's scheme, never the link itself (the payment request).
		FString Scheme;
		if (!Url.Split(TEXT(":"), &Scheme, nullptr))
		{
			Scheme = TEXT("(no scheme)");
		}
		UE_LOG(LogScfg, Warning, TEXT("scfg: cannot open the %s: link: %s"), *Scheme, *Error);
	}
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
