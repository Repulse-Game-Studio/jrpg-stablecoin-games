// No Python twin: the SCFG project settings (Project Settings > Plugins > SCFG, saved to Config/DefaultGame.ini), the
// twin of Unity's SCFG Settings asset (ScfgSettings.cs): configuration without code. Code stays the source of truth.
//
// APPLIED AT START. UScfgSubsystem::Initialize applies the project's settings (GetDefault<UScfgSettings>()) through
// the same calls a game makes: AddChain() for each chain row, Configure(Options), then AddItem() for each item row. It
// is opt-in: settings without a wallet (neither Options.Wallet nor Options.SolanaWallet) are not applied at all, so a
// game that configures from code is unaffected (rows without a wallet get one warning). A row that fails is reported
// as every misuse is ("LogScfg: Error: scfg: ..."), followed by a line naming the settings; a failing chain or
// Configure applies nothing, a failing item leaves the configuration and the items before it.
//
// A CODE CALL AFTER IT WINS (Unity's rule). The settings are one unit, set aside as a whole by the first configuration
// call from code:
// - Configure() from code replaces everything the settings applied: their options, logo, chains and items. It sees
//   only the chains the code added itself (none, unless AddChain() came first, below), so a settings chain must be
//   added again from code to be named. A Configure() that fails leaves the settings' configuration in place.
// - AddChain() from code sets the settings' configuration (and their chains) aside, with a warning: call Configure()
//   next, from code. Once payments have started with the settings (the first frame after the game instance started),
//   it is refused instead (configure.after_add_chain), and the settings stay.
// - AddItem() from code adds to the settings' configuration (their items stay); an id or code they already use is the
//   core's item.duplicate_id / item.duplicate_code.
// Payments start on the first frame after a successful configuration, so a game that overrides the settings calls
// Configure() before that: in its game instance's Init() (after Super::Init()) or the first level's BeginPlay. Later
// it gets configure.after_start, as any second Configure() does.
//
// VALIDATE. Validate() runs the core's build_chain, build_config and add_item on the settings, as the start would, and
// the facade's logo check, without touching any subsystem: every problem, with the core's ConfigError code. The editor
// module (ScfgEditor) puts a Validate button in the settings panel that shows the result as a notification and on the
// "SCFG" page of the Message Log.
//
// PACKAGING. The logo is a loose file, which the cooker does not stage: a packaged game has it only when it is under
// Content/, in a folder listed in Project Settings > Packaging > Additional Non-Asset Directories to Package (inside
// the pak) or To Copy (beside it), which are relative to Content/. Anywhere else, Configure() in the packaged game fails
// with logo.file_missing and the settings are not applied. Warnings() says so; Validate shows it as a warning, since
// the editor and play in the editor still find the file.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "ScfgTypes.h"

#include "ScfgSettings.generated.h"

// One chain row: AddChain(Key, Chain).
USTRUCT(BlueprintType)
struct FScfgSettingsChain
{
	GENERATED_BODY()

	// The chain's key, as Configure's Chains, RpcUrls and Confirmations name it ("myl2").
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Key;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FScfgChain Chain;
};

// One item row: AddItem(Id, ...), a paid unlock.
USTRUCT(BlueprintType)
struct FScfgSettingsItem
{
	GENERATED_BODY()

	// The item's id, as Owns("chapter2") and Pay ask for it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Id;
	// 1-99, unique per game. Never change it after release: receipts are matched by it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	int32 Code = 1;
	// The price as a decimal string, such as 4.99.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Price;
	// USDC or USDT.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Token = TEXT("USDC");
	// What the player sees on the payment sheet.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString Title;
	// Optional: the least a payment may be and still unlock (empty: the price).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SCFG")
	FString MinPrice;

	// AddItem()'s options.
	SCFG_API FScfgItem ToItem() const;
};

// What Validate() found wrong: the core's code ("address.invalid"; the Err kind for an Err without one) and its
// message, prefixed with the row ("item \"chapter2\": ...", "chain \"myl2\": ...").
struct FScfgSettingsProblem
{
	FString Code;
	FString Message;
};

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "SCFG"))
class SCFG_API UScfgSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UScfgSettings();

	// The options new settings start with (the constructor's): Configure()'s defaults with testnet on, as Unity's asset.
	// A settings object made later copies the class default instead, which holds the project's DefaultGame.ini.
	static FScfgOptions NewOptions();

	// Configure()'s options. At least one of the two wallets is required, else the settings are not applied. Testnet
	// starts on: develop with it, turn it off for the release.
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Configure")
	FScfgOptions Options;
	// Your own EVM chains (AddChain()), added before Configure().
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Chains", meta = (TitleProperty = "Key"))
	TArray<FScfgSettingsChain> Chains;
	// The paid unlocks (AddItem()).
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Items", meta = (TitleProperty = "Id"))
	TArray<FScfgSettingsItem> Items;

	// No wallet: the settings are not applied at start (see the file header).
	bool IsEmpty() const;
	// Something besides the wallets is set (a chain or an item row): worth a warning when the settings are empty.
	bool HasRows() const { return Chains.Num() > 0 || Items.Num() > 0; }

	// The core's validation of the settings, as the start would apply them, without touching any subsystem: every
	// problem found, in order (each chain row, then the configuration, the logo file, then each item row; the items
	// are checked only when the configuration builds). Empty: valid. ProjectDir is what a logo is relative to
	// (empty: FPaths::ProjectDir()).
	TArray<FScfgSettingsProblem> Validate(const FString& ProjectDir = FString()) const;
	// What Validate() does not refuse but a packaged game would: the logo kept out of the package (see PACKAGING in the
	// file header). One line per warning; none when no logo is set or it will be packaged.
	TArray<FString> Warnings(const FString& ProjectDir = FString()) const;
	// What the Validate button shows: one line per problem ("<code>: <message>"), or the line saying they are valid,
	// then each warning, and a note when the settings are empty (not applied) or on testnet.
	TArray<FString> Verdict(const TArray<FScfgSettingsProblem>& Problems, const TArray<FString>& InWarnings = {}) const;

	// The packaging check of a logo (Options.Logo, or one passed to Configure()), relative to ProjectDir (empty:
	// FPaths::ProjectDir()): "" when it is under Content/, in one of Staged (folders relative to Content/, as
	// StagedDirectories() gives them), else the warning line.
	static FString LogoPackagingWarning(const FString& Logo, const FString& ProjectDir, const TArray<FString>& Staged);
	// The project's Additional Non-Asset Directories to Package and To Copy (Project Settings > Packaging:
	// DirectoriesToAlwaysStageAsUFS and DirectoriesToAlwaysStageAsNonUFS in the Game ini's
	// [/Script/UnrealEd.ProjectPackagingSettings]), relative to Content/, read as UAT's staging reads them
	// (CopyBuildToStagingDirectory.Automation.cs, StageAdditionalDirectoriesFromConfig).
	static TArray<FString> StagedDirectories();

	// Where a game finds them: Project Settings > Plugins > SCFG.
	virtual FName GetContainerName() const override;
	virtual FName GetCategoryName() const override;
	virtual FName GetSectionName() const override;
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif
};
