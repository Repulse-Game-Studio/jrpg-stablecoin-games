// No Python twin: the Blueprint forms of `await Scfg.pay(...)` and `await Scfg.restore()` (godot/addons/scfg/scfg.gd):
// async nodes that show the screen and fire OnResolved once, when the player closes it (or at once: AlreadyOwned,
// Failed). They are UScfgSubsystem::PayAsync and RestoreAsync.
//
// A Pay node has no callback, so a cancel and every later result of the payment (the Paid result of a payment the
// player kept playing through) also reach the subsystem's OnPaymentResolved: a handler of both must be idempotent.
//
// When the game instance shuts down with the screen open, the call resolves without delivering anything: OnResolved
// does not fire (no Blueprint runs during the teardown), and the node is released.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"

#include "ScfgTypes.h"

#include "ScfgPayAsyncAction.generated.h"

class UScfgSubsystem;

UCLASS()
class SCFG_API UScfgPayAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	// Show the payment sheet; OnResolved fires once with this call's result.
	UFUNCTION(BlueprintCallable, Category = "SCFG",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Pay (SCFG)"))
	static UScfgPayAsyncAction* Pay(UObject* WorldContextObject, EScfgPayKind Kind, FScfgPayRequest Request);
	// The same on a given subsystem (C++ and the tests; Pay finds it from the world context's game instance).
	static UScfgPayAsyncAction* PayWith(UScfgSubsystem* Subsystem, EScfgPayKind Kind, const FScfgPayRequest& Request);

	UPROPERTY(BlueprintAssignable, Category = "SCFG")
	FOnScfgPaymentResolved OnResolved;

	virtual void Activate() override;

private:
	void Finish(const FScfgPaymentResult& Result);

	TWeakObjectPtr<UScfgSubsystem> Subsystem;
	EScfgPayKind Kind = EScfgPayKind::Unlock;
	FScfgPayRequest Request;
};

UCLASS()
class SCFG_API UScfgRestoreAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	// Show the restore screen; OnResolved fires once, when it is closed: Restored, NothingFound, Cancelled (closed
	// before a restore finished) or Failed.
	UFUNCTION(BlueprintCallable, Category = "SCFG",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Restore (SCFG)"))
	static UScfgRestoreAsyncAction* Restore(UObject* WorldContextObject);
	static UScfgRestoreAsyncAction* RestoreWith(UScfgSubsystem* Subsystem);

	UPROPERTY(BlueprintAssignable, Category = "SCFG")
	FOnScfgRestoreResolved OnResolved;

	virtual void Activate() override;

private:
	void Finish(const FScfgRestoreResult& Result);

	TWeakObjectPtr<UScfgSubsystem> Subsystem;
};
