// No Python twin: the query nodes of the SCFG API (godot/addons/scfg/scfg.gd's queries): Owns, HasTipped, TipTotal
// and Supported (pure), Receipts and Pending (impure: they copy every record), each on the world context's game instance's UScfgSubsystem. The calls
// with effects are the subsystem's own BlueprintCallable functions, and Pay / Restore its async nodes
// (ScfgPayAsyncAction.h).
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "ScfgTypes.h"

#include "ScfgBlueprintLibrary.generated.h"

class UScfgSubsystem;

UCLASS()
class SCFG_API UScfgBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// The SCFG subsystem of the world context's game instance, or null (no game instance).
	UFUNCTION(BlueprintPure, Category = "SCFG", meta = (WorldContext = "WorldContextObject"))
	static UScfgSubsystem* GetSubsystem(const UObject* WorldContextObject);

	// A verified receipt unlocks ItemId. Instant and offline. An unknown item is a developer error (logged; false).
	UFUNCTION(BlueprintPure, Category = "SCFG", meta = (WorldContext = "WorldContextObject", DisplayName = "Owns (SCFG)"))
	static bool Owns(const UObject* WorldContextObject, const FString& ItemId);
	// True once the player has tipped any amount.
	UFUNCTION(BlueprintPure, Category = "SCFG",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Has Tipped (SCFG)"))
	static bool HasTipped(const UObject* WorldContextObject);
	// The sum of the player's tips as a decimal string ("0", "2.50").
	UFUNCTION(BlueprintPure, Category = "SCFG",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Tip Total (SCFG)"))
	static FString TipTotal(const UObject* WorldContextObject);
	// The stored receipts, by receipt key: a copy of every receipt, so an impure node (call it once and keep the map;
	// a pure node would copy them again for every use of its output).
	UFUNCTION(BlueprintCallable, Category = "SCFG",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Receipts (SCFG)"))
	static TMap<FString, FScfgReceipt> Receipts(const UObject* WorldContextObject);
	// The payments still being watched for, by session id (a copy: impure, as Receipts).
	UFUNCTION(BlueprintCallable, Category = "SCFG",
		meta = (WorldContext = "WorldContextObject", DisplayName = "Pending (SCFG)"))
	static TMap<FString, FScfgPendingPayment> Pending(const UObject* WorldContextObject);
	// True where payments can run: every platform the plugin builds for.
	UFUNCTION(BlueprintPure, Category = "SCFG", meta = (DisplayName = "Supported (SCFG)"))
	static bool Supported();

	// A node without a game instance logs one error per node kind per session (a UMG binding is evaluated every
	// frame); the tests start each check afresh.
	static void ResetLogOnceForTests();
};
