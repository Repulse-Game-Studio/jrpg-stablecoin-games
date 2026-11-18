// Copyright 2026 Repulse
// Twin of godot/addons/scfg/store.gd (see ScfgStore.h), with conformance/python/scfg_conformance/state_schema.py's
// pending-record check.
#include "ScfgStore.h"

#include "ScfgLog.h"
#include "ScfgString.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include "Scfg/Py.h"

#include <cstdint>
#include <string_view>
#include <utility>

// The POSIX desktops: the store makes its own file calls (see ScfgStore.h, SAVING).
#if PLATFORM_LINUX || PLATFORM_MAC
#define SCFG_STORE_POSIX 1
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define SCFG_STORE_POSIX 0
#endif

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif

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

enum class EKind
{
	Missing,
	File,
	Directory
};

#if SCFG_STORE_POSIX

// The OS's text for an errno read right at the failing call, e.g. "errno=13 (Permission denied)".
FString ErrnoText(int Code)
{
	return FString::Printf(TEXT("errno=%d (%s)"), Code, *ScfgString::FromUtf8(std::strerror(Code)));
}

// What is at P: stat(2), so a directory above it that cannot be searched is an error, not a missing file.
scfg::Result<EKind> Stat(const FString& P)
{
	struct stat Info;
	if (::stat(ScfgString::ToUtf8(P).c_str(), &Info) == 0)
	{
		return S_ISDIR(Info.st_mode) ? EKind::Directory : EKind::File;
	}
	const int Code = errno;
	if (Code == ENOENT)
	{
		return EKind::Missing;
	}
	return RuntimeError(FString::Printf(TEXT("cannot read %s: %s"), *P, *ErrnoText(Code)));
}

scfg::Result<std::string> ReadBytes(const FString& Source)
{
	const int File = ::open(ScfgString::ToUtf8(Source).c_str(), O_RDONLY | O_CLOEXEC);
	if (File < 0)
	{
		const int Code = errno;
		return RuntimeError(FString::Printf(TEXT("cannot read %s: %s"), *Source, *ErrnoText(Code)));
	}
	std::string Bytes;
	char Buffer[16384];
	for (;;)
	{
		const ssize_t Got = ::read(File, Buffer, sizeof(Buffer));
		if (Got > 0)
		{
			Bytes.append(Buffer, static_cast<size_t>(Got));
			continue;
		}
		if (Got == 0)
		{
			break;
		}
		const int Code = errno;
		if (Code == EINTR)
		{
			continue;
		}
		::close(File);
		return RuntimeError(FString::Printf(TEXT("cannot read %s: %s"), *Source, *ErrnoText(Code)));
	}
	::close(File);
	return Bytes;
}

// Writes Bytes to Target (created or truncated) and flushes them to the disk; Target is removed on a failure.
scfg::Status WriteBytes(const FString& Target, const std::string& Bytes)
{
	const std::string Name = ScfgString::ToUtf8(Target);
	const int File = ::open(Name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (File < 0)
	{
		const int Code = errno;
		return RuntimeError(FString::Printf(TEXT("cannot write %s: %s"), *Target, *ErrnoText(Code)));
	}
	int Code = 0;
	size_t Done = 0;
	while (Done < Bytes.size() && Code == 0)
	{
		const ssize_t Put = ::write(File, Bytes.data() + Done, Bytes.size() - Done);
		if (Put >= 0)
		{
			Done += static_cast<size_t>(Put);
		}
		else if (errno != EINTR)
		{
			Code = errno;
		}
	}
#if PLATFORM_LINUX
	if (Code == 0 && ::fdatasync(File) != 0)
#else
	if (Code == 0 && ::fsync(File) != 0)
#endif
	{
		Code = errno;
	}
	if (::close(File) != 0 && Code == 0)
	{
		Code = errno;
	}
	if (Code != 0)
	{
		::unlink(Name.c_str());
		return RuntimeError(FString::Printf(TEXT("cannot write %s: %s"), *Target, *ErrnoText(Code)));
	}
	return scfg::ok();
}

// mkdir -p.
scfg::Status MakeDirs(const FString& Dir)
{
	const std::string Path = ScfgString::ToUtf8(Dir);
	for (size_t End = 1; End <= Path.size(); ++End)
	{
		if (End != Path.size() && Path[End] != '/')
		{
			continue;
		}
		const std::string Part = Path.substr(0, End);
		if (::mkdir(Part.c_str(), 0777) != 0)
		{
			const int Code = errno;
			struct stat Info;
			if (::stat(Part.c_str(), &Info) != 0 || !S_ISDIR(Info.st_mode))  // an existing directory is fine
			{
				return RuntimeError(FString::Printf(TEXT("cannot create %s: %s"), *Dir, *ErrnoText(Code)));
			}
		}
	}
	return scfg::ok();
}

// rename(2): replaces To in one step when it exists. Why gets the reason on a failure.
bool Rename(const FString& From, const FString& To, bool bReplace, FString& Why)
{
	(void)bReplace;  // the callers that do not replace pick a free name first
	if (::rename(ScfgString::ToUtf8(From).c_str(), ScfgString::ToUtf8(To).c_str()) == 0)
	{
		return true;
	}
	const int Code = errno;
	Why = ErrnoText(Code);
	return false;
}

#else  // !SCFG_STORE_POSIX: through IPlatformFile (Windows: MoveFileExW for the replace)

FString LastErrorText()
{
	const uint32 Code = FPlatformMisc::GetLastError();
	if (Code == 0)
	{
		return TEXT("unknown error");
	}
	TCHAR Buffer[512];
	FPlatformMisc::GetSystemErrorMessage(Buffer, UE_ARRAY_COUNT(Buffer), static_cast<int32>(Code));
	return FString::Printf(TEXT("error %u (%s)"), Code, Buffer);
}

// What is at P (here a failure to look is a missing file: unverified, no such platform builds here).
scfg::Result<EKind> Stat(const FString& P)
{
	if (IFileManager::Get().DirectoryExists(*P))
	{
		return EKind::Directory;
	}
	return IFileManager::Get().FileExists(*P) ? EKind::File : EKind::Missing;
}

scfg::Result<std::string> ReadBytes(const FString& Source)
{
	IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> File(Files.OpenRead(*Source));
	if (!File)
	{
		return RuntimeError(FString::Printf(TEXT("cannot read %s: %s"), *Source, *LastErrorText()));
	}
	const int64 Size = File->Size();
	std::string Bytes(static_cast<size_t>(Size > 0 ? Size : 0), '\0');
	if (Size < 0 || (Size > 0 && !File->Read(reinterpret_cast<uint8*>(Bytes.data()), Size)))
	{
		return RuntimeError(FString::Printf(TEXT("cannot read %s: %s"), *Source, *LastErrorText()));
	}
	return Bytes;
}

scfg::Status WriteBytes(const FString& Target, const std::string& Bytes)
{
	IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> File(Files.OpenWrite(*Target));
	if (!File)
	{
		return RuntimeError(FString::Printf(TEXT("cannot write %s: %s"), *Target, *LastErrorText()));
	}
	const bool bWritten = (Bytes.empty() || File->Write(reinterpret_cast<const uint8*>(Bytes.data()),
		static_cast<int64>(Bytes.size()))) && File->Flush();
	const FString Why = bWritten ? FString() : LastErrorText();
	File.Reset();
	if (!bWritten)
	{
		Files.DeleteFile(*Target);
		return RuntimeError(FString::Printf(TEXT("cannot write %s: %s"), *Target, *Why));
	}
	return scfg::ok();
}

scfg::Status MakeDirs(const FString& Dir)
{
	if (IFileManager::Get().DirectoryExists(*Dir) || IFileManager::Get().MakeDirectory(*Dir, true))
	{
		return scfg::ok();
	}
	return RuntimeError(FString::Printf(TEXT("cannot create %s: %s"), *Dir, *LastErrorText()));
}

bool Rename(const FString& From, const FString& To, bool bReplace, FString& Why)
{
#if PLATFORM_WINDOWS
	// IPlatformFile has no replacing move (MoveFileW refuses an existing target, and IFileManager::Move deletes it
	// first). Unverified: no Win64 build here.
	const DWORD Flags = MOVEFILE_WRITE_THROUGH | (bReplace ? MOVEFILE_REPLACE_EXISTING : 0);
	if (::MoveFileExW(*From, *To, Flags))
	{
		return true;
	}
#else
	(void)bReplace;
	if (FPlatformFileManager::Get().GetPlatformFile().MoveFile(*To, *From))
	{
		return true;
	}
#endif
	Why = LastErrorText();
	return false;
}

#endif  // SCFG_STORE_POSIX

// FScfgStore::ReplaceHookForTests(): empty in a game.
TFunction<scfg::Status(const FString&, const FString&)>& ReplaceHook()
{
	static TFunction<scfg::Status(const FString&, const FString&)> Hook;
	return Hook;
}

// The final step of a save: the test hook, else Rename.
scfg::Status Replace(const FString& From, const FString& To)
{
	if (const TFunction<scfg::Status(const FString&, const FString&)>& Hook = ReplaceHook(); Hook)
	{
		return Hook(From, To);
	}
	FString Why;
	if (!Rename(From, To, /*bReplace=*/true, Why))
	{
		return RuntimeError(FString::Printf(TEXT("cannot replace %s: %s"), *To, *Why));
	}
	return scfg::ok();
}

// The atomic save: <Target>.tmp written and flushed, then renamed over Target (nothing is deleted first).
scfg::Status WriteText(const FString& Target, const std::string& Text)
{
	SCFG_TRY_VOID(MakeDirs(FPaths::GetPath(Target)));
	const FString Tmp = Target + TEXT(".tmp");
	SCFG_TRY_VOID(WriteBytes(Tmp, Text));
	return Replace(Tmp, Target);
}

Json MakeDoc(const Json& Pending, const Json& Receipts)
{
	return Json::dict({{"version", FScfgStore::VERSION}, {"pending", Pending}, {"receipts", Receipts}});
}

// Godot's _pending_key: what must reach the disk at once (which sessions exist, their state and hint_tx).
Json PendingKey(const Json& Pending)
{
	Json Key = Json::dict();
	if (Pending.is_dict())
	{
		for (const auto& [Sid, Record] : Pending.as_dict())
		{
			Key.as_dict().set(Sid, Record.is_dict() ?
				Json::list({Record.as_dict().get("state"), Record.as_dict().get("hint_tx")}) : Record);
		}
	}
	return Key;
}

std::string TextOf(const Json& V)
{
	scfg::Result<std::string> Got = scfg::json::dumps(V);
	return Got.ok() ? Got.take() : std::string();
}

// The first free name for a copy of a bad Target: <Target>.corrupt, then .corrupt.2, ... (an error from stat counts
// as taken).
FString AsideName(const FString& Target)
{
	FString Aside = Target + TEXT(".corrupt");
	for (int32 N = 2;; ++N)
	{
		const scfg::Result<EKind> Kind = Stat(Aside);
		if (!Kind.ok() || Kind.value() == EKind::Missing)
		{
			return Aside;  // a name that cannot be looked at: the move then fails with the reason
		}
		Aside = FString::Printf(TEXT("%s.corrupt.%d"), *Target, N);
	}
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

TFunction<scfg::Status(const FString& From, const FString& To)>& FScfgStore::ReplaceHookForTests()
{
	return ScfgStoreDetail::ReplaceHook();
}

#if !UE_BUILD_SHIPPING
TFunction<bool(const FString& Path)>& FScfgStore::DefaultPathHookForTests()
{
	static TFunction<bool(const FString&)> Hook;
	return Hook;
}
#endif

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
	scfg::Result<EKind> Kind = Stat(Target);
	if (!Kind.ok())
	{
		Out.Error = Kind.err();
		return Out;
	}
	if (Kind.value() == EKind::Missing)
	{
		// A save stopped before its rename: the complete .tmp is the newest state.
		const FString Tmp = Target + TEXT(".tmp");
		scfg::Result<EKind> TmpKind = Stat(Tmp);
		if (!TmpKind.ok())
		{
			Out.Error = TmpKind.err();
			return Out;
		}
		if (TmpKind.value() == EKind::Missing)
		{
			return Out;
		}
		Source = Tmp;
		Kind = TmpKind;
	}
	if (Kind.value() == EKind::Directory)
	{
		Out.Error = RuntimeError(FString::Printf(TEXT("cannot read %s: it is a directory"), *Source));
		return Out;
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
		FString Stuck;
		if (!Rename(Source, Aside, /*bReplace=*/false, Stuck))
		{
			// Starting empty would let the next save replace it with nothing kept anywhere.
			Out.Error = RuntimeError(FString::Printf(
				TEXT("the payment state file %s %s, and it could not be moved aside (%s)"), *Source, *Why, *Stuck));
			return Out;
		}
		Out.Warnings.Add(FString::Printf(TEXT("the payment state file %s %s; starting with no receipts or pending "
			"payments (the file was kept as %s). Players can bring purchases back with Restore."), *Source, *Why,
			*Aside));
		return Out;
	}
	if (Source != Target)
	{
		// The .tmp is the document: it becomes the file now, so the next save's .tmp never truncates the only copy.
		FString Stuck;
		if (!Rename(Source, Target, /*bReplace=*/true, Stuck))
		{
			Out.Error = RuntimeError(FString::Printf(
				TEXT("the payment state file %s was read but could not be renamed to %s (%s)"), *Source, *Target, *Stuck));
			return Out;
		}
		Source = Target;
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
				"was dropped (the file was kept as %s). A payment made for it can be brought back with Restore."),
				*Source, *Each, *Aside));
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
#if !UE_BUILD_SHIPPING
	if (const TFunction<bool(const FString&)>& Hook = DefaultPathHookForTests(); Hook && Path == DefaultPath() && Hook(Path))
	{
		Blocked = scfg::Err::make("RuntimeError", "cannot read " + scfg::py::repr_str(ScfgString::ToUtf8(Path)) +
			": the automation tests refuse the project's own payment state file");
		ReportError(*Blocked);
		return;
	}
#endif
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
	DiskText = ScfgStoreDetail::TextOf(ScfgStoreDetail::MakeDoc(Pending, Receipts));
	DiskReceiptsText = ScfgStoreDetail::TextOf(Receipts);
	DiskKeyText = ScfgStoreDetail::TextOf(ScfgStoreDetail::PendingKey(Pending));
}

FScfgStore::~FScfgStore()
{
	check(IsInGameThread());
	*Alive = false;
	Detach();
	DisarmRetry();
	if (Owed && !Blocked)
	{
		// What is still owed (scan progress inside the interval, or a failed save): the game is quitting. A failure
		// was reported already, or is reported now; OnError must not touch the store here.
		const scfg::Json Doc = MoveTemp(*Owed);
		Owed.Reset();
		TrySave(Doc);
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
		ReportError(Made.err());  // may destroy the store: only the local is touched after
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

double FScfgStore::Now() const
{
	return Options.Clock ? Options.Clock() : FPlatformTime::Seconds();
}

bool FScfgStore::IsUrgent(const scfg::Json& InPending, const scfg::Json& InReceipts) const
{
	using namespace ScfgStoreDetail;
	return DiskReceiptsText.empty() || TextOf(InReceipts) != DiskReceiptsText ||
		TextOf(PendingKey(InPending)) != DiskKeyText;
}

bool FScfgStore::Save(const scfg::Json& InPending, const scfg::Json& InReceipts)
{
	check(IsInGameThread());
	if (Blocked)
	{
		return false;  // what is on disk could not be read: what is in memory must not replace it
	}
	const scfg::Json Doc = ScfgStoreDetail::MakeDoc(InPending, InReceipts);
	const scfg::Result<std::string> Text = scfg::json::dumps(Doc);
	if (Text.ok() && Text.value() == DiskText)
	{
		Owed.Reset();  // the file already holds it: nothing is owed
		DisarmRetry();
		bSaveFailing = false;
		return true;
	}
	const double Time = Now();
	if (Text.ok() && !IsUrgent(InPending, InReceipts) && Time - LastAttempt < PENDING_SAVE_INTERVAL)
	{
		Owed = scfg::deep_copy(Doc);  // scan progress only: the ticker writes it within RETRY_INTERVAL
		ArmRetry();
		return true;
	}
	LastAttempt = Time;
	const TSharedRef<bool> StillAlive = Alive;
	if (TrySave(Doc))
	{
		Owed.Reset();
		DisarmRetry();
		return true;
	}
	if (!*StillAlive)
	{
		return false;  // OnError destroyed the store
	}
	Owed = scfg::deep_copy(Doc);  // the newest state replaces an older one still owed
	ArmRetry();
	return false;
}

bool FScfgStore::TrySave(const scfg::Json& Doc)
{
	scfg::Result<std::string> Text = scfg::json::dumps(Doc);
	if (Text.ok() && Text.value() == DiskText)
	{
		bSaveFailing = false;  // the file already holds it: nothing is owed
		return true;
	}
	const scfg::Status Written = Text.ok() ? ScfgStoreDetail::WriteText(Path, Text.value()) : scfg::Status(Text.err());
	if (!Written.ok())
	{
		if (!bSaveFailing)  // once, not at every retry, until a save succeeds
		{
			bSaveFailing = true;
			ReportError(Written.err());  // may destroy the store: nothing is touched after
		}
		return false;
	}
	++Writes;
	DiskText = Text.take();
	DiskReceiptsText = ScfgStoreDetail::TextOf(Doc.as_dict().get("receipts"));
	DiskKeyText = ScfgStoreDetail::TextOf(ScfgStoreDetail::PendingKey(Doc.as_dict().get("pending")));
	bSaveFailing = false;
	return true;
}

bool FScfgStore::Retry(float DeltaTime)
{
	(void)DeltaTime;
	if (Owed && !Blocked)
	{
		LastAttempt = Now();
		const TSharedRef<bool> StillAlive = Alive;
		if (!TrySave(*Owed))
		{
			// Again in RETRY_INTERVAL seconds, unless OnError destroyed the store (its destructor removed this ticker).
			return *StillAlive;
		}
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
		// A copy: a handler that destroys the store destroys Options with it.
		const TFunction<void(const scfg::Err&)> Handler = Options.OnError;
		Handler(Error);
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
		const TFunction<void(const FString&)> Handler = Options.OnWarning;
		Handler(Warning);
	}
	else
	{
		UE_LOG(LogScfg, Warning, TEXT("scfg: %s"), *Warning);
	}
}
