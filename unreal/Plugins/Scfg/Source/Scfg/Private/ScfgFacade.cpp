// Twin of godot/addons/scfg/scfg.gd (see ScfgFacade.h).
#include "ScfgFacade.h"

#include "ScfgConvert.h"
#include "ScfgGuard.h"
#include "ScfgHttpTransport.h"
#include "ScfgLocalServer.h"
#include "ScfgLog.h"
#include "ScfgString.h"
#include "ScfgSubmitter.h"
#include "UI/ScfgTexts.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "Scfg/Chains.h"
#include "Scfg/Dec.h"
#include "Scfg/Py.h"
#include "Scfg/Watcher.h"

#include <algorithm>
#include <cmath>

namespace ScfgFacadeDetail
{

// Characters that can appear in a transaction hash (hex) or a Solana signature (base58).
const FString TxChars = TEXT("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");

bool Truthy(const std::optional<std::string>& S)
{
	return S.has_value() && !S->empty();
}

bool IsFinal(const std::string& State)
{
	return State == "paid" || State == "underpaid";
}

// A Json number as a double (an int or a float), else 0.
double Number(const scfg::Json& V)
{
	if (V.is_int64())
	{
		return static_cast<double>(V.as_int64());
	}
	return V.is_float() ? V.as_float() : 0.0;
}

scfg::Err WrongThread(const TCHAR* What)
{
	return scfg::Err::make("RuntimeError", ScfgString::ToUtf8(FString::Printf(TEXT("%s must be called on the game thread"), What)));
}

FScfgPaymentResult Result(EScfgPaymentStatus Status, EScfgPayKind Kind, const FString& Item, const FString& Token)
{
	FScfgPaymentResult R;
	R.Status = Status;
	R.Kind = Kind;
	R.Item = Item;
	R.Token = Token;
	return R;
}

}  // namespace ScfgFacadeDetail

// --- the reporter -------------------------------------------------------------------------------------------------

void FScfgReporter::Error(const FString& Line)
{
	if (bQuiet)
	{
		Errors.Add(Line);
		return;
	}
	UE_LOG(LogScfg, Error, TEXT("%s"), *Line);
}

void FScfgReporter::Warning(const FString& Line)
{
	if (bQuiet)
	{
		Warnings.Add(Line);
		return;
	}
	UE_LOG(LogScfg, Warning, TEXT("%s"), *Line);
}

// --- the controllers' buttons ---------------------------------------------------------------------------------------

void FScfgCheckout::OpenBrowser()
{
	if (const TSharedPtr<FScfgFacade> F = Facade.Pin())
	{
		F->OpenBrowser(*this);
	}
}

void FScfgCheckout::OpenWalletApp()
{
	if (const TSharedPtr<FScfgFacade> F = Facade.Pin())
	{
		F->OpenWalletApp(*this);
	}
}

void FScfgCheckout::OpenExplorer(const FString& Url)
{
	if (const TSharedPtr<FScfgFacade> F = Facade.Pin())
	{
		F->OpenExplorer(Url);
	}
}

void FScfgCheckout::Copy(const FString& Text)
{
	if (const TSharedPtr<FScfgFacade> F = Facade.Pin())
	{
		F->Copy(Text);
	}
}

void FScfgCheckout::Paste()
{
	if (const TSharedPtr<FScfgFacade> F = Facade.Pin())
	{
		F->PasteInto(*this);
	}
}

void FScfgRestore::Paste()
{
	if (const TSharedPtr<FScfgFacade> F = Facade.Pin())
	{
		F->PasteInto(*this);
	}
}

void FScfgRestore::Copy(const FString& Text)
{
	if (const TSharedPtr<FScfgFacade> F = Facade.Pin())
	{
		F->Copy(Text);
	}
}

// --- lifetime -------------------------------------------------------------------------------------------------------

FScfgFacade::FScfgFacade(FScfgFacadeOptions InOptions) : Options(MoveTemp(InOptions))
{
	Reporter->bQuiet = Options.bQuiet;
	if (!Options.Platform.IsValid())
	{
		Options.Platform = MakeShared<FScfgEnginePlatform>();
	}
	if (!Options.ServerFactory)
	{
		// The localhost checkout page (ScfgLocalServer.h): its Err, when it cannot start, is reported here (Checkout.h:
		// the factory and the server report their own Err; the controller then shows "browser_unavailable"). The
		// handlers hold the reporter and the logo path, never the facade: a controller may outlive it.
		FScfgLocalServerPage Page;
		Page.LogoPath = PageLogo;
		Page.OnError = [Reporter = Reporter](const FString& Line) { Reporter->Error(Line); };
		Options.ServerFactory = FScfgLocalServer::Factory(MoveTemp(Page));
	}
}

FScfgFacade::~FScfgFacade()
{
	Shutdown();
}

FTSTicker& FScfgFacade::Ticker() const
{
	return Options.Ticker ? *Options.Ticker : FTSTicker::GetCoreTicker();
}

void FScfgFacade::Start()
{
	if (ScfgGuard::OffGameThread(TEXT("Start()")))
	{
		return;
	}
	if (PumpHandle.IsValid() || bShuttingDown || !Options.bPumpOnTicker)
	{
		return;
	}
	TWeakPtr<FScfgFacade> Weak = AsShared();
	PumpHandle = Ticker().AddTicker(TEXT("ScfgFacade"), 0.0f,
		[Weak](float)
		{
			const TSharedPtr<FScfgFacade> Self = Weak.Pin();
			if (!Self.IsValid())
			{
				return false;
			}
			Self->Pump();
			return true;
		});
}

void FScfgFacade::Shutdown()
{
	check(IsInGameThread());
	if (bShuttingDown)
	{
		return;
	}
	bShuttingDown = true;
	// 1. Every flow waiting on a screen ends without delivering anything; then every controller is closed.
	TArray<FWait> Released = MoveTemp(Waits);
	Waits.Reset();
	for (FWait& Each : Released)
	{
		EndWait(Each, TEXT("shutdown"), !Each.Screen.IsValid());
	}
	for (const TSharedRef<FScfgCheckout>& Each : Checkouts)
	{
		Each->Controller->close();
	}
	for (const TSharedRef<FScfgCheckout>& Each : Background)
	{
		Each->Controller->close();
	}
	Checkouts.Reset();
	Background.Reset();
	Restores.Reset();
	Callbacks.Reset();
	// 2. The worker: stopped, its last snapshot handed to the store, destroyed (Jobs, Engine, transport).
	if (Worker)
	{
		Worker->Stop();
		if (Store && !bWiping)
		{
			const scfg::engine::Snapshot& Last = Worker->GetSnapshot();
			PersistedPending = Last.pending;
			PersistedReceipts = Last.receipts;
			Store->Save(Last.pending, Last.receipts);
		}
		Worker.Reset();
	}
	// 3. The store: what it still owes is written by its destructor (its OnError holds only the reporter).
	Store.Reset();
	// 4. The pump.
	FTSTicker::RemoveTicker(PumpHandle);
	PumpHandle.Reset();
	bShutDown = true;
}

FString FScfgFacade::NewId(const TCHAR* Prefix)
{
	++NextId;
	return FString::Printf(TEXT("%s%d"), Prefix, NextId);
}

// --- reports --------------------------------------------------------------------------------------------------------

TOptional<scfg::Err> FScfgFacade::Misuse(const scfg::Err& Error)
{
	Reporter->Error(TEXT("scfg: ") + ScfgString::FromUtf8(Error.message));
	return Error;
}

TOptional<scfg::Err> FScfgFacade::ConfigMisuse(const char* Code, const std::string& Message)
{
	return Misuse(scfg::Err::config_error(Code, Message));
}

void FScfgFacade::ReportError(const FString& Heading, const scfg::Err& Error)
{
	Reporter->Error(FString::Printf(TEXT("scfg: %s %s"), *Heading, *ScfgString::FromUtf8(Error.str())));
}

void FScfgFacade::Warn(const FString& Message)
{
	Reporter->Warning(TEXT("scfg: ") + Message);
}

scfg::Result<const scfg::config::Config*> FScfgFacade::RequireConfig(bool bReport)
{
	if (!Config)
	{
		scfg::Err Error = scfg::Err::config_error("configure.not_called", "call Configure(...) first");
		if (bReport)
		{
			Misuse(Error);
		}
		return Error;
	}
	return &*Config;
}

// --- configuration --------------------------------------------------------------------------------------------------

TOptional<scfg::Err> FScfgFacade::AddChain(const FString& Key, const FScfgChain& Chain)
{
	if (ScfgGuard::OffGameThread(TEXT("AddChain()")))
	{
		return ScfgFacadeDetail::WrongThread(TEXT("AddChain()"));
	}
	if (Config && bConfigFromSettings)
	{
		if (Worker || StartError || bShuttingDown)
		{
			return ConfigMisuse("configure.after_add_chain",
				"call AddChain() before payments start: the SCFG settings (Project Settings > Plugins > SCFG) configured "
				"SCFG when the game instance started, and payments started with them. Add your chains and call "
				"Configure() from your game instance's Init() or the first level's BeginPlay");
		}
		// The settings cannot name a chain added from code, so the Configure() from code that has to follow replaces
		// their configuration anyway: set it aside now, with the settings' own chains (ScfgSettings.h).
		SetSettingsAside();
		Warn(TEXT("AddChain() set aside the SCFG settings' configuration (Project Settings > Plugins > SCFG: it cannot "
				  "name your own chains): call Configure() next, from code."));
	}
	if (Config)
	{
		return ConfigMisuse("configure.after_add_chain", "call AddChain() before Configure()");
	}
	scfg::Result<scfg::chains::Chain> Built = scfg::config::build_chain(ScfgConvert::ChainKwargs(Key, Chain));
	if (!Built.ok())
	{
		return Misuse(Built.err());
	}
	CustomChains.emplace_back(Built.take());
	return {};
}

TOptional<scfg::Err> FScfgFacade::Configure(const FScfgOptions& InOptions)
{
	if (ScfgGuard::OffGameThread(TEXT("Configure()")))
	{
		return ScfgFacadeDetail::WrongThread(TEXT("Configure()"));
	}
	if (Worker || StartError || bShuttingDown)
	{
		return ConfigMisuse(
			"configure.after_start", "Configure() can't change the configuration once payments have started");
	}
	// A Configure() from code replaces the settings' configuration with their chains: it sees only the chains the
	// code added (none: AddChain() would have set the settings aside first). ScfgSettings.h, "A CODE CALL AFTER IT WINS".
	const bool bReplacesSettings = Config && bConfigFromSettings && !bApplyingSettings;
	scfg::Result<scfg::config::Config> Built = scfg::config::build_config(ScfgConvert::ConfigKwargs(InOptions),
		bReplacesSettings ? std::vector<scfg::config::CustomChain>() : CustomChains);
	if (!Built.ok())
	{
		return Misuse(Built.err());
	}
	scfg::config::Config Made = Built.take();
	scfg::Result<FString> Logo = LogoFile(Options.ProjectDir, Made.logo);
	if (!Logo.ok())
	{
		return Misuse(Logo.err());
	}
	if (bReplacesSettings)
	{
		CustomChains.clear();
	}
	Config = std::move(Made);
	bConfigFromSettings = bApplyingSettings;
	LogoPath = Logo.value();
	*PageLogo = LogoPath;
	return {};
}

scfg::Result<FString> FScfgFacade::LogoFile(const FString& ProjectDir, const std::optional<std::string>& Logo)
{
	if (!Logo)
	{
		return FString();
	}
	const FString Root = ProjectDir.IsEmpty() ? FPaths::ProjectDir() : ProjectDir;
	const FString Full = FPaths::ConvertRelativePathToFull(Root / ScfgString::FromUtf8(*Logo));
	if (!IFileManager::Get().FileExists(*Full))
	{
		return scfg::Err::config_error(
			"logo.file_missing", "logo file not found in the project: " + scfg::py::repr_str(ScfgString::ToUtf8(Full)));
	}
	return Full;
}

void FScfgFacade::SetSettingsAside()
{
	Config.reset();
	CustomChains.clear();
	bConfigFromSettings = false;
	LogoPath.Reset();
	PageLogo->Reset();
}

TOptional<scfg::Err> FScfgFacade::ApplySettings(
	const FScfgOptions& InOptions, const TArray<FScfgSettingsChain>& Chains, const TArray<FScfgSettingsItem>& Items)
{
	if (ScfgGuard::OffGameThread(TEXT("ApplySettings()")))
	{
		return ScfgFacadeDetail::WrongThread(TEXT("ApplySettings()"));
	}
	const TCHAR* const Where = TEXT("the SCFG settings (Project Settings > Plugins > SCFG)");
	if (Config || !CustomChains.empty() || Worker || StartError || bShuttingDown)
	{
		return Misuse(scfg::Err::make("RuntimeError", ScfgString::ToUtf8(FString::Printf(
			TEXT("%s are applied when the game instance starts, before any other call"), Where))));
	}
	TGuardValue<bool> Applying(bApplyingSettings, true);
	for (const FScfgSettingsChain& Row : Chains)
	{
		if (TOptional<scfg::Err> Error = AddChain(Row.Key, Row.Chain))
		{
			CustomChains.clear();
			Reporter->Error(FString::Printf(TEXT("scfg: %s were not applied: chain \"%s\" (%s). Press Validate there."),
				Where, *Row.Key, *ScfgGuard::ErrCode(*Error)));
			return Error;
		}
	}
	if (TOptional<scfg::Err> Error = Configure(InOptions))
	{
		CustomChains.clear();
		Reporter->Error(FString::Printf(TEXT("scfg: %s were not applied (%s). Press Validate there."), Where,
			*ScfgGuard::ErrCode(*Error)));
		return Error;
	}
	for (const FScfgSettingsItem& Row : Items)
	{
		if (TOptional<scfg::Err> Error = AddItem(Row.Id, Row.ToItem()))
		{
			Reporter->Error(FString::Printf(TEXT("scfg: %s: item \"%s\" was not added (%s). Press Validate there."),
				Where, *Row.Id, *ScfgGuard::ErrCode(*Error)));
			return Error;
		}
	}
	return {};
}

TOptional<scfg::Err> FScfgFacade::AddItem(const FString& ItemId, const FScfgItem& Item)
{
	if (ScfgGuard::OffGameThread(TEXT("AddItem()")))
	{
		return ScfgFacadeDetail::WrongThread(TEXT("AddItem()"));
	}
	scfg::Result<const scfg::config::Config*> Cfg = RequireConfig(true);
	if (!Cfg.ok())
	{
		return Cfg.err();
	}
	const scfg::Json Id(ScfgString::ToUtf8(ItemId));
	const scfg::Json Kwargs = ScfgConvert::ItemKwargs(Item);
	scfg::Result<scfg::models::Item> Added = scfg::config::add_item(*Config, Id, Kwargs);
	if (!Added.ok())
	{
		return Misuse(Added.err());
	}
	if (Worker)
	{
		// Python's Engine shares the game's Config; this one holds a copy made when the worker started, so the item
		// goes to it too, in the worker's turn (before any Pay submitted after this call). The same add_item on the
		// same items cannot fail differently; if it did, the worker reports the Err.
		Worker->Run(
			[Id, Kwargs](scfg::engine::Engine& E) -> scfg::Result<scfg::Json>
			{
				SCFG_TRY(scfg::models::Item Same, E.add_item(Id, Kwargs));
				(void)Same;
				return scfg::Json();
			},
			nullptr);
	}
	return {};
}

// --- the store and the worker ---------------------------------------------------------------------------------------

FScfgStore& FScfgFacade::Stored()
{
	if (!Store)
	{
		FScfgStore::FOptions StoreOptions;
		StoreOptions.Directory = Options.StoreDirectory;
		StoreOptions.Ticker = Options.Ticker;
		StoreOptions.Clock = Options.Clock;
		// The handlers hold the reporter, never the facade: the store's destructor may call OnError (TEARDOWN).
		StoreOptions.OnError = [Reporter = Reporter, Loading = StoreLoading](const scfg::Err& Error)
		{
			const TCHAR* Heading = *Loading ? TEXT("cannot read the stored receipts:")
											: TEXT("cannot save the payment state:");
			Reporter->Error(FString::Printf(TEXT("scfg: %s %s"), Heading, *ScfgString::FromUtf8(Error.str())));
		};
		StoreOptions.OnWarning = [Reporter = Reporter](const FString& Warning)
		{ Reporter->Warning(TEXT("scfg: ") + Warning); };
		*StoreLoading = true;
		Store = MakeUnique<FScfgStore>(MoveTemp(StoreOptions));
		*StoreLoading = false;
		PersistedPending = Store->GetPending();
		PersistedReceipts = Store->GetReceipts();
	}
	return *Store;
}

int64 FScfgFacade::EngineNow() const
{
	if (Options.Clock)
	{
		return static_cast<int64>(std::floor(Options.Clock()));
	}
	return FDateTime::UtcNow().ToUnixTimestamp();
}

FScfgWorker* FScfgFacade::EnsureStarted()
{
	if (Worker || StartError || !Config || bShuttingDown)
	{
		return Worker.Get();
	}
	FScfgStore& S = Stored();
	if (S.IsBlocked())
	{
		// Not corrupt, only unreadable for now (a lock, a permission): starting with empty stores would save them
		// over the player's receipts. The store reported it; the next launch retries.
		StartError = *S.GetBlockReason();
		return nullptr;
	}
	TUniquePtr<FScfgTransport> Transport =
		Options.MakeTransport ? Options.MakeTransport() : TUniquePtr<FScfgTransport>(MakeUnique<FScfgHttpTransport>());
	FScfgWorker::FOptions WorkerOptions;
	WorkerOptions.Ticker = Options.Ticker;
	WorkerOptions.RpcFactory = Options.RpcFactory;
	if (Options.Clock)
	{
		WorkerOptions.Clock = [Clock = Options.Clock]() { return static_cast<int64>(std::floor(Clock())); };
	}
	WorkerOptions.OnError = [Reporter = Reporter](const scfg::Err& Error)
	{ Reporter->Error(TEXT("scfg: worker error: ") + ScfgString::FromUtf8(Error.str())); };
	if (Options.WorkerInterval)
	{
		WorkerOptions.Interval = Options.WorkerInterval;
	}
	else
	{
		TWeakPtr<FScfgFacade> Weak = AsShared();
		WorkerOptions.Interval = [Weak]()
		{
			const TSharedPtr<FScfgFacade> Self = Weak.Pin();
			return Self.IsValid() ? Self->PollInterval() : POLL_IDLE_S;
		};
	}
	scfg::Result<TUniquePtr<FScfgWorker>> Made = FScfgWorker::Create(
		*Config, MoveTemp(Transport), PersistedPending, PersistedReceipts, MoveTemp(WorkerOptions));
	if (!Made.ok())
	{
		// The one Err nobody else reports.
		StartError = Made.err();
		ReportError(TEXT("cannot start the payment engine:"), Made.err());
		return nullptr;
	}
	Worker = Made.take();
	Submitter = std::make_shared<FScfgSubmitter>(*Worker);
	Worker->OnStateChanged->AddSPLambda(AsShared(), [this]() { OnWorkerStateChanged(); });
	Worker->Start();
	return Worker.Get();
}

scfg::Err FScfgFacade::StartFailure() const
{
	if (StartError)
	{
		return *StartError;
	}
	return scfg::Err::make("RuntimeError", "payments could not start");
}

void FScfgFacade::OnWorkerStateChanged()
{
	// ClearLocalData(): the snapshots made before the Engine was wiped are skipped.
	if (bWiping || !Worker)
	{
		return;
	}
	const scfg::engine::Snapshot& Snap = Worker->GetSnapshot();
	const bool bReceipts = !scfg::py::eq(PersistedReceipts, Snap.receipts);
	const bool bPending = !scfg::py::eq(PersistedPending, Snap.pending);
	if (!bReceipts && !bPending)
	{
		return;
	}
	// The snapshot's stores are detached copies, never changed after: holding them is Godot's duplicate(true).
	PersistedPending = Snap.pending;
	PersistedReceipts = Snap.receipts;
	Stored().Save(Snap.pending, Snap.receipts);  // true: written or owed; false: blocked or failed (reported)
	if (bReceipts)
	{
		bReceiptsChangedPending = true;  // OnReceiptsChanged from the pump, never from inside the worker's ticker
	}
}

const scfg::engine::Snapshot* FScfgFacade::CurrentSnapshot() const
{
	if (SnapshotForTests)
	{
		return SnapshotForTests();
	}
	return Worker ? &Worker->GetSnapshot() : nullptr;
}

const scfg::engine::Snapshot* FScfgFacade::LiveSnapshot() const
{
	return Worker && !bWiping ? CurrentSnapshot() : nullptr;
}

const scfg::Json& FScfgFacade::SourcePending()
{
	if (const scfg::engine::Snapshot* Live = LiveSnapshot())
	{
		return Live->pending;
	}
	if (!bShuttingDown)
	{
		Stored();  // read on first use; after the teardown, what was last mirrored
	}
	return PersistedPending;
}

const scfg::Json& FScfgFacade::SourceReceipts()
{
	if (const scfg::engine::Snapshot* Live = LiveSnapshot())
	{
		return Live->receipts;
	}
	if (!bShuttingDown)
	{
		Stored();
	}
	return PersistedReceipts;
}

TOptional<scfg::receipts::Ownership> FScfgFacade::Ownership()
{
	if (const scfg::engine::Snapshot* Live = LiveSnapshot())
	{
		return scfg::receipts::Ownership{Live->owned, Live->tip_total};
	}
	const scfg::Json& Have = SourceReceipts();
	scfg::Result<scfg::receipts::Ownership> Got = scfg::receipts::compute_ownership(
		Have.is_dict() ? Have.as_dict() : scfg::Json::Dict(), Config->items);
	if (!Got.ok())
	{
		ReportError(TEXT("cannot read the stored receipts:"), Got.err());
		return {};
	}
	return Got.take();
}

// --- queries --------------------------------------------------------------------------------------------------------

bool FScfgFacade::Owns(const FString& ItemId)
{
	if (ScfgGuard::OffGameThread(TEXT("Owns()")) || !RequireConfig(true).ok())
	{
		return false;
	}
	const std::string Id = ScfgString::ToUtf8(ItemId);
	if (!Config->items.contains(Id))
	{
		ConfigMisuse("item.unknown", "unknown item " + scfg::py::repr_str(Id) + "; declare it with AddItem()");
		return false;
	}
	const TOptional<scfg::receipts::Ownership> O = Ownership();
	return O.IsSet() && std::find(O->owned.begin(), O->owned.end(), Id) != O->owned.end();
}

bool FScfgFacade::HasTipped()
{
	if (ScfgGuard::OffGameThread(TEXT("HasTipped()")) || !RequireConfig(true).ok())
	{
		return false;
	}
	const TOptional<scfg::receipts::Ownership> O = Ownership();
	return O.IsSet() && !O->tip_total.is_zero() && !O->tip_total.negative();
}

FString FScfgFacade::TipTotal()
{
	if (ScfgGuard::OffGameThread(TEXT("TipTotal()")) || !RequireConfig(true).ok())
	{
		return TEXT("0");
	}
	const TOptional<scfg::receipts::Ownership> O = Ownership();
	return O.IsSet() ? ScfgString::FromUtf8(O->tip_total.str()) : FString(TEXT("0"));
}

scfg::Json FScfgFacade::Receipts()
{
	if (ScfgGuard::OffGameThread(TEXT("Receipts()")))
	{
		return scfg::Json::dict();
	}
	return scfg::deep_copy(SourceReceipts());
}

scfg::Json FScfgFacade::Pending()
{
	if (ScfgGuard::OffGameThread(TEXT("Pending()")))
	{
		return scfg::Json::dict();
	}
	return scfg::deep_copy(SourcePending());
}

void FScfgFacade::ClearLocalData()
{
	if (ScfgGuard::OffGameThread(TEXT("ClearLocalData()")) || bShuttingDown)
	{
		return;
	}
	const scfg::Json& Before = SourceReceipts();
	const bool bHadReceipts = Before.is_dict() && !Before.as_dict().empty();
	if (Worker)
	{
		// worker.py's submit(fn): the wipe runs in its turn; the snapshots made before it are skipped meanwhile.
		bWiping = true;
		const int32 Serial = ++WipeSerial;
		TWeakPtr<FScfgFacade> Weak = AsShared();
		Worker->Run(
			[](scfg::engine::Engine& E) -> scfg::Result<scfg::Json>
			{
				for (const scfg::Json& Each : {E.pending(), E.receipts()})
				{
					scfg::Json Alias = Each;  // a shared handle: erasing through it changes the Engine's own store
					for (const std::string& Key : Alias.as_dict().keys())
					{
						Alias.as_dict().erase(Key);
					}
				}
				return scfg::Json();
			},
			[Weak, Serial](scfg::Result<scfg::Json>)
			{
				const TSharedPtr<FScfgFacade> Self = Weak.Pin();
				if (Self.IsValid() && Self->WipeSerial == Serial)
				{
					Self->bWiping = false;
				}
			});
	}
	PersistedPending = scfg::Json::dict();
	PersistedReceipts = scfg::Json::dict();
	Stored().Save(PersistedPending, PersistedReceipts);
	// Once: a receipts change mirrored this frame and not yet announced by the pump is announced by this one.
	if (bHadReceipts || bReceiptsChangedPending)
	{
		bReceiptsChangedPending = false;
		EmitReceiptsChanged();
	}
}

scfg::Result<FString> FScfgFacade::ExportReceipts(const FString& Path)
{
	if (ScfgGuard::OffGameThread(TEXT("ExportReceipts()")))
	{
		return ScfgFacadeDetail::WrongThread(TEXT("ExportReceipts()"));
	}
	// Empty: Saved/scfg-receipts.json; a relative path is under Saved/ too (not the process's working directory).
	const FString Target = FPaths::ConvertRelativePathToFull(Path.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("scfg-receipts.json")
			: FPaths::IsRelative(Path)                                       ? FPaths::ProjectSavedDir() / Path
																			  : Path);
	scfg::Json::List Values;
	const scfg::Json All = Receipts();
	if (All.is_dict())
	{
		for (const auto& [Key, Value] : All.as_dict())
		{
			Values.push_back(Value);
		}
	}
	scfg::json::DumpOptions Pretty;
	Pretty.sort_keys = true;
	Pretty.indent = 2;
	scfg::Result<std::string> Text = scfg::json::dumps(scfg::Json(std::move(Values)), Pretty);
	if (!Text.ok())
	{
		Misuse(Text.err());
		return Text.err();
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), true);
	const std::string& Bytes = Text.value();
	TArray<uint8> Data(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size()));
	FString Why;
	if (IFileManager::Get().DirectoryExists(*Target))
	{
		Why = TEXT("it is a directory");
	}
	else if (!FFileHelper::SaveArrayToFile(Data, *Target))
	{
		const uint32 Code = FPlatformMisc::GetLastError();
		TCHAR Message[512] = {};
		FPlatformMisc::GetSystemErrorMessage(Message, UE_ARRAY_COUNT(Message), static_cast<int32>(Code));
		Why = Message[0] != 0 ? FString(Message) : FString::Printf(TEXT("error %u"), Code);
	}
	if (!Why.IsEmpty())
	{
		scfg::Err Error = scfg::Err::make("RuntimeError", ScfgString::ToUtf8(FString::Printf(TEXT("cannot write %s: %s"), *Target, *Why)));
		Misuse(Error);
		return Error;
	}
	return Target;
}

// --- flows ----------------------------------------------------------------------------------------------------------

scfg::Result<scfg::payments::PaymentSpec> FScfgFacade::PaymentSpec(EScfgPayKind Kind, const FScfgPayRequest& Request)
{
	SCFG_TRY(const scfg::config::Config* Cfg, RequireConfig(false));
	return scfg::payments::prepare_payment(*Cfg, ScfgConvert::KindJson(Kind), ScfgConvert::PayKwargs(Request));
}

void FScfgFacade::Pay(EScfgPayKind Kind, const FScfgPayRequest& Request, FScfgPayCallback Callback, FPayDone Done)
{
	if (!Done)
	{
		Done = [](const FScfgPaymentResult&) {};
	}
	if (ScfgGuard::OffGameThread(TEXT("Pay()")))
	{
		const FScfgPaymentResult R = ScfgGuard::PaymentNotRunning(Kind, Request.Item, Request.Token);
		Call(Callback, R);
		Done(R);
		return;
	}
	const TSharedRef<FScfgFacade> Self = AsShared();
	scfg::Result<scfg::payments::PaymentSpec> Spec = PaymentSpec(Kind, Request);
	if (!Spec.ok())
	{
		FailedPayment(Kind, Request, Callback, Done, Spec.err(), false);
		return;
	}
	const scfg::payments::PaymentSpec& S = Spec.value();
	if (S.kind == "unlock")
	{
		const TOptional<scfg::receipts::Ownership> Owned = Ownership();
		if (Owned.IsSet() && S.item.is_str() &&
			std::find(Owned->owned.begin(), Owned->owned.end(), S.item.as_str()) != Owned->owned.end())
		{
			const FScfgPaymentResult Short = ScfgFacadeDetail::Result(EScfgPaymentStatus::AlreadyOwned, Kind,
				ScfgConvert::Str(S.item), ScfgString::FromUtf8(S.token));
			Call(Callback, Short);
			Done(Short);
			return;
		}
	}
	FScfgWorker* W = EnsureStarted();
	if (!W)
	{
		FailedPayment(Kind, Request, Callback, Done, StartFailure(), StartError.IsSet());
		return;
	}
	scfg::checkout::CheckoutOptions Made;
	Made.config = *Config;
	Made.spec = S;
	Made.submit = Submitter;
	Made.mobile = Options.Platform->IsMobile();
	Made.server_factory = Options.ServerFactory;
	TWeakPtr<FScfgFacade> Weak = Self;
	Made.unready = [Weak]() -> scfg::checkout::Routes
	{
		const TSharedPtr<FScfgFacade> F = Weak.Pin();
		const scfg::engine::Snapshot* Snap = F.IsValid() ? F->CurrentSnapshot() : nullptr;
		return Snap ? Snap->unready : scfg::checkout::Routes();
	};
	Made.game_name = ScfgString::ToUtf8(Options.GameName.IsEmpty() ? FString(FApp::GetProjectName()) : Options.GameName);
	scfg::Result<std::unique_ptr<scfg::checkout::CheckoutController>> Ctl =
		scfg::checkout::CheckoutController::make(std::move(Made));
	if (!Ctl.ok())
	{
		FailedPayment(Kind, Request, Callback, Done, Ctl.err(), false);
		return;
	}
	const TSharedRef<FScfgCheckout> Checkout(new FScfgCheckout());
	Checkout->Controller = Ctl.take();
	Checkout->Cid = NewId(TEXT("c"));
	Checkout->Callback = MoveTemp(Callback);
	Checkout->Facade = Self;
	Checkouts.Add(Checkout);
	const TSharedPtr<IScfgSheetHost> Host = ActiveSheetHost();
	const TSharedPtr<FScfgScreen> Screen = Host.IsValid() ? Host->OpenPayment(Checkout) : nullptr;
	if (!Screen.IsValid())
	{
		Checkouts.Remove(Checkout);
		Checkout->Controller->cancel();  // discards a session start already submitted, as a cancel does
		FailedPayment(Kind, Request, Checkout->Callback, Done, NoScreen(TEXT("OpenPayment")), false);
		return;
	}
	FWait Wait;
	Wait.Screen = Screen;
	Wait.Host = Host;
	Wait.Checkout = Checkout;
	Wait.PayDone = MoveTemp(Done);
	AddWait(MoveTemp(Wait));
}

void FScfgFacade::Restore(FScfgRestoreCallback Callback, FRestoreDone Done)
{
	if (!Done)
	{
		Done = [](const FScfgRestoreResult&) {};
	}
	if (ScfgGuard::OffGameThread(TEXT("Restore()")))
	{
		const FScfgRestoreResult R = ScfgGuard::RestoreNotRunning();
		Call(Callback, R);
		Done(R);
		return;
	}
	const TSharedRef<FScfgFacade> Self = AsShared();
	scfg::Result<const scfg::config::Config*> Cfg = RequireConfig(false);
	if (!Cfg.ok())
	{
		FailedRestore(Callback, Done, Cfg.err(), false);
		return;
	}
	FScfgWorker* W = EnsureStarted();
	if (!W)
	{
		FailedRestore(Callback, Done, StartFailure(), StartError.IsSet());
		return;
	}
	const TSharedRef<FScfgRestore> Ctl(new FScfgRestore());
	Ctl->Controller = std::make_unique<scfg::restore_flow::RestoreController>(Submitter);
	Ctl->Rid = NewId(TEXT("r"));
	Ctl->Callback = MoveTemp(Callback);
	Ctl->Facade = Self;
	Restores.Add(Ctl);
	const TSharedPtr<IScfgSheetHost> Host = ActiveSheetHost();
	const TSharedPtr<FScfgScreen> Screen = Host.IsValid() ? Host->OpenRestore(Ctl) : nullptr;
	if (!Screen.IsValid())
	{
		Restores.Remove(Ctl);
		const FScfgRestoreCallback Cb = MoveTemp(Ctl->Callback);
		Ctl->Callback = FScfgRestoreCallback();
		FailedRestore(Cb, Done, NoScreen(TEXT("OpenRestore")), false);
		return;
	}
	FWait Wait;
	Wait.Screen = Screen;
	Wait.Host = Host;
	Wait.Restore = Ctl;
	Wait.RestoreDone = MoveTemp(Done);
	AddWait(MoveTemp(Wait));
}

scfg::Err FScfgFacade::NoScreen(const TCHAR* Method)
{
	return scfg::Err::config_error("sheet.unavailable",
		"no screen to show: the sheet host's " + ScfgString::ToUtf8(Method) + "() returned null");
}

void FScfgFacade::FailedPayment(EScfgPayKind Kind, const FScfgPayRequest& Request, const FScfgPayCallback& Callback,
	const FPayDone& Done, const scfg::Err& Error, bool bReported)
{
	if (!bReported)
	{
		Reporter->Error(TEXT("scfg: ") + ScfgString::FromUtf8(Error.message));
	}
	FScfgPaymentResult R = ScfgFacadeDetail::Result(EScfgPaymentStatus::Failed, Kind, Request.Item, Request.Token);
	R.Error = ScfgGuard::ErrCode(Error);
	Call(Callback, R);
	Done(R);
}

void FScfgFacade::FailedRestore(
	const FScfgRestoreCallback& Callback, const FRestoreDone& Done, const scfg::Err& Error, bool bReported)
{
	if (!bReported)
	{
		Reporter->Error(TEXT("scfg: ") + ScfgString::FromUtf8(Error.message));
	}
	FScfgRestoreResult R;
	R.Status = EScfgRestoreStatus::Failed;
	R.Error = ScfgGuard::ErrCode(Error);
	Call(Callback, R);
	Done(R);
}

void FScfgFacade::AddWait(FWait Wait)
{
	if (bShuttingDown)
	{
		EndWait(Wait, TEXT("shutdown"), !Wait.Screen.IsValid());
		return;
	}
	Waits.Add(MoveTemp(Wait));
}

void FScfgFacade::ProcessWaits()
{
	for (int32 I = 0; I < Waits.Num();)
	{
		const TSharedPtr<FScfgScreen> Screen = Waits[I].Screen.Pin();
		if (Screen.IsValid() && !Screen->IsClosed())
		{
			++I;
			continue;
		}
		FWait Taken = MoveTemp(Waits[I]);
		Waits.RemoveAt(I);
		// Released by the host without Close: "cancel", and not closed again.
		const bool bGone = !Screen.IsValid();
		EndWait(Taken, bGone ? FString(TEXT("cancel")) : Screen->GetHow(), bGone);
		if (bShuttingDown)
		{
			return;
		}
	}
}

void FScfgFacade::EndWait(FWait& Wait, const FString& How, bool bGone)
{
	if (Wait.Checkout.IsValid())
	{
		CloseCheckout(Wait, How, bGone);
	}
	else if (Wait.Restore.IsValid())
	{
		CloseRestore(Wait, bGone);
	}
}

void FScfgFacade::CloseScreen(const FWait& Wait, bool bGone)
{
	const TSharedPtr<FScfgScreen> Shown = Wait.Screen.Pin();
	if (!bGone && Shown.IsValid() && Wait.Host.IsValid())
	{
		Wait.Host->Close(Shown.ToSharedRef());
	}
}

void FScfgFacade::CloseCheckout(FWait& Wait, const FString& How, bool bGone)
{
	const TSharedRef<FScfgCheckout> Ctl = Wait.Checkout.ToSharedRef();
	Checkouts.Remove(Ctl);
	scfg::checkout::CheckoutController& Core = *Ctl->Controller;
	if (How == TEXT("shutdown"))
	{
		// Ren'Py's _shutdown: close the page and deliver nothing; the session stays watched.
		const scfg::models::PaymentResult Result = Core.finish();
		CloseScreen(Wait, bGone);
		Wait.PayDone(ScfgConvert::PaymentResult(Result));
		return;
	}
	RegisterCallback(*Ctl);
	scfg::models::PaymentResult Result;
	if (How == TEXT("cancel"))
	{
		Result = Core.cancel();
	}
	else if (How == TEXT("keep_playing") && ScfgFacadeDetail::Truthy(Core.session_id()))
	{
		Result = Core.keep_playing();
		Background.Add(Ctl);  // keeps the browser page answering until final
	}
	else
	{
		Result = Core.finish();
	}
	CloseScreen(Wait, bGone);
	const FScfgPaymentResult Typed = ScfgConvert::PaymentResult(Result);
	if (How == TEXT("cancel"))
	{
		Resolve(&Ctl->Callback, Typed);
	}
	Wait.PayDone(Typed);
}

void FScfgFacade::CloseRestore(FWait& Wait, bool bGone)
{
	const TSharedRef<FScfgRestore> Ctl = Wait.Restore.ToSharedRef();
	Restores.Remove(Ctl);
	Ctl->Callback = FScfgRestoreCallback();
	CloseScreen(Wait, bGone);
	Wait.RestoreDone(ScfgConvert::RestoreResult(Ctl->Controller->result()));
}

bool FScfgFacade::IsOpen(const FScfgCheckout& Checkout) const
{
	return Checkouts.ContainsByPredicate([&Checkout](const TSharedRef<FScfgCheckout>& Each)
		{ return &*Each == &Checkout; });
}

bool FScfgFacade::IsOpen(const FScfgRestore& Flow) const
{
	return Restores.ContainsByPredicate([&Flow](const TSharedRef<FScfgRestore>& Each) { return &*Each == &Flow; });
}

// --- the screens' buttons -------------------------------------------------------------------------------------------

void FScfgFacade::OpenBrowser(FScfgCheckout& Checkout)
{
	if (!IsOpen(Checkout))
	{
		return;
	}
	const std::optional<std::string> Url = Checkout.Controller->open_browser();
	if (ScfgFacadeDetail::Truthy(Url))
	{
		Options.Platform->OpenUrl(ScfgString::FromUtf8(*Url));
	}
}

void FScfgFacade::OpenWalletApp(FScfgCheckout& Checkout)
{
	if (!IsOpen(Checkout))
	{
		return;
	}
	const scfg::Result<std::optional<std::string>> Uri = Checkout.Controller->payment_uri();
	if (!Uri.ok())
	{
		ReportPaymentLinkError(Checkout, Uri.err());
		return;
	}
	if (ScfgFacadeDetail::Truthy(Uri.value()))
	{
		Options.Platform->OpenUrl(ScfgString::FromUtf8(*Uri.value()));
	}
}

void FScfgFacade::ReportPaymentLinkError(FScfgCheckout& Checkout, const scfg::Err& Error)
{
	if (Checkout.bPaymentLinkReported)
	{
		return;
	}
	Checkout.bPaymentLinkReported = true;
	ReportError(TEXT("cannot build the payment link:"), Error);
}

void FScfgFacade::OpenExplorer(const FString& Url)
{
	if (!Url.IsEmpty())
	{
		Options.Platform->OpenUrl(Url);
	}
}

void FScfgFacade::Copy(const FString& Text)
{
	Options.Platform->ClipboardSet(Text);
}

void FScfgFacade::PasteInto(FScfgCheckout& Checkout)
{
	if (!IsOpen(Checkout))
	{
		return;
	}
	scfg::checkout::CheckoutController& Core = *Checkout.Controller;
	ApplyPaste(Core.tx_input, Core.error, Options.Platform->ClipboardGet());
}

void FScfgFacade::PasteInto(FScfgRestore& Flow)
{
	if (!IsOpen(Flow))
	{
		return;
	}
	scfg::restore_flow::RestoreController& Core = *Flow.Controller;
	if (Core.state() != "input")
	{
		return;  // the player moved on (Check was pressed): drop the paste
	}
	ApplyPaste(Core.tx_input, Core.error, Options.Platform->ClipboardGet());
}

void FScfgFacade::ApplyPaste(std::string& Field, std::optional<std::string>& Error, const FString& Text)
{
	const FString Cleaned = CleanPaste(Text);
	if (!Cleaned.IsEmpty())
	{
		Field = ScfgString::ToUtf8(Cleaned);
		Error.reset();
	}
}

FString FScfgFacade::CleanPaste(const FString& Text)
{
	const int32 Len = Text.Len();
	int32 I = 0;
	while (I < Len && scfg::py::is_space(static_cast<uint32_t>(Text[I])))
	{
		++I;
	}
	FString Out;
	while (I < Len && Out.Len() < TX_MAX_LENGTH && !scfg::py::is_space(static_cast<uint32_t>(Text[I])))
	{
		int32 Found = INDEX_NONE;
		if (ScfgFacadeDetail::TxChars.FindChar(Text[I], Found))
		{
			Out.AppendChar(Text[I]);
		}
		++I;
	}
	return Out;
}

const FString& FScfgFacade::TxChars()
{
	return ScfgFacadeDetail::TxChars;
}

FString FScfgFacade::ItemTitle(const FString& ItemId) const
{
	if (Config)
	{
		if (const scfg::models::Item* Item = Config->items.find(ScfgString::ToUtf8(ItemId)))
		{
			return ScfgString::FromUtf8(Item->title);
		}
	}
	return ItemId;
}

FString FScfgFacade::ChainName(const FString& ChainKey) const
{
	if (Config)
	{
		const scfg::OrderedMap<scfg::chains::Chain> Registry = Config->registry();
		if (const scfg::chains::Chain* Chain = Registry.find(ScfgString::ToUtf8(ChainKey)))
		{
			return ScfgString::FromUtf8(Chain->name);
		}
	}
	return ChainKey;
}

FString FScfgFacade::Accent() const
{
	return ScfgString::FromUtf8(Config ? Config->accent : std::string(scfg::config::DEFAULT_ACCENT));
}

bool FScfgFacade::IsMobile() const
{
	return Options.Platform->IsMobile();
}

// --- the pump -------------------------------------------------------------------------------------------------------

double FScfgFacade::PollInterval() const
{
	return Checkouts.Num() > 0 || Restores.Num() > 0 ? POLL_OPEN_S : POLL_IDLE_S;
}

void FScfgFacade::Pump()
{
	if (ScfgGuard::OffGameThread(TEXT("Pump()")) || !Config || bShuttingDown)
	{
		return;
	}
	const TSharedRef<FScfgFacade> Self = AsShared();  // a callback below may shut the facade down
	FScfgWorker* W = EnsureStarted();
	if (!W)
	{
		return;
	}
	ProcessWaits();
	if (bShuttingDown)
	{
		return;
	}
	// Read before draining: every outcome of the pass that made this snapshot is queued.
	TOptional<TSet<FString>> PendingAtDrain;
	if (const scfg::engine::Snapshot* Snap = CurrentSnapshot(); Snap && Snap->pending.is_dict())
	{
		TSet<FString> Keys;
		for (const auto& [Sid, Record] : Snap->pending.as_dict())
		{
			Keys.Add(ScfgString::FromUtf8(Sid));
		}
		PendingAtDrain = MoveTemp(Keys);
	}
	TArray<TSharedRef<FScfgCheckout>> Controllers = Checkouts;
	Controllers.Append(Background);
	const FScfgWorker::FOutcomes Outcomes = DrainForTests ? DrainForTests() : W->DrainOutcomes();
	for (const scfg::watcher::StepOutcome& Outcome : Outcomes)
	{
		for (const TSharedRef<FScfgCheckout>& Each : Controllers)
		{
			Each->Controller->on_outcome(Outcome);
		}
		if (Outcome.result)
		{
			Deliver(*Outcome.result);
			if (bShuttingDown)
			{
				return;
			}
		}
	}
	for (const TSharedRef<FScfgCheckout>& Each : Controllers)
	{
		Each->Controller->poll();
		RegisterCallback(*Each);
	}
	PruneCallbacks(PendingAtDrain);
	CloseFinishedBackground();
	PollRestores();
	if (bShuttingDown)
	{
		return;
	}
	if (const scfg::engine::Snapshot* Snap = CurrentSnapshot())
	{
		WarnUnready(Snap->unready);
	}
	if (bReceiptsChangedPending)
	{
		bReceiptsChangedPending = false;
		EmitReceiptsChanged();
	}
}

void FScfgFacade::RegisterCallback(FScfgCheckout& Checkout)
{
	// Once per session, until Deliver or PruneCallbacks drops it: registering every pump would revive the callback of
	// a sheet left open on an expired session that no snapshot lists to prune any more. Skipped once paid/underpaid.
	const std::optional<std::string> Sid = Checkout.Controller->session_id();
	if (ScfgFacadeDetail::Truthy(Sid) && Checkout.Callback.Valid() && Sid != Checkout.CallbackSid &&
		!ScfgFacadeDetail::IsFinal(Checkout.Controller->state()))
	{
		Callbacks.Add(ScfgString::FromUtf8(*Sid), Checkout.Callback);
		Checkout.CallbackSid = Sid;
	}
}

void FScfgFacade::PruneCallbacks(const TOptional<TSet<FString>>& PendingAtDrain)
{
	// Drop the callbacks of sessions that left the Engine since a snapshot listed them: no result can reach them any
	// more. The snapshot was read before the drain, so a paid session had its result delivered first. A session no
	// snapshot listed yet is kept.
	if (!PendingAtDrain.IsSet())
	{
		return;
	}
	TSet<FString> Seen;
	for (auto It = Callbacks.CreateIterator(); It; ++It)
	{
		if (PendingAtDrain->Contains(It.Key()))
		{
			Seen.Add(It.Key());
		}
		else if (SeenPending.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	SeenPending = MoveTemp(Seen);
}

void FScfgFacade::CloseFinishedBackground()
{
	const scfg::Json Browser = Config->methods.is_dict() ? Config->methods.as_dict().get("browser") : scfg::Json();
	const double Timeout =
		ScfgFacadeDetail::Number(Browser.is_dict() ? Browser.as_dict().get("timeout_min") : scfg::Json()) * 60.0;
	const double Now = static_cast<double>(EngineNow());
	for (int32 I = 0; I < Background.Num();)
	{
		scfg::checkout::CheckoutController& Core = *Background[I]->Controller;
		const scfg::Json& Session = Core.session();
		const bool bExpired = !Session.is_dict() ||
			Now - ScfgFacadeDetail::Number(Session.as_dict().get("created_at")) > Timeout;
		const std::string& State = Core.state();
		if (bExpired || State == "paid" || State == "underpaid" || State == "expired" || State == "choose")
		{
			Core.close();
			Background.RemoveAt(I);
			continue;
		}
		++I;
	}
}

void FScfgFacade::PollRestores()
{
	const TArray<TSharedRef<FScfgRestore>> Open = Restores;
	for (const TSharedRef<FScfgRestore>& Each : Open)
	{
		const std::string Before = Each->Controller->state();
		Each->Controller->poll();
		if (Each->Controller->state() != Before && Each->Controller->state() == "done")
		{
			DeliverRestore(*Each);
			if (bShuttingDown)
			{
				return;
			}
		}
	}
}

void FScfgFacade::WarnUnready(const std::vector<std::pair<std::string, std::string>>& Unready)
{
	if (Unready.empty() || Unready == WarnedUnready)
	{
		return;
	}
	WarnedUnready = Unready;
	for (const auto& [Chain, Token] : Unready)
	{
		const FString C = ScfgString::FromUtf8(Chain);
		const FString T = ScfgString::FromUtf8(Token);
		Warn(FString::Printf(TEXT("your wallet can't receive %s on %s yet (no token account). Receive any amount of "
								  "%s there once; until then players aren't offered it."),
			*T, *C, *T));
	}
}

void FScfgFacade::Deliver(const scfg::models::PaymentResult& Result)
{
	const FScfgPaymentResult Typed = ScfgConvert::PaymentResult(Result);
	TOptional<FScfgPayCallback> Callback;
	if (const FScfgPayCallback* Found = Callbacks.Find(Typed.SessionId))
	{
		Callback = *Found;
	}
	if (Typed.Status == EScfgPaymentStatus::Paid || Typed.Status == EScfgPaymentStatus::Underpaid)
	{
		Callbacks.Remove(Typed.SessionId);
	}
	if (Typed.Status == EScfgPaymentStatus::Paid && bNotify)
	{
		const scfg::models::Item* Item =
			Config && Result.item.is_str() ? Config->items.find(Result.item.as_str()) : nullptr;
		Notify(Item ? ScfgTexts::Format(TEXT("payment_confirmed_item"), {ScfgString::FromUtf8(Item->title)})
					: ScfgTexts::Get(TEXT("payment_confirmed")));
	}
	Resolve(Callback.GetPtrOrNull(), Typed);
}

void FScfgFacade::DeliverRestore(FScfgRestore& Flow)
{
	const FScfgRestoreCallback Callback = MoveTemp(Flow.Callback);
	Flow.Callback = FScfgRestoreCallback();
	const FScfgRestoreResult Typed = ScfgConvert::RestoreResult(Flow.Controller->result());
	if (Typed.Restored.Num() > 0 && bNotify)
	{
		Notify(ScfgTexts::Get(TEXT("purchases_restored")));
	}
	Call(Callback, Typed);
}

void FScfgFacade::Resolve(const FScfgPayCallback* Callback, const FScfgPaymentResult& Result)
{
	if (Callback && Callback->Valid())
	{
		Callback->Fn(Result);
	}
	else if (OnPaymentResolved)
	{
		OnPaymentResolved(Result);
	}
}

void FScfgFacade::Call(const FScfgPayCallback& Callback, const FScfgPaymentResult& Result)
{
	if (Callback.Valid())
	{
		Callback.Fn(Result);
	}
}

void FScfgFacade::Call(const FScfgRestoreCallback& Callback, const FScfgRestoreResult& Result)
{
	if (Callback.Valid())
	{
		Callback.Fn(Result);
	}
}

void FScfgFacade::Notify(const FString& Text)
{
	if (const TSharedPtr<IScfgSheetHost> Host = ActiveSheetHost())
	{
		Host->Notify(Text);
	}
}

void FScfgFacade::EmitReceiptsChanged()
{
	++ReceiptsRev;
	if (OnReceiptsChanged)
	{
		OnReceiptsChanged();
	}
}

int32 FScfgFacade::NumRestoreCallbacks() const
{
	int32 N = 0;
	for (const TSharedRef<FScfgRestore>& Each : Restores)
	{
		N += Each->Callback.Valid() ? 1 : 0;
	}
	return N;
}

TArray<FString> FScfgFacade::CallbackSessionIds() const
{
	TArray<FString> Out;
	Callbacks.GetKeys(Out);
	return Out;
}

TArray<FString> FScfgFacade::CheckoutIds() const
{
	TArray<FString> Out;
	for (const TSharedRef<FScfgCheckout>& Each : Checkouts)
	{
		Out.Add(Each->Cid);
	}
	return Out;
}
