// No Python twin: see ScfgSettings.h.
#include "ScfgSettings.h"

#include "ScfgConvert.h"
#include "ScfgFacade.h"
#include "ScfgGuard.h"
#include "ScfgString.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

#include "Scfg/Config.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"

#include <utility>
#include <vector>

#define LOCTEXT_NAMESPACE "ScfgSettings"

namespace ScfgSettingsDetail
{

FScfgSettingsProblem Problem(const scfg::Err& Error, const FString& Row)
{
	FScfgSettingsProblem P;
	P.Code = ScfgGuard::ErrCode(Error);
	P.Message = Row + ScfgString::FromUtf8(Error.message);
	return P;
}

FString Count(int32 N, const TCHAR* One, const TCHAR* Many)
{
	return FString::Printf(TEXT("%d %s"), N, N == 1 ? One : Many);
}

}  // namespace ScfgSettingsDetail

FScfgItem FScfgSettingsItem::ToItem() const
{
	FScfgItem Item;
	Item.Code = Code;
	Item.Price = Price;
	Item.Token = Token;
	Item.Title = Title;
	Item.MinPrice = MinPrice;
	return Item;
}

UScfgSettings::UScfgSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("SCFG");
	Options = NewOptions();
}

FScfgOptions UScfgSettings::NewOptions()
{
	FScfgOptions New;
	// Unity's asset starts on testnet too: test payments first, then turn it off for the release.
	New.bTestnet = true;
	return New;
}

bool UScfgSettings::IsEmpty() const
{
	return Options.Wallet.TrimStartAndEnd().IsEmpty() && Options.SolanaWallet.TrimStartAndEnd().IsEmpty();
}

TArray<FScfgSettingsProblem> UScfgSettings::Validate(const FString& ProjectDir) const
{
	using namespace ScfgSettingsDetail;
	TArray<FScfgSettingsProblem> Problems;
	std::vector<scfg::config::CustomChain> Built;
	for (const FScfgSettingsChain& Row : Chains)
	{
		scfg::Result<scfg::chains::Chain> Chain = scfg::config::build_chain(ScfgConvert::ChainKwargs(Row.Key, Row.Chain));
		if (!Chain.ok())
		{
			Problems.Add(Problem(Chain.err(), FString::Printf(TEXT("chain \"%s\": "), *Row.Key)));
			continue;
		}
		Built.emplace_back(Chain.take());
	}
	scfg::Result<scfg::config::Config> Config = scfg::config::build_config(ScfgConvert::ConfigKwargs(Options), std::move(Built));
	if (!Config.ok())
	{
		Problems.Add(Problem(Config.err(), FString()));
		return Problems;
	}
	scfg::config::Config Cfg = Config.take();
	scfg::Result<FString> Logo = FScfgFacade::LogoFile(ProjectDir, Cfg.logo);
	if (!Logo.ok())
	{
		Problems.Add(Problem(Logo.err(), FString()));
	}
	for (const FScfgSettingsItem& Row : Items)
	{
		scfg::Result<scfg::models::Item> Added = scfg::config::add_item(
			Cfg, scfg::Json(ScfgString::ToUtf8(Row.Id)), ScfgConvert::ItemKwargs(Row.ToItem()));
		if (!Added.ok())
		{
			Problems.Add(Problem(Added.err(), FString::Printf(TEXT("item \"%s\": "), *Row.Id)));
		}
	}
	return Problems;
}

TArray<FString> UScfgSettings::Warnings(const FString& ProjectDir) const
{
	TArray<FString> Out;
	if (const FString Logo = LogoPackagingWarning(Options.Logo, ProjectDir, StagedDirectories()); !Logo.IsEmpty())
	{
		Out.Add(Logo);
	}
	return Out;
}

FString UScfgSettings::LogoPackagingWarning(const FString& Logo, const FString& ProjectDir, const TArray<FString>& Staged)
{
	if (Logo.IsEmpty())
	{
		return FString();
	}
	const FString Root = ProjectDir.IsEmpty() ? FPaths::ProjectDir() : ProjectDir;
	const FString Full = FPaths::ConvertRelativePathToFull(Root / Logo);
	const FString Content = FPaths::ConvertRelativePathToFull(Root / TEXT("Content"));
	const TCHAR* const Setting = TEXT("Project Settings > Packaging > Additional Non-Asset Directories to Package");
	FString Folder = FPaths::GetPath(Full);
	FPaths::MakePathRelativeTo(Folder, *(Content + TEXT("/")));
	if (!FPaths::IsUnderDirectory(Full, Content) || Folder.IsEmpty() || Folder == TEXT("."))
	{
		return FString::Printf(TEXT("Logo \"%s\" is not in a folder under Content/, so a packaged game cannot include "
			"it and its Configure() fails with logo.file_missing: move it into a folder under Content/ and add that "
			"folder to %s."), *Logo, Setting);
	}
	for (const FString& Dir : Staged)
	{
		if (!Dir.IsEmpty() && FPaths::IsUnderDirectory(Full, FPaths::ConvertRelativePathToFull(Content / Dir)))
		{
			return FString();
		}
	}
	return FString::Printf(TEXT("Logo \"%s\" is not packaged: its folder is not in %s, so a packaged game does not "
		"include it and its Configure() fails with logo.file_missing. Add \"%s\" there (the folders are relative to "
		"Content/)."), *Logo, Setting, *Folder);
}

TArray<FString> UScfgSettings::StagedDirectories()
{
	TArray<FString> Out;
	for (const TCHAR* const Key : {TEXT("DirectoriesToAlwaysStageAsUFS"), TEXT("DirectoriesToAlwaysStageAsNonUFS")})
	{
		TArray<FString> Entries;
		if (GConfig)
		{
			GConfig->GetArray(TEXT("/Script/UnrealEd.ProjectPackagingSettings"), Key, Entries, GGameIni);
		}
		for (const FString& Entry : Entries)
		{
			// '(Path="Showcase")', or a bare path: UAT splits on '"' and takes the middle of three parts, or the one.
			TArray<FString> Parts;
			Entry.ParseIntoArray(Parts, TEXT("\""), false);
			if (Parts.Num() == 3 || Parts.Num() == 1)
			{
				Out.Add(Parts[Parts.Num() == 3 ? 1 : 0]);
			}
		}
	}
	return Out;
}

TArray<FString> UScfgSettings::Verdict(const TArray<FScfgSettingsProblem>& Problems, const TArray<FString>& InWarnings) const
{
	using namespace ScfgSettingsDetail;
	TArray<FString> Lines;
	for (const FScfgSettingsProblem& Each : Problems)
	{
		Lines.Add(Each.Code + TEXT(": ") + Each.Message);
	}
	if (Problems.Num() == 0)
	{
		Lines.Add(FString::Printf(TEXT("Valid: the configuration, %s and %s pass the checks SCFG runs when the game starts."),
			*Count(Chains.Num(), TEXT("chain"), TEXT("chains")), *Count(Items.Num(), TEXT("item"), TEXT("items"))));
	}
	Lines.Append(InWarnings);
	if (IsEmpty())
	{
		Lines.Add(TEXT("No wallet is set, so these settings are not applied: the game configures SCFG from code."));
	}
	else if (Options.bTestnet)
	{
		Lines.Add(TEXT("Testnet is on: players pay with test tokens. Turn it off for the release."));
	}
	return Lines;
}

FName UScfgSettings::GetContainerName() const
{
	return TEXT("Project");
}

FName UScfgSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FName UScfgSettings::GetSectionName() const
{
	return TEXT("SCFG");
}

#if WITH_EDITOR
FText UScfgSettings::GetSectionText() const
{
	return LOCTEXT("Section", "SCFG");
}

FText UScfgSettings::GetSectionDescription() const
{
	return LOCTEXT("Description",
		"Stablecoin for Games: the payout wallet, options, chains and items, applied when the game instance starts "
		"(only with a wallet set). A Configure() call from your code replaces them.");
}
#endif

#undef LOCTEXT_NAMESPACE
