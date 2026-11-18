// Copyright 2026 Repulse
// No Python twin: the C++ seam for a custom payment sheet or restore screen, the Unreal-typed view of the core's
// CheckoutController / RestoreController (Godot's ScfgCheckoutController, which a replacement sheet scene binds to;
// Unity's ScfgCheckoutController). The plugin's own Slate screens (SScfgPaymentSheet, SScfgRestoreScreen) are built
// on exactly this view, so a custom sheet sees what they see and can do what they do.
//
// HOW A CUSTOM SHEET BINDS (a C++ host; IScfgSheetHost.h has the contract):
//
//     class FMySheetHost : public IScfgSheetHost
//     {
//         TWeakObjectPtr<UMySheetWidget> MyWidget;   // weak: the host must not keep (or revive) a UObject
//
//         virtual TSharedPtr<FScfgScreen> OpenPayment(const TSharedRef<FScfgCheckout>& Checkout) override
//         {
//             TSharedRef<FScfgScreen> Screen = MakeShared<FScfgScreen>();
//             UMySheetWidget* Made = CreateWidget<UMySheetWidget>(PlayerController);
//             Made->Bind(FScfgCheckoutController(Checkout), Screen);   // reads it every frame, calls its actions
//             Made->AddToViewport(10000);                               // it calls Screen->Close("cancel")
//             MyWidget = Made;
//             return Screen;
//         }
//         // May run at game-instance shutdown, inside garbage collection: check the widget, create no UObject.
//         virtual void Close(const TSharedRef<FScfgScreen>& Screen) override
//         {
//             if (UMySheetWidget* Shown = MyWidget.Get()) { Shown->RemoveFromParent(); }
//         }
//     };
//     GetGameInstance()->GetSubsystem<UScfgSubsystem>()->SetSheetHost(MakeShared<FMySheetHost>());
//
// A UMG host can show the plugin's own sheet instead: UScfgSheetWidget (ScfgSheetWidget.h).
//
// Read it every frame (the flows poll the controller; a view never polls). The step names, states, methods and message
// ids are the core's (Checkout.h): steps amount, network, method, browser, qr, manual, waiting, done; states choose,
// starting, waiting, confirming, paid, underpaid, expired, closed; methods browser, qr, manual. Texts for the ids are
// ScfgTexts' English (the ids of conformance/vectors/messages.json). The actions are the buttons; an Err a core call
// returns (unreachable with the Engine's own results) is reported once ("scfg: screen: ...") and the action does
// nothing more.
//
// Not a UObject: Blueprint has no IScfgSheetHost to implement (it is C++, see IScfgSheetHost.h), so a Blueprint-only
// custom sheet is not offered; this seam is for C++ game code.
//
// Lifetime: a view holds its controller wrapper alive; after the sheet closed the controller is closed too, and the
// actions do nothing (the facade checks).
#pragma once

#include "CoreMinimal.h"

class FScfgCheckout;
class FScfgRestore;

// One network the payment can use (the core's chain_options()).
struct FScfgChainOptionView
{
	FString Key;
	FString Name;
	FString FeeHint;  // "low", "lowest", "high", or the chain's own word
};

// A receipt as the restore screen lists it.
struct FScfgReceiptLine
{
	FString Kind;  // "unlock" or "tip"
	FString Item;
	FString Amount;
	FString Token;
	FString Chain;
	FString TxHash;
};

// A payment the restore found that the player must name (the core's RestoreCandidate with kind None).
struct FScfgRestoreCandidateView
{
	FString Chain;
	FString Token;
	FString Amount;  // format_units of the transfer, six decimals ("5.000000")
	TArray<FString> Choices;  // item ids, then "tip"
};

class SCFG_API FScfgCheckoutController
{
public:
	explicit FScfgCheckoutController(const TSharedRef<FScfgCheckout>& InCheckout);

	// --- what the sheet shows ---

	FString GetId() const;
	FString State() const;
	FString Step() const;
	FString Method() const;      // "" before one is chosen
	FString ChainKey() const;    // "" before one is chosen
	FString Family() const;      // "evm", "solana", or ""
	FString ChainDisplay() const;  // the chosen chain's name, or ""
	FString Kind() const;        // "unlock" or "tip"
	FString Title() const;       // the item's title, or the tip's
	FString Token() const;
	FString PriceText() const;   // "4.99"
	FString PayAmountText() const;  // the exact amount with the code and nonce ("4.993742")
	FString Amount() const;      // the tip amount (a Dec string)
	TArray<FString> TipPresets() const;  // "1.00", "3.00", ...
	// The tip amount equals preset Index (as numbers: "3" is "3.00").
	bool IsPresetSelected(int32 Index) const;
	TArray<FScfgChainOptionView> ChainOptions() const;
	TArray<FString> Methods() const;
	FString Recipient() const;
	FString TokenAddress() const;
	FString SessionId() const;   // "" before a session started
	bool HasSession() const;
	FString PaymentUri() const;  // "" before a session started
	FString ExplorerUrl() const;  // "" without a reported or found transaction
	// The buy page for the chosen network (configure's onramp), or "" where it is not offered.
	FString OnrampUrl() const;
	// This step offers "No USDC yet? Buy some with a card" (OpenOnramp).
	bool ShowsOnramp() const;
	// This step points to it instead, "No USDC yet? Pick another network to buy some with a card", whose button goes
	// back to the network step (Back): the chosen network has none (Solana) but another one the player can pick has it.
	bool ShowsOnrampHint() const;
	FString HintTx() const;      // the transaction reported for this payment, or ""
	FString ResultTx() const;    // the final result's transaction, or ""
	int64 Confirmations() const;
	int64 Required() const;
	bool CanGoBack() const;
	bool TxSeen() const;
	bool BrowserFailed() const;
	// (index of the current step, number of steps) for the step dots.
	TPair<int32, int32> StepDots() const;
	// The message id (CheckoutController's error, or "solana_unavailable"), or "".
	FString MessageId() const;
	// Its English text (ScfgTexts), or "".
	FString Message() const;
	TArray<FString> DevWarnings() const;
	bool IsTestnet() const;
	// methods.qr.wallet_button and methods.manual.allow_tx_paste.
	bool QrWalletButton() const;
	bool AllowTxPaste() const;
	FString CustomAmount() const;
	FString TxInput() const;
	bool IsMobile() const;
	// The configured accent ("#rrggbb") and logo path ("" without one).
	FString Accent() const;
	FString LogoPath() const;

	// --- the buttons ---

	void Back();
	// Leave the amount step (applying a typed amount first).
	void Next();
	void SetTip(const FString& Value);
	void SetCustomAmount(const FString& Text);
	void ApplyCustomAmount();
	void ChooseNetwork(const FString& Key);
	void ChooseMethod(const FString& Name);
	void MarkSent();
	void SetTxInput(const FString& Text);
	void SubmitHash();
	void OpenBrowser();
	void OpenWalletApp();
	// "No USDC yet? Buy some with a card": opens configure's onramp page for the chosen network.
	void OpenOnramp();
	void OpenExplorer(const FString& Url);
	void Copy(const FString& Text);
	void Paste();

	const TSharedRef<FScfgCheckout>& GetCheckout() const { return Checkout; }

private:
	TSharedRef<FScfgCheckout> Checkout;
};

class SCFG_API FScfgRestoreController
{
public:
	explicit FScfgRestoreController(const TSharedRef<FScfgRestore>& InRestore);

	FString GetId() const;
	// input, checking, choose, saving, done, closed.
	FString State() const;
	FString MessageId() const;  // RestoreController's error, or ""
	FString Message() const;    // its English (ScfgTexts' "restore_<id>"), or ""
	FString TxInput() const;
	int32 NumCandidates() const;
	TArray<int32> PendingChoices() const;
	FScfgRestoreCandidateView Candidate(int32 Index) const;
	// The choice made for candidate Index, or "".
	FString Choice(int32 Index) const;
	// Every choice, "index=choice" joined (what the screen compares to rebuild).
	FString ChoicesSignature() const;
	bool CanConfirm() const;
	TArray<FScfgReceiptLine> Added() const;
	// The receipts on this device, and a counter that changes whenever they do.
	TArray<FScfgReceiptLine> Receipts() const;
	int32 ReceiptsRevision() const;
	// The declared title, else the id; the chain's name, else its key.
	FString ItemTitle(const FString& ItemId) const;
	FString ChainName(const FString& ChainKey) const;
	bool IsMobile() const;
	FString Accent() const;
	FString LogoPath() const;

	void SetTxInput(const FString& Text);
	// Check the hash (only in the input state).
	void Check();
	void Choose(int32 Index, const FString& Choice);
	void Confirm();
	void Copy(const FString& Text);
	void Paste();

	const TSharedRef<FScfgRestore>& GetRestore() const { return Restore; }

private:
	TSharedRef<FScfgRestore> Restore;
};
