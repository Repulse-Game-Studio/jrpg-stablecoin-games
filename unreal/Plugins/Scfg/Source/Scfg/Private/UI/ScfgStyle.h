// Twin of godot/addons/scfg/ui/theme.gd (the "Margin" look, itself Ren'Py's scfg_* styles in scfg_screens.rpy): the
// screens' FSlateStyleSet. A light sheet, ink text, outlined buttons and options with rounded corners, and the
// developer's accent (configure's "#rrggbb") for the selected option, the pinned forward button, the focus ring and
// the progress; the text on the accent is the core's config::text_on. Fonts are the engine's default
// (FCoreStyle::GetDefaultFontStyle): Regular, Bold and Mono. The logo brush is the configured logo file (PNG, JPEG,
// whatever the engine's ImageWrapper reads), decoded once; without a renderer (-nullrhi) only its size is known.
//
// Sizes are Godot's design pixels for a 1280x720 game, in Slate units: the game viewport's DPI scale (Project
// Settings -> User Interface) grows them with the screen, as Godot's scale_for() does. Font sizes are Godot's pixel
// sizes times 0.75 (Slate's sizes are points at 96 DPI).
//
// The set is not registered in FSlateStyleRegistry: every screen makes its own (Create) and keeps it while it is
// shown, so the accent and the logo are the ones configured when it opened, and nothing global is left behind.
//
// Customisation: an Override style (FScfgDefaultSheetHost::SetStyleOverride) wins for every widget style and brush it
// defines under the same name (Godot merges the developer's Theme the same way); the override itself is not changed.
//
// Names. Text (FTextBlockStyle): Scfg.Text, .Title, .Heading, .Amount, .AmountToken, .Note, .Label, .Muted, .Warning,
// .Dev, .Error, .Ok, .Status, .Mono, .QrStep, .OptionText, .OptionSub, .OptionText.Selected, .OptionSub.Selected,
// .ToastText, .ButtonText, .ButtonText.Small, .ButtonText.Primary, .ButtonText.Accent. Buttons (FButtonStyle):
// Scfg.Button, .Button.Small, .Button.Primary, .Button.Accent, .Button.Back, .Option, .Option.Selected. Field
// (FEditableTextBoxStyle): Scfg.Field. Progress (FProgressBarStyle): Scfg.Progress. Brushes: Scfg.Dim, .Sheet,
// .QrFrame, .Toast, .Rule, .Focus, .NoFocus, .Dot, .White, .Black, .Logo (only with a logo).
#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateTypes.h"

struct FSlateDynamicImageBrush;

class SCFG_API FScfgStyle final : public FSlateStyleSet
{
public:
	// The palette (theme.gd's constants).
	static const FLinearColor SheetColor, InkColor, InkHoverColor, MutedColor, FaintColor, LineColor, RuleColor,
		HoverColor, DimColor, OkColor, WarnColor, ErrorColor, WhiteColor;
	// The docked sheet's width, the step dots' and the status dot's size, the logo's box (design pixels).
	static constexpr float SheetWidth = 520.f;
	static constexpr float DotSize = 10.f;
	static constexpr float LogoMaxWidth = 240.f;
	static constexpr float LogoMaxHeight = 64.f;

	// AccentHex: "#rrggbb" (anything else: the default accent). LogoPath: the logo's full path, or "".
	static TSharedRef<FScfgStyle> Create(
		const FString& AccentHex, const FString& LogoPath, TSharedPtr<const ISlateStyle> Override = nullptr);
	virtual ~FScfgStyle() override;

	// Godot's sheet_width(): 520 docked on the right, or the whole width on a portrait or narrow screen.
	static float SheetWidthFor(const FVector2D& Size);
	// "#rrggbb" as a linear colour (sRGB decoded); Fallback for anything else.
	static FLinearColor ParseHex(const FString& Hex, const FLinearColor& Fallback);

	FLinearColor Accent() const { return AccentColor; }
	FLinearColor OnAccent() const { return OnAccentColor; }
	// The logo's pixel size (zero without a logo or when it cannot be read), and its size fitted into
	// LogoMaxWidth x LogoMaxHeight.
	FVector2D LogoPixels() const { return LogoSize; }
	FVector2D LogoFitted() const;
	bool HasLogo() const { return LogoSize.X > 0 && LogoSize.Y > 0; }

	// The named styles, the override's first (see Customisation).
	const FTextBlockStyle& Text(FName Name) const;
	const FButtonStyle& Button(FName Name) const;
	const FEditableTextBoxStyle& Field() const;
	const FProgressBarStyle& Progress() const;
	const FSlateBrush* Brush(FName Name) const;

private:
	FScfgStyle();
	void Build(const FString& AccentHex);
	void LoadLogo(const FString& LogoPath);

	FLinearColor AccentColor;
	FLinearColor OnAccentColor;
	FVector2D LogoSize = FVector2D::ZeroVector;
	TSharedPtr<FSlateDynamicImageBrush> LogoBrush;
	TSharedPtr<const ISlateStyle> Override;
};
