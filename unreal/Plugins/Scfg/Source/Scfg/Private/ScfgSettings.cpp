// No Python twin: see ScfgSettings.h.
#include "ScfgSettings.h"

#include "ScfgConvert.h"
#include "ScfgFacade.h"
#include "ScfgGuard.h"
#include "ScfgString.h"

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
	// Unity's asset starts on testnet too: test payments first, then turn it off for the release.
	Options.bTestnet = true;
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

TArray<FString> UScfgSettings::Verdict(const TArray<FScfgSettingsProblem>& Problems) const
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
