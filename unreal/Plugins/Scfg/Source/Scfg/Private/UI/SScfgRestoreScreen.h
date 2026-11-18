// Copyright 2026 Repulse
// Twin of godot/addons/scfg/ui/restore.gd (itself the `scfg_restore` screen of plugin/game/scfg/scfg_screens.rpy):
// the restore screen FScfgDefaultSheetHost shows for UScfgSubsystem::Restore.
//
// By the restore controller's state: input / checking (the hash field and Check), choose (each payment the player
// must name, with its choices, then Restore), saving, done (what was restored); then any message and the purchases on
// this device, each with Copy hash. Close is pinned at the bottom and closes with "close"; so do Escape and the
// gamepad's back.
//
// Control keys: title, logo, tx_input, check, status, choice_<index>_<choice>, restore, message, copy_<n>, close.
#pragma once

#include "CoreMinimal.h"

#include "ScfgCheckoutController.h"
#include "UI/SScfgScreen.h"

class SCFG_API SScfgRestoreScreen : public SScfgScreen
{
public:
	SLATE_BEGIN_ARGS(SScfgRestoreScreen) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const FScfgRestoreController& InController,
		const TSharedRef<FScfgStyle>& InStyle);

	const FScfgRestoreController& GetController() const { return *Controller; }

protected:
	virtual bool ReadyToShow() const override { return Controller.IsSet(); }
	virtual FString Signature() const override;
	virtual void BuildContent() override;
	virtual FString StepKey() const override;
	virtual FString PrimaryKey() const override;
	virtual void Sync() override;
	virtual void OnBack() override;

private:
	void InputPart(SVerticalBox& Body);
	void ChoosePart(SVerticalBox& Body);
	FString ReceiptLine(const FScfgReceiptLine& Receipt) const;
	FString CandidateLine(const FScfgRestoreCandidateView& Candidate) const;
	FString ChoiceLabel(const FString& Choice) const;

	TOptional<FScfgRestoreController> Controller;
	TOptional<FString> Synced;  // the controller's tx_input last copied into the field
};
