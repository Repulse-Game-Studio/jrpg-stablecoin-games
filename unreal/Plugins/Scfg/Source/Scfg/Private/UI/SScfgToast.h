// Copyright 2026 Repulse
// Twin of godot/addons/scfg/ui/toast.gd: a short notification ("Payment confirmed: ...") at the top right of the
// screen, shown by FScfgDefaultSheetHost::Notify when the subsystem's Notify is on (Ren'Py's renpy.notify). It fades
// out after ShowSeconds of real time (Slate's tick: a paused game or a time dilation of 0 does not hold it) and then asks
// its host to remove it (OnFinished). It never takes the focus or the mouse. The text is already worded.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

#include "UI/ScfgStyle.h"

class SCFG_API SScfgToast : public SCompoundWidget
{
public:
	// Seconds shown before it fades, and the fade's.
	static constexpr float ShowSeconds = 3.f;
	static constexpr float FadeSeconds = 0.3f;

	SLATE_BEGIN_ARGS(SScfgToast) {}
		SLATE_ARGUMENT(FString, Text)
		SLATE_EVENT(FSimpleDelegate, OnFinished)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<FScfgStyle>& InStyle);

	// Real seconds pass (Tick calls it with Slate's delta).
	void Advance(float Seconds);
	bool IsFinished() const { return bFinished; }
	float Age() const { return Elapsed; }
	const FString& GetText() const { return Text; }

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual bool SupportsKeyboardFocus() const override { return false; }

private:
	FLinearColor Fade() const;

	TSharedPtr<FScfgStyle> Style;
	FString Text;
	FSimpleDelegate OnFinished;
	float Elapsed = 0.f;
	bool bFinished = false;
};
