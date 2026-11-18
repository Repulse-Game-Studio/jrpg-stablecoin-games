// Copyright 2026 Repulse
// No Python twin: the Unreal-typed view of the core's controllers for the screens (see ScfgCheckoutController.h).
#include "ScfgCheckoutController.h"

#include "ScfgControllers.h"
#include "ScfgConvert.h"
#include "ScfgFacade.h"
#include "ScfgString.h"
#include "UI/ScfgTexts.h"

#include "Scfg/Amounts.h"
#include "Scfg/Checkout.h"
#include "Scfg/Py.h"
#include "Scfg/Restore.h"
#include "Scfg/RestoreFlow.h"

#include <variant>

namespace ScfgCheckoutControllerDetail
{

static FString Opt(const std::optional<std::string>& S)
{
	return S ? ScfgString::FromUtf8(*S) : FString();
}

// An int as an int64, clamped (the confirmation counts are small; BigInt only for Python's int).
static int64 Clamp(const scfg::BigInt& V)
{
	if (V < scfg::BigInt(0))
	{
		return 0;
	}
	const scfg::Result<int64_t> Small = V.to_int64();
	return Small.ok() ? int64(Small.value()) : MAX_int64;
}

// Godot's _report(err): an Err a screen action got back, for the developer.
static void Report(const TSharedPtr<FScfgFacade>& Facade, const scfg::Err& Error)
{
	if (Facade.IsValid())
	{
		Facade->ReportScreenError(Error);
	}
}

static FScfgReceiptLine Line(const scfg::Json& Record)
{
	const FScfgReceipt R = ScfgConvert::Receipt(Record);
	FScfgReceiptLine Out;
	Out.Kind = R.Kind == EScfgPayKind::Tip ? TEXT("tip") : TEXT("unlock");
	Out.Item = R.Item;
	Out.Amount = R.Amount;
	Out.Token = R.Token;
	Out.Chain = R.Chain;
	Out.TxHash = R.TxHash;
	return Out;
}

static bool MethodOption(const scfg::checkout::CheckoutController& Ctl, const char* Method, const char* Option)
{
	const scfg::Json& Methods = Ctl.config().methods;
	if (!Methods.is_dict())
	{
		return false;
	}
	const scfg::Json* M = Methods.as_dict().find(Method);
	if (!M || !M->is_dict())
	{
		return false;
	}
	const scfg::Json* V = M->as_dict().find(Option);
	return V && scfg::py::truthy(*V);
}

}  // namespace ScfgCheckoutControllerDetail

// --- the checkout ---------------------------------------------------------------------------------------------------

FScfgCheckoutController::FScfgCheckoutController(const TSharedRef<FScfgCheckout>& InCheckout) : Checkout(InCheckout) {}

FString FScfgCheckoutController::GetId() const { return Checkout->GetId(); }
FString FScfgCheckoutController::State() const { return ScfgString::FromUtf8(Checkout->Core().state()); }
FString FScfgCheckoutController::Step() const { return ScfgString::FromUtf8(Checkout->Core().step()); }
FString FScfgCheckoutController::Method() const { return ScfgCheckoutControllerDetail::Opt(Checkout->Core().method()); }
FString FScfgCheckoutController::ChainKey() const { return ScfgCheckoutControllerDetail::Opt(Checkout->Core().chain_key()); }
FString FScfgCheckoutController::Family() const { return ScfgCheckoutControllerDetail::Opt(Checkout->Core().family()); }

FString FScfgCheckoutController::ChainDisplay() const
{
	const scfg::chains::Chain* C = Checkout->Core().chain();
	return C ? ScfgString::FromUtf8(C->name) : FString();
}

FString FScfgCheckoutController::Kind() const { return ScfgString::FromUtf8(Checkout->Core().spec().kind); }
FString FScfgCheckoutController::Title() const { return ScfgConvert::Str(Checkout->Core().spec().title); }
FString FScfgCheckoutController::Token() const { return ScfgString::FromUtf8(Checkout->Core().spec().token); }
FString FScfgCheckoutController::PriceText() const { return ScfgString::FromUtf8(Checkout->Core().price_text()); }
FString FScfgCheckoutController::PayAmountText() const { return ScfgString::FromUtf8(Checkout->Core().pay_amount_text()); }
FString FScfgCheckoutController::Amount() const { return ScfgString::FromUtf8(Checkout->Core().amount().str()); }

TArray<FString> FScfgCheckoutController::TipPresets() const
{
	TArray<FString> Out;
	for (const scfg::Dec& Preset : Checkout->Core().tip_presets())
	{
		Out.Add(ScfgString::FromUtf8(Preset.str()));
	}
	return Out;
}

bool FScfgCheckoutController::IsPresetSelected(int32 Index) const
{
	const std::array<scfg::Dec, 4>& Presets = Checkout->Core().tip_presets();
	return Index >= 0 && size_t(Index) < Presets.size() && Checkout->Core().amount() == Presets[size_t(Index)];
}

TArray<FScfgChainOptionView> FScfgCheckoutController::ChainOptions() const
{
	TArray<FScfgChainOptionView> Out;
	for (const scfg::checkout::ChainOption& O : Checkout->Core().chain_options())
	{
		Out.Add({ScfgString::FromUtf8(O.key), ScfgString::FromUtf8(O.name), ScfgString::FromUtf8(O.fee_hint)});
	}
	return Out;
}

TArray<FString> FScfgCheckoutController::Methods() const
{
	TArray<FString> Out;
	for (const std::string& M : Checkout->Core().methods())
	{
		Out.Add(ScfgString::FromUtf8(M));
	}
	return Out;
}

FString FScfgCheckoutController::Recipient() const { return ScfgCheckoutControllerDetail::Opt(Checkout->Core().recipient()); }
FString FScfgCheckoutController::TokenAddress() const { return ScfgCheckoutControllerDetail::Opt(Checkout->Core().token_address()); }
FString FScfgCheckoutController::SessionId() const { return ScfgCheckoutControllerDetail::Opt(Checkout->Core().session_id()); }
bool FScfgCheckoutController::HasSession() const { return scfg::py::truthy(Checkout->Core().session()); }

FString FScfgCheckoutController::PaymentUri() const
{
	const scfg::Result<std::optional<std::string>> Uri = Checkout->Core().payment_uri();
	if (!Uri.ok())
	{
		// (once per controller: the QR step reads it on every rebuild, the wallet-app button on every press)
		if (const TSharedPtr<FScfgFacade> F = Checkout->GetFacade())
		{
			F->ReportPaymentLinkError(*Checkout, Uri.err());
		}
		return FString();
	}
	return ScfgCheckoutControllerDetail::Opt(Uri.value());
}

FString FScfgCheckoutController::ExplorerUrl() const
{
	const scfg::Result<std::optional<std::string>> Url = Checkout->Core().explorer_url();
	if (!Url.ok())
	{
		ScfgCheckoutControllerDetail::Report(Checkout->GetFacade(), Url.err());
		return FString();
	}
	return ScfgCheckoutControllerDetail::Opt(Url.value());
}

FString FScfgCheckoutController::HintTx() const { return ScfgCheckoutControllerDetail::Opt(Checkout->Core().hint_tx); }

FString FScfgCheckoutController::ResultTx() const
{
	const std::optional<scfg::models::PaymentResult>& R = Checkout->Core().result();
	return R ? ScfgConvert::Str(R->tx_hash) : FString();
}

int64 FScfgCheckoutController::Confirmations() const { return ScfgCheckoutControllerDetail::Clamp(Checkout->Core().confirmations()); }
int64 FScfgCheckoutController::Required() const { return ScfgCheckoutControllerDetail::Clamp(Checkout->Core().required()); }
FString FScfgCheckoutController::OnrampUrl() const
{
	const scfg::Result<std::optional<std::string>> Url = Checkout->Core().onramp_url();
	if (!Url.ok())
	{
		ScfgCheckoutControllerDetail::Report(Checkout->GetFacade(), Url.err());
		return FString();
	}
	return ScfgCheckoutControllerDetail::Opt(Url.value());
}

bool FScfgCheckoutController::ShowsOnramp() const { return Checkout->Core().shows_onramp(); }
bool FScfgCheckoutController::ShowsOnrampHint() const { return Checkout->Core().shows_onramp_hint(); }
bool FScfgCheckoutController::CanGoBack() const { return Checkout->Core().can_go_back(); }
bool FScfgCheckoutController::TxSeen() const { return Checkout->Core().tx_seen(); }
bool FScfgCheckoutController::BrowserFailed() const { return Checkout->Core().browser_failed(); }

TPair<int32, int32> FScfgCheckoutController::StepDots() const
{
	const std::pair<int64_t, int64_t> D = Checkout->Core().step_dots();
	return TPair<int32, int32>(int32(D.first), int32(D.second));
}

FString FScfgCheckoutController::MessageId() const
{
	const scfg::checkout::CheckoutController& Ctl = Checkout->Core();
	const std::optional<std::string> Text = Ctl.message();
	if (!Text)
	{
		return FString();
	}
	if (Ctl.error && !Ctl.error->empty())
	{
		return ScfgString::FromUtf8(*Ctl.error);
	}
	// message() without an error: the text of one of MESSAGES (solana_unavailable), found by its text.
	for (const auto& [Id, English] : scfg::checkout::MESSAGES())
	{
		if (English == *Text)
		{
			return ScfgString::FromUtf8(Id);
		}
	}
	return FString();
}

FString FScfgCheckoutController::Message() const { return ScfgTexts::Message(MessageId()); }

TArray<FString> FScfgCheckoutController::DevWarnings() const
{
	TArray<FString> Out;
	for (const std::string& W : Checkout->Core().dev_warnings())
	{
		Out.Add(ScfgString::FromUtf8(W));
	}
	return Out;
}

bool FScfgCheckoutController::IsTestnet() const { return Checkout->Core().config().testnet; }
bool FScfgCheckoutController::QrWalletButton() const { return ScfgCheckoutControllerDetail::MethodOption(Checkout->Core(), "qr", "wallet_button"); }
bool FScfgCheckoutController::AllowTxPaste() const { return ScfgCheckoutControllerDetail::MethodOption(Checkout->Core(), "manual", "allow_tx_paste"); }
FString FScfgCheckoutController::CustomAmount() const { return ScfgString::FromUtf8(Checkout->Core().custom_amount); }
FString FScfgCheckoutController::TxInput() const { return ScfgString::FromUtf8(Checkout->Core().tx_input); }

bool FScfgCheckoutController::IsMobile() const
{
	const TSharedPtr<FScfgFacade> F = Checkout->GetFacade();
	return F.IsValid() && F->IsMobile();
}

FString FScfgCheckoutController::Accent() const { return ScfgString::FromUtf8(Checkout->Core().config().accent); }

FString FScfgCheckoutController::LogoPath() const
{
	const TSharedPtr<FScfgFacade> F = Checkout->GetFacade();
	return F.IsValid() ? F->GetLogoPath() : FString();
}

void FScfgCheckoutController::Back() { Checkout->Core().back(); }

void FScfgCheckoutController::Next()
{
	const scfg::Result<bool> R = Checkout->Core().next_step();
	if (!R.ok())
	{
		ScfgCheckoutControllerDetail::Report(Checkout->GetFacade(), R.err());
	}
}

void FScfgCheckoutController::SetTip(const FString& Value)
{
	const scfg::Result<bool> R = Checkout->Core().set_tip_amount(scfg::Json(ScfgString::ToUtf8(Value)));
	if (!R.ok())
	{
		ScfgCheckoutControllerDetail::Report(Checkout->GetFacade(), R.err());
	}
}

void FScfgCheckoutController::SetCustomAmount(const FString& Text) { Checkout->Core().custom_amount = ScfgString::ToUtf8(Text); }

void FScfgCheckoutController::ApplyCustomAmount()
{
	const scfg::Result<bool> R = Checkout->Core().apply_custom_amount();
	if (!R.ok())
	{
		ScfgCheckoutControllerDetail::Report(Checkout->GetFacade(), R.err());
	}
}

void FScfgCheckoutController::ChooseNetwork(const FString& Key)
{
	const scfg::Status R = Checkout->Core().choose_network(ScfgString::ToUtf8(Key));
	if (!R.ok())
	{
		ScfgCheckoutControllerDetail::Report(Checkout->GetFacade(), R.err());
	}
}

void FScfgCheckoutController::ChooseMethod(const FString& Name) { Checkout->Core().choose_method(ScfgString::ToUtf8(Name)); }
void FScfgCheckoutController::MarkSent() { Checkout->Core().mark_sent(); }
void FScfgCheckoutController::SetTxInput(const FString& Text) { Checkout->Core().tx_input = ScfgString::ToUtf8(Text); }

void FScfgCheckoutController::SubmitHash()
{
	const scfg::Result<bool> R = Checkout->Core().submit_hash();
	if (!R.ok())
	{
		ScfgCheckoutControllerDetail::Report(Checkout->GetFacade(), R.err());
	}
}

void FScfgCheckoutController::OpenBrowser() { Checkout->OpenBrowser(); }
void FScfgCheckoutController::OpenWalletApp() { Checkout->OpenWalletApp(); }
void FScfgCheckoutController::OpenOnramp() { Checkout->OpenOnramp(); }
void FScfgCheckoutController::OpenExplorer(const FString& Url) { Checkout->OpenExplorer(Url); }
void FScfgCheckoutController::Copy(const FString& Text) { Checkout->Copy(Text); }
void FScfgCheckoutController::Paste() { Checkout->Paste(); }

// --- the restore ----------------------------------------------------------------------------------------------------

FScfgRestoreController::FScfgRestoreController(const TSharedRef<FScfgRestore>& InRestore) : Restore(InRestore) {}

FString FScfgRestoreController::GetId() const { return Restore->GetId(); }
FString FScfgRestoreController::State() const { return ScfgString::FromUtf8(Restore->Core().state()); }
FString FScfgRestoreController::MessageId() const { return Restore->Core().message() ? ScfgCheckoutControllerDetail::Opt(Restore->Core().error) : FString(); }
FString FScfgRestoreController::Message() const { return ScfgTexts::RestoreMessage(MessageId()); }
FString FScfgRestoreController::TxInput() const { return ScfgString::FromUtf8(Restore->Core().tx_input); }
int32 FScfgRestoreController::NumCandidates() const { return int32(Restore->Core().candidates().size()); }

TArray<int32> FScfgRestoreController::PendingChoices() const
{
	TArray<int32> Out;
	for (const int64_t I : Restore->Core().pending_choices())
	{
		Out.Add(int32(I));
	}
	return Out;
}

FScfgRestoreCandidateView FScfgRestoreController::Candidate(int32 Index) const
{
	FScfgRestoreCandidateView Out;
	const std::vector<scfg::restore::RestoreCandidate>& All = Restore->Core().candidates();
	if (Index < 0 || size_t(Index) >= All.size())
	{
		return Out;
	}
	const scfg::restore::RestoreCandidate& C = All[size_t(Index)];
	Out.Chain = ScfgString::FromUtf8(C.chain);
	Out.Token = ScfgString::FromUtf8(C.token);
	const scfg::BigInt Value = std::visit([](const auto& T) { return T.value; }, C.transfer);
	Out.Amount = ScfgString::FromUtf8(scfg::amounts::format_units(Value));
	for (const std::string& Choice : C.choices)
	{
		Out.Choices.Add(ScfgString::FromUtf8(Choice));
	}
	return Out;
}

FString FScfgRestoreController::Choice(int32 Index) const
{
	const std::map<int64_t, std::string>& Choices = Restore->Core().choices();
	const auto Found = Choices.find(Index);
	return Found != Choices.end() ? ScfgString::FromUtf8(Found->second) : FString();
}

FString FScfgRestoreController::ChoicesSignature() const
{
	FString Out;
	for (const auto& [Index, Choice] : Restore->Core().choices())
	{
		Out += FString::Printf(TEXT("%lld=%s;"), static_cast<long long>(Index), *ScfgString::FromUtf8(Choice));
	}
	return Out;
}

bool FScfgRestoreController::CanConfirm() const { return Restore->Core().can_confirm(); }

TArray<FScfgReceiptLine> FScfgRestoreController::Added() const
{
	TArray<FScfgReceiptLine> Out;
	for (const scfg::Json& R : Restore->Core().added())
	{
		Out.Add(ScfgCheckoutControllerDetail::Line(R));
	}
	return Out;
}

TArray<FScfgReceiptLine> FScfgRestoreController::Receipts() const
{
	TArray<FScfgReceiptLine> Out;
	const TSharedPtr<FScfgFacade> F = Restore->GetFacade();
	if (!F.IsValid() || F->IsShuttingDown())
	{
		return Out;
	}
	const scfg::Json All = F->Receipts();
	if (All.is_dict())
	{
		for (const auto& [Key, Record] : All.as_dict())
		{
			Out.Add(ScfgCheckoutControllerDetail::Line(Record));
		}
	}
	return Out;
}

int32 FScfgRestoreController::ReceiptsRevision() const
{
	const TSharedPtr<FScfgFacade> F = Restore->GetFacade();
	return F.IsValid() ? F->ReceiptsRevision() : 0;
}

FString FScfgRestoreController::ItemTitle(const FString& ItemId) const
{
	const TSharedPtr<FScfgFacade> F = Restore->GetFacade();
	return F.IsValid() ? F->ItemTitle(ItemId) : ItemId;
}

FString FScfgRestoreController::ChainName(const FString& ChainKey) const
{
	const TSharedPtr<FScfgFacade> F = Restore->GetFacade();
	return F.IsValid() ? F->ChainName(ChainKey) : ChainKey;
}

bool FScfgRestoreController::IsMobile() const
{
	const TSharedPtr<FScfgFacade> F = Restore->GetFacade();
	return F.IsValid() && F->IsMobile();
}

FString FScfgRestoreController::Accent() const
{
	const TSharedPtr<FScfgFacade> F = Restore->GetFacade();
	return F.IsValid() ? F->Accent() : ScfgString::FromUtf8(scfg::config::DEFAULT_ACCENT);
}

FString FScfgRestoreController::LogoPath() const
{
	const TSharedPtr<FScfgFacade> F = Restore->GetFacade();
	return F.IsValid() ? F->GetLogoPath() : FString();
}

void FScfgRestoreController::SetTxInput(const FString& Text) { Restore->Core().tx_input = ScfgString::ToUtf8(Text); }

void FScfgRestoreController::Check()
{
	if (Restore->Core().state() != "input")
	{
		return;
	}
	const scfg::Result<bool> R = Restore->Core().check_tx();
	if (!R.ok())
	{
		ScfgCheckoutControllerDetail::Report(Restore->GetFacade(), R.err());
	}
}

void FScfgRestoreController::Choose(int32 Index, const FString& Choice) { Restore->Core().choose(Index, ScfgString::ToUtf8(Choice)); }
void FScfgRestoreController::Confirm() { Restore->Core().confirm(); }
void FScfgRestoreController::Copy(const FString& Text) { Restore->Copy(Text); }
void FScfgRestoreController::Paste() { Restore->Paste(); }
