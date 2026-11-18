// Copyright 2026 Repulse
// Twin of godot/addons/scfg/ui/theme.gd (see ScfgStyle.h).
#include "UI/ScfgStyle.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Styling/CoreStyle.h"

#include "ScfgString.h"

#include "Scfg/Config.h"

namespace ScfgStyleDetail
{

static FLinearColor Hex(const TCHAR* Text)
{
	return FLinearColor(FColor::FromHex(Text));
}

// Godot's pixel sizes as Slate points (see ScfgStyle.h).
static FSlateFontInfo Font(const TCHAR* Typeface, float Pixels)
{
	return FCoreStyle::GetDefaultFontStyle(Typeface, FMath::Max(1, FMath::RoundToInt(Pixels * 0.75f)));
}

static FTextBlockStyle TextStyle(float Pixels, const FLinearColor& Color, bool bBold)
{
	return FTextBlockStyle()
		.SetFont(Font(bBold ? TEXT("Bold") : TEXT("Regular"), Pixels))
		.SetColorAndOpacity(FSlateColor(Color))
		.SetShadowOffset(FVector2D::ZeroVector)
		.SetShadowColorAndOpacity(FLinearColor::Transparent);
}

static constexpr float Radius = 10.f;
static constexpr float LineWidth = 1.5f;

static FSlateRoundedBoxBrush Box(const FLinearColor& Fill, const FLinearColor& Outline, float Width)
{
	return FSlateRoundedBoxBrush(Fill, Radius, Outline, Width);
}

static FButtonStyle ButtonStyle(const FSlateBrush& Normal, const FSlateBrush& Hovered, const FSlateBrush& Pressed,
	const FSlateBrush& Disabled, const FMargin& Padding)
{
	return FButtonStyle()
		.SetNormal(Normal)
		.SetHovered(Hovered)
		.SetPressed(Pressed)
		.SetDisabled(Disabled)
		.SetNormalPadding(Padding)
		.SetPressedPadding(Padding)
		.SetNormalForeground(FSlateColor(FScfgStyle::InkColor))
		.SetHoveredForeground(FSlateColor(FScfgStyle::InkColor))
		.SetPressedForeground(FSlateColor(FScfgStyle::InkColor))
		.SetDisabledForeground(FSlateColor(FScfgStyle::FaintColor));
}

// theme.gd's _outlined(): clear inside, a line around it, the hover and press tints.
static FButtonStyle Outlined(const FMargin& Padding)
{
	return ButtonStyle(Box(FLinearColor::Transparent, FScfgStyle::LineColor, LineWidth),
		Box(FScfgStyle::HoverColor, FScfgStyle::InkColor, LineWidth), Box(FScfgStyle::HoverColor, FScfgStyle::InkColor, LineWidth),
		Box(FLinearColor::Transparent, FScfgStyle::RuleColor, LineWidth), Padding);
}

static FButtonStyle Filled(const FLinearColor& Fill, const FLinearColor& Hover, const FLinearColor& Press, const FMargin& Padding)
{
	return ButtonStyle(Box(Fill, Fill, 0.f), Box(Hover, Hover, 0.f), Box(Press, Press, 0.f),
		Box(FScfgStyle::LineColor, FScfgStyle::LineColor, 0.f), Padding);
}

static bool IsHexColor(const FString& Text)
{
	if (Text.Len() != 7 || Text[0] != TEXT('#'))
	{
		return false;
	}
	for (int32 I = 1; I < 7; ++I)
	{
		if (!FChar::IsHexDigit(Text[I]))
		{
			return false;
		}
	}
	return true;
}

static int32 NextLogoId = 0;

}  // namespace ScfgStyleDetail

const FLinearColor FScfgStyle::SheetColor = ScfgStyleDetail::Hex(TEXT("#fbfcfd"));
const FLinearColor FScfgStyle::InkColor = ScfgStyleDetail::Hex(TEXT("#17202e"));
const FLinearColor FScfgStyle::InkHoverColor = ScfgStyleDetail::Hex(TEXT("#2c3a50"));
const FLinearColor FScfgStyle::MutedColor = ScfgStyleDetail::Hex(TEXT("#5b6676"));
const FLinearColor FScfgStyle::FaintColor = ScfgStyleDetail::Hex(TEXT("#a3abb6"));
const FLinearColor FScfgStyle::LineColor = ScfgStyleDetail::Hex(TEXT("#cfd5dd"));
const FLinearColor FScfgStyle::RuleColor = ScfgStyleDetail::Hex(TEXT("#e4e8ed"));
const FLinearColor FScfgStyle::HoverColor = ScfgStyleDetail::Hex(TEXT("#eef1f4"));
const FLinearColor FScfgStyle::DimColor = ScfgStyleDetail::Hex(TEXT("#0a0c1866"));
const FLinearColor FScfgStyle::OkColor = ScfgStyleDetail::Hex(TEXT("#17804f"));
const FLinearColor FScfgStyle::WarnColor = ScfgStyleDetail::Hex(TEXT("#9a5b00"));
const FLinearColor FScfgStyle::ErrorColor = ScfgStyleDetail::Hex(TEXT("#c0392b"));
const FLinearColor FScfgStyle::WhiteColor = FLinearColor::White;

FScfgStyle::FScfgStyle() : FSlateStyleSet(TEXT("ScfgStyle")) {}

FScfgStyle::~FScfgStyle()
{
	LogoBrush.Reset();
}

TSharedRef<FScfgStyle> FScfgStyle::Create(
	const FString& AccentHex, const FString& LogoPath, TSharedPtr<const ISlateStyle> InOverride)
{
	const TSharedRef<FScfgStyle> Style = MakeShareable(new FScfgStyle());
	Style->Override = MoveTemp(InOverride);
	Style->Build(AccentHex);
	if (!LogoPath.IsEmpty())
	{
		Style->LoadLogo(LogoPath);
	}
	return Style;
}

float FScfgStyle::SheetWidthFor(const FVector2D& Size)
{
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return SheetWidth;
	}
	return (Size.X < Size.Y || SheetWidth * 1.6f > Size.X) ? float(Size.X) : SheetWidth;
}

FLinearColor FScfgStyle::ParseHex(const FString& Hex, const FLinearColor& Fallback)
{
	return ScfgStyleDetail::IsHexColor(Hex) ? FLinearColor(FColor::FromHex(Hex)) : Fallback;
}

FVector2D FScfgStyle::LogoFitted() const
{
	if (!HasLogo())
	{
		return FVector2D::ZeroVector;
	}
	const double Fit = FMath::Min(LogoMaxWidth / FMath::Max(1.0, LogoSize.X), LogoMaxHeight / FMath::Max(1.0, LogoSize.Y));
	return FVector2D(LogoSize.X * Fit, LogoSize.Y * Fit);
}

void FScfgStyle::Build(const FString& AccentHex)
{
	using namespace ScfgStyleDetail;
	const FString DefaultAccent = ScfgString::FromUtf8(scfg::config::DEFAULT_ACCENT);
	const FString Accent = IsHexColor(AccentHex) ? AccentHex : DefaultAccent;
	AccentColor = FLinearColor(FColor::FromHex(Accent));
	const scfg::Result<std::string> On = scfg::config::text_on(ScfgString::ToUtf8(Accent));
	OnAccentColor = On.ok() ? FLinearColor(FColor::FromHex(ScfgString::FromUtf8(On.value()))) : WhiteColor;

	// text (theme.gd's label variations)
	struct FText
	{
		const TCHAR* Name;
		float Pixels;
		FLinearColor Color;
		bool bBold;
	};
	const FText Texts[] = {
		{TEXT("Scfg.Text"), 18, InkColor, false},
		{TEXT("Scfg.Title"), 26, InkColor, true},
		{TEXT("Scfg.Heading"), 21, InkColor, true},
		{TEXT("Scfg.Amount"), 40, InkColor, true},
		{TEXT("Scfg.AmountToken"), 22, MutedColor, false},
		{TEXT("Scfg.Note"), 14, MutedColor, false},
		{TEXT("Scfg.Label"), 15, MutedColor, false},
		{TEXT("Scfg.Muted"), 16, MutedColor, false},
		{TEXT("Scfg.Warning"), 16, WarnColor, false},
		{TEXT("Scfg.Dev"), 14, WarnColor, false},
		{TEXT("Scfg.Error"), 16, ErrorColor, false},
		{TEXT("Scfg.Ok"), 18, OkColor, false},
		{TEXT("Scfg.Status"), 16, InkColor, false},
		{TEXT("Scfg.QrStep"), 16, InkColor, false},
		{TEXT("Scfg.OptionText"), 15, InkColor, false},
		{TEXT("Scfg.OptionSub"), 12, MutedColor, false},
		{TEXT("Scfg.OptionText.Selected"), 15, OnAccentColor, false},
		{TEXT("Scfg.OptionSub.Selected"), 12, OnAccentColor, false},
		{TEXT("Scfg.ToastText"), 16, WhiteColor, false},
		{TEXT("Scfg.ButtonText"), 17, InkColor, true},
		{TEXT("Scfg.ButtonText.Small"), 15, InkColor, true},
		{TEXT("Scfg.ButtonText.Primary"), 17, WhiteColor, true},
		{TEXT("Scfg.ButtonText.Accent"), 17, OnAccentColor, true},
	};
	for (const FText& T : Texts)
	{
		Set(T.Name, TextStyle(T.Pixels, T.Color, T.bBold));
	}
	Set(TEXT("Scfg.Mono"), FTextBlockStyle(TextStyle(14, InkColor, false)).SetFont(Font(TEXT("Mono"), 14)));

	// buttons and options
	Set(TEXT("Scfg.Button"), Outlined(FMargin(16, 11)));
	Set(TEXT("Scfg.Button.Small"), Outlined(FMargin(10, 6)));
	Set(TEXT("Scfg.Button.Back"), Outlined(FMargin(0)));
	Set(TEXT("Scfg.Option"), Outlined(FMargin(6, 7)));
	Set(TEXT("Scfg.Button.Primary"), Filled(InkColor, InkHoverColor, InkHoverColor, FMargin(16, 11)));
	Set(TEXT("Scfg.Button.Accent"), Filled(AccentColor, AccentColor * 1.08f, AccentColor * 0.92f, FMargin(16, 11)));
	const FSlateRoundedBoxBrush Selected = Box(AccentColor, AccentColor, LineWidth);
	Set(TEXT("Scfg.Option.Selected"), ButtonStyle(Selected, Selected, Selected, Selected, FMargin(6, 7)));

	// the text field
	const FSlateRoundedBoxBrush FieldBox = Box(FLinearColor::Transparent, LineColor, LineWidth);
	Set(TEXT("Scfg.Field"), FEditableTextBoxStyle()
		.SetTextStyle(TextStyle(17, InkColor, false))
		.SetBackgroundImageNormal(FieldBox)
		.SetBackgroundImageHovered(Box(FLinearColor::Transparent, MutedColor, LineWidth))
		.SetBackgroundImageFocused(Box(FLinearColor::Transparent, AccentColor, 2.f))
		.SetBackgroundImageReadOnly(Box(FLinearColor::Transparent, RuleColor, LineWidth))
		.SetPadding(FMargin(12, 9))
		.SetForegroundColor(FSlateColor(InkColor))
		.SetBackgroundColor(FSlateColor(FLinearColor::White))
		.SetReadOnlyForegroundColor(FSlateColor(MutedColor))
		.SetFocusedForegroundColor(FSlateColor(InkColor)));

	// the progress bar
	Set(TEXT("Scfg.Progress"), FProgressBarStyle()
		.SetBackgroundImage(FSlateRoundedBoxBrush(RuleColor, 4.f))
		.SetFillImage(FSlateRoundedBoxBrush(AccentColor, 4.f))
		.SetMarqueeImage(FSlateRoundedBoxBrush(AccentColor, 4.f)));

	// brushes the screens draw with
	Set(TEXT("Scfg.Dim"), new FSlateColorBrush(DimColor));
	Set(TEXT("Scfg.Sheet"), new FSlateColorBrush(SheetColor));
	Set(TEXT("Scfg.Rule"), new FSlateColorBrush(RuleColor));
	Set(TEXT("Scfg.White"), new FSlateColorBrush(WhiteColor));
	Set(TEXT("Scfg.Black"), new FSlateColorBrush(FLinearColor::Black));
	Set(TEXT("Scfg.QrFrame"), new FSlateRoundedBoxBrush(WhiteColor, Radius, LineColor, LineWidth));
	Set(TEXT("Scfg.Toast"), new FSlateRoundedBoxBrush(InkColor, Radius));
	Set(TEXT("Scfg.Focus"), new FSlateRoundedBoxBrush(FLinearColor::Transparent, Radius + 2.f, AccentColor, 2.f));
	Set(TEXT("Scfg.NoFocus"), new FSlateNoResource());
	// a circle: the step dots and the status dot (tinted with ColorAndOpacity)
	Set(TEXT("Scfg.Dot"), new FSlateRoundedBoxBrush(FLinearColor::White, DotSize / 2.f, FVector2f(DotSize, DotSize)));
}

void FScfgStyle::LoadLogo(const FString& LogoPath)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *LogoPath, FILEREAD_Silent) || Bytes.IsEmpty())
	{
		return;
	}
	IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const EImageFormat Format = Images.DetectImageFormat(Bytes.GetData(), Bytes.Num());
	if (Format == EImageFormat::Invalid)
	{
		return;
	}
	const TSharedPtr<IImageWrapper> Wrapper = Images.CreateImageWrapper(Format);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
	{
		return;
	}
	LogoSize = FVector2D(Wrapper->GetWidth(), Wrapper->GetHeight());
	// A texture only where something renders: -nullrhi has no RHI for it.
	if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
	{
		return;
	}
	TArray64<uint8> Raw;
	if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw) || Raw.Num() > MAX_int32)
	{
		return;
	}
	TArray<uint8> Pixels(Raw.GetData(), int32(Raw.Num()));
	const FName Name(*FString::Printf(TEXT("ScfgLogo%d"), ++ScfgStyleDetail::NextLogoId));
	LogoBrush = FSlateDynamicImageBrush::CreateWithImageData(Name, LogoSize, Pixels);
}

const FTextBlockStyle& FScfgStyle::Text(FName Name) const
{
	if (Override.IsValid() && Override->HasWidgetStyle<FTextBlockStyle>(Name))
	{
		return Override->GetWidgetStyle<FTextBlockStyle>(Name);
	}
	return GetWidgetStyle<FTextBlockStyle>(Name);
}

const FButtonStyle& FScfgStyle::Button(FName Name) const
{
	if (Override.IsValid() && Override->HasWidgetStyle<FButtonStyle>(Name))
	{
		return Override->GetWidgetStyle<FButtonStyle>(Name);
	}
	return GetWidgetStyle<FButtonStyle>(Name);
}

const FEditableTextBoxStyle& FScfgStyle::Field() const
{
	static const FName Name(TEXT("Scfg.Field"));
	if (Override.IsValid() && Override->HasWidgetStyle<FEditableTextBoxStyle>(Name))
	{
		return Override->GetWidgetStyle<FEditableTextBoxStyle>(Name);
	}
	return GetWidgetStyle<FEditableTextBoxStyle>(Name);
}

const FProgressBarStyle& FScfgStyle::Progress() const
{
	static const FName Name(TEXT("Scfg.Progress"));
	if (Override.IsValid() && Override->HasWidgetStyle<FProgressBarStyle>(Name))
	{
		return Override->GetWidgetStyle<FProgressBarStyle>(Name);
	}
	return GetWidgetStyle<FProgressBarStyle>(Name);
}

const FSlateBrush* FScfgStyle::Brush(FName Name) const
{
	if (Override.IsValid())
	{
		if (const FSlateBrush* Theirs = Override->GetOptionalBrush(Name, nullptr, nullptr))
		{
			return Theirs;
		}
	}
	if (Name == TEXT("Scfg.Logo"))
	{
		return LogoBrush.Get();
	}
	return GetBrush(Name);
}
