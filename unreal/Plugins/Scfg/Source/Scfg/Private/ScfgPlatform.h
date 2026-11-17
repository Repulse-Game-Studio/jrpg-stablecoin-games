// Twin of scfg.gd's _Platform (and Unity's IScfgPlatform): where the game runs and how it reaches the world outside
// (the browser, the wallet app, the clipboard). The tests replace it. The web build has no Unreal twin: Unreal's
// desktop targets only.
#pragma once

#include "CoreMinimal.h"

class SCFG_API IScfgPlatform
{
public:
	virtual ~IScfgPlatform() = default;
	// Ren'Py's renpy.mobile: a phone or tablet.
	virtual bool IsMobile() const = 0;
	virtual void OpenUrl(const FString& Url) = 0;
	virtual void ClipboardSet(const FString& Text) = 0;
	virtual FString ClipboardGet() = 0;
};

// FPlatformProcess::LaunchURL and FPlatformApplicationMisc's clipboard.
class SCFG_API FScfgDesktopPlatform final : public IScfgPlatform
{
public:
	virtual bool IsMobile() const override;
	virtual void OpenUrl(const FString& Url) override;
	virtual void ClipboardSet(const FString& Text) override;
	virtual FString ClipboardGet() override;
};
