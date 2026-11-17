// Twin of scfg.gd's _Checkout and _Restore (and Unity's ScfgCheckoutController / ScfgRestoreController): what a payment
// sheet or a restore screen binds to (IScfgSheetHost.h). Each holds the core's controller (Core(): read its fields,
// call its methods from buttons), the flow's id and the call's callback, plus the buttons that leave the game.
//
// The flows (FScfgFacade) own these and poll the controller every frame; a host may keep a reference past the sheet's
// close (the controller is closed by then, and the buttons do nothing).
#pragma once

#include "CoreMinimal.h"

#include "ScfgTypes.h"

#include "Scfg/Checkout.h"
#include "Scfg/RestoreFlow.h"

#include <memory>
#include <optional>
#include <string>

class FScfgFacade;

// A Pay() callback (Godot's callback Callable): Fn, while IsValid() says its owner is alive (a dynamic delegate whose
// object went away is not valid; Godot's is_valid()). An invalid one counts as no callback.
struct FScfgPayCallback
{
	TFunction<void(const FScfgPaymentResult&)> Fn;
	TFunction<bool()> IsValid;  // empty: valid while Fn is set

	bool Valid() const { return Fn && (!IsValid || IsValid()); }
};

struct FScfgRestoreCallback
{
	TFunction<void(const FScfgRestoreResult&)> Fn;
	TFunction<bool()> IsValid;

	bool Valid() const { return Fn && (!IsValid || IsValid()); }
};

// The controller behind a payment sheet.
class SCFG_API FScfgCheckout
{
public:
	FScfgCheckout(const FScfgCheckout&) = delete;
	FScfgCheckout& operator=(const FScfgCheckout&) = delete;

	scfg::checkout::CheckoutController& Core() const { return *Controller; }
	// "c1", "c2", ... (Godot's cid).
	const FString& GetId() const { return Cid; }

	// "Open checkout in browser": starts the controller's page and opens the browser on it.
	void OpenBrowser();
	// "Open wallet app" (phones): the payment request link (EIP-681 or Solana Pay).
	void OpenWalletApp();
	// "View on explorer": opens Url (nothing for "").
	void OpenExplorer(const FString& Url);
	void Copy(const FString& Text);
	// The clipboard's first word, hash characters only, into the hash field.
	void Paste();
	// The flows this belongs to (null once they are gone).
	TSharedPtr<FScfgFacade> GetFacade() const { return Facade.Pin(); }

private:
	friend class FScfgFacade;
	FScfgCheckout() = default;

	std::unique_ptr<scfg::checkout::CheckoutController> Controller;
	FString Cid;
	FScfgPayCallback Callback;
	// The session id the callback was registered for (Godot's callback_sid).
	std::optional<std::string> CallbackSid;
	TWeakPtr<FScfgFacade> Facade;
	// A failing payment_uri was reported (once per controller: FScfgFacade::ReportPaymentLinkError).
	bool bPaymentLinkReported = false;
};

// The controller behind a restore screen.
class SCFG_API FScfgRestore
{
public:
	FScfgRestore(const FScfgRestore&) = delete;
	FScfgRestore& operator=(const FScfgRestore&) = delete;

	scfg::restore_flow::RestoreController& Core() const { return *Controller; }
	// "r1", "r2", ...
	const FString& GetId() const { return Rid; }

	void Paste();
	void Copy(const FString& Text);
	TSharedPtr<FScfgFacade> GetFacade() const { return Facade.Pin(); }

private:
	friend class FScfgFacade;
	FScfgRestore() = default;

	std::unique_ptr<scfg::restore_flow::RestoreController> Controller;
	FString Rid;
	FScfgRestoreCallback Callback;
	TWeakPtr<FScfgFacade> Facade;
};
