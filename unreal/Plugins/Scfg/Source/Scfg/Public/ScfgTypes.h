// Copyright 2026 Repulse
// No Python twin: the Blueprint and C++ value types of the SCFG API (UScfgSubsystem), the twins of Godot's
// configure() / add_chain() / add_item() / pay() option dictionaries and of its ScfgPaymentResult and ScfgRestoreResult
// (godot/addons/scfg/payment_result.gd, restore_result.gd), and of Unity's ScfgOptions, ScfgItem, ScfgPayRequest.
//
// The option structs convert to the core's keyword arguments (a Json dict, plugin-internal), so the core's own
// validation runs on them and its ConfigError codes come back unchanged. A text field left empty is None (Python's
// default) where the core has a default; an empty list or map is "not given". Prices and amounts are strings ("4.99"),
// never floats.
//
// A result field that the core leaves None is an empty string here (or 0). The option structs are what a game fills
// in (Blueprint can make and set them); the result and record structs (FScfgPaymentResult, FScfgReceipt,
// FScfgRestoreResult, FScfgPendingPayment) are what the plugin gives back, read-only in Blueprint and in the editor.
//
// The field names follow the core's keyword arguments: Methods.Browser is configure()'s methods.browser,
// Methods.Browser.TimeoutMinutes its timeout_min, Onramp its onramp, FScfgChain build_chain's arguments.
#pragma once

#include "CoreMinimal.h"

#include "ScfgTypes.generated.h"

// Whether a payment is an unlock (a paid item) or a tip.
UENUM(BlueprintType)
enum class EScfgPayKind : uint8
{
	// A paid item: Pay needs the item's id.
	Unlock,
	// A tip: Pay needs the amount and the token.
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
	// The restore added receipts (Restored lists them).
	Restored,
	// The transaction holds no payment to this game that is not already stored.
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

	// The https:// urls, tried in order.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Urls;
};

// The checkout page in the player's browser, where a browser wallet pays (desktop only).
USTRUCT(BlueprintType)
struct FScfgBrowserMethod
{
	GENERATED_BODY()

	// Offer the checkout page on the payment sheet.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bEnabled = true;
	// Minutes the page keeps answering a payment the player kept playing through.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int32 TimeoutMinutes = 30;
};

// A QR code the player scans with a phone wallet.
USTRUCT(BlueprintType)
struct FScfgQrMethod
{
	GENERATED_BODY()

	// Offer the QR code on the payment sheet.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bEnabled = true;
	// On a phone, an "Open wallet app" button below the code, which opens the payment in a wallet on the same device.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bWalletButton = true;
	// The chains that offer the QR code; empty means every chain.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Chains;
};

// The address and amount for the player to send by hand, and a field to paste the transaction hash.
USTRUCT(BlueprintType)
struct FScfgManualMethod
{
	GENERATED_BODY()

	// Offer the manual transfer on the payment sheet.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bEnabled = true;
	// Let the player paste the hash of the transaction they sent, so it is checked at once.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bAllowTxPaste = true;
};

// The ways the player can pay on the payment sheet; all three are on by default.
USTRUCT(BlueprintType)
struct FScfgMethods
{
	GENERATED_BODY()

	// The checkout page in the player's browser (desktop only).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgBrowserMethod Browser;
	// A QR code for a phone wallet.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgQrMethod Qr;
	// The address and amount to send by hand.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgManualMethod Manual;
};

// The "No USDC yet? Buy some with a card" link on the payment sheet. By default it is on, opens thirdweb's buy page,
// and is offered on the built-in EVM networks of a mainnet game (none on testnet, where cards buy no test tokens).
USTRUCT(BlueprintType)
struct FScfgOnramp
{
	GENERATED_BODY()

	// Show the link; off hides it everywhere.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bEnabled = true;
	// Your own buy page, an https:// template with {chain}, {chain_id}, {token}, {token_address} and {amount}; empty:
	// thirdweb's buy page.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Url;
	// The EVM chain keys it is offered on (each one of Chains); empty: the built-in EVM chains of a mainnet game.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Chains;
};

// Configure()'s options: the payout wallets, the network and tokens, the payment methods and the sheet's look (custom
// chains come from AddChain()).
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
	// Use the test networks (Sepolia, Base Sepolia, Solana Devnet, ...), where no real money moves; off: the mainnets.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	bool bTestnet = false;
	// The chain keys players may pay on; empty: every chain of the network.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Chains;
	// "USDC" and/or "USDT"; empty: both.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Tokens;
	// The ways the player can pay (browser page, QR code, manual transfer).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgMethods Methods;
	// The "No USDC yet? Buy some with a card" link.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgOnramp Onramp;
	// Your own RPC urls per chain key, tried before the public ones.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TMap<FString, FScfgRpcUrls> RpcUrls;
	// Confirmation overrides per chain key.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TMap<FString, int32> Confirmations;
	// Wallets earlier versions of the game were paid to (their receipts stay valid).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> PreviousWallets;
	// An image in the project, relative to the project directory ("Content/Scfg/logo.png"): .png, .jpg or .webp
	// (the in-game sheet draws only a png or jpg).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Logo;
	// "#rrggbb" for selected options; empty: the default.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Accent;
};

// AddChain()'s options: an EVM chain that isn't built in.
USTRUCT(BlueprintType)
struct FScfgChain
{
	GENERATED_BODY()

	// The name the payment sheet shows ("My L2").
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Name;
	// The chain's EIP-155 id, which wallets check before they send.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int64 ChainId = 0;
	// https urls.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TArray<FString> Rpc;
	// Token symbol -> contract address ("USDC" -> "0x..."); 6-decimal tokens only.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	TMap<FString, FString> Tokens;
	// The blocks a payment waits for before it counts (1 or more: it must be set).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int32 Confirmations = 0;
	// An https url with "{}" where the transaction hash goes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString ExplorerTx;
	// The chain's gas token, which the sheet tells the player to hold for the fee.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString NativeSymbol = TEXT("ETH");
	// "lowest", "low" or "high".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString FeeHint = TEXT("low");
	// The most blocks one eth_getLogs request may span on the chain's RPC endpoints.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int64 MaxLogRange = 1000;
	// A test network: offered only when Configure()'s bTestnet is on (and a mainnet only when it is off).
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
	// What the player sees on the payment sheet; empty: the item's id.
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

	// The item's id, for an unlock.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Item;
	// The amount as a decimal string ("4.99"): a tip's suggested amount, or an unlock's when it differs from the price
	// (pay what you want); empty: the item's price.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Amount;
	// "USDC" or "USDT"; empty: the item's token.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Token;
};

// What Pay() gives the game (the core's PaymentResult).
USTRUCT(BlueprintType)
struct FScfgPaymentResult
{
	GENERATED_BODY()

	// How the payment ended.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	EScfgPaymentStatus Status = EScfgPaymentStatus::Failed;
	// An unlock or a tip, as Pay was asked.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	EScfgPayKind Kind = EScfgPayKind::Unlock;
	// The unlocked item's id; empty for a tip.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Item;
	// The amount actually paid ("4.993742"); empty when nothing was matched on chain.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Amount;
	// "USDC" or "USDT".
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Token;
	// The key of the chain the payment was made on ("base"); empty when none was chosen.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Chain;
	// The payment's transaction hash; empty when nothing was matched on chain.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString TxHash;
	// The address the payment came from; empty when nothing was matched on chain.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Payer;
	// The payment session's id (the key of Pending() while it is watched); empty when no session started.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString SessionId;
	// Empty, a ConfigError code for a misused call ("item.unknown"), an Err kind ("RuntimeError"), or the core's
	// reason for a failed payment.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Error;
};

// A stored receipt: a verified payment on this device.
USTRUCT(BlueprintType)
struct FScfgReceipt
{
	GENERATED_BODY()

	// The key of the chain the payment was made on ("base").
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Chain;
	// The payment's transaction hash.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString TxHash;
	// Which transfer of the transaction it is (the EVM log index, the Solana token account's index): Chain, TxHash
	// and LogIndex identify the receipt.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	int64 LogIndex = 0;
	// The block (EVM) or slot (Solana) the transaction is in.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	int64 Block = 0;
	// An unlock or a tip.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	EScfgPayKind Kind = EScfgPayKind::Unlock;
	// The unlocked item's id; empty for a tip.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Item;
	// The amount paid, as a decimal string ("4.993742").
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Amount;
	// "USDC" or "USDT".
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Token;
	// The address the payment came from.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Payer;
	// The wallet it was paid to (the configured wallet, or one of PreviousWallets).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Recipient;
	// "payment" or "restore".
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Source;
};

// What Restore() gives the game (the core's RestoreResult).
USTRUCT(BlueprintType)
struct FScfgRestoreResult
{
	GENERATED_BODY()

	// How the restore ended.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	EScfgRestoreStatus Status = EScfgRestoreStatus::Cancelled;
	// The receipts the restore added.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	TArray<FScfgReceipt> Restored;
	// Empty, the core's code for a lookup that failed ("tx_not_found"), or a ConfigError code for a misused call.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Error;
};

// A payment still being watched for (a pending session record).
USTRUCT(BlueprintType)
struct FScfgPendingPayment
{
	GENERATED_BODY()

	// The payment session's id (its key in Pending(), and FScfgPaymentResult::SessionId).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString SessionId;
	// An unlock or a tip.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	EScfgPayKind Kind = EScfgPayKind::Unlock;
	// The item's id; empty for a tip.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Item;
	// The title the payment sheet showed.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Title;
	// "USDC" or "USDT".
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Token;
	// The key of the chain it is watched on ("base").
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Chain;
	// The amount asked, as a decimal string, before the item code and nonce are added to it.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString Price;
	// "awaiting" or "confirming".
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString State;
	// "open", "pending", "cancelled" or "expired".
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString UiStatus;
	// The transaction the player reported for it, if any.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	FString HintTx;
	// Unix seconds.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
	double CreatedAt = 0;
	// The last block scanned.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SCFG")
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
