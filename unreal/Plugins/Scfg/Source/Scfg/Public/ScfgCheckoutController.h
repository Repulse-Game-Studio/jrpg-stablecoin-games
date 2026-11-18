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
//             Made->AddToViewport(FScfgDefaultSheetHost::ZOrder);       // it calls Screen->Close("cancel")
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
// ids are the core's: steps amount, network, method, browser, qr, manual, waiting, done; states choose,
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

// The payment sheet's view of one payment: what to show (read every frame) and the buttons (see the file header).
class SCFG_API FScfgCheckoutController
{
public:
	explicit FScfgCheckoutController(const TSharedRef<FScfgCheckout>& InCheckout);

	// --- what the sheet shows ---

	// This sheet's id ("c1", "c2", ...), unique in the session.
	FString GetId() const;
	// The payment's state (choose, starting, waiting, confirming, paid, underpaid, expired, closed).
	FString State() const;
	// The step the sheet shows (amount, network, method, browser, qr, manual, waiting, done).
	FString Step() const;
	FString Method() const;      // "" before one is chosen
	FString ChainKey() const;    // "" before one is chosen
	FString Family() const;      // "evm", "solana", or ""
	FString ChainDisplay() const;  // the chosen chain's name, or ""
	FString Kind() const;        // "unlock" or "tip"
	FString Title() const;       // the item's title, or the tip's
	FString Token() const;       // "USDC" or "USDT"
	FString PriceText() const;   // "4.99"
	FString PayAmountText() const;  // the exact amount with the code and nonce ("4.993742")
	FString Amount() const;      // the tip amount (a Dec string)
	TArray<FString> TipPresets() const;  // "1.00", "3.00", ...
	// The tip amount equals preset Index (as numbers: "3" is "3.00").
	bool IsPresetSelected(int32 Index) const;
	// The networks the player can pick, in display order.
	TArray<FScfgChainOptionView> ChainOptions() const;
	// The methods offered on the chosen network ("browser", "qr", "manual"), in display order.
	TArray<FString> Methods() const;
	// The address to pay and the token's contract (or mint) on the chosen network, "" before one is chosen.
	FString Recipient() const;
	FString TokenAddress() const;
	FString SessionId() const;   // "" before a session started
	// A payment session started (the amount and the address to pay are fixed).
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
	// The confirmations the payment has, and the ones it needs.
	int64 Confirmations() const;
	int64 Required() const;
	// Back is offered (there is a step before this one, and no transaction was seen yet).
	bool CanGoBack() const;
	// A transaction for this payment exists (reported or found), so going back could start a second one.
	bool TxSeen() const;
	// The browser could not be opened: the browser method is no longer offered.
	bool BrowserFailed() const;
	// (index of the current step, number of steps) for the step dots.
	TPair<int32, int32> StepDots() const;
	// The message id (CheckoutController's error, or "solana_unavailable"), or "".
	FString MessageId() const;
	// Its English text (ScfgTexts), or "".
	FString Message() const;
	// Setup problems the developer must fix, shown on testnet only ("Your Solana wallet can't receive USDC yet ...").
	TArray<FString> DevWarnings() const;
	// The game runs on the test networks (the sheet says no real money moves).
	bool IsTestnet() const;
	// methods.qr.wallet_button and methods.manual.allow_tx_paste.
	bool QrWalletButton() const;
	bool AllowTxPaste() const;
	// The text in the tip's custom amount field and in the transaction hash field.
	FString CustomAmount() const;
	FString TxInput() const;
	// A phone or tablet: no browser method, and the QR step offers "Open wallet app".
	bool IsMobile() const;
	// The configured accent ("#rrggbb") and logo path ("" without one).
	FString Accent() const;
	FString LogoPath() const;

	// --- the buttons ---

	// Go back one step (when CanGoBack()).
	void Back();
	// Leave the amount step (applying a typed amount first).
	void Next();
	// Pick a tip amount (a preset's value).
	void SetTip(const FString& Value);
	// The custom amount field changed; ApplyCustomAmount() makes it the tip amount.
	void SetCustomAmount(const FString& Text);
	void ApplyCustomAmount();
	// Pick a network (a ChainOptions() key) or a method (one of Methods()).
	void ChooseNetwork(const FString& Key);
	void ChooseMethod(const FString& Name);
	// "I've sent it" on the QR or manual step: the sheet moves on to waiting for the payment.
	void MarkSent();
	// The transaction hash field changed; SubmitHash() reports it, so the payment is checked at once.
	void SetTxInput(const FString& Text);
	void SubmitHash();
	// Open the checkout page in the player's browser.
	void OpenBrowser();
	// Open the payment in a wallet app on this device (the QR step on a phone).
	void OpenWalletApp();
	// "No USDC yet? Buy some with a card": opens configure's onramp page for the chosen network.
	void OpenOnramp();
	// Open a block explorer url (ExplorerUrl()) in the browser.
	void OpenExplorer(const FString& Url);
	// Copy Text (an address, an amount) to the clipboard; paste the clipboard into the transaction hash field.
	void Copy(const FString& Text);
	void Paste();

	// The controller this view reads.
	const TSharedRef<FScfgCheckout>& GetCheckout() const { return Checkout; }

private:
	TSharedRef<FScfgCheckout> Checkout;
};

// The restore screen's view of one restore: what to show (read every frame) and the buttons (see the file header).
class SCFG_API FScfgRestoreController
{
public:
	explicit FScfgRestoreController(const TSharedRef<FScfgRestore>& InRestore);

	// This screen's id ("r1", "r2", ...), unique in the session.
	FString GetId() const;
	// input, checking, choose, saving, done, closed.
	FString State() const;
	FString MessageId() const;  // RestoreController's error, or ""
	FString Message() const;    // its English (ScfgTexts' "restore_<id>"), or ""
	// The text in the transaction hash field.
	FString TxInput() const;
	// The payments the check found, and the indexes of those that still need the player to name them.
	int32 NumCandidates() const;
	TArray<int32> PendingChoices() const;
	// Candidate Index (0 .. NumCandidates() - 1).
	FScfgRestoreCandidateView Candidate(int32 Index) const;
	// The choice made for candidate Index, or "".
	FString Choice(int32 Index) const;
	// Every choice, "index=choice" joined (what the screen compares to rebuild).
	FString ChoicesSignature() const;
	// Every candidate that needs a choice has one.
	bool CanConfirm() const;
	// The receipts the restore added.
	TArray<FScfgReceiptLine> Added() const;
	// The receipts on this device, and a counter that changes whenever they do.
	TArray<FScfgReceiptLine> Receipts() const;
	int32 ReceiptsRevision() const;
	// The declared title, else the id; the chain's name, else its key.
	FString ItemTitle(const FString& ItemId) const;
	FString ChainName(const FString& ChainKey) const;
	// A phone or tablet.
	bool IsMobile() const;
	// The configured accent ("#rrggbb") and logo path ("" without one).
	FString Accent() const;
	FString LogoPath() const;

	// The transaction hash field changed.
	void SetTxInput(const FString& Text);
	// Check the hash (only in the input state).
	void Check();
	// Name candidate Index: an item id or "tip" (one of its Choices).
	void Choose(int32 Index, const FString& Choice);
	// Store the named candidates (when CanConfirm()).
	void Confirm();
	// Copy Text to the clipboard; paste the clipboard into the hash field.
	void Copy(const FString& Text);
	void Paste();

	// The controller this view reads.
	const TSharedRef<FScfgRestore>& GetRestore() const { return Restore; }

private:
	TSharedRef<FScfgRestore> Restore;
};
