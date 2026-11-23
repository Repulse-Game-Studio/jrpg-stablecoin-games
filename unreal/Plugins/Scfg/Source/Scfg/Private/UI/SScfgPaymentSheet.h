// Copyright 2026 Repulse
// Twin of godot/addons/scfg/ui/sheet.gd (itself the `scfg_payment` screen of plugin/game/scfg/scfg_screens.rpy): the
// payment sheet FScfgDefaultSheetHost shows for UScfgSubsystem::Pay.
//
// One step at a time, as the controller's step says: amount (tips) -> network -> method -> browser | qr | manual ->
// waiting -> done, or network -> card -> waiting -> done through a network's "Pay with card" link. The network step
// shows the main networks as tiles and the rest behind "More networks" (the controller's NetworkLayout), with a
// "Pay with card" link under each tile that has one. Every step has the header (the back arrow while the controller
// can_go_back, the step dots, the logo, the title and the price), a scrolling body, the pinned forward button when the
// step has one, and the footer
// (the message, the status line and Cancel / Continue playing / Continue). Development builds also show the developer
// lines (DEV MODE and the controller's dev_warnings()); Shipping does not.
//
// Keys (SScfgScreen.h): each step focuses its primary control (the selected option, the forward button, the first
// Copy, Continue); Escape and the gamepad's back go back, or close as "cancel" on the first step (not once a
// transaction was seen, nor on waiting and done). The sheet closes with "cancel", "keep_playing" or "finish".
//
// Control keys (for tests and drivers, Godot's): back, title, price, forward, cancel, keep_playing, continue,
// message, status, logo; preset_<amount>, custom_amount, set_amount; network_<key>, card_<key>, more_networks;
// method_<name>; qr, wallet_app; copy_amount, copy_recipient, copy_token, tx_input, check; progress, explorer, reopen;
// copy_tx.
#pragma once

#include "CoreMinimal.h"

#include "ScfgCheckoutController.h"
#include "UI/SScfgScreen.h"

class SCFG_API SScfgPaymentSheet : public SScfgScreen
{
public:
	SLATE_BEGIN_ARGS(SScfgPaymentSheet) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const FScfgCheckoutController& InController,
		const TSharedRef<FScfgStyle>& InStyle);

	const FScfgCheckoutController& GetController() const { return *Controller; }
	// The step the body shows (the controller's step as of the last build).
	const FString& ShownStep() const { return Shown; }
	// The step dots shown: (index, total), or (0, 0) when there are none.
	TPair<int32, int32> Dots() const;
	// Show the developer lines (default: every build but Shipping).
	static bool bShowDevLines;

protected:
	virtual bool ReadyToShow() const override { return Controller.IsSet(); }
	virtual FString Signature() const override;
	virtual void BuildContent() override;
	virtual FString StepKey() const override;
	virtual FString PrimaryKey() const override;
	virtual void Sync() override;
	virtual void OnBack() override;

private:
	void BuildHeader();
	// The body of the step; (label, action) of its forward button, or an empty label.
	TPair<FString, TFunction<void()>> BuildStep(SVerticalBox& Body);
	void StepAmount(SVerticalBox& Body);
	void StepNetwork(SVerticalBox& Body);
	// Ren'Py's scfg_network_tiles: Keys as tiles in Columns columns (the last row padded so every tile is as wide), a
	// "Pay with card" link under each one in Card.
	void NetworkTiles(SVerticalBox& Body, const TArray<FString>& Keys, const TArray<FScfgChainOptionView>& Options,
		const TArray<FString>& Card, int32 Columns, int32& Row);
	void StepMethod(SVerticalBox& Body);
	void StepCard(SVerticalBox& Body);
	void StepBrowser(SVerticalBox& Body);
	void StepQr(SVerticalBox& Body);
	void StepManual(SVerticalBox& Body);
	void StepWaiting(SVerticalBox& Body);
	void StepDone(SVerticalBox& Body);
	void BuildFooter();

	// Ren'Py's helpers of the same names.
	FString TxWord() const;
	FString MethodLabel(const FString& Method) const;
	FString MethodHint(const FString& Method) const;
	static FString FeeLabel(const FString& Fee);
	FString StatusText() const;
	FString ProgressText() const;
	FString DoneTitle() const;
	FLinearColor StatusColor() const;
	FString AmountLine(bool bExact) const;
	FString ExactNote() const;
	static FString PlainAmount(const FString& Text);
	static FString Short(const FString& Address);
	bool Ended() const;

	TOptional<FScfgCheckoutController> Controller;
	FString Shown;
	TMap<FString, FString> Synced;  // field key -> the controller's value last copied into it
};
