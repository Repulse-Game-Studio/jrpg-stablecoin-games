// Copyright 2026 Repulse
// Twin of godot/addons/scfg/ui/qr_view.gd (see SScfgQrView.h).
#include "UI/SScfgQrView.h"

#include "Rendering/DrawElements.h"

#include "ScfgString.h"

#include "Scfg/Paylinks.h"

SScfgQrView::SScfgQrView() : White(FLinearColor::White), Black(FLinearColor::Black) {}

void SScfgQrView::Construct(const FArguments& InArgs)
{
	Load(InArgs._Text, InArgs._TargetSize);
}

bool SScfgQrView::ShowText(const FString& Text, float InTargetSize)
{
	const bool bShown = Load(Text, InTargetSize);
	Invalidate(EInvalidateWidgetReason::Layout);
	return bShown;
}

bool SScfgQrView::Load(const FString& Text, float InTargetSize)
{
	TargetSize = InTargetSize;
	Runs.Reset();
	Size = 0;
	Error.Reset();
	const scfg::Result<scfg::paylinks::QrRects> Made = scfg::paylinks::qr_rects(ScfgString::ToUtf8(Text), BORDER);
	if (!Made.ok())
	{
		Error = ScfgString::FromUtf8(Made.err().message);
		return false;
	}
	Size = int32(Made.value().size);
	for (const scfg::paylinks::QrRun& R : Made.value().runs)
	{
		Runs.Add({int32(R.x), int32(R.y), int32(R.width)});
	}
	return true;
}

int32 SScfgQrView::ModuleScale(float Available) const
{
	if (Size <= 0)
	{
		return 1;
	}
	return FMath::Max(1, FMath::FloorToInt(FMath::Min(TargetSize, Available) / float(Size)));
}

TArray<FIntRect> SScfgQrView::DarkRects(int32 Scale) const
{
	TArray<FIntRect> Out;
	Out.Reserve(Runs.Num());
	for (const FRun& R : Runs)
	{
		Out.Add(FIntRect(R.X * Scale, R.Y * Scale, (R.X + R.Width) * Scale, (R.Y + 1) * Scale));
	}
	return Out;
}

FVector2D SScfgQrView::ComputeDesiredSize(float) const
{
	const float Side = float(Size * ModuleScale(TargetSize));
	return FVector2D(Side, Side);
}

int32 SScfgQrView::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	PaintedBoxes = 0;
	if (Size <= 0)
	{
		PaintedScale = 0;
		return LayerId;
	}
	const FVector2f Local = AllottedGeometry.GetLocalSize();
	const int32 Scale = ModuleScale(FMath::Min(Local.X, Local.Y));
	PaintedScale = Scale;
	const float Side = float(Size * Scale);
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
		AllottedGeometry.ToPaintGeometry(FVector2f(Side, Side), FSlateLayoutTransform()), &White);
	for (const FIntRect& R : DarkRects(Scale))
	{
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(FVector2f(float(R.Width()), float(R.Height())),
				FSlateLayoutTransform(FVector2f(float(R.Min.X), float(R.Min.Y)))),
			&Black);
		++PaintedBoxes;
	}
	return LayerId + 1;
}
