// Copyright 2026 Repulse
// Twin of scfg.gd's _Platform (and Unity's IScfgPlatform): where the game runs and how it reaches the world outside
// (the browser, the wallet app, the clipboard). The tests replace it. The web build has no Unreal twin (UE5 has no
// web target); a phone or tablet is Android or iOS.
#pragma once

#include "CoreMinimal.h"

class SCFG_API IScfgPlatform
{
public:
	virtual ~IScfgPlatform() = default;
	// Ren'Py's renpy.mobile: a phone or tablet. The checkout then offers no browser method (no localhost page: see
	// unreal/README.md, "Mobile") and the QR step offers "Open wallet app".
	virtual bool IsMobile() const = 0;
	virtual void OpenUrl(const FString& Url) = 0;
	virtual void ClipboardSet(const FString& Text) = 0;
	virtual FString ClipboardGet() = 0;
};

// The engine's own: FPlatformProcess::LaunchURL (the system browser, or on a phone the app that handles the link's
// scheme: the wallet for ethereum: and solana:) and FPlatformApplicationMisc's clipboard, on every platform the plugin
// builds for. The facade's default. A link LaunchURL reports it could not open (a game's FCoreDelegates::ShouldLaunchUrl
// refused it) is logged once per press, "scfg: cannot open the <scheme>: link: <why>", without the link itself.
class SCFG_API FScfgEnginePlatform final : public IScfgPlatform
{
public:
	// The switch that makes a desktop game run the mobile checkout (not in Shipping): -scfg-mobile.
	static const TCHAR* const MobileSwitch;
	// The build's own answer: PLATFORM_ANDROID || PLATFORM_IOS.
	static constexpr bool bMobileTarget = PLATFORM_ANDROID || PLATFORM_IOS;
	// The test override: CommandLine has MobileSwitch, outside Shipping (always false in Shipping).
	static bool MobileOverride(const TCHAR* CommandLine);
	// What a game gets: bMobileTarget, or this process's command line has MobileSwitch (see MobileOverride).
	static bool DefaultIsMobile();

	FScfgEnginePlatform() : bMobile(DefaultIsMobile()) {}
	// The tests: the mobile answer given.
	explicit FScfgEnginePlatform(bool bInMobile) : bMobile(bInMobile) {}

	virtual bool IsMobile() const override { return bMobile; }
	virtual void OpenUrl(const FString& Url) override;
	virtual void ClipboardSet(const FString& Text) override;
	virtual FString ClipboardGet() override;

private:
	bool bMobile;
};
