// No Python twin: an object the Blueprint-layer tests bind the subsystem's dynamic delegates and the async nodes'
// output pins to (a dynamic delegate needs a UFUNCTION).
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ScfgTypes.h"

#include "ScfgTestReceiver.generated.h"

UCLASS()
class UScfgTestReceiver : public UObject
{
	GENERATED_BODY()

public:
	TArray<FScfgPaymentResult> Paid;
	TArray<FScfgPaymentResult> Resolved;
	TArray<FScfgRestoreResult> Restored;
	int32 ReceiptsChanged = 0;

	UFUNCTION()
	void OnPaid(const FScfgPaymentResult& Result) { Paid.Add(Result); }
	UFUNCTION()
	void OnResolved(const FScfgPaymentResult& Result) { Resolved.Add(Result); }
	UFUNCTION()
	void OnRestored(const FScfgRestoreResult& Result) { Restored.Add(Result); }
	UFUNCTION()
	void OnReceipts() { ++ReceiptsChanged; }
};
