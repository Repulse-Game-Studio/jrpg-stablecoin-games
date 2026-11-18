// Copyright 2026 Repulse
// No Python twin: the SCFG project settings (Public/ScfgSettings.h), Scfg.Glue.Settings.<name>, the twins of Unity's
// SettingsTests (unity-p3-5: unity/Packages/com.repulse.scfg/Tests/Editor/SettingsTests.cs) for what both have:
//   AValidAssetPassesValidate                 -> AValidSettingsPassValidate
//   ValidateShowsTheCoresCodeAndMessage       -> EachInvalidFieldYieldsTheCoresCode (every field, and the start agrees)
//   ValidateChecksEveryItem                   -> ValidateReportsEveryProblem
//   ApplyConfiguresAndAddsTheItems            -> TheSettingsAreAppliedAtStart
//   ApplyAtStartupUsesTheFoundAsset           -> InitializeAppliesTheProjectSettings, EmptySettingsAreNotApplied
//   ABadAssetIsReportedNotThrown              -> BadSettingsAreReportedNotApplied
//   AConfigureFromCodeAfterTheAssetWins       -> ACodeConfigureWins
//   AddChainSetsTheAssetsConfigurationAside...-> ACodeAddChainSetsTheSettingsAside
//   AddChainAfterTheAssetsPaymentsStarted...  -> ACodeAddChainSetsTheSettingsAside (its second half)
// Unity's logo-sprite, asset-finding and build-preload tests have no counterpart: the logo is a path, and the project
// has exactly one settings object (the class default, in DefaultGame.ini).
// C++ only:
//   ValidatePostsToTheMessageLog              -> the editor module's Validate action (ScfgSettingsValidation.h)
//   TheProjectSettingsPanelHasAValidateButton -> Project Settings > Plugins > SCFG is the class default, and a details
//       view of it (painted under -nullrhi) shows the Validate button and Configure()'s options as rows
//
// The subsystem is started as Initialize starts it, with a settings object of the test's (or, in
// InitializeAppliesTheProjectSettings, the project's class default through a real game instance), over FWorld's
// fakes and quiet reports.
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

#include "ScfgFacade.h"
#include "ScfgFacadeTestKit.h"
#include "ScfgSettings.h"
#include "ScfgString.h"
#include "ScfgSubsystem.h"
#include "ScfgSubsystemAccess.h"
#include "ScfgSubsystemTestAccess.h"

#if WITH_EDITOR
#include "Framework/Application/SlateApplication.h"
#include "IDetailsView.h"
#include "IMessageLogListing.h"
#include "Input/HittestGrid.h"
#include "ISettingsCategory.h"
#include "ISettingsContainer.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Layout/WidgetPath.h"
#include "Logging/TokenizedMessage.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Rendering/DrawElements.h"
#include "ScfgSettingsValidation.h"
#include "Widgets/SVirtualWindow.h"
#include "Widgets/Text/STextBlock.h"
#endif

#include <functional>

#if WITH_DEV_AUTOMATION_TESTS

#define SCFG_SETTINGS_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.Settings." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ScfgSettingsTests
{

using namespace ScfgFacadeTests;

FScfgChain MyL2()
{
	FScfgChain C;
	C.Name = TEXT("My L2");
	C.ChainId = 4242;
	C.Rpc = {TEXT("https://rpc.example.org")};
	C.Tokens.Add(TEXT("USDC"), TEXT("0x0000000000000000000000000000000000000001"));
	C.Confirmations = 3;
	C.ExplorerTx = TEXT("https://explorer.example.org/tx/{}");
	return C;
}

FScfgSettingsChain ChainRow(const FString& Key, const FScfgChain& Chain = MyL2())
{
	FScfgSettingsChain Row;
	Row.Key = Key;
	Row.Chain = Chain;
	return Row;
}

FScfgSettingsItem ItemRow(const FString& Id, int32 Code, const FString& Price, const FString& Title)
{
	FScfgSettingsItem Row;
	Row.Id = Id;
	Row.Code = Code;
	Row.Price = Price;
	Row.Title = Title;
	return Row;
}

// New settings without the project's values: what UScfgSettings() makes (testnet on, nothing else). NewObject copies
// the class default, which holds the project's own settings from its DefaultGame.ini (the dev project has the
// showcase's: unreal/Project/ScfgDev/Config/DefaultGame.ini), so each test starts from these instead.
TStrongObjectPtr<UScfgSettings> Blank()
{
	TStrongObjectPtr<UScfgSettings> S(NewObject<UScfgSettings>(GetTransientPackage()));
	S->Options = UScfgSettings::NewOptions();
	S->Chains.Reset();
	S->Items.Reset();
	return S;
}

// Unity's Make(): valid settings (a mainnet wallet on Arbitrum and the developer's own chain, one item), edited.
TStrongObjectPtr<UScfgSettings> Make(TFunction<void(UScfgSettings&)> Edit = nullptr)
{
	TStrongObjectPtr<UScfgSettings> S = Blank();
	S->Options.Wallet = UTF8_TO_TCHAR(kDev);
	S->Options.bTestnet = false;
	S->Options.Chains = {TEXT("arbitrum"), TEXT("myl2")};
	S->Chains = {ChainRow(TEXT("myl2"))};
	S->Items = {ItemRow(TEXT("chapter2"), 1, TEXT("4.99"), TEXT("Chapter 2"))};
	if (Edit)
	{
		Edit(*S);
	}
	return S;
}

// A subsystem (its game instance never initialised) whose facade has the world's hooks, started as Initialize starts
// it, with Settings.
struct FGame
{
	TStrongObjectPtr<UGameInstance> Instance;
	TStrongObjectPtr<UScfgSubsystem> Scfg;

	FGame(FWorld& W, const UScfgSettings* Settings)
	{
		Instance.Reset(NewObject<UGameInstance>());
		Scfg.Reset(NewObject<UScfgSubsystem>(Instance.Get()));
		FScfgSubsystemTestAccess::Start(*Scfg, W.Options(), Settings);
		W.Facade = FScfgSubsystemAccess::Facade(*Scfg)->AsShared();
		Scfg->SetSheetHost(W.Host);
	}
	FGame(const FGame&) = delete;
	FGame& operator=(const FGame&) = delete;
	~FGame() { Scfg->Deinitialize(); }

	FScfgFacade& F() const { return *FScfgSubsystemAccess::Facade(*Scfg); }
};

FScfgOptions CodeOptions(const TArray<FString>& Chains)
{
	FScfgOptions O;
	O.Wallet = UTF8_TO_TCHAR(kDev);
	O.Chains = Chains;
	return O;
}

FString Lines(const TArray<FString>& All)
{
	return FString::Join(All, TEXT(" | "));
}

void WriteFile(const FString& Path, const std::string& Bytes)
{
	TArray<uint8> Data(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size()));
	FFileHelper::SaveArrayToFile(Data, *Path);
}

}  // namespace ScfgSettingsTests

SCFG_SETTINGS_TEST(FScfgSettingsValidTest, "AValidSettingsPassValidate")

bool FScfgSettingsValidTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	const TStrongObjectPtr<UScfgSettings> S = Make();
	const TArray<FScfgSettingsProblem> Problems = S->Validate();
	TestEqual(TEXT("no problem"), Problems.Num(), 0);
	TestEqual(TEXT("the verdict"), Lines(S->Verdict(Problems)),
		FString(TEXT("Valid: the configuration, 1 chain and 1 item pass the checks SCFG runs when the game starts.")));
	S->Options.bTestnet = true;
	S->Options.Chains = {TEXT("arbitrum")};
	S->Chains.Reset();
	S->Items.Add(ItemRow(TEXT("chapter3"), 2, TEXT("2.50"), TEXT("Chapter 3")));
	TestEqual(TEXT("testnet is valid too"), S->Validate().Num(), 0);
	TestEqual(TEXT("with a reminder"), Lines(S->Verdict({})),
		FString(TEXT("Valid: the configuration, 0 chains and 2 items pass the checks SCFG runs when the game starts. | "
					 "Testnet is on: players pay with test tokens. Turn it off for the release.")));
	// New settings copy the class default: the constructor's values (NewOptions(): testnet on, as Unity's asset), then
	// the project's DefaultGame.ini over them. In this project the ini has the showcase's settings (testnet on too), so
	// the constructor's own default is asserted through NewOptions(), which it uses, and not through a new object.
	const TStrongObjectPtr<UScfgSettings> Fresh(NewObject<UScfgSettings>(GetTransientPackage()));
	const UScfgSettings* Project = GetDefault<UScfgSettings>();
	TestTrue(TEXT("new settings start on testnet, as Unity's asset"), UScfgSettings::NewOptions().bTestnet);
	TestFalse(TEXT("where Configure()'s own default is mainnet"), FScfgOptions().bTestnet);
	FScfgOptions Rest = UScfgSettings::NewOptions();
	Rest.bTestnet = false;
	const FScfgOptions Defaults;
	TestTrue(TEXT("and nothing else differs from Configure()'s defaults"),
		FScfgOptions::StaticStruct()->CompareScriptStruct(&Rest, &Defaults, PPF_None));
	TestTrue(TEXT("and copy the project's settings"), Fresh->Options.Wallet == Project->Options.Wallet &&
		Fresh->Items.Num() == Project->Items.Num() && Fresh->Chains.Num() == Project->Chains.Num());
	TestTrue(TEXT("blank settings are empty"), Blank()->IsEmpty() && !Blank()->HasRows());
	TestEqual(TEXT("where they are"), Fresh->GetContainerName().ToString() + TEXT("/") + Fresh->GetCategoryName().ToString() +
		TEXT("/") + Fresh->GetSectionName().ToString(), FString(TEXT("Project/Plugins/SCFG")));
	TestEqual(TEXT("saved to DefaultGame.ini"), UScfgSettings::StaticClass()->ClassConfigName, FName(TEXT("Game")));
	TestTrue(TEXT("the project's file, not a user's"), UScfgSettings::StaticClass()->HasAnyClassFlags(CLASS_DefaultConfig));
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsFieldsTest, "EachInvalidFieldYieldsTheCoresCode")

// Every field of the settings that the core checks: Validate's first problem is the core's ConfigError code, and the
// start (ApplySettings over the same values) fails with the same code.
bool FScfgSettingsFieldsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	struct FCase
	{
		const TCHAR* Field;
		TFunction<void(UScfgSettings&)> Edit;
		const TCHAR* Code;
	};
	const TArray<FCase> Cases = {
		{TEXT("Wallet"), [](UScfgSettings& S) { S.Options.Wallet = TEXT("0x123"); }, TEXT("address.invalid")},
		{TEXT("SolanaWallet"), [](UScfgSettings& S) { S.Options.SolanaWallet = TEXT("not base58 0OIl"); }, TEXT("solana_address.invalid")},
		{TEXT("PreviousWallets"), [](UScfgSettings& S) { S.Options.PreviousWallets = {TEXT("0xnothex")}; }, TEXT("address.invalid")},
		{TEXT("Chains"), [](UScfgSettings& S) { S.Options.Chains = {TEXT("nope")}; }, TEXT("chains.unknown")},
		{TEXT("Chains twice"), [](UScfgSettings& S) { S.Options.Chains = {TEXT("base"), TEXT("base")}; }, TEXT("chains.duplicate")},
		{TEXT("Tokens"), [](UScfgSettings& S) { S.Options.Tokens = {TEXT("DAI")}; }, TEXT("tokens.unknown")},
		{TEXT("Methods off"), [](UScfgSettings& S)
			{
				S.Options.Methods.Browser.bEnabled = false;
				S.Options.Methods.Qr.bEnabled = false;
				S.Options.Methods.Manual.bEnabled = false;
			}, TEXT("methods.none_enabled")},
		{TEXT("Methods.Browser.TimeoutMin"), [](UScfgSettings& S) { S.Options.Methods.Browser.TimeoutMin = 0; }, TEXT("methods.bad_timeout")},
		{TEXT("Methods.Qr.Chains"), [](UScfgSettings& S) { S.Options.Methods.Qr.Chains = {TEXT("polygon")}; }, TEXT("methods.qr_disabled_chain")},
		{TEXT("Onramp.Url"), [](UScfgSettings& S) { S.Options.Onramp.Url = TEXT("http://buy.example"); }, TEXT("onramp.bad_url")},
		{TEXT("Onramp.Chains"), [](UScfgSettings& S) { S.Options.Onramp.Chains = {TEXT("nope")}; }, TEXT("onramp.disabled_chain")},
		{TEXT("RpcUrls"), [](UScfgSettings& S)
			{
				FScfgRpcUrls Urls;
				Urls.Urls = {TEXT("http://example.org")};
				S.Options.RpcUrls.Add(TEXT("arbitrum"), Urls);
			}, TEXT("rpc.not_https")},
		{TEXT("RpcUrls key"), [](UScfgSettings& S)
			{
				FScfgRpcUrls Urls;
				Urls.Urls = {TEXT("https://x.example.org")};
				S.Options.RpcUrls.Add(TEXT("nope"), Urls);
			}, TEXT("rpc_urls.unknown_chain")},
		{TEXT("Confirmations"), [](UScfgSettings& S) { S.Options.Confirmations.Add(TEXT("arbitrum"), 0); }, TEXT("confirmations.invalid")},
		{TEXT("Logo type"), [](UScfgSettings& S) { S.Options.Logo = TEXT("images/logo.svg"); }, TEXT("logo.bad_type")},
		{TEXT("Logo path"), [](UScfgSettings& S) { S.Options.Logo = TEXT("../logo.png"); }, TEXT("logo.bad_path")},
		{TEXT("Logo file"), [](UScfgSettings& S) { S.Options.Logo = TEXT("Content/missing.png"); }, TEXT("logo.file_missing")},
		{TEXT("Accent"), [](UScfgSettings& S) { S.Options.Accent = TEXT("red"); }, TEXT("accent.invalid")},
		{TEXT("chain Key"), [](UScfgSettings& S) { S.Chains.Add(ChainRow(TEXT("Not A Key"))); }, TEXT("chain.invalid_key")},
		{TEXT("chain Key built in"), [](UScfgSettings& S)
			{
				FScfgChain C = MyL2();
				C.ChainId = 4343;
				S.Chains.Add(ChainRow(TEXT("base"), C));
			}, TEXT("chain.is_builtin")},
		{TEXT("chain ChainId"), [](UScfgSettings& S)
			{
				FScfgChain C = MyL2();
				C.ChainId = 0;
				S.Chains.Add(ChainRow(TEXT("other"), C));
			}, TEXT("int.below_minimum")},
		{TEXT("chain Rpc"), [](UScfgSettings& S)
			{
				FScfgChain C = MyL2();
				C.Rpc = {TEXT("http://example.org")};
				S.Chains.Add(ChainRow(TEXT("other"), C));
			}, TEXT("rpc.not_https")},
		{TEXT("chain ExplorerTx"), [](UScfgSettings& S)
			{
				FScfgChain C = MyL2();
				C.ExplorerTx = TEXT("http://explorer.example.org/tx/{}");
				S.Chains.Add(ChainRow(TEXT("other"), C));
			}, TEXT("chain.bad_explorer_tx")},
		{TEXT("chain FeeHint"), [](UScfgSettings& S)
			{
				FScfgChain C = MyL2();
				C.FeeHint = TEXT("medium");
				S.Chains.Add(ChainRow(TEXT("other"), C));
			}, TEXT("chain.bad_fee_hint")},
		{TEXT("chain twice"), [](UScfgSettings& S) { S.Chains.Add(ChainRow(TEXT("myl2"))); }, TEXT("chain.duplicate")},
		{TEXT("item Id"), [](UScfgSettings& S) { S.Items[0].Id = TEXT("Chapter"); }, TEXT("item.invalid_id")},
		{TEXT("item Code"), [](UScfgSettings& S) { S.Items[0].Code = 0; }, TEXT("item.invalid_code")},
		{TEXT("item Price"), [](UScfgSettings& S) { S.Items[0].Price = TEXT("2.5.0"); }, TEXT("item.invalid_price")},
		{TEXT("item no Price"), [](UScfgSettings& S) { S.Items[0].Price.Reset(); }, TEXT("item.invalid_price")},
		{TEXT("item Token"), [](UScfgSettings& S) { S.Items[0].Token = TEXT("DAI"); }, TEXT("token.unavailable")},
		{TEXT("item Title"), [](UScfgSettings& S) { S.Items[0].Title.Reset(); }, TEXT("item.invalid_title")},
		{TEXT("item MinPrice"), [](UScfgSettings& S) { S.Items[0].MinPrice = TEXT("9"); }, TEXT("item.min_price_above_price")},
		{TEXT("item Code twice"), [](UScfgSettings& S) { S.Items.Add(ItemRow(TEXT("chapter3"), 1, TEXT("2"), TEXT("C3"))); }, TEXT("item.duplicate_code")},
		{TEXT("item Id twice"), [](UScfgSettings& S) { S.Items.Add(ItemRow(TEXT("chapter2"), 2, TEXT("2"), TEXT("C2"))); }, TEXT("item.duplicate_id")},
	};
	for (const FCase& Case : Cases)
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make(Case.Edit);
		const TArray<FScfgSettingsProblem> Problems = S->Validate(W.Dir);
		if (!TestTrue(FString(Case.Field) + TEXT(": a problem"), Problems.Num() > 0))
		{
			continue;
		}
		TestEqual(FString(Case.Field) + TEXT(": the core's code"), Problems[0].Code, FString(Case.Code));
		TestFalse(FString(Case.Field) + TEXT(": and its message"), Problems[0].Message.IsEmpty());
		TestTrue(FString(Case.Field) + TEXT(": the verdict line"),
			S->Verdict(Problems)[0] == FString(Case.Code) + TEXT(": ") + Problems[0].Message);
		// The start runs the same calls: the same code, and nothing the core refused is applied.
		FScfgFacade& F = W.Make();
		const TOptional<scfg::Err> Applied = F.ApplySettings(S->Options, S->Chains, S->Items);
		TestEqual(FString(Case.Field) + TEXT(": the start fails the same way"),
			Applied.IsSet() ? ScfgString::FromUtf8(Applied->code.empty() ? Applied->kind : Applied->code) : FString(),
			FString(Case.Code));
	}
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsEveryProblemTest, "ValidateReportsEveryProblem")

bool FScfgSettingsEveryProblemTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	const TStrongObjectPtr<UScfgSettings> S = Make([](UScfgSettings& E)
	{
		FScfgChain Bad = MyL2();
		Bad.ExplorerTx = TEXT("https://explorer.example.org/tx/");
		E.Chains.Add(ChainRow(TEXT("other"), Bad));
		E.Items.Add(ItemRow(TEXT("chapter3"), 1, TEXT("2"), TEXT("Chapter 3")));
		E.Items.Add(ItemRow(TEXT("chapter4"), 4, TEXT("2.5.0"), TEXT("Chapter 4")));
		E.Items.Add(ItemRow(TEXT("chapter5"), 5, TEXT("1"), TEXT("Chapter 5")));
	});
	TArray<FScfgSettingsProblem> Problems = S->Validate();
	TArray<FString> Got;
	for (const FScfgSettingsProblem& P : Problems)
	{
		Got.Add(P.Code + TEXT(" @ ") + P.Message.Left(P.Message.Find(TEXT(": ")) + 1));
	}
	SameLines(*this, TEXT("each chain row, then the configuration, then each item"), Got,
		{TEXT("chain.bad_explorer_tx @ chain \"other\":"), TEXT("item.duplicate_code @ item \"chapter3\":"),
			TEXT("item.invalid_price @ item \"chapter4\":")});
	// A configuration that does not build: the items cannot be checked.
	S->Options.Wallet = TEXT("0x123");
	Problems = S->Validate();
	TestEqual(TEXT("the chain and the configuration only"), Problems.Num(), 2);
	TestTrue(TEXT("the configuration's"), Problems.Num() == 2 && Problems[1].Code == TEXT("address.invalid") &&
		!Problems[1].Message.StartsWith(TEXT("item")));
	// Empty settings (testbed F6): no wallet is the opt-out, not a problem; the verdict says they are valid, not
	// applied at start, and on testnet. The rest is still checked (with stand-in wallets).
	const TStrongObjectPtr<UScfgSettings> Empty = Blank();
	Problems = Empty->Validate();
	TestEqual(TEXT("empty: no problem"), Problems.Num(), 0);
	const TArray<FString> Verdict = Empty->Verdict(Problems);
	TestTrue(TEXT("valid, not applied, testnet"), Verdict.Num() == 3 && Verdict[0].StartsWith(TEXT("Valid: ")) &&
		Verdict[1].Contains(TEXT("not applied")) && Verdict[2].StartsWith(TEXT("Testnet is on")));
	Empty->Options.Chains = {TEXT("nope")};
	Problems = Empty->Validate();
	TestTrue(TEXT("empty: the rest is still checked"), Problems.Num() == 1 && Problems[0].Code == TEXT("chains.unknown"));
	Empty->Options.Chains.Reset();
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsLogoPackagingTest, "ValidateWarnsWhenTheLogoWillNotBePackaged")

// PACKAGING (ScfgSettings.h): a logo ships only from a folder under Content/ that is in Additional Non-Asset Directories
// to Package (or To Copy), relative to Content/; anywhere else Validate warns (Configure() would fail in the packaged
// game), without counting it as a problem.
bool FScfgSettingsLogoPackagingTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	FWorld W;
	auto Warning = [&W](const TCHAR* Logo, const TArray<FString>& Staged)
	{
		return UScfgSettings::LogoPackagingWarning(Logo, W.Dir, Staged);
	};
	TestEqual(TEXT("no logo, no warning"), Warning(TEXT(""), {}), FString());
	const FString Outside = Warning(TEXT("Branding/logo.png"), {TEXT("Branding")});
	TestTrue(TEXT("outside Content/: never packaged"), Outside.StartsWith(TEXT("Logo \"Branding/logo.png\" is not in a "
		"folder under Content/, so a packaged game cannot include it and its Configure() fails with logo.file_missing")));
	TestTrue(TEXT("naming the setting"), Outside.EndsWith(TEXT("add that folder to Project Settings > Packaging > "
		"Additional Non-Asset Directories to Package.")));
	TestTrue(TEXT("Content/ itself is no folder to stage"), Warning(TEXT("Content/logo.png"), {}).Contains(TEXT("is not in a folder under Content/")));
	TestTrue(TEXT("a path that leaves Content/"), Warning(TEXT("Content/../Branding/logo.png"), {TEXT("Branding")})
		.Contains(TEXT("is not in a folder under Content/")));
	const FString Unstaged = Warning(TEXT("Content/Branding/logo.png"), {TEXT("Other")});
	TestEqual(TEXT("under Content/ in a folder that is not staged"), Unstaged, FString(TEXT("Logo \"Content/Branding/logo.png\" "
		"is not packaged: its folder is not in Project Settings > Packaging > Additional Non-Asset Directories to Package, "
		"so a packaged game does not include it and its Configure() fails with logo.file_missing. Add \"Branding\" there "
		"(the folders are relative to Content/).")));
	TestTrue(TEXT("a nested folder is named whole"), Warning(TEXT("Content/Branding/Ui/logo.png"), {})
		.Contains(TEXT("Add \"Branding/Ui\" there")));
	TestEqual(TEXT("its folder staged: no warning"), Warning(TEXT("Content/Branding/logo.png"), {TEXT("Branding")}), FString());
	TestEqual(TEXT("written with a slash"), Warning(TEXT("Content/Branding/logo.png"), {TEXT("Branding/")}), FString());
	TestEqual(TEXT("a parent folder staged"), Warning(TEXT("Content/Branding/Ui/logo.png"), {TEXT("Branding")}), FString());
	TestFalse(TEXT("a folder whose name only starts the same is another folder"),
		Warning(TEXT("Content/Branding/logo.png"), {TEXT("Brand")}).IsEmpty());
	TestFalse(TEXT("an empty entry stages nothing"), Warning(TEXT("Content/Branding/logo.png"), {TEXT("")}).IsEmpty());

	// The settings' own: Warnings() checks Options.Logo against the project's staged folders; Verdict() shows each after
	// the valid line, before the testnet note; Validate() itself is not refused for it.
	WriteFile(W.Dir / TEXT("Content/NotStagedAnywhere/logo.png"), std::string("\x89PNG\r\n\x1a\n", 8));
	const TStrongObjectPtr<UScfgSettings> S = Make([](UScfgSettings& E)
	{
		E.Options.Logo = TEXT("Content/NotStagedAnywhere/logo.png");
		E.Options.bTestnet = true;
		E.Options.Chains = {TEXT("arbitrum")};
		E.Chains.Reset();
	});
	TestEqual(TEXT("the logo exists: valid"), S->Validate(W.Dir).Num(), 0);
	const TArray<FString> Warned = S->Warnings(W.Dir);
	TestTrue(TEXT("but not packaged: one warning"), Warned.Num() == 1 && Warned[0].Contains(TEXT("Add \"NotStagedAnywhere\" there")));
	const TArray<FString> Lines = S->Verdict({}, Warned);
	TestTrue(TEXT("the verdict: valid, the warning, the testnet note"), Lines.Num() == 3 && Lines[0].StartsWith(TEXT("Valid: ")) &&
		Lines[1] == Warned[0] && Lines[2].StartsWith(TEXT("Testnet is on")));
	S->Options.Logo.Reset();
	TestEqual(TEXT("no logo: no warning"), S->Warnings(W.Dir).Num(), 0);
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsAppliedTest, "TheSettingsAreAppliedAtStart")

bool FScfgSettingsAppliedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	FWorld W;
	WriteFile(W.Dir / TEXT("Content/logo.png"), std::string("\x89PNG\r\n\x1a\n", 8));
	const TStrongObjectPtr<UScfgSettings> S = Make([](UScfgSettings& E)
	{
		E.Options.Accent = TEXT("#7a5cff");
		E.Options.Logo = TEXT("Content/logo.png");
	});
	FGame G(W, S.Get());
	FScfgFacade& F = G.F();
	TestTrue(TEXT("configured from the settings"), F.GetConfig() != nullptr && F.IsConfigFromSettings());
	TestEqual(TEXT("their accent"), F.Accent(), FString(TEXT("#7a5cff")));
	TestEqual(TEXT("their logo"), F.GetLogoPath(), W.Dir / TEXT("Content/logo.png"));
	TestEqual(TEXT("their item"), F.ItemTitle(TEXT("chapter2")), FString(TEXT("Chapter 2")));
	TestEqual(TEXT("their chain"), F.ChainName(TEXT("myl2")), FString(TEXT("My L2")));
	TestTrue(TEXT("mainnet, as they say"), F.GetConfig() && !F.GetConfig()->testnet);
	TestFalse(TEXT("not owned"), G.Scfg->Owns(TEXT("chapter2")));
	SameLines(*this, TEXT("nothing reported"), W.Reports().Errors, {});
	SameLines(*this, TEXT("no warning"), W.Reports().Warnings, {});
	W.Pump();
	TestNotNull(TEXT("the first pump starts payments with them"), F.GetWorker());
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsInitializeTest, "InitializeAppliesTheProjectSettings")

// Initialize itself, through a real game instance: the project's settings (the class default, edited for the test and
// put back) are applied; without a wallet they are not.
bool FScfgSettingsInitializeTest::RunTest(const FString& Parameters)
{
	const FScfgBlankProjectSettings BlankSettings;  // put back afterwards
	UScfgSettings* Project = BlankSettings.Project;
	for (const bool bWallet : {true, false})
	{
		Project->Options = UScfgSettings::NewOptions();
		Project->Options.Wallet = bWallet ? FString(UTF8_TO_TCHAR(ScfgFacadeTests::kDev)) : FString();
		Project->Options.bTestnet = true;
		Project->Chains.Reset();
		Project->Items = {ScfgSettingsTests::ItemRow(TEXT("chapter2"), 1, TEXT("4.99"), TEXT("Chapter 2"))};
		if (!bWallet)
		{
			AddExpectedMessagePlain(TEXT("have chains or items but no wallet, so they are not applied"),
				ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		}
		TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GEngine));
		Instance->InitializeStandalone();
		UWorld* World = Instance->GetWorld();
		UScfgSubsystem* Scfg = Instance->GetSubsystem<UScfgSubsystem>();
		const FScfgFacade* F = Scfg ? FScfgSubsystemAccess::Facade(*Scfg) : nullptr;
		if (TestNotNull(TEXT("the game instance's subsystem"), F))
		{
			if (bWallet)
			{
				TestTrue(TEXT("Initialize applied the project's settings"), F->IsConfigFromSettings() &&
					F->GetConfig()->testnet && F->ItemTitle(TEXT("chapter2")) == TEXT("Chapter 2"));
			}
			else
			{
				TestNull(TEXT("no wallet: not applied"), F->GetConfig());
			}
		}
		Instance->Shutdown();
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	}
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsEmptyTest, "EmptySettingsAreNotApplied")

bool FScfgSettingsEmptyTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Blank();
		FGame G(W, S.Get());
		TestNull(TEXT("new settings: nothing is configured"), G.F().GetConfig());
		TestEqual(TEXT("silently"), W.Reports().Errors.Num() + W.Reports().Warnings.Num(), 0);
		TestEqual(TEXT("the game configures from code as before"), G.Scfg->Configure(CodeOptions({TEXT("base")})), FString());
	}
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make([](UScfgSettings& E) { E.Options.Wallet = TEXT("   "); });
		FGame G(W, S.Get());
		TestNull(TEXT("a blank wallet is no wallet"), G.F().GetConfig());
		SameLines(*this, TEXT("rows without a wallet: one warning"), W.Reports().Warnings,
			{TEXT("scfg: the SCFG settings (Project Settings > Plugins > SCFG) have chains or items but no wallet, so they "
				  "are not applied at start: set a wallet there, configure from code, or call ConfigureWithSettings(Wallet) "
				  "with a wallet supplied at runtime.")});
		TestEqual(TEXT("no error"), W.Reports().Errors.Num(), 0);
		TestEqual(TEXT("the game's own chain is not refused"), G.Scfg->AddChain(TEXT("myl2"), MyL2()), FString());
	}
	{
		FWorld W;
		FGame G(W, nullptr);
		TestNull(TEXT("no settings object (the tests' default)"), G.F().GetConfig());
	}
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsRuntimeWalletTest, "ARuntimeWalletAppliesTheSettingsChainsOptionsAndItems")

// Testbed F7: settings without a wallet (not applied at start) and a wallet supplied at runtime: ApplySettingsWith
// (ConfigureWithSettings for the project's settings) runs their chains, options and items with that wallet.
bool FScfgSettingsRuntimeWalletTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	FWorld W;
	const TStrongObjectPtr<UScfgSettings> S = Make([](UScfgSettings& E) { E.Options.Wallet.Reset(); });
	FGame G(W, S.Get());
	TestNull(TEXT("not applied at start"), G.F().GetConfig());
	TestEqual(TEXT("applied with the runtime wallet"),
		G.Scfg->ApplySettingsWith(*S, UTF8_TO_TCHAR(kDev), FString()), FString());
	if (TestNotNull(TEXT("configured"), G.F().GetConfig()))
	{
		TestEqual(TEXT("that wallet"), ScfgString::FromUtf8(G.F().GetConfig()->wallet.value_or("")),
			FString(UTF8_TO_TCHAR(kDev)));
	}
	TestEqual(TEXT("the settings' item"), G.F().ItemTitle(S->Items[0].Id), S->Items[0].Title);
	TestEqual(TEXT("no error"), W.Reports().Errors.Num(), 0);
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsWalletMessageTest, "TheMissingWalletNamesUnrealsFields")

// Testbed U7 in Unreal: wallet.missing names Options.Wallet / SolanaWallet, not the core's Python keywords.
bool FScfgSettingsWalletMessageTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	FWorld W;
	FGame G(W, nullptr);
	TestEqual(TEXT("the code"), G.Scfg->Configure(FScfgOptions()), FString(TEXT("wallet.missing")));
	TestTrue(TEXT("Unreal's names"), W.Reports().Errors.Num() == 1 &&
		W.Reports().Errors[0].Contains(TEXT("Options.SolanaWallet")) && !W.Reports().Errors[0].Contains(TEXT("wallet=")));
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsBadTest, "BadSettingsAreReportedNotApplied")

bool FScfgSettingsBadTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make([](UScfgSettings& E) { E.Options.Wallet = TEXT("0x123"); });
		FGame G(W, S.Get());
		TestNull(TEXT("a bad wallet: nothing applied"), G.F().GetConfig());
		TestEqual(TEXT("the misuse and the settings' line"), W.Reports().Errors.Num(), 2);
		TestTrue(TEXT("every line is scfg's"), W.Reports().Errors.Num() == 2 &&
			W.Reports().Errors[0].StartsWith(TEXT("scfg: ")) &&
			W.Reports().Errors[1] == TEXT("scfg: the SCFG settings (Project Settings > Plugins > SCFG) were not applied "
										  "(address.invalid). Press Validate there."));
		TestEqual(TEXT("their chain went too: the game's AddChain is not refused"),
			G.Scfg->AddChain(TEXT("myl2"), MyL2()), FString());
	}
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make([](UScfgSettings& E) { E.Chains[0].Chain.Rpc = {TEXT("http://x.example.org")}; });
		FGame G(W, S.Get());
		TestNull(TEXT("a bad chain: nothing applied"), G.F().GetConfig());
		TestTrue(TEXT("named"), W.Reports().Errors.Num() == 2 && W.Reports().Errors[1].Contains(
			TEXT("were not applied: chain \"myl2\" (rpc.not_https)")));
		TestEqual(TEXT("and no chain kept"), G.Scfg->Configure(CodeOptions({TEXT("myl2")})), FString(TEXT("chains.unknown")));
	}
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make([](UScfgSettings& E)
		{
			E.Items.Add(ItemRow(TEXT("chapter3"), 3, TEXT("x"), TEXT("Chapter 3")));
			E.Items.Add(ItemRow(TEXT("chapter4"), 4, TEXT("1"), TEXT("Chapter 4")));
		});
		FGame G(W, S.Get());
		TestTrue(TEXT("a bad item: the configuration stays"), G.F().IsConfigFromSettings());
		TestEqual(TEXT("with the items before it"), G.F().ItemTitle(TEXT("chapter2")), FString(TEXT("Chapter 2")));
		TestEqual(TEXT("not the bad one"), G.F().ItemTitle(TEXT("chapter3")), FString(TEXT("chapter3")));
		TestEqual(TEXT("nor the ones after it"), G.F().ItemTitle(TEXT("chapter4")), FString(TEXT("chapter4")));
		TestTrue(TEXT("named"), W.Reports().Errors.Num() == 2 && W.Reports().Errors[1] ==
			TEXT("scfg: the SCFG settings (Project Settings > Plugins > SCFG): item \"chapter3\" was not added "
				 "(item.invalid_price). Press Validate there."));
	}
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make();
		FScfgFacade& F = W.Make();
		TestFalse(TEXT("applied once"), F.ApplySettings(S->Options, S->Chains, S->Items).IsSet());
		const TOptional<scfg::Err> Again = F.ApplySettings(S->Options, S->Chains, S->Items);
		TestTrue(TEXT("only on a new facade"), Again.IsSet() && Again->kind == "RuntimeError");
	}
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsCodeConfigureTest, "ACodeConfigureWins")

// "Wins": a Configure() from code replaces everything the settings applied (options, logo, chains, items); a failing
// one leaves them; AddItem() adds to them; once payments started, Configure() is configure.after_start.
bool FScfgSettingsCodeConfigureTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make([](UScfgSettings& E) { E.Options.Accent = TEXT("#7a5cff"); });
		FGame G(W, S.Get());
		TestEqual(TEXT("a Configure from code"), G.Scfg->Configure(CodeOptions({TEXT("base")})), FString());
		FScfgFacade& F = G.F();
		TestFalse(TEXT("the configuration is the code's"), F.IsConfigFromSettings());
		TestTrue(TEXT("its chains"), F.GetConfig() && F.GetConfig()->chains == std::vector<std::string>{"base"});
		TestEqual(TEXT("the settings' accent went"), F.Accent(), ScfgString::FromUtf8(std::string(scfg::config::DEFAULT_ACCENT)));
		TestEqual(TEXT("their items went with it"), F.ItemTitle(TEXT("chapter2")), FString(TEXT("chapter2")));
		TestEqual(TEXT("their chain went with it"), F.ChainName(TEXT("myl2")), FString(TEXT("myl2")));
		TestEqual(TEXT("so the code names only its own chains"), G.Scfg->Configure(CodeOptions({TEXT("myl2")})),
			FString(TEXT("chains.unknown")));
		FScfgItem Item;
		Item.Code = 1;
		Item.Price = TEXT("1");
		Item.Token = TEXT("USDC");
		Item.Title = TEXT("From code");
		TestEqual(TEXT("the code's item"), G.Scfg->AddItem(TEXT("chapter2"), Item), FString());
		TestEqual(TEXT("is the one sold"), F.ItemTitle(TEXT("chapter2")), FString(TEXT("From code")));
		W.Pump();
		TestNotNull(TEXT("payments start with the code's configuration"), F.GetWorker());
	}
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make();
		FGame G(W, S.Get());
		FScfgFacade& F = G.F();
		FScfgOptions Bad = CodeOptions({});
		Bad.Wallet = TEXT("0x123");
		TestEqual(TEXT("a failing Configure from code"), G.Scfg->Configure(Bad), FString(TEXT("address.invalid")));
		TestTrue(TEXT("leaves the settings' configuration"), F.IsConfigFromSettings() &&
			F.ItemTitle(TEXT("chapter2")) == TEXT("Chapter 2") && F.ChainName(TEXT("myl2")) == TEXT("My L2"));
		FScfgItem Item;
		Item.Code = 3;
		Item.Price = TEXT("2.50");
		Item.Token = TEXT("USDC");
		Item.Title = TEXT("Chapter 3");
		TestEqual(TEXT("AddItem from code adds to the settings'"), G.Scfg->AddItem(TEXT("chapter3"), Item), FString());
		TestTrue(TEXT("both items"), F.ItemTitle(TEXT("chapter3")) == TEXT("Chapter 3") &&
			F.ItemTitle(TEXT("chapter2")) == TEXT("Chapter 2") && F.IsConfigFromSettings());
		Item.Code = 4;
		TestEqual(TEXT("an id the settings use is the core's error"), G.Scfg->AddItem(TEXT("chapter2"), Item),
			FString(TEXT("item.duplicate_id")));
		W.Pump();
		TestNotNull(TEXT("the first pump starts payments with the settings"), F.GetWorker());
		TestEqual(TEXT("a Configure from code after that"), G.Scfg->Configure(CodeOptions({TEXT("base")})),
			FString(TEXT("configure.after_start")));
		TestTrue(TEXT("says the settings started payments, and where to call it"),
			W.Reports().Errors.Num() == 3 && W.Reports().Errors[2].Contains(TEXT("once payments have started: the SCFG "
				"settings (Project Settings > Plugins > SCFG) configured SCFG when the game instance started")) &&
			W.Reports().Errors[2].Contains(TEXT("game instance's Init() or the first level's BeginPlay, or clear the wallet")));
		TestTrue(TEXT("the settings stay"), F.IsConfigFromSettings());
	}
	{
		FWorld W;
		FGame G(W, nullptr);
		TestEqual(TEXT("configured from code"), G.Scfg->Configure(CodeOptions({TEXT("base")})), FString());
		W.Pump();
		TestEqual(TEXT("a second Configure once payments started"), G.Scfg->Configure(CodeOptions({TEXT("base")})),
			FString(TEXT("configure.after_start")));
		TestTrue(TEXT("keeps the plain message (no settings involved)"), W.Reports().Errors.Num() == 1 &&
			W.Reports().Errors[0].EndsWith(TEXT("Configure() can't change the configuration once payments have started")));
	}
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsCodeAddChainTest, "ACodeAddChainSetsTheSettingsAside")

bool FScfgSettingsCodeAddChainTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make();
		FGame G(W, S.Get());
		FScfgFacade& F = G.F();
		FScfgChain Other = MyL2();
		Other.Name = TEXT("Other L2");
		Other.ChainId = 4343;
		TestEqual(TEXT("AddChain from code"), G.Scfg->AddChain(TEXT("other"), Other), FString());
		TestNull(TEXT("sets the settings' configuration aside"), F.GetConfig());
		TestTrue(TEXT("with a warning"), W.Reports().Warnings.Num() == 1 &&
			W.Reports().Warnings[0].EndsWith(TEXT("call Configure() next, from code.")));
		TestEqual(TEXT("the Configure from code that follows"),
			G.Scfg->Configure(CodeOptions({TEXT("arbitrum"), TEXT("other")})), FString());
		TestEqual(TEXT("has the code's chain"), F.ChainName(TEXT("other")), FString(TEXT("Other L2")));
		TestEqual(TEXT("not the settings'"), F.ChainName(TEXT("myl2")), FString(TEXT("myl2")));
		TestEqual(TEXT("nor their items"), F.ItemTitle(TEXT("chapter2")), FString(TEXT("chapter2")));
		TestEqual(TEXT("after a Configure from code: refused, as always"), G.Scfg->AddChain(TEXT("third"), Other),
			FString(TEXT("configure.after_add_chain")));
		TestTrue(TEXT("Ren'Py's message"), W.Reports().Errors.Num() == 1 &&
			W.Reports().Errors[0].EndsWith(TEXT("call AddChain() before Configure()")));
	}
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make();
		FGame G(W, S.Get());
		FScfgFacade& F = G.F();
		W.Pump();
		TestNotNull(TEXT("payments started with the settings"), F.GetWorker());
		TestEqual(TEXT("AddChain from code then"), G.Scfg->AddChain(TEXT("other"), MyL2()),
			FString(TEXT("configure.after_add_chain")));
		TestTrue(TEXT("says why"), W.Reports().Errors.Num() == 1 &&
			W.Reports().Errors[0].Contains(TEXT("call AddChain() before payments start: the SCFG settings")));
		TestTrue(TEXT("the settings stay"), F.IsConfigFromSettings() && F.ItemTitle(TEXT("chapter2")) == TEXT("Chapter 2"));
	}
	{
		FWorld W;
		const TStrongObjectPtr<UScfgSettings> S = Make();
		FGame G(W, S.Get());
		FScfgFacade& F = G.F();
		FScfgChain Typo = MyL2();
		Typo.Rpc = {TEXT("http://rpc.example.org")};  // a typo: http, not https
		TestEqual(TEXT("a chain from code that cannot be built is the core's error"), G.Scfg->AddChain(TEXT("other"), Typo),
			FString(TEXT("rpc.not_https")));
		TestTrue(TEXT("and leaves the settings' configuration in place, as a failing Configure does"),
			F.IsConfigFromSettings() && F.ItemTitle(TEXT("chapter2")) == TEXT("Chapter 2") &&
			F.ChainName(TEXT("myl2")) == TEXT("My L2"));
		TestEqual(TEXT("without the set-aside warning"), W.Reports().Warnings.Num(), 0);
		W.Pump();
		TestNotNull(TEXT("payments then start with the settings"), F.GetWorker());
	}
	return true;
}

#if WITH_EDITOR

SCFG_SETTINGS_TEST(FScfgSettingsMessageLogTest, "ValidatePostsToTheMessageLog")

// The Validate button's action without its notification: a new page of the "SCFG" Message Log.
bool FScfgSettingsMessageLogTest::RunTest(const FString& Parameters)
{
	using namespace ScfgSettingsTests;
	FMessageLogModule& Module = FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
	TestTrue(TEXT("the editor module registered the SCFG page"), Module.IsRegisteredLogListing(ScfgEditor::MessageLogName));
	const TSharedRef<IMessageLogListing> Listing = Module.GetLogListing(ScfgEditor::MessageLogName);
	auto Messages = [&Listing]()
	{
		TArray<FString> Out;
		for (const TSharedRef<FTokenizedMessage>& M : Listing->GetFilteredMessages())
		{
			const TCHAR* Severity = M->GetSeverity() == EMessageSeverity::Error ? TEXT("error")
				: M->GetSeverity() == EMessageSeverity::Warning ? TEXT("warning") : TEXT("info");
			Out.Add(FString(Severity) + TEXT(" ") + M->ToText().ToString());
		}
		return Out;
	};
	const TStrongObjectPtr<UScfgSettings> Valid = Make();
	TestEqual(TEXT("valid"), ScfgEditor::ValidateSettings(*Valid, /*bNotify=*/false).Num(), 0);
	SameLines(*this, TEXT("one info line"), Messages(),
		{TEXT("info Valid: the configuration, 1 chain and 1 item pass the checks SCFG runs when the game starts.")});
	const TStrongObjectPtr<UScfgSettings> Bad = Make([](UScfgSettings& E)
	{
		E.Options.bTestnet = true;
		E.Options.Chains = {TEXT("arbitrum")};
		E.Chains.Reset();
		E.Items[0].Price = TEXT("x");
		E.Items.Add(ItemRow(TEXT("chapter3"), 3, TEXT("1"), TEXT("")));
	});
	const TArray<FScfgSettingsProblem> Problems = ScfgEditor::ValidateSettings(*Bad, /*bNotify=*/false);
	const TArray<FString> Got = Messages();
	TestEqual(TEXT("two problems"), Problems.Num(), 2);
	TestEqual(TEXT("a new page: an error per problem, then the testnet reminder"), Got.Num(), 3);
	TestTrue(TEXT("the problems"), Problems.Num() == 2 && Got.Num() == 3 &&
		Got[0] == TEXT("error ") + Problems[0].Code + TEXT(": ") + Problems[0].Message &&
		Got[0].StartsWith(TEXT("error item.invalid_price: item \"chapter2\": ")) &&
		Got[1].StartsWith(TEXT("error item.invalid_title: item \"chapter3\": ")) &&
		Got[2] == TEXT("warning Testnet is on: players pay with test tokens. Turn it off for the release."));
	const TStrongObjectPtr<UScfgSettings> Empty = Blank();
	ScfgEditor::ValidateSettings(*Empty, /*bNotify=*/false);
	const TArray<FString> EmptyGot = Messages();
	TestTrue(TEXT("empty: valid, then the not-applied note and the testnet reminder (no wallet.missing)"),
		EmptyGot.Num() == 3 && EmptyGot[0].StartsWith(TEXT("info Valid: ")) &&
		EmptyGot[1].StartsWith(TEXT("warning No wallet is set")) && EmptyGot[2].StartsWith(TEXT("warning Testnet is on")));
	// A logo outside Content/ (in the project, so it validates): a warning line after the valid one.
	const FString Loose = FPaths::ProjectDir() / TEXT("Saved/ScfgTestsLooseLogo.png");
	WriteFile(Loose, std::string("\x89PNG\r\n\x1a\n", 8));
	const TStrongObjectPtr<UScfgSettings> Logo = Make([](UScfgSettings& E) { E.Options.Logo = TEXT("Saved/ScfgTestsLooseLogo.png"); });
	TestEqual(TEXT("a loose logo validates"), ScfgEditor::ValidateSettings(*Logo, /*bNotify=*/false).Num(), 0);
	IFileManager::Get().Delete(*Loose);
	const TArray<FString> LogoGot = Messages();
	TestTrue(TEXT("with the packaging warning after the valid line"), LogoGot.Num() == 2 && LogoGot[0].StartsWith(TEXT("info Valid: ")) &&
		LogoGot[1].StartsWith(TEXT("warning Logo \"Saved/ScfgTestsLooseLogo.png\" is not in a folder under Content/")));
	return true;
}

SCFG_SETTINGS_TEST(FScfgSettingsPanelTest, "TheProjectSettingsPanelHasAValidateButton")

bool FScfgSettingsPanelTest::RunTest(const FString& Parameters)
{
	UScfgSettings* Project = GetMutableDefault<UScfgSettings>();
	ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings"));
	const TSharedPtr<ISettingsContainer> Container = Settings ? Settings->GetContainer(TEXT("Project")) : nullptr;
	const TSharedPtr<ISettingsCategory> Category = Container ? Container->GetCategory(TEXT("Plugins")) : nullptr;
	const TSharedPtr<ISettingsSection> Section = Category ? Category->GetSection(TEXT("SCFG"), true) : nullptr;
	if (TestTrue(TEXT("Project Settings > Plugins > SCFG"), Section.IsValid()))
	{
		TestTrue(TEXT("edits the class default"), Section->GetSettingsObject().Get() == Project);
		TestEqual(TEXT("its name"), Section->GetDisplayName().ToString(), FString(TEXT("SCFG")));
	}
	if (!TestTrue(TEXT("Slate is up"), FSlateApplication::IsInitialized()))
	{
		return false;
	}
	// A details view of the class default, as the settings panel shows it, painted in a virtual window (which ticks
	// it: its rows are made then).
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs Args;
	Args.bAllowSearch = false;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	const TSharedRef<IDetailsView> View = PropertyEditor.CreateDetailView(Args);
	View->SetObject(Project, true);
	const TSharedRef<SVirtualWindow> Window = SNew(SVirtualWindow).Size(FVector2D(1200, 6000));
	Window->SetContent(View);
	FSlateApplication::Get().RegisterVirtualWindow(Window);
	ON_SCOPE_EXIT { FSlateApplication::Get().UnregisterVirtualWindow(Window); };
	for (int32 Frame = 0; Frame < 6; ++Frame)
	{
		const FVector2f Size = FVector2f(Window->GetSizeInScreen());
		Window->SlatePrepass(FSlateApplication::Get().GetApplicationScale());
		FHittestGrid& Grid = Window->GetHittestGrid();
		Grid.SetHittestArea(FVector2f::ZeroVector, Size);
		Grid.Clear();
		FPaintArgs Paint(nullptr, Grid, FVector2f::ZeroVector, Frame / 60.0, 1.f / 60.f);
		FSlateWindowElementList Elements(Window);
		Window->Paint(Paint, FGeometry::MakeRoot(Size, FSlateLayoutTransform()), FSlateRect(FVector2f::ZeroVector, Size),
			Elements, 0, FWidgetStyle(), true);
	}
	TSet<FString> Texts;
	TFunction<void(const TSharedRef<SWidget>&)> Walk = [&Walk, &Texts](const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == TEXT("STextBlock"))
		{
			Texts.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 I = 0; Children && I < Children->Num(); ++I)
		{
			Walk(Children->GetChildAt(I));
		}
	};
	Walk(Window);
	TestTrue(TEXT("the Validate button"), Texts.Contains(TEXT("Validate")) &&
		Texts.Contains(TEXT("Applied when the game instance starts (only with a wallet set), through AddChain, Configure "
							"and AddItem. A Configure() call from your code replaces these settings.")));
	TestTrue(TEXT("Configure()'s options as rows"), Texts.Contains(TEXT("Wallet")) && Texts.Contains(TEXT("Solana Wallet")) &&
		Texts.Contains(TEXT("Testnet")) && Texts.Contains(TEXT("Accent")) && Texts.Contains(TEXT("Logo")));
	TestFalse(TEXT("not one folded Options row"), Texts.Contains(TEXT("Options")));
	TestTrue(TEXT("the chains and items"), Texts.Contains(TEXT("Chains")) && Texts.Contains(TEXT("Items")));
	if (HasAnyErrors())
	{
		AddInfo(TEXT("texts: ") + FString::Join(Texts.Array(), TEXT(" | ")));
	}
	return true;
}

#endif  // WITH_EDITOR

#endif  // WITH_DEV_AUTOMATION_TESTS
