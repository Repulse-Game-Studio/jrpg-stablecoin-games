// Copyright 2026 Repulse
// Twin of godot/addons/scfg/ui/qr_view.gd: a QR code drawn from the core's module runs (scfg::paylinks::qr_rects, the
// same geometry as Ren'Py's scfg.QR): black runs on white, with a quiet zone of BORDER modules around them, as solid
// boxes at the largest integer scale (at least 1) that fits both the target size and the space the layout gives it,
// so every module is a whole number of Slate units, stays sharp, and the code shrinks with a narrow sheet.
//
//     SNew(SScfgQrView).Text(Uri).TargetSize(176)      // GetError() is the Err of a text no QR version holds
#pragma once

#include "CoreMinimal.h"
#include "Brushes/SlateColorBrush.h"
#include "Widgets/SLeafWidget.h"

class SCFG_API SScfgQrView : public SLeafWidget
{
public:
	static constexpr int32 BORDER = 2;

	SLATE_BEGIN_ARGS(SScfgQrView) : _TargetSize(176.f) {}
		SLATE_ARGUMENT(FString, Text)
		SLATE_ARGUMENT(float, TargetSize)
	SLATE_END_ARGS()

	SScfgQrView();
	void Construct(const FArguments& InArgs);

	// Show the QR code of Text in TargetSize units. False (nothing shown, GetError() set) when no QR version can hold it.
	bool ShowText(const FString& Text, float InTargetSize);
	const FString& GetError() const { return Error; }
	// Modules per side, the quiet zone included (0: nothing shown).
	int32 Modules() const { return Size; }
	int32 NumRuns() const { return Runs.Num(); }
	float GetTargetSize() const { return TargetSize; }
	// Units per module in Available units: the largest integer that fits min(TargetSize, Available), at least 1.
	int32 ModuleScale(float Available) const;
	// The dark runs as rectangles at Scale units per module.
	TArray<FIntRect> DarkRects(int32 Scale) const;
	// What the last OnPaint drew: its scale and its dark boxes (the white background not counted).
	int32 LastScale() const { return PaintedScale; }
	int32 LastBoxes() const { return PaintedBoxes; }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	bool Load(const FString& Text, float InTargetSize);

	struct FRun
	{
		int32 X;
		int32 Y;
		int32 Width;
	};

	float TargetSize = 176.f;
	int32 Size = 0;
	TArray<FRun> Runs;
	FString Error;
	FSlateColorBrush White;
	FSlateColorBrush Black;
	mutable int32 PaintedScale = 0;
	mutable int32 PaintedBoxes = 0;
};
