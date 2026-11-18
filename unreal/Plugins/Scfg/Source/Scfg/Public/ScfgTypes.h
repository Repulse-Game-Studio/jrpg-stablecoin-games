// Copyright 2026 Repulse
// No Python twin: the Blueprint and C++ value types of the SCFG API (UScfgSubsystem), the twins of Godot's
// configure() / add_chain() / add_item() / pay() option dictionaries and of its ScfgPaymentResult and ScfgRestoreResult
// (godot/addons/scfg/payment_result.gd, restore_result.gd), and of Unity's ScfgOptions, ScfgItem, ScfgPayRequest.
//
// The option structs convert to the core's keyword arguments (a Json dict, Private/ScfgConvert.h), so the core's own
// validation runs on them and its ConfigError codes come back unchanged. A text field left empty is None (Python's
// default) where the core has a default; an empty list or map is "not given". Prices and amounts are strings ("4.99"),
// never floats.
//
// A result field that the core leaves None is an empty string here (or 0).
#pragma once

#include "CoreMinimal.h"

#include "ScfgTypes.generated.h"

// pay()'s kind.
UENUM(BlueprintType)
enum class EScfgPayKind : uint8
{
	Unlock,
	Tip,
};

// A payment's outcome (the core's PaymentResult.status).
UENUM(BlueprintType)
enum class EScfgPaymentStatus : uint8
{
	// The payment confirmed.
	Paid,
	// An unlock the player already owns: no sheet was shown.
	AlreadyOwned,
	// The player closed the sheet without paying.
	Cancelled,
	// The payment window closed before a payment was found (it is still watched for 24 h).
	Expired,
	// A payment arrived but for less than the price.
	Underpaid,
	// A misused call (logged once; Error holds the ConfigError code), or payments could not start.
	Failed,
	// The player chose to keep playing while the payment confirms: the final result comes later (the callback, else
	// OnPaymentResolved).
	Pending,
};

// A restore's outcome (the core's RestoreResult.status).
UENUM(BlueprintType)
enum class EScfgRestoreStatus : uint8
{
	Restored,
	NothingFound,
	// Closed before a restore finished.
	Cancelled,
	// A misused call (logged once; Error holds the code).
	Failed,
};

// One chain's RPC urls (a map value: Unreal maps cannot hold arrays directly).
USTRUCT(BlueprintType)
struct FScfgRpcUrls
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Urls;
};

// methods.browser: the checkout page in the player's browser (desktop).
USTRUCT(BlueprintType)
struct FScfgBrowserMethod
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bEnabled = true;
	// Minutes the page keeps answering a payment the player kept playing through.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int32 TimeoutMin = 30;
};

// methods.qr: a QR code for a phone wallet.
USTRUCT(BlueprintType)
struct FScfgQrMethod
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bEnabled = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bWalletButton = true;
	// The chains that offer it; empty: every chain (None).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Chains;
};

// methods.manual: the address and amount to send by hand, and a field for the transaction hash.
USTRUCT(BlueprintType)
struct FScfgManualMethod
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bEnabled = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bAllowTxPaste = true;
};

// configure()'s methods; the defaults are the core's DEFAULT_METHODS.
USTRUCT(BlueprintType)
struct FScfgMethods
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgBrowserMethod Browser;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgQrMethod Qr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgManualMethod Manual;
};

// Configure()'s options: the payout wallet and the rest of build_config's keyword arguments (custom chains come from
// AddChain()).
USTRUCT(BlueprintType)
struct FScfgOptions
{
	GENERATED_BODY()

	// The EVM payout address ("0x..."); empty: none (a Solana-only game).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Wallet;
	// The Solana payout address; empty: none.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString SolanaWallet;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bTestnet = false;
	// The chain keys players may pay on; empty: every chain of the network.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Chains;
	// "USDC" and/or "USDT"; empty: both.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Tokens;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgMethods Methods;
	// Your own RPC urls per chain key, tried before the public ones.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TMap<FString, FScfgRpcUrls> RpcUrls;
	// Confirmation overrides per chain key.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TMap<FString, int32> Confirmations;
	// Wallets earlier versions of the game were paid to (their receipts stay valid).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> PreviousWallets;
	// An image in the project, relative to the project directory ("Content/Scfg/logo.png"): .png, .jpg or .webp.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Logo;
	// "#rrggbb" for selected options; empty: the default.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Accent;
};

// AddChain()'s options: an EVM chain that isn't built in (build_chain's keyword arguments).
USTRUCT(BlueprintType)
struct FScfgChain
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int64 ChainId = 0;
	// https urls.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Rpc;
	// Token symbol -> contract address ("USDC" -> "0x..."); 6-decimal tokens only.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TMap<FString, FString> Tokens;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int32 Confirmations = 0;
	// An https url with "{}" where the transaction hash goes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString ExplorerTx;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString NativeSymbol = TEXT("ETH");
	// "lowest", "low" or "high".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString FeeHint = TEXT("low");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int64 MaxLogRange = 1000;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bTestnet = false;
};

// AddItem()'s options: a paid unlock.
USTRUCT(BlueprintType)
struct FScfgItem
{
	GENERATED_BODY()

	// 1-99. Never change it after release: receipts are matched by it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int32 Code = 0;
	// "4.99".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Price;
	// "USDC" or "USDT".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Token;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Title;
	// The least a payment must be to unlock it (a discount you may give later); empty: the price.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString MinPrice;
};

// Pay()'s options: an unlock needs Item (Amount and Token default to the item's); a tip needs Amount (the suggested
// amount, which the player may change) and Token.
USTRUCT(BlueprintType)
struct FScfgPayRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Item;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Amount;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Token;
};

// What Pay() gives the game (the core's PaymentResult).
USTRUCT(BlueprintType)
struct FScfgPaymentResult
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	EScfgPaymentStatus Status = EScfgPaymentStatus::Failed;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	EScfgPayKind Kind = EScfgPayKind::Unlock;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Item;
	// The amount actually paid ("4.993742"); empty when nothing was matched on chain.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Amount;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Token;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Chain;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString TxHash;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Payer;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString SessionId;
	// Empty, a ConfigError code for a misused call ("item.unknown"), an Err kind ("RuntimeError"), or the core's
	// reason for a failed payment.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Error;
};

// A stored receipt: a verified payment on this device.
USTRUCT(BlueprintType)
struct FScfgReceipt
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Chain;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString TxHash;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int64 LogIndex = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int64 Block = 0;
	// "unlock" or "tip".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Kind;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Item;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Amount;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Token;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Payer;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Recipient;
	// "payment" or "restore".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Source;
};

// What Restore() gives the game (the core's RestoreResult).
USTRUCT(BlueprintType)
struct FScfgRestoreResult
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	EScfgRestoreStatus Status = EScfgRestoreStatus::Cancelled;
	// The receipts the restore added.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FScfgReceipt> Restored;
	// Empty, the core's code for a lookup that failed ("tx_not_found"), or a ConfigError code for a misused call.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Error;
};

// A payment still being watched for (a pending session record).
USTRUCT(BlueprintType)
struct FScfgPendingPayment
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Id;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Kind;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Item;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Title;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Token;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Chain;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Price;
	// "awaiting" or "confirming".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString State;
	// "open", "pending", "cancelled" or "expired".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString UiStatus;
	// The transaction the player reported for it, if any.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString HintTx;
	// Unix seconds.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	double CreatedAt = 0;
	// The last block scanned.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int64 Cursor = 0;
};

// Pay()'s callback: this call's own result when it is AlreadyOwned, Failed or Cancelled, and every later result of
// the payment (Paid, Underpaid, Expired), also one confirmed after the player kept playing. Handlers must be
// idempotent.
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnScfgPaid, const FScfgPaymentResult&, Result);
// Restore()'s callback: the result once a restore finishes, while the screen is still open.
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnScfgRestored, const FScfgRestoreResult&, Result);
// A final result of a payment that has no Pay() callback (a cancel too).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnScfgPaymentResolved, const FScfgPaymentResult&, Result);
// The stored receipts changed (a payment confirmed, a restore, ClearLocalData()).
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnScfgReceiptsChanged);
// The Restore async node's output pin.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnScfgRestoreResolved, const FScfgRestoreResult&, Result);
