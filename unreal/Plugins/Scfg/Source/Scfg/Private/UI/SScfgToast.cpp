// Copyright 2026 Repulse
// Twin of godot/addons/scfg/ui/toast.gd (see SScfgToast.h).
#include "UI/SScfgToast.h"

#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

void SScfgToast::Construct(const FArguments& InArgs, const TSharedRef<FScfgStyle>& InStyle)
{
	Style = InStyle;
	Text = InArgs._Text;
	OnFinished = InArgs._OnFinished;
	SetCanTick(true);
	SetVisibility(EVisibility::HitTestInvisible);
	ChildSlot
	.HAlign(HAlign_Right)
	.VAlign(VAlign_Top)
	.Padding(FMargin(0, 24, 24, 0))
	[
		SNew(SBorder)
		.BorderImage(Style->Brush(TEXT("Scfg.Toast")))
		.Padding(FMargin(18, 12))
		.ColorAndOpacity(this, &SScfgToast::Fade)
		.BorderBackgroundColor_Lambda([this]() { return FSlateColor(FLinearColor(1, 1, 1, Fade().A)); })
		[
			SNew(STextBlock)
			.Text(FText::FromString(Text))
			.TextStyle(&Style->Text(TEXT("Scfg.ToastText")))
		]
	];
}

FLinearColor SScfgToast::Fade() const
{
	const float Past = Elapsed - ShowSeconds;
	const float Alpha = Past <= 0.f ? 1.f : FMath::Clamp(1.f - Past / FadeSeconds, 0.f, 1.f);
	return FLinearColor(1, 1, 1, Alpha);
}

void SScfgToast::Advance(float Seconds)
{
	if (bFinished)
	{
		return;
	}
	Elapsed += FMath::Max(0.f, Seconds);
	if (Elapsed >= ShowSeconds + FadeSeconds)
	{
		bFinished = true;
		OnFinished.ExecuteIfBound();
	}
}

void SScfgToast::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	Advance(InDeltaTime);
}
