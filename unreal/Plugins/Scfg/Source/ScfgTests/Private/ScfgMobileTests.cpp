// No Python twin: the mobile checkout path of the glue (phase 3, best effort), Scfg.Glue.Mobile.<name>, run on Linux
// with the mobile override: the fake platform's bMobile (ScfgFacadeTestKit.h), or FScfgEnginePlatform(true), the
// engine's own platform told it is a phone. On a phone (PLATFORM_ANDROID || PLATFORM_IOS, ScfgPlatform.h) the checkout
// offers no browser method (no localhost page), the QR step offers "Open wallet app", which hands the payment request
// (EIP-681 or Solana Pay) to FPlatformProcess::LaunchURL, and the payment is found by the worker's polling. No Android
// or iOS build ran (no SDK installed): unreal/README.md, "Mobile".
//
// Mapping (the Python and Godot mobile tests -> their twins):
//   tests/unit/test_checkout.py, godot/tests/test_checkout.gd: test_methods_on_mobile_and_config
//       -> the core's twin of the same name (core-tests/CheckoutTests.cpp, ctest), and test_methods_on_mobile_and_config
//          here, through the facade: mobile is what the platform says, the methods options are configure()'s
//   tests/unit/test_checkout_steps.py, godot/tests/test_checkout_steps.gd: test_web_browser_wallet_ignores_the_mobile_flag
//       -> the core's twin of the same name (core-tests/CheckoutStepsTests.cpp, ctest) only: browser_wallet is the web
//          build's, and UE5 has no web target (the facade never sets it)
//   godot/tests/test_scfg_flows.gd: test_mobile_offers_wallet_app_link
//       -> ScfgFacadeFlowsTests.cpp, the same name (Scfg.Glue.Facade.Flows.test_mobile_offers_wallet_app_link)
//   Ren'Py's _is_mobile / _open_wallet_app (scfg_api.rpy) and the sheets' wallet button (scfg_screens.rpy:687,
//   godot/addons/scfg/ui/sheet.gd:304) have no test in tests/ or godot/tests: OnAPhoneTheQrStepOffersTheWalletApp below
// C++ only:
//   TheEnginePlatformIsMobileOnPhonesOnly   -> FScfgEnginePlatform: PLATFORM_ANDROID || PLATFORM_IOS, the -scfg-mobile
//       override (not in Shipping), the tests' constructor
//   TheCheckoutIsMobileAsThePlatformSays    -> the facade passes the platform's answer to every controller, the engine's
//       platform included
//   AWalletAppPaymentIsFoundByPolling       -> an EVM unlock on a phone: QR, Open wallet app (the EIP-681 link), I've
//       sent it, paid on chain, found by the worker's passes; no page server is ever made
//   TheSolanaWalletLinkIsASolanaPayRequest  -> a Solana unlock on a phone: the link is the Solana Pay request with the
//       session's reference
//   OnAPhoneTheQrStepOffersTheWalletApp     -> the default Slate sheet: no browser method, the wallet button on the QR
//       step, pressed by keys; on a computer no wallet button (TipWalksEveryStepByKeys)
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"

#include "ScfgCheckoutController.h"
#include "ScfgFacade.h"
#include "ScfgFacadeTestKit.h"
#include "ScfgPlatform.h"
#include "ScfgSheetTestKit.h"
#include "ScfgString.h"
#include "UI/SScfgPaymentSheet.h"
#include "UI/ScfgTexts.h"

#include "Scfg/Chains.h"
#include "Scfg/Checkout.h"
#include "Scfg/Json.h"

#include <optional>
#include <string>
#include <vector>

#if WITH_DEV_AUTOMATION_TESTS

#define SCFG_MOBILE_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.Mobile." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ScfgMobileTests
{

using namespace ScfgFacadeTests;

// A fixed Solana account (core-tests/CheckoutSolanaTests.cpp's DEV).
constexpr const char* kSolanaDev = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM";

FString Joined(const std::vector<std::string>& Names)
{
	TArray<FString> Out;
	for (const std::string& Name : Names)
	{
		Out.Add(ScfgString::FromUtf8(Name));
	}
	return FString::Join(Out, TEXT(","));
}

FString Link(const FScfgCheckout& Checkout)
{
	const scfg::Result<std::optional<std::string>> Uri = Checkout.Core().payment_uri();
	return Uri.ok() && Uri.value() ? ScfgString::FromUtf8(*Uri.value()) : FString();
}

// FWorld::Make with another platform than the fake one (the facade's options as FWorld gives them otherwise).
FScfgFacade& MakeWith(FWorld& W, TSharedRef<IScfgPlatform> Platform)
{
	W.Drop();
	FScfgFacadeOptions Options = W.Options();
	Options.Platform = Platform;
	W.Facade = MakeShared<FScfgFacade>(Options);
	W.Facade->SheetHost = W.Host;
	W.Facade->Start();
	return *W.Facade;
}

FScfgOptions DevWallet()
{
	FScfgOptions O;
	O.Wallet = UTF8_TO_TCHAR(kDev);
	return O;
}

void AddChapter2(FAutomationTestBase& Test, FScfgFacade& S)
{
	FScfgItem Item;
	Item.Code = 37;
	Item.Price = TEXT("4.99");
	Item.Token = TEXT("USDC");
	Item.Title = TEXT("Chapter 2");
	Test.TestFalse(TEXT("add_item"), S.AddItem(TEXT("chapter2"), Item).IsSet());
}

}  // namespace ScfgMobileTests

SCFG_MOBILE_TEST(FScfgMobileEnginePlatformTest, "TheEnginePlatformIsMobileOnPhonesOnly")

bool FScfgMobileEnginePlatformTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("the build's answer is PLATFORM_ANDROID || PLATFORM_IOS"), FScfgEnginePlatform::bMobileTarget,
		static_cast<bool>(PLATFORM_ANDROID || PLATFORM_IOS));
	TestFalse(TEXT("a Linux editor is not a phone"), FScfgEnginePlatform::bMobileTarget);
	TestEqual(TEXT("the default is the build's answer or the switch"), FScfgEnginePlatform().IsMobile(),
		FScfgEnginePlatform::DefaultIsMobile());
	TestEqual(TEXT("the tests' editor runs without the switch"), FScfgEnginePlatform::DefaultIsMobile(),
		FScfgEnginePlatform::MobileOverride(FCommandLine::Get()));
	TestTrue(TEXT("-scfg-mobile"), FScfgEnginePlatform::MobileOverride(TEXT("-scfg-mobile")));
	TestTrue(TEXT("among other switches"), FScfgEnginePlatform::MobileOverride(TEXT("ScfgDev -game -scfg-mobile -log")));
	TestFalse(TEXT("no switch"), FScfgEnginePlatform::MobileOverride(TEXT("ScfgDev -game -log")));
	TestFalse(TEXT("another switch that starts with it"), FScfgEnginePlatform::MobileOverride(TEXT("-scfg-mobiles")));
	TestFalse(TEXT("a word, not a switch"), FScfgEnginePlatform::MobileOverride(TEXT("scfg-mobile")));
	TestFalse(TEXT("no command line"), FScfgEnginePlatform::MobileOverride(nullptr));
	TestTrue(TEXT("the tests' override"), FScfgEnginePlatform(true).IsMobile());
	TestFalse(TEXT("and its desktop"), FScfgEnginePlatform(false).IsMobile());
	return true;
}

SCFG_MOBILE_TEST(FScfgMobileFacadeFlagTest, "TheCheckoutIsMobileAsThePlatformSays")

bool FScfgMobileFacadeFlagTest::RunTest(const FString& Parameters)
{
	using namespace ScfgMobileTests;
	struct FCase
	{
		const TCHAR* What;
		TSharedRef<IScfgPlatform> Platform;
		bool bMobile;
	};
	TSharedRef<FFakePlatform> Phone = MakeShared<FFakePlatform>();
	Phone->bMobile = true;
	const FCase Cases[] = {
		{TEXT("a computer"), MakeShared<FFakePlatform>(), false},
		{TEXT("a phone"), Phone, true},
		{TEXT("the engine's platform told it is a phone"), MakeShared<FScfgEnginePlatform>(true), true},
	};
	for (const FCase& Case : Cases)
	{
		FWorld W;
		FScfgFacade& S = MakeWith(W, Case.Platform);
		TestFalse(TEXT("configure"), S.Configure(DevWallet()).IsSet());
		AddChapter2(*this, S);
		W.Start();
		W.Pay(EScfgPayKind::Unlock, Unlock());  // the engine's platform: nothing below opens a URL
		const FScfgCheckout& Ctl = W.Session(*this);
		TestEqual(FString(Case.What) + TEXT(": the facade's answer"), S.IsMobile(), Case.bMobile);
		TestEqual(FString(Case.What) + TEXT(": the controller's mobile"), Ctl.Core().mobile, Case.bMobile);
		TestEqual(FString(Case.What) + TEXT(": the methods"), Joined(Ctl.Core().methods()),
			FString(Case.bMobile ? TEXT("qr,manual") : TEXT("browser,qr,manual")));
		W.Drop();
	}
	return true;
}

SCFG_MOBILE_TEST(FScfgMobileMethodsTest, "test_methods_on_mobile_and_config")

// test_checkout.py's: on a phone no browser method; a QR limited to Base is not offered on Arbitrum.
bool FScfgMobileMethodsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgMobileTests;
	{
		FWorld W;
		W.Platform->bMobile = true;
		W.Started(*this);
		W.Pay(EScfgPayKind::Unlock, Unlock());
		TestEqual(TEXT("mobile"), Joined(W.Session(*this).Core().methods()), FString(TEXT("qr,manual")));
	}
	{
		FWorld W;
		FScfgFacade& S = W.Make();
		FScfgOptions Options = DevWallet();
		Options.Methods.Qr.Chains = {TEXT("base")};
		TestFalse(TEXT("configure"), S.Configure(Options).IsSet());
		AddChapter2(*this, S);
		W.Start();
		W.Pay(EScfgPayKind::Unlock, Unlock());
		TestEqual(TEXT("QR limited to base, we're on arbitrum"), Joined(W.Session(*this).Core().methods()),
			FString(TEXT("browser,manual")));
	}
	return true;
}

SCFG_MOBILE_TEST(FScfgMobileEvmPaymentTest, "AWalletAppPaymentIsFoundByPolling")

bool FScfgMobileEvmPaymentTest::RunTest(const FString& Parameters)
{
	using namespace ScfgMobileTests;
	FWorld W;
	W.bRecordingServer = true;  // a page server, if one were made, would be logged in W.Servers
	W.Platform->bMobile = true;
	FScfgFacade& S = W.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	FScfgCheckout& Ctl = W.Session(*this);
	scfg::checkout::CheckoutController& Core = Ctl.Core();
	TestEqual(TEXT("QR and manual, no browser"), Joined(Core.methods()), FString(TEXT("qr,manual")));
	Core.choose_method("qr");
	TestEqual(TEXT("the QR step"), ScfgString::FromUtf8(Core.step()), FString(TEXT("qr")));
	const FString Uri = Link(Ctl);
	TestEqual(TEXT("the EIP-681 request for this session"), Uri,
		FString::Printf(TEXT("ethereum:%s@42161/transfer?address=%s&uint256=%lld"), *ScfgString::FromUtf8(UsdcArbitrum()),
			UTF8_TO_TCHAR(kDev), static_cast<long long>(Core.session().as_dict().get("units").as_int64())));
	Ctl.OpenWalletApp();
	SameLines(*this, TEXT("Open wallet app hands it to the platform"), W.Platform->Opened, TArray<FString>{Uri});
	Core.mark_sent();
	TestEqual(TEXT("I've sent it: waiting"), ScfgString::FromUtf8(Core.step()), FString(TEXT("waiting")));
	// The player is back from the wallet: no callback, the worker's passes find the transfer.
	W.PayOnChain(Ctl);
	TestEqual(TEXT("found by polling and confirmed"), ScfgString::FromUtf8(Core.state()), FString(TEXT("paid")));
	W.Host->CloseSheet(-1, TEXT("finish"));
	W.Pump();
	TestTrue(TEXT("Pay resolves paid"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Paid);
	TestTrue(TEXT("chapter2 is owned"), S.Owns(TEXT("chapter2")));
	TestEqual(TEXT("no page server was made"), W.Servers->Num(), 0);
	TestEqual(TEXT("nothing was reported"), W.Reports().Errors.Num() + W.Reports().Warnings.Num(), 0);
	return true;
}

SCFG_MOBILE_TEST(FScfgMobileSolanaLinkTest, "TheSolanaWalletLinkIsASolanaPayRequest")

bool FScfgMobileSolanaLinkTest::RunTest(const FString& Parameters)
{
	using namespace ScfgMobileTests;
	FWorld W;
	W.bRecordingServer = true;
	W.Platform->bMobile = true;
	FScfgFacade& S = W.Make();
	FScfgOptions Options;
	Options.SolanaWallet = UTF8_TO_TCHAR(kSolanaDev);
	Options.Chains = {TEXT("solana")};
	TestFalse(TEXT("configure"), S.Configure(Options).IsSet());
	AddChapter2(*this, S);
	W.Start();
	W.Pay(EScfgPayKind::Unlock, Unlock());
	FScfgCheckout& Ctl = W.Host->CheckoutOf(-1);
	W.Pass();  // one network: the session starts at once
	scfg::checkout::CheckoutController& Core = Ctl.Core();
	if (!TestEqual(TEXT("a Solana session"), ScfgString::FromUtf8(Core.family().value_or("")), FString(TEXT("solana"))) ||
		!TestFalse(TEXT("started"), SessionIdOf(Ctl).IsEmpty()))
	{
		return false;
	}
	TestEqual(TEXT("QR and manual, no browser"), Joined(Core.methods()), FString(TEXT("qr,manual")));
	const std::string Mint = scfg::chains::MAINNET().find("solana")->tokens.find("USDC")->address;
	const FString Uri = Link(Ctl);
	const FString Reference = ScfgString::FromUtf8(Core.session().as_dict().get("reference").as_str());
	TestTrue(*FString::Printf(TEXT("the Solana Pay request (%s)"), *Uri),
		Uri.StartsWith(FString::Printf(TEXT("solana:%s?amount=4.9937"), UTF8_TO_TCHAR(kSolanaDev))) &&
		Uri.Contains(TEXT("spl-token=") + ScfgString::FromUtf8(Mint)) && Uri.Contains(TEXT("reference=") + Reference));
	Ctl.OpenWalletApp();
	SameLines(*this, TEXT("Open wallet app hands it to the platform"), W.Platform->Opened, TArray<FString>{Uri});
	TestEqual(TEXT("no page server was made"), W.Servers->Num(), 0);
	SameLines(*this, TEXT("the fake node answered every request"), W.Chains->Problems, TArray<FString>());
	return true;
}

SCFG_MOBILE_TEST(FScfgMobileSheetTest, "OnAPhoneTheQrStepOffersTheWalletApp")

bool FScfgMobileSheetTest::RunTest(const FString& Parameters)
{
	using namespace ScfgMobileTests;
	using namespace ScfgSheetTests;
	FSheetRig R(*this);
	if (!R.bReady)
	{
		return false;
	}
	R.W.Platform->bMobile = true;
	FScfgFacade& S = R.Started(*this);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = R.W.Pay(EScfgPayKind::Unlock, Unlock());
	R.Frame(2);
	const TSharedPtr<SScfgPaymentSheet> Sheet = R.Sheet();
	if (!TestTrue(TEXT("the sheet"), Sheet.IsValid()))
	{
		return false;
	}
	const FScfgCheckoutController& C = Sheet->GetController();
	FScfgCheckout& Ctl = C.GetCheckout().Get();
	TestTrue(TEXT("the screen's controller says mobile"), C.IsMobile());
	TestTrue(TEXT("Down reaches Arbitrum"), R.KeyTo(*Sheet, TEXT("network_arbitrum"), EKeys::Down));
	R.Key(EKeys::Enter);
	R.Pass();
	TestEqual(TEXT("the method step"), Sheet->ShownStep(), FString(TEXT("method")));
	TestFalse(TEXT("no browser method on a phone"), Sheet->IsShown(TEXT("method_browser")));
	TestTrue(TEXT("the phone wallet and the manual transfer"),
		Sheet->IsShown(TEXT("method_qr")) && Sheet->IsShown(TEXT("method_manual")));
	TestTrue(TEXT("Up reaches the phone wallet, the first method"), R.KeyTo(*Sheet, TEXT("method_qr"), EKeys::Up));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("the phone wallet step"), Sheet->ShownStep(), FString(TEXT("qr")));
	TestTrue(TEXT("Open wallet app"), Sheet->IsShown(TEXT("wallet_app")));
	TestEqual(TEXT("its text"), Sheet->TextOf(TEXT("wallet_app")), ScfgTexts::Get(TEXT("open_wallet_app")));
	TestTrue(TEXT("Up reaches it"), R.KeyTo(*Sheet, TEXT("wallet_app"), EKeys::Up));
	R.Key(EKeys::Enter);
	const FString Uri = Link(Ctl);
	TestTrue(TEXT("an EIP-681 request"), Uri.StartsWith(TEXT("ethereum:")));
	SameLines(*this, TEXT("Enter opens it"), R.W.Platform->Opened, TArray<FString>{Uri});
	TestEqual(TEXT("the sheet stays on the QR step"), Sheet->ShownStep(), FString(TEXT("qr")));
	TestTrue(TEXT("Down reaches I've sent it"), R.KeyTo(*Sheet, TEXT("forward"), EKeys::Down));
	R.Key(EKeys::Enter);
	TestEqual(TEXT("the waiting step"), Sheet->ShownStep(), FString(TEXT("waiting")));
	R.W.PayOnChain(Ctl);
	R.Frame(2);
	TestEqual(TEXT("found by polling: the done step"), Sheet->ShownStep(), FString(TEXT("done")));
	R.Key(EKeys::Enter);
	R.Frame(2);
	TestTrue(TEXT("Pay resolves paid"), Box->IsSet() && (*Box)->Status == EScfgPaymentStatus::Paid);
	TestTrue(TEXT("chapter2 is owned"), S.Owns(TEXT("chapter2")));
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
