// Twin of godot/addons/scfg/store.gd (see ScfgStore.h), with conformance/python/scfg_conformance/state_schema.py's
// pending-record check.
#include "ScfgStore.h"

#include "ScfgLog.h"
#include "ScfgString.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

#include "Scfg/Py.h"

#include <cstdint>
#include <string_view>
#include <utility>

namespace ScfgStoreDetail
{

using scfg::Json;

// A copy of conformance/schema/state.json (Scfg.Glue.Store.TheSchemaIsTheConformanceSchema compares the two).
constexpr const char* kSchema = R"SCHEMA({"match": {"block": "int", "log_index": "int", "payer": "str|null", )SCHEMA"
	R"SCHEMA("recipient": "str", "tx_hash": "str", "value": "int"}, "match_optional": {"block_hash": "str"}, )SCHEMA"
	R"SCHEMA("pending": {"chain": "str", "code": "int", "created_at": "number", "cursor": "int", )SCHEMA"
	R"SCHEMA("hint_at": "number|null", "hint_tx": "str|null", "id": "str", "item": "str|null", )SCHEMA"
	R"SCHEMA("kind": ["enum", "unlock", "tip"], "match": "match|null", "nonce": "int", "price": "str", )SCHEMA"
	R"SCHEMA("start_block": "int", "state": ["enum", "awaiting", "confirming"], "title": "str", "token": "str", )SCHEMA"
	R"SCHEMA("ui_status": ["enum", "open", "pending", "cancelled", "expired"], "units": "int"}, )SCHEMA"
	R"SCHEMA("pending_optional": {"dest": "str", "reference": "str", "scan_before": "str|null", )SCHEMA"
	R"SCHEMA("scan_cursor": "str|null"}, "receipt": {"amount": "str", "block": "int", "chain": "str", )SCHEMA"
	R"SCHEMA("item": "str|null", "kind": ["enum", "unlock", "tip"], "log_index": "int", "payer": "str|null", )SCHEMA"
	R"SCHEMA("recipient": "str", "source": ["enum", "payment", "restore"], "token": "str", "tx_hash": "str"}})SCHEMA";

std::string Repr(const Json& Value)
{
	scfg::Result<std::string> Got = scfg::py::repr(Value);
	return Got.ok() ? Got.take() : std::string("<unprintable>");
}

std::string QuotedKey(std::string_view Key)
{
	return scfg::py::repr_str(Key);
}

// state_schema._TYPES: booleans are never ints.
bool HasType(const Json& Value, std::string_view Base)
{
	if (Base == "str")
	{
		return Value.is_str();
	}
	if (Base == "int")
	{
		return Value.is_int();
	}
	if (Base == "bool")
	{
		return Value.is_bool();
	}
	if (Base == "number")
	{
		return Value.is_int() || Value.is_float();
	}
	return false;
}

void CheckRecord(const Json& Record, const Json& Required, const Json& Optional, const std::string& Where,
	std::vector<std::string>& Out);

// state_schema._value.
void CheckValue(const Json& Value, const Json& Spec, const std::string& Where, std::vector<std::string>& Out)
{
	if (Spec.is_list())
	{
		const Json::List& Enum = Spec.as_list();
		for (size_t I = 1; I < Enum.size(); ++I)
		{
			if (Value.is_str() && Value.as_str() == Enum[I].as_str())
			{
				return;
			}
		}
		Json Allowed = Json::list();
		for (size_t I = 1; I < Enum.size(); ++I)
		{
			Allowed.as_list().push_back(Enum[I]);
		}
		Out.push_back(Where + ": expected one of " + Repr(Allowed) + ", got " + Repr(Value));
		return;
	}
	const std::string& Text = Spec.as_str();
	if (Text == "match|null")
	{
		if (Value.is_null())
		{
			return;
		}
		if (!Value.is_dict())
		{
			Out.push_back(Where + ": expected object or null, got " + Repr(Value));
			return;
		}
		const Json& Schema = FScfgStore::Schema();
		CheckRecord(Value, Schema.as_dict().get("match"), Schema.as_dict().get("match_optional"), Where, Out);
		return;
	}
	const size_t Bar = Text.find('|');
	const std::string_view Base = std::string_view(Text).substr(0, Bar);
	if (Bar != std::string::npos && Value.is_null())
	{
		return;
	}
	if (!HasType(Value, Base))
	{
		Out.push_back(Where + ": expected " + Text + ", got " + Repr(Value));
	}
}

// state_schema._record: required fields present, each field known and of its type.
void CheckRecord(const Json& Record, const Json& Required, const Json& Optional, const std::string& Where,
	std::vector<std::string>& Out)
{
	if (!Record.is_dict())
	{
		Out.push_back(Where + ": expected object");
		return;
	}
	const Json::Dict& Fields = Record.as_dict();
	for (const auto& [Key, Unused] : Required.as_dict())
	{
		(void)Unused;
		if (!Fields.contains(Key))
		{
			Out.push_back(Where + ": missing " + QuotedKey(Key));
		}
	}
	for (const auto& [Key, Value] : Fields)
	{
		const Json* Spec = Required.as_dict().find(Key);
		if (Spec == nullptr)
		{
			Spec = Optional.as_dict().find(Key);
		}
		if (Spec == nullptr)
		{
			Out.push_back(Where + ": unknown " + QuotedKey(Key));
		}
		else
		{
			CheckValue(Value, *Spec, Where + "." + Key, Out);
		}
	}
}

scfg::Err RuntimeError(const FString& Message)
{
	return scfg::Err::make("RuntimeError", ScfgString::ToUtf8(Message));
}

// The OS's text for the last failed call (captured right after it), e.g. "errno=13 (Permission denied)".
FString LastError()
{
	const uint32 Code = FPlatformMisc::GetLastError();
	if (Code == 0)
	{
		return TEXT("unknown error");
	}
	TCHAR Buffer[512];
	FPlatformMisc::GetSystemErrorMessage(Buffer, UE_ARRAY_COUNT(Buffer), static_cast<int32>(Code));
	return FString(Buffer);
}

// A file or a directory is at P, readable or not (FileExists stats the file, so an unreadable one counts).
bool Exists(const FString& P)
{
	return IFileManager::Get().FileExists(*P) || IFileManager::Get().DirectoryExists(*P);
}

// The bytes of Source, or a RuntimeError naming it.
scfg::Result<std::string> ReadBytes(const FString& Source)
{
	if (IFileManager::Get().DirectoryExists(*Source))
	{
		return RuntimeError(FString::Printf(TEXT("cannot read %s: it is a directory"), *Source));
	}
	IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> File(Files.OpenRead(*Source));
	if (!File)
	{
		return RuntimeError(FString::Printf(TEXT("cannot read %s: %s"), *Source, *LastError()));
	}
	const int64 Size = File->Size();
	if (Size < 0)
	{
		return RuntimeError(FString::Printf(TEXT("cannot read %s: %s"), *Source, *LastError()));
	}
	std::string Bytes(static_cast<size_t>(Size), '\0');
	if (Size > 0 && !File->Read(reinterpret_cast<uint8*>(Bytes.data()), Size))
	{
		return RuntimeError(FString::Printf(TEXT("cannot read %s: %s"), *Source, *LastError()));
	}
	return Bytes;
}

// Writes Bytes to Target (created or truncated) and flushes them to the disk; Target is removed on a failure.
scfg::Status WriteBytes(const FString& Target, const std::string& Bytes)
{
	IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> File(Files.OpenWrite(*Target));
	if (!File)
	{
		return RuntimeError(FString::Printf(TEXT("cannot write %s: %s"), *Target, *LastError()));
	}
	const bool bWritten = (Bytes.empty() || File->Write(reinterpret_cast<const uint8*>(Bytes.data()),
		static_cast<int64>(Bytes.size()))) && File->Flush();
	const FString Why = bWritten ? FString() : LastError();
	File.Reset();  // closes it
	if (!bWritten)
	{
		Files.DeleteFile(*Target);
		return RuntimeError(FString::Printf(TEXT("cannot write %s: %s"), *Target, *Why));
	}
	return scfg::ok();
}

// The atomic save: <Target>.tmp written and flushed, then moved over Target (Move deletes Target first).
scfg::Status WriteText(const FString& Target, const std::string& Text)
{
	const FString Dir = FPaths::GetPath(Target);
	if (!IFileManager::Get().DirectoryExists(*Dir) && !IFileManager::Get().MakeDirectory(*Dir, true))
	{
		return RuntimeError(FString::Printf(TEXT("cannot create %s: %s"), *Dir, *LastError()));
	}
	const FString Tmp = Target + TEXT(".tmp");
	SCFG_TRY_VOID(WriteBytes(Tmp, Text));
	// bDoNotRetryOrError: no 0.5 s sleeps on the game thread and no engine log line; the failure is ours to report.
	if (!IFileManager::Get().Move(*Target, *Tmp, /*Replace=*/true, /*EvenIfReadOnly=*/false, /*Attributes=*/false,
			/*bDoNotRetryOrError=*/true))
	{
		return RuntimeError(FString::Printf(TEXT("cannot replace %s: %s"), *Target, *LastError()));
	}
	return scfg::ok();
}

Json MakeDoc(const Json& Pending, const Json& Receipts)
{
	return Json::dict({{"version", FScfgStore::VERSION}, {"pending", Pending}, {"receipts", Receipts}});
}

// The first free name for a copy of a bad Target: <Target>.corrupt, then .corrupt.2, ...
FString AsideName(const FString& Target)
{
	FString Aside = Target + TEXT(".corrupt");
	for (int32 N = 2; Exists(Aside); ++N)
	{
		Aside = FString::Printf(TEXT("%s.corrupt.%d"), *Target, N);
	}
	return Aside;
}

}  // namespace ScfgStoreDetail

FString FScfgStore::DefaultPath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Scfg") / TEXT("state.json"));
}

const scfg::Json& FScfgStore::Schema()
{
	static const scfg::Json Parsed = []()
	{
		scfg::Result<scfg::Json> Got = scfg::json::loads(ScfgStoreDetail::kSchema);
		check(Got.ok() && Got.value().is_dict());
		return Got.take();
	}();
	return Parsed;
}

std::vector<std::string> FScfgStore::CheckPending(const std::string& Sid, const scfg::Json& Record)
{
	const scfg::Json& S = Schema();
	std::vector<std::string> Problems;
	const std::string Where = "pending[" + Sid + "]";
	ScfgStoreDetail::CheckRecord(Record, S.as_dict().get("pending"), S.as_dict().get("pending_optional"), Where,
		Problems);
	if (Problems.empty() && Record.as_dict().get("id").as_str() != Sid)
	{
		Problems.push_back(Where + ": id does not match its key");
	}
	return Problems;
}

FScfgStoreLoad FScfgStore::Load(const FString& Target)
{
	using namespace ScfgStoreDetail;
	FScfgStoreLoad Out;
	FString Source = Target;
	if (!Exists(Target))
	{
		// A save stopped between Move's delete and its rename: the complete .tmp is the newest state.
		if (!Exists(Target + TEXT(".tmp")))
		{
			return Out;
		}
		Source = Target + TEXT(".tmp");
	}
	scfg::Result<std::string> Raw = ReadBytes(Source);
	if (!Raw.ok())
	{
		Out.Error = Raw.err();
		return Out;
	}
	FString Why;
	Json Doc;
	scfg::Result<Json> Parsed = scfg::json::loads(Raw.value());
	if (!Parsed.ok())
	{
		Why = TEXT("is not valid JSON");
	}
	else
	{
		Doc = Parsed.take();
		const Json Version = Doc.is_dict() ? Doc.as_dict().get("version") : Json();
		if (!Doc.is_dict())
		{
			Why = TEXT("is not a JSON object");
		}
		else if (!(Version.is_int64() && Version.as_int64() == VERSION))
		{
			Why = TEXT("has an unknown version ") + ScfgString::FromUtf8(Repr(Version));
		}
		else if (!(Doc.as_dict().get("pending").is_dict() && Doc.as_dict().get("receipts").is_dict()))
		{
			Why = TEXT("has no pending/receipts objects");
		}
	}
	if (!Why.IsEmpty())
	{
		const FString Aside = AsideName(Target);
		if (!IFileManager::Get().Move(*Aside, *Source, /*Replace=*/false, /*EvenIfReadOnly=*/false,
				/*Attributes=*/false, /*bDoNotRetryOrError=*/true))
		{
			// Starting empty would let the next save replace it with nothing kept anywhere.
			Out.Error = RuntimeError(FString::Printf(
				TEXT("the payment state file %s %s, and it could not be moved aside (%s)"), *Source, *Why, *LastError()));
			return Out;
		}
		Out.Warnings.Add(FString::Printf(TEXT("the payment state file %s %s; starting with no receipts or pending "
			"payments (the file was kept as %s). Players can bring purchases back with Restore."), *Source, *Why,
			*Aside));
		return Out;
	}

	// Record by record: a malformed pending session is dropped (the whole file copied aside first).
	Json Pending = Json::dict();
	TArray<FString> Dropped;
	for (const auto& [Sid, Record] : Doc.as_dict().get("pending").as_dict())
	{
		const std::vector<std::string> Problems = CheckPending(Sid, Record);
		if (Problems.empty())
		{
			Pending.as_dict().set(Sid, Record);
			continue;
		}
		std::string Joined;
		for (const std::string& Each : Problems)
		{
			Joined += (Joined.empty() ? "" : "; ") + Each;
		}
		Dropped.Add(FString::Printf(TEXT("%s (%s)"), *ScfgString::FromUtf8(QuotedKey(Sid)),
			*ScfgString::FromUtf8(Joined)));
	}
	if (Dropped.Num() > 0)
	{
		const FString Aside = AsideName(Target);
		const scfg::Status Copied = WriteBytes(Aside, Raw.value());
		if (!Copied.ok())
		{
			Out.Error = RuntimeError(FString::Printf(
				TEXT("the payment state file %s has malformed pending payments, and it could not be copied aside (%s)"),
				*Source, *ScfgString::FromUtf8(Copied.err().message)));
			return Out;
		}
		for (const FString& Each : Dropped)
		{
			Out.Warnings.Add(FString::Printf(TEXT("the payment state file %s has a malformed pending payment %s; it "
				"was dropped (the file was kept as %s)"), *Source, *Each, *Aside));
		}
		Out.bCleaned = true;
	}
	Out.Pending = std::move(Pending);
	Out.Receipts = Doc.as_dict().get("receipts");
	return Out;
}

scfg::Status FScfgStore::SaveFile(const FString& Target, const scfg::Json& InPending, const scfg::Json& InReceipts)
{
	SCFG_TRY(std::string Text, scfg::json::dumps(ScfgStoreDetail::MakeDoc(InPending, InReceipts)));
	return ScfgStoreDetail::WriteText(Target, Text);
}

FScfgStore::FScfgStore(FOptions InOptions) : Options(MoveTemp(InOptions))
{
	check(IsInGameThread());
	const FString Dir = Options.Directory.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("Scfg") : Options.Directory;
	Path = FPaths::ConvertRelativePathToFull(Dir / TEXT("state.json"));
	UE_LOG(LogScfg, Log, TEXT("scfg: the payment state file is %s"), *Path);
	FScfgStoreLoad Got = Load(Path);
	for (const FString& Each : Got.Warnings)
	{
		ReportWarning(Each);
	}
	if (Got.Error)
	{
		Blocked = MoveTemp(Got.Error);
		ReportError(*Blocked);
		return;
	}
	Pending = std::move(Got.Pending);
	Receipts = std::move(Got.Receipts);
	if (Got.bCleaned)
	{
		Save(Pending, Receipts);  // the dropped records are kept in the copy aside, not loaded again next launch
		return;
	}
	scfg::Result<std::string> Text = scfg::json::dumps(ScfgStoreDetail::MakeDoc(Pending, Receipts));
	if (Text.ok())
	{
		LastText = Text.take();
	}
}

FScfgStore::~FScfgStore()
{
	check(IsInGameThread());
	Detach();
	DisarmRetry();
	if (Owed && !Blocked)
	{
		TrySave(*Owed);  // the last chance (the game quitting); a failure was reported already
	}
}

scfg::Result<TUniquePtr<FScfgWorker>> FScfgStore::CreateWorker(scfg::config::Config Config,
	TUniquePtr<FScfgTransport> Transport, FScfgWorker::FOptions WorkerOptions)
{
	check(IsInGameThread());
	if (Blocked)
	{
		return *Blocked;  // reported when the file was read
	}
	scfg::Result<TUniquePtr<FScfgWorker>> Made =
		FScfgWorker::Create(MoveTemp(Config), MoveTemp(Transport), Pending, Receipts, MoveTemp(WorkerOptions));
	if (!Made.ok())
	{
		ReportError(Made.err());
		return Made;
	}
	Attach(*Made.value());
	return Made;
}

void FScfgStore::Attach(FScfgWorker& InWorker)
{
	check(IsInGameThread());
	Detach();
	Worker = &InWorker;
	Event = InWorker.OnStateChanged;
	// A raw `this`, removed in Detach() (the destructor's too): the list is shared, so this is safe whichever of the
	// store and the worker goes first.
	Handle = InWorker.OnStateChanged->AddLambda([this]() { OnStateChanged(); });
}

void FScfgStore::Detach()
{
	check(IsInGameThread());
	if (const TSharedPtr<FScfgStateChanged> Pinned = Event.Pin())
	{
		Pinned->Remove(Handle);
	}
	Event.Reset();
	Handle.Reset();
	Worker = nullptr;
}

void FScfgStore::OnStateChanged()
{
	if (Worker != nullptr)
	{
		const scfg::engine::Snapshot& Snapshot = Worker->GetSnapshot();
		Save(Snapshot.pending, Snapshot.receipts);
	}
}

bool FScfgStore::Save(const scfg::Json& InPending, const scfg::Json& InReceipts)
{
	check(IsInGameThread());
	if (Blocked)
	{
		return false;  // what is on disk could not be read: what is in memory must not replace it
	}
	const scfg::Json Doc = ScfgStoreDetail::MakeDoc(InPending, InReceipts);
	if (TrySave(Doc))
	{
		Owed.Reset();
		DisarmRetry();
		return true;
	}
	Owed = scfg::deep_copy(Doc);  // the newest state replaces an older one still owed
	ArmRetry();
	return false;
}

bool FScfgStore::TrySave(const scfg::Json& Doc)
{
	scfg::Result<std::string> Text = scfg::json::dumps(Doc);
	scfg::Status Written = scfg::ok();
	if (!Text.ok())
	{
		Written = Text.err();
	}
	else if (Text.value() == LastText)
	{
		bSaveFailing = false;  // the file already holds it: nothing is owed
		return true;
	}
	else
	{
		Written = ScfgStoreDetail::WriteText(Path, Text.value());
	}
	if (!Written.ok())
	{
		if (!bSaveFailing)  // once, not at every retry, until a save succeeds
		{
			bSaveFailing = true;
			ReportError(Written.err());
		}
		return false;
	}
	++Writes;
	LastText = Text.take();
	bSaveFailing = false;
	return true;
}

bool FScfgStore::Retry(float DeltaTime)
{
	(void)DeltaTime;
	if (Owed && !Blocked && !TrySave(*Owed))
	{
		return true;  // again in RETRY_INTERVAL seconds
	}
	Owed.Reset();
	RetryHandle.Reset();  // returning false removes this ticker
	return false;
}

void FScfgStore::ArmRetry()
{
	if (!RetryHandle.IsValid())
	{
		FTSTicker& Ticker = Options.Ticker ? *Options.Ticker : FTSTicker::GetCoreTicker();
		RetryHandle = Ticker.AddTicker(TEXT("ScfgStoreRetry"), RETRY_INTERVAL,
			[this](float DeltaTime) { return Retry(DeltaTime); });
	}
}

void FScfgStore::DisarmRetry()
{
	if (RetryHandle.IsValid())
	{
		FTSTicker::RemoveTicker(RetryHandle);
		RetryHandle.Reset();
	}
}

void FScfgStore::ReportError(const scfg::Err& Error) const
{
	if (Options.OnError)
	{
		Options.OnError(Error);
	}
	else
	{
		UE_LOG(LogScfg, Error, TEXT("scfg: %s"), *ScfgString::FromUtf8(Error.str()));
	}
}

void FScfgStore::ReportWarning(const FString& Warning) const
{
	if (Options.OnWarning)
	{
		Options.OnWarning(Warning);
	}
	else
	{
		UE_LOG(LogScfg, Warning, TEXT("scfg: %s"), *Warning);
	}
}
