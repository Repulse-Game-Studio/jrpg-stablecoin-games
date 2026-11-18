// Copyright 2026 Repulse
// No Python twin: FScfgStore, the twin of godot/tests/test_store.gd one test at a time, then the store's own rules
// (the worker round trip, saving only what changed, the retries, a dumps Err, the record-level check). Every test works
// in its own directory under FPaths::ProjectSavedDir()/ScfgTests/<random>, removed afterwards.
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#include "ScfgStore.h"
#include "ScfgString.h"
#include "ScfgTransport.h"
#include "ScfgWorker.h"

#include "Scfg/BigInt.h"
#include "Scfg/Co.h"
#include "Scfg/Config.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#if PLATFORM_LINUX || PLATFORM_MAC
#include <sys/stat.h>
#define SCFG_STORE_TESTS_CHMOD 1
#else
#define SCFG_STORE_TESTS_CHMOD 0
#endif

#if WITH_DEV_AUTOMATION_TESTS

// FScfgStore's private file writer and its test hook (the store's friend).
struct FScfgStoreTestAccess
{
	static scfg::Status SaveFile(const FString& Path, const scfg::Json& Pending, const scfg::Json& Receipts)
	{
		return FScfgStore::SaveFile(Path, Pending, Receipts);
	}
	static TFunction<scfg::Status(const FString&, const FString&)>& ReplaceHook()
	{
		return FScfgStore::ReplaceHookForTests();
	}
};

namespace ScfgStoreTests
{

using scfg::Json;
using scfg::Result;

constexpr const char* kWallet = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
constexpr int64_t kNow = 1700000100;
const char* const kTx = "0x135de9b0d45e605167347488c624be20bda97481b0bbabbc6e1d97bb816d2d1a";

// A pending EVM session as the Engine stores it (every field of the schema): godot's _pending() has only a few, which
// this store's record check would drop.
Json Session(const char* Id, int64_t Cursor = 900)
{
	return Json::dict({{"id", Id}, {"kind", "unlock"}, {"item", "chapter2"}, {"title", "Chapter 2"}, {"token", "USDC"},
		{"chain", "base"}, {"price", "4.99"}, {"code", 37}, {"nonce", 6}, {"units", 4993706}, {"start_block", 900},
		{"cursor", Cursor}, {"created_at", kNow}, {"state", "awaiting"}, {"ui_status", "open"}, {"hint_tx", Json()},
		{"hint_at", Json()}, {"match", Json()}});
}

Json Pending()
{
	return Json::dict({{"ps_1", Session("ps_1")}});
}

Json StoredReceipts()
{
	const std::string Key = std::string("base:") + kTx + ":3";
	return Json::dict({{Key, Json::dict({{"amount", "4.99"}, {"block", 1234}, {"chain", "base"}, {"item", "chapter2"},
		{"kind", "unlock"}, {"log_index", 3}, {"payer", Json()}, {"recipient", kWallet}, {"source", "payment"},
		{"token", "USDC"}, {"tx_hash", kTx}})}});
}

// Two JSON values print the same with sorted keys (the conformance rule: key order ignored, true is not 1).
bool Same(const Json& A, const Json& B)
{
	scfg::json::DumpOptions Sorted;
	Sorted.sort_keys = true;
	Result<std::string> X = scfg::json::dumps(A, Sorted);
	Result<std::string> Y = scfg::json::dumps(B, Sorted);
	return X.ok() && Y.ok() && X.value() == Y.value();
}

FString Text(const Json& V)
{
	Result<std::string> Got = scfg::json::dumps(V);
	return Got.ok() ? ScfgString::FromUtf8(Got.value()) : FString(TEXT("<dumps failed>"));
}

std::optional<std::string> ReadAll(const FString& P)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *P, FILEREAD_Silent))
	{
		return std::nullopt;
	}
	return std::string(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
}

void WriteAll(const FString& P, const std::string& Bytes)
{
	TArray<uint8> Data(reinterpret_cast<const uint8*>(Bytes.data()), static_cast<int32>(Bytes.size()));
	FFileHelper::SaveArrayToFile(Data, *P);
}

bool FileExists(const FString& P)
{
	return IFileManager::Get().FileExists(*P);
}

// This test's directory, removed (with the permissions it changed put back) when the test ends.
struct FTempDir
{
	FString Dir;
	TArray<FString> Restore;

	FTempDir()
	{
		Dir = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("ScfgTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Dir, true);
	}
	FTempDir(const FTempDir&) = delete;
	FTempDir& operator=(const FTempDir&) = delete;
	~FTempDir()
	{
		for (const FString& Each : Restore)
		{
			SetMode(Each, 0700);
		}
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Dir), false, false);  // ScfgTests/, once it is empty
	}

	FString operator/(const TCHAR* Name) const { return Dir / Name; }

	// chmod; false where this platform has none.
	bool SetMode(const FString& P, int Mode)
	{
#if SCFG_STORE_TESTS_CHMOD
		Restore.AddUnique(P);
		return chmod(TCHAR_TO_UTF8(*P), static_cast<mode_t>(Mode)) == 0;
#else
		(void)P, (void)Mode;
		return false;
#endif
	}
};

// The store's options for a test: its directory, a ticker the test ticks, and what was reported.
struct FReports
{
	TArray<FString> Errors;
	TArray<FString> Warnings;
};

FScfgStore::FOptions Options(const FString& Dir, FTSTicker& Ticker, FReports& Reports, double* Clock = nullptr)
{
	FScfgStore::FOptions Out;
	Out.Directory = Dir;
	Out.Ticker = &Ticker;
	if (Clock != nullptr)
	{
		Out.Clock = [Clock]() { return *Clock; };
	}
	Out.OnError = [&Reports](const scfg::Err& E) { Reports.Errors.Add(ScfgString::FromUtf8(E.str())); };
	Out.OnWarning = [&Reports](const FString& W) { Reports.Warnings.Add(W); };
	return Out;
}

scfg::config::Config BaseConfig()
{
	Result<scfg::config::Config> Built = scfg::config::build_config(
		Json::dict({{"wallet", kWallet}, {"chains", Json::list({"base"})}, {"testnet", true}}));
	scfg::config::Config Out = Built.take();
	Result<scfg::models::Item> Item = scfg::config::add_item(
		Out, "chapter2", Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}));
	(void)Item.take();
	return Out;
}

// A node that answers at once: a head that grows by one per eth_blockNumber, no logs.
class FInstantTransport final : public FScfgTransport
{
public:
	int64_t Head = 1000;

	virtual scfg::Future<Json> post_json(std::string Url, Json Payload, double Timeout) override
	{
		(void)Url, (void)Timeout;
		const std::string Method = Payload.as_dict().get("method").as_str();
		Json Answer;
		if (Method == "eth_blockNumber")
		{
			Answer = Json(ScfgString::ToUtf8(FString::Printf(TEXT("0x%llx"), static_cast<unsigned long long>(Head++))));
		}
		else if (Method == "eth_getLogs")
		{
			Answer = Json::list();
		}
		else
		{
			return scfg::Future<Json>::completed(scfg::Err::transport_error("no answer for " + Method, std::nullopt));
		}
		return scfg::Future<Json>::completed(
			Json::dict({{"jsonrpc", "2.0"}, {"id", Payload.as_dict().get("id")}, {"result", Answer}}));
	}
	virtual void Shutdown() override {}
};

FScfgWorker::FOptions WorkerOptions(FTSTicker& Ticker, TArray<FString>& Errors)
{
	FScfgWorker::FOptions Out;
	Out.Ticker = &Ticker;
	Out.Clock = []() { return kNow; };
	Out.OnError = [&Errors](const scfg::Err& E) { Errors.Add(ScfgString::FromUtf8(E.str())); };
	return Out;
}

void Frames(FTSTicker& Ticker, int32 N)
{
	for (int32 I = 0; I < N; ++I)
	{
		Ticker.Tick(0.1f);
	}
}

// FUnixPlatformFile logs a warning for each open() that fails other than with ENOENT (UnixPlatformFile.cpp:807,
// :1315). The store makes its own calls on Linux, so only a test's own read through the engine (ReadAll) logs it.
void ExpectFileWarnings(FAutomationTestBase& Test)
{
	Test.AddExpectedMessagePlain(TEXT("failed: errno="), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
}

}  // namespace ScfgStoreTests

#define SCFG_STORE_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.Store." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

SCFG_STORE_TEST(FScfgStoreRoundTripTest, "SaveThenLoadRoundTripsWithInts")

// test_save_then_load_round_trips_with_ints.
bool FScfgStoreRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	TestTrue(TEXT("saved"), FScfgStoreTestAccess::SaveFile(Path, Pending(), StoredReceipts()).ok());
	const FScfgStoreLoad Got = FScfgStore::Load(Path);
	TestEqual(TEXT("no warning"), Got.Warnings.Num(), 0);
	TestFalse(TEXT("no error"), Got.Error.IsSet());
	TestTrue(TEXT("pending"), Same(Got.Pending, Pending()));
	TestTrue(TEXT("receipts"), Same(Got.Receipts, StoredReceipts()));
	TestTrue(TEXT("ints stay ints"), Got.Pending.as_dict().get("ps_1").as_dict().get("cursor").is_int64());
	const std::optional<std::string> Raw = ReadAll(Path);
	Result<Json> Doc = scfg::json::loads(Raw.value_or(""));
	if (TestTrue(TEXT("the file is JSON"), Doc.ok() && Doc.value().is_dict()))
	{
		TestTrue(TEXT("version 1"), Same(Doc.value().as_dict().get("version"), Json(1)));
		TestEqual(TEXT("version, pending and receipts only"), static_cast<int32>(Doc.value().as_dict().size()), 3);
		TestEqual(TEXT("in that order (json.dumps of the dict, keys as inserted)"),
			ScfgString::FromUtf8(Raw->substr(0, 24)), TEXT("{\"version\": 1, \"pending\""));
	}
	TestFalse(TEXT("no .tmp left behind"), FileExists(Path + TEXT(".tmp")));
	TestTrue(TEXT("the default file is Saved/Scfg/state.json, absolute"),
		FScfgStore::DefaultPath() == FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()) / TEXT("Scfg/state.json")
		&& !FPaths::IsRelative(FScfgStore::DefaultPath()));
	return true;
}

SCFG_STORE_TEST(FScfgStoreDefaultPathRefusedTest, "TheTestsRefuseTheProjectsOwnStateFile")

// ScfgTestsModule.cpp's hook: while a test runs, a store on the project's own Saved/Scfg/state.json (DefaultPath())
// does not read it (blocked, reported, nothing written there) and logs the Error that fails the test that did it.
bool FScfgStoreDefaultPathRefusedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	if (!TestTrue(TEXT("the tests' module set the hook"), static_cast<bool>(FScfgStore::DefaultPathHookForTests())))
	{
		return true;
	}
	const FString Path = FScfgStore::DefaultPath();
	const bool bExisted = FileExists(Path);
	const std::optional<std::string> Before = bExisted ? ReadAll(Path) : std::nullopt;
	AddExpectedMessagePlain(TEXT("scfg tests: a test opened the project's own payment state file"), ELogVerbosity::Error,
		EAutomationExpectedMessageFlags::Contains, 1);
	FTSTicker Ticker;
	FReports Reports;
	{
		FScfgStore Store(Options(FPaths::ProjectSavedDir() / TEXT("Scfg"), Ticker, Reports));
		TestEqual(TEXT("the default path"), Store.GetPath(), Path);
		TestTrue(TEXT("refused unread: blocked"), Store.IsBlocked() && Store.GetPending().as_dict().size() == 0 &&
			Store.GetReceipts().as_dict().size() == 0);
		TestTrue(TEXT("saying why"), Store.IsBlocked() &&
			ScfgString::FromUtf8(Store.GetBlockReason()->message).Contains(TEXT("refuse the project's own payment state file")));
		TestFalse(TEXT("no save"), Store.Save(Json::dict(), Json::dict()));
	}
	TestEqual(TEXT("the block reported once"), Reports.Errors.Num(), 1);
	TestEqual(TEXT("the file is as it was (or still absent)"), FileExists(Path), bExisted);
	TestTrue(TEXT("its text untouched"), !bExisted || ReadAll(Path) == Before);
	FTempDir T;
	{
		FScfgStore Own(Options(T.Dir, Ticker, Reports));
		TestFalse(TEXT("a directory of the test's own is read as always"), Own.IsBlocked());
	}
	return true;
}

SCFG_STORE_TEST(FScfgStoreMakesDirectoryTest, "SaveCreatesTheDirectory")

// test_save_creates_the_directory.
bool FScfgStoreMakesDirectoryTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("deeper/scfg/state.json");
	TestTrue(TEXT("saved"), FScfgStoreTestAccess::SaveFile(Path, Json::dict(), Json::dict()).ok());
	TestTrue(TEXT("written"), FileExists(Path));
	return true;
}

SCFG_STORE_TEST(FScfgStoreMissingTest, "AMissingFileIsEmptyWithoutAWarning")

// test_missing_file_is_empty_without_a_warning; the store itself starts empty and writes nothing.
bool FScfgStoreMissingTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FScfgStoreLoad Got = FScfgStore::Load(T / TEXT("state.json"));
	TestTrue(TEXT("empty, quietly (a first launch)"), Same(Got.Pending, Json::dict()) &&
		Same(Got.Receipts, Json::dict()) && Got.Warnings.Num() == 0 && !Got.Error.IsSet());
	FTSTicker Ticker;
	FReports Reports;
	{
		FScfgStore Store(Options(T.Dir, Ticker, Reports));
		TestFalse(TEXT("not blocked"), Store.IsBlocked());
		TestEqual(TEXT("its path"), Store.GetPath(), T / TEXT("state.json"));
		TestTrue(TEXT("saving the same empty stores writes nothing"), Store.Save(Json::dict(), Json::dict()));
		TestEqual(TEXT("no write"), Store.NumWrites(), 0);
	}
	TestFalse(TEXT("no file"), FileExists(T / TEXT("state.json")));
	TestEqual(TEXT("nothing reported"), Reports.Errors.Num() + Reports.Warnings.Num(), 0);
	return true;
}

SCFG_STORE_TEST(FScfgStoreCorruptTest, "ACorruptFileIsEmptyWithAWarningAndKeptAside")

// test_corrupt_file_is_empty_with_a_warning_and_kept_aside, with two more shapes (a bool version, receipts a list).
bool FScfgStoreCorruptTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const TArray<TPair<FString, std::string>> Cases = {
		{TEXT("not json"), "{\"version\": 1, \"pending\": {"},
		{TEXT("not an object"), "[1, 2]"},
		{TEXT("wrong version"), "{\"version\": 2, \"pending\": {}, \"receipts\": {}}"},
		{TEXT("a bool version"), "{\"version\": true, \"pending\": {}, \"receipts\": {}}"},
		{TEXT("pending not an object"), "{\"version\": 1, \"pending\": [], \"receipts\": {}}"},
		{TEXT("receipts not an object"), "{\"version\": 1, \"pending\": {}, \"receipts\": [1]}"},
		{TEXT("receipts missing"), "{\"version\": 1, \"pending\": {}}"},
		{TEXT("not utf-8"), "{\xff}"},
	};
	int32 N = 0;
	for (const TPair<FString, std::string>& Case : Cases)
	{
		const FString Path = T / *FString::Printf(TEXT("state%d.json"), ++N);
		WriteAll(Path, Case.Value);
		const FScfgStoreLoad Got = FScfgStore::Load(Path);
		TestTrue(Case.Key + TEXT(": empty stores"), Same(Got.Pending, Json::dict()) && Same(Got.Receipts, Json::dict()));
		TestTrue(Case.Key + TEXT(": a warning naming the file"),
			Got.Warnings.Num() == 1 && Got.Warnings[0].Contains(Path) && !Got.Error.IsSet());
		TestEqual(Case.Key + TEXT(": the bad file is kept as .corrupt"),
			ScfgString::FromUtf8(ReadAll(Path + TEXT(".corrupt")).value_or("<missing>")),
			ScfgString::FromUtf8(Case.Value));
		TestFalse(Case.Key + TEXT(": and moved out of the way"), FileExists(Path));
	}
	// The store reports the warning and starts empty.
	FTSTicker Ticker;
	FReports Reports;
	WriteAll(T / TEXT("state.json"), "[]");
	FScfgStore Store(Options(T.Dir, Ticker, Reports));
	TestFalse(TEXT("a corrupt file does not block"), Store.IsBlocked());
	TestTrue(TEXT("the store warned once"), Reports.Warnings.Num() == 1 && Reports.Errors.Num() == 0 &&
		Reports.Warnings[0].Contains(TEXT("Restore")));
	return true;
}

SCFG_STORE_TEST(FScfgStoreAtomicTest, "TheAtomicWriteLeavesTheOldFileOnFailure")

// test_atomic_write_leaves_old_file_on_failure: a directory where the temporary file goes, so the write fails before
// the rename.
bool FScfgStoreAtomicTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	TestTrue(TEXT("first save"), FScfgStoreTestAccess::SaveFile(Path, Pending(), StoredReceipts()).ok());
	IFileManager::Get().MakeDirectory(*(Path + TEXT(".tmp")), true);
	const scfg::Status Failed = FScfgStoreTestAccess::SaveFile(Path, Json::dict(), Json::dict());
	TestTrue(TEXT("the failed write is a RuntimeError"), !Failed.ok() && Failed.err().kind == "RuntimeError" &&
		Failed.err().message.rfind("cannot write", 0) == 0);
	const FScfgStoreLoad Got = FScfgStore::Load(Path);
	TestTrue(TEXT("the old file is intact"), Same(Got.Receipts, StoredReceipts()) && Same(Got.Pending, Pending()));
	return true;
}

SCFG_STORE_TEST(FScfgStoreFailedRenameTest, "AFailedRenameLeavesTheTargetUntouched")

// test_a_failed_rename_leaves_the_target_untouched: a directory at the target, so the .tmp is written and the rename
// fails. (Loading that path is an Error, blocked: a directory cannot be read.)
bool FScfgStoreFailedRenameTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	IFileManager::Get().MakeDirectory(*Path, true);
	WriteAll(Path / TEXT("keep.txt"), "old");
	const scfg::Status Failed = FScfgStoreTestAccess::SaveFile(Path, Pending(), StoredReceipts());
	TestTrue(TEXT("the failed rename is a RuntimeError"), !Failed.ok() && Failed.err().kind == "RuntimeError");
	TestTrue(TEXT("at the rename"), !Failed.ok() && Failed.err().message.rfind("cannot replace", 0) == 0);
	TestEqual(TEXT("what was there is untouched"), ScfgString::FromUtf8(ReadAll(Path / TEXT("keep.txt")).value_or("")),
		TEXT("old"));
	const FScfgStoreLoad Got = FScfgStore::Load(Path);
	TestTrue(TEXT("a directory in its place cannot be read"), Got.Error.IsSet() && Got.Error->kind == "RuntimeError");
	return true;
}

SCFG_STORE_TEST(FScfgStoreTmpFallbackTest, "LoadFallsBackToACompleteTmp")

// test_load_falls_back_to_a_complete_tmp.
bool FScfgStoreTmpFallbackTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	TestTrue(TEXT("a complete file"), FScfgStoreTestAccess::SaveFile(T / TEXT("other.json"), Pending(), StoredReceipts()).ok());
	IFileManager::Get().Move(*(Path + TEXT(".tmp")), *(T / TEXT("other.json")));
	FScfgStoreLoad Got = FScfgStore::Load(Path);
	TestTrue(TEXT("a save stopped before its rename: the .tmp is read"), Same(Got.Pending, Pending()) &&
		Same(Got.Receipts, StoredReceipts()) && Got.Warnings.Num() == 0 && !Got.Error.IsSet());
	TestTrue(TEXT("and becomes the file at once (the next save's .tmp never truncates the only copy)"),
		FileExists(Path) && !FileExists(Path + TEXT(".tmp")) && Same(FScfgStore::Load(Path).Pending, Pending()));
	TestTrue(TEXT("a later save"), FScfgStoreTestAccess::SaveFile(Path, Json::dict(), Json::dict()).ok());
	WriteAll(Path + TEXT(".tmp"), "{\"version\": 1, \"pend");  // a torn write beside a good file
	Got = FScfgStore::Load(Path);
	TestTrue(TEXT("the file wins over a .tmp"), Same(Got.Pending, Json::dict()) && Same(Got.Receipts, Json::dict()) &&
		Got.Warnings.Num() == 0);
	return true;
}

SCFG_STORE_TEST(FScfgStoreUnreadableTest, "AnUnreadableFileBlocksPaymentsAndIsNeverOverwritten")

// test_unreadable_file_is_left_untouched, and the store on top: blocked, it refuses to start a worker and to save (also
// when it is destroyed), and reports the block once; the next launch reads the file again.
bool FScfgStoreUnreadableTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	ExpectFileWarnings(*this);
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	TestTrue(TEXT("a good file"), FScfgStoreTestAccess::SaveFile(Path, Pending(), StoredReceipts()).ok());
	const std::optional<std::string> Before = ReadAll(Path);
	if (!T.SetMode(Path, 0) || ReadAll(Path).has_value())
	{
		T.SetMode(Path, 0600);
		AddInfo(TEXT("skipped: this process reads files without permission (root), or has no chmod"));
		return true;
	}
	const FScfgStoreLoad Got = FScfgStore::Load(Path);
	TestTrue(TEXT("no stores and no corruption warning"), Same(Got.Pending, Json::dict()) &&
		Same(Got.Receipts, Json::dict()) && Got.Warnings.Num() == 0);
	TestTrue(TEXT("an error instead, naming the file"), Got.Error.IsSet() && Got.Error->kind == "RuntimeError" &&
		ScfgString::FromUtf8(Got.Error->message).Contains(Path));
	FTSTicker Ticker;
	FReports Reports;
	{
		FScfgStore Store(Options(T.Dir, Ticker, Reports));
		TestTrue(TEXT("blocked"), Store.IsBlocked() && Store.GetBlockReason() != nullptr &&
			ScfgString::FromUtf8(Store.GetBlockReason()->message).Contains(Path));
		AddInfo(FString::Printf(TEXT("block reason: %s"), *ScfgString::FromUtf8(Store.GetBlockReason()->str())));
		TArray<FString> WorkerErrors;
		Result<TUniquePtr<FScfgWorker>> Made =
			Store.CreateWorker(BaseConfig(), MakeUnique<FInstantTransport>(), WorkerOptions(Ticker, WorkerErrors));
		TestTrue(TEXT("no payments: no worker"), !Made.ok() && Made.err().kind == "RuntimeError");
		TestFalse(TEXT("no save"), Store.Save(Json::dict(), Json::dict()));
		TestFalse(TEXT("and none owed"), Store.IsSaveOwed());
		Ticker.Tick(30.0f);
		TestEqual(TEXT("the block was reported once"), Reports.Errors.Num(), 1);
	}
	TestEqual(TEXT("not even at destruction"), Reports.Errors.Num(), 1);
	T.SetMode(Path, 0600);
	TestFalse(TEXT("not moved aside as corrupt"), FileExists(Path + TEXT(".corrupt")));
	TestTrue(TEXT("the file is untouched"), ReadAll(Path) == Before);
	const FScfgStoreLoad Again = FScfgStore::Load(Path);
	TestTrue(TEXT("read again once it is readable (the next launch)"), Same(Again.Pending, Pending()) &&
		Same(Again.Receipts, StoredReceipts()) && !Again.Error.IsSet());
	return true;
}

SCFG_STORE_TEST(FScfgStoreCorruptTwiceTest, "LaterCorruptionKeepsEarlierCopies")

// test_later_corruption_keeps_earlier_copies: .corrupt, then .corrupt.2, then .corrupt.3, none overwritten.
bool FScfgStoreCorruptTwiceTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	WriteAll(Path, "first bad");
	FScfgStore::Load(Path);
	WriteAll(Path, "second bad");
	const FScfgStoreLoad Got = FScfgStore::Load(Path);
	WriteAll(Path, "third bad");
	FTSTicker Ticker;
	FReports Reports;
	{
		FScfgStore Store(Options(T.Dir, Ticker, Reports));  // the third, through the store
	}
	TestEqual(TEXT("the first copy is kept"), ScfgString::FromUtf8(ReadAll(Path + TEXT(".corrupt")).value_or("")),
		TEXT("first bad"));
	TestEqual(TEXT("the second beside it"), ScfgString::FromUtf8(ReadAll(Path + TEXT(".corrupt.2")).value_or("")),
		TEXT("second bad"));
	TestEqual(TEXT("and the third"), ScfgString::FromUtf8(ReadAll(Path + TEXT(".corrupt.3")).value_or("")),
		TEXT("third bad"));
	TestTrue(TEXT("the warning names where it went"),
		Got.Warnings.Num() == 1 && Got.Warnings[0].Contains(Path + TEXT(".corrupt.2")));
	TestTrue(TEXT("the store's too"), Reports.Warnings.Num() == 1 && Reports.Warnings[0].Contains(Path + TEXT(".corrupt.3")));
	TestFalse(TEXT("the bad file is out of the way"), FileExists(Path));
	return true;
}

SCFG_STORE_TEST(FScfgStoreStuckTest, "AFileInAReadOnlyDirectory")

// test_corrupt_file_that_cannot_be_moved_aside_is_unreadable: a corrupt file in a read-only directory cannot be moved
// aside, so it blocks (an empty start would let the next save replace it with nothing kept anywhere). Then a good
// file in a read-only directory: read, not blocked, but each save fails and is retried, and the file stays whole.
bool FScfgStoreStuckTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Dir = T / TEXT("stuck");
	const FString Path = Dir / TEXT("state.json");
	IFileManager::Get().MakeDirectory(*Dir, true);
	WriteAll(Path, "{\"version\": 1, \"pend");
	T.SetMode(Dir, 0500);
	if (IFileManager::Get().Move(*(Dir / TEXT("probe")), *Path, false, false, false, true))
	{
		IFileManager::Get().Move(*Path, *(Dir / TEXT("probe")), false, false, false, true);
		T.SetMode(Dir, 0700);
		AddInfo(TEXT("skipped: this process renames inside a read-only directory (root), or has no chmod"));
		return true;
	}
	FTSTicker Ticker;
	FReports Reports;
	{
		FScfgStore Store(Options(Dir, Ticker, Reports));
		TestTrue(TEXT("blocked: never saved over this run"), Store.IsBlocked());
		const FString Message = Store.IsBlocked() ? ScfgString::FromUtf8(Store.GetBlockReason()->message) : FString();
		TestTrue(TEXT("naming the file, what is wrong with it and why it stays"), Message.Contains(Path) &&
			Message.Contains(TEXT("is not valid JSON")) && Message.Contains(TEXT("could not be moved aside")));
		TestTrue(TEXT("no warning, one error"), Reports.Warnings.Num() == 0 && Reports.Errors.Num() == 1);
		TestFalse(TEXT("no save"), Store.Save(Pending(), StoredReceipts()));
	}
	T.SetMode(Dir, 0700);
	TestEqual(TEXT("the file is untouched"), ScfgString::FromUtf8(ReadAll(Path).value_or("")),
		TEXT("{\"version\": 1, \"pend"));
	TestFalse(TEXT("and no copy was made"), FileExists(Path + TEXT(".corrupt")));

	TestTrue(TEXT("a good file"), FScfgStoreTestAccess::SaveFile(Path, Pending(), StoredReceipts()).ok());
	const std::optional<std::string> Good = ReadAll(Path);
	WriteAll(Path + TEXT(".tmp"), "");  // a writable .tmp: the write succeeds and the rename over the file fails
	T.SetMode(Dir, 0500);
	Reports = FReports();
	{
		FScfgStore Store(Options(Dir, Ticker, Reports));
		TestFalse(TEXT("readable: not blocked"), Store.IsBlocked());
		TestFalse(TEXT("the save fails"), Store.Save(Json::dict(), Json::dict()));
		TestTrue(TEXT("at the rename over the existing file"),
			Reports.Errors.Num() == 1 && Reports.Errors[0].Contains(TEXT("cannot replace")));
		TestTrue(TEXT("and is owed"), Store.IsSaveOwed());
		Ticker.Tick(FScfgStore::RETRY_INTERVAL);
		TestTrue(TEXT("still owed after a retry"), Store.IsSaveOwed());
		TestEqual(TEXT("reported once"), Reports.Errors.Num(), 1);
	}
	T.SetMode(Dir, 0700);
	TestTrue(TEXT("the file stays whole, byte for byte"), ReadAll(Path) == Good);
	return true;
}

SCFG_STORE_TEST(FScfgStoreFailedReplaceTest, "AFailedReplaceKeepsTheOldFile")

// The save's last step, the rename over an existing state.json, fails (a test hook stands in for it): nothing was
// deleted before it, so the old file is still there byte for byte, and the store reports and owes the save.
bool FScfgStoreFailedReplaceTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	TestTrue(TEXT("a good file"), FScfgStoreTestAccess::SaveFile(Path, Pending(), StoredReceipts()).ok());
	const std::optional<std::string> Before = ReadAll(Path);
	struct FHook
	{
		FHook()
		{
			FScfgStoreTestAccess::ReplaceHook() = [](const FString& From, const FString& To) -> scfg::Status
			{
				return scfg::Err::make("RuntimeError", ScfgString::ToUtf8(TEXT("cannot replace ") + To +
					TEXT(": injected (the .tmp ") + From + TEXT(" is held)")));
			};
		}
		~FHook() { FScfgStoreTestAccess::ReplaceHook() = nullptr; }
	} Hook;
	const scfg::Status Failed = FScfgStoreTestAccess::SaveFile(Path, Json::dict(), Json::dict());
	TestTrue(TEXT("the replace failed"), !Failed.ok() && Failed.err().message.rfind("cannot replace", 0) == 0);
	TestTrue(TEXT("the old file is still there, byte for byte"), ReadAll(Path) == Before);
	FTSTicker Ticker;
	FReports Reports;
	{
		FScfgStore Store(Options(T.Dir, Ticker, Reports));
		TestFalse(TEXT("the store's save fails"), Store.Save(Json::dict(), Json::dict()));
		TestTrue(TEXT("reported once, and owed"), Reports.Errors.Num() == 1 && Store.IsSaveOwed());
		TestTrue(TEXT("a state equal to the old file owes nothing"), Store.Save(Pending(), StoredReceipts()) &&
			!Store.IsSaveOwed());
		TestTrue(TEXT("still byte for byte"), ReadAll(Path) == Before);
	}
	return true;
}

SCFG_STORE_TEST(FScfgStoreUnsearchableTest, "AnUnsearchableDirectoryBlocks")

// A directory this process cannot search (no x bit): stat fails with EACCES, which is not "missing". The store
// blocks instead of starting empty beside a file it cannot see.
bool FScfgStoreUnsearchableTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Dir = T / TEXT("hidden");
	const FString Path = Dir / TEXT("state.json");
	TestTrue(TEXT("a good file"), FScfgStoreTestAccess::SaveFile(Path, Pending(), StoredReceipts()).ok());
	const std::optional<std::string> Before = ReadAll(Path);
	if (!T.SetMode(Dir, 0600) || FileExists(Path))
	{
		T.SetMode(Dir, 0700);
		AddInfo(TEXT("skipped: this process searches directories without permission (root), or has no chmod"));
		return true;
	}
	FTSTicker Ticker;
	FReports Reports;
	{
		FScfgStore Store(Options(Dir, Ticker, Reports));
		TestTrue(TEXT("blocked, naming the file"), Store.IsBlocked() &&
			ScfgString::FromUtf8(Store.GetBlockReason()->message).Contains(Path));
		if (Store.IsBlocked())
		{
			AddInfo(ScfgString::FromUtf8(Store.GetBlockReason()->str()));
		}
		TestFalse(TEXT("no save"), Store.Save(Json::dict(), Json::dict()));
	}
	T.SetMode(Dir, 0700);
	TestTrue(TEXT("the file is untouched"), ReadAll(Path) == Before);
	return true;
}

SCFG_STORE_TEST(FScfgStoreOnErrorDestroysTest, "OnErrorMayDestroyTheStore")

// The facade may tear everything down from OnError: from a failed save, and from a retry the ticker runs. The store
// touches nothing after the report.
bool FScfgStoreOnErrorDestroysTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	FTSTicker Ticker;
	FReports Reports;
	double Clock = 1000;
	TUniquePtr<FScfgStore> Store;
	int32 Calls = 0;
	FScfgStore::FOptions Opts = Options(T.Dir, Ticker, Reports, &Clock);
	Opts.OnError = [&](const scfg::Err&)
	{
		++Calls;
		Store.Reset();
	};
	Store = MakeUnique<FScfgStore>(Opts);
	TestTrue(TEXT("a first save"), Store->Save(Pending(), StoredReceipts()));
	IFileManager::Get().MakeDirectory(*(Path + TEXT(".tmp")), true);
	TestFalse(TEXT("a failing save"), Store->Save(Json::dict(), StoredReceipts()));
	TestTrue(TEXT("destroyed from OnError, once"), !Store.IsValid() && Calls == 1);
	IFileManager::Get().DeleteDirectory(*(Path + TEXT(".tmp")), false, true);

	Store = MakeUnique<FScfgStore>(Opts);
	TestTrue(TEXT("a first save"), Store->Save(Json::dict(), StoredReceipts()));
	Clock += 1;
	Json Moved = Json::dict({{"ps_1", Session("ps_1", 901)}});
	TestTrue(TEXT("a session"), Store->Save(Pending(), StoredReceipts()));
	Clock += 1;
	TestTrue(TEXT("scan progress is owed"), Store->Save(Moved, StoredReceipts()) && Store->IsSaveOwed());
	IFileManager::Get().MakeDirectory(*(Path + TEXT(".tmp")), true);
	Ticker.Tick(FScfgStore::RETRY_INTERVAL);
	TestTrue(TEXT("destroyed from OnError during the retry"), !Store.IsValid() && Calls == 2);
	Ticker.Tick(FScfgStore::RETRY_INTERVAL);
	TestEqual(TEXT("nothing ran after"), Calls, 2);
	IFileManager::Get().DeleteDirectory(*(Path + TEXT(".tmp")), false, true);
	return true;
}

SCFG_STORE_TEST(FScfgStoreThrottleTest, "ScanProgressIsSavedAtMostEveryTenSeconds")

// Godot's _sync rule: the receipts, the set of sessions, a session's state or hint_tx changing is written at once;
// a change to anything else in pending (a cursor) at most every PENDING_SAVE_INTERVAL seconds, by the ticker or by
// the next save that may write; what is owed is written when the store is destroyed.
bool FScfgStoreThrottleTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	FTSTicker Ticker;
	FReports Reports;
	double Clock = 1000;
	auto At = [](int64_t Cursor, Json Hint = Json(), const char* State = "awaiting")
	{
		Json Record = Session("ps_1", Cursor);
		Record.as_dict().set("hint_tx", std::move(Hint));
		Record.as_dict().set("state", State);
		return Json::dict({{"ps_1", Record}});
	};
	auto Cursor = [&Path]()
	{
		const Json Stored = FScfgStore::Load(Path).Pending;
		return Stored.as_dict().contains("ps_1") ? Stored.as_dict().get("ps_1").as_dict().get("cursor").as_int64() : -1;
	};
	{
		FScfgStore Store(Options(T.Dir, Ticker, Reports, &Clock));
		TestTrue(TEXT("a new session: at once"), Store.Save(At(900), StoredReceipts()) && Store.NumWrites() == 1);
		Clock = 1001;
		TestTrue(TEXT("a cursor: owed"), Store.Save(At(901), StoredReceipts()) && Store.IsSaveOwed());
		Clock = 1002;
		Store.Save(At(902), StoredReceipts());
		TestEqual(TEXT("not written yet"), Store.NumWrites(), 1);
		Ticker.Tick(FScfgStore::RETRY_INTERVAL - 1.0f);
		TestEqual(TEXT("not before the interval"), Store.NumWrites(), 1);
		Ticker.Tick(1.0f);
		TestTrue(TEXT("the ticker wrote the newest"), Store.NumWrites() == 2 && Cursor() == 902 && !Store.IsSaveOwed());
		Clock = 1003;
		TestTrue(TEXT("a cursor right after: owed"), Store.Save(At(903), StoredReceipts()) && Store.NumWrites() == 2);
		TestTrue(TEXT("a hint_tx: at once (with the cursor)"), Store.Save(At(903, Json(kTx)), StoredReceipts()) &&
			Store.NumWrites() == 3 && Cursor() == 903 && !Store.IsSaveOwed());
		Clock = 1004;
		Store.Save(At(904, Json(kTx), "confirming"), StoredReceipts());
		TestEqual(TEXT("a state: at once"), Store.NumWrites(), 4);
		Clock = 1005;
		Store.Save(At(904, Json(kTx), "confirming"), Json::dict());
		TestEqual(TEXT("the receipts: at once"), Store.NumWrites(), 5);
		Clock = 1006;
		Json Two = At(904, Json(kTx), "confirming");
		Two.as_dict().set("ps_2", Session("ps_2"));
		Store.Save(Two, Json::dict());
		TestEqual(TEXT("another session: at once"), Store.NumWrites(), 6);
		Clock = 1007;
		Json Progress = At(905, Json(kTx), "confirming");
		Progress.as_dict().set("ps_2", Session("ps_2"));
		Store.Save(Progress, Json::dict());
		TestEqual(TEXT("a cursor: owed"), Store.NumWrites(), 6);
		Clock = 1016;
		Progress.as_dict().get("ps_1").as_dict().set("cursor", 906);
		Store.Save(Progress, Json::dict());
		TestTrue(TEXT("ten seconds after the last write: at once"), Store.NumWrites() == 7 && Cursor() == 906);
		Clock = 1017;
		Progress.as_dict().get("ps_1").as_dict().set("cursor", 907);
		Store.Save(Progress, Json::dict());
		TestTrue(TEXT("owed at the end"), Store.NumWrites() == 7 && Store.IsSaveOwed());
	}
	TestEqual(TEXT("destroying the store wrote what it owed"), Cursor(), 907);
	TestEqual(TEXT("nothing reported"), Reports.Errors.Num() + Reports.Warnings.Num(), 0);
	return true;
}

SCFG_STORE_TEST(FScfgStoreSchemaTest, "TheSchemaIsTheConformanceSchema")

// The store's copy of the frozen format is conformance/schema/state.json (found above the dev project).
bool FScfgStoreSchemaTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString Found;
	for (int32 Up = 0; Up < 8 && Found.IsEmpty(); ++Up)
	{
		const FString Candidate = Dir / TEXT("conformance/schema/state.json");
		if (FileExists(Candidate))
		{
			Found = Candidate;
		}
		Dir = FPaths::GetPath(Dir);
	}
	if (!TestFalse(TEXT("conformance/schema/state.json is above the project"), Found.IsEmpty()))
	{
		return false;
	}
	Result<Json> Frozen = scfg::json::loads(ReadAll(Found).value_or(""));
	TestTrue(TEXT("the same schema"), Frozen.ok() && Same(Frozen.value(), FScfgStore::Schema()));
	return true;
}

SCFG_STORE_TEST(FScfgStoreCheckPendingTest, "PendingRecordsAreCheckedAgainstTheSchema")

// state_schema.check's pending rules, one by one.
bool FScfgStoreCheckPendingTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	auto Problems = [](const std::string& Sid, const Json& Record)
	{
		FString Out;
		for (const std::string& Each : FScfgStore::CheckPending(Sid, Record))
		{
			Out += (Out.IsEmpty() ? TEXT("") : TEXT(" | ")) + ScfgString::FromUtf8(Each);
		}
		return Out;
	};
	auto With = [](const char* Key, Json Value)
	{
		Json Record = Session("ps_a");
		Record.as_dict().set(Key, std::move(Value));
		return Record;
	};
	TestEqual(TEXT("a conforming EVM session"), Problems("ps_a", Session("ps_a")), TEXT(""));
	Json Solana = Session("ps_a");
	Solana.as_dict().set("reference", "Ref1");
	Solana.as_dict().set("dest", "Dest1");
	Solana.as_dict().set("scan_before", Json());
	Solana.as_dict().set("scan_cursor", "sig");
	TestEqual(TEXT("a Solana session's optional fields"), Problems("ps_a", Solana), TEXT(""));
	TestEqual(TEXT("not an object"), Problems("ps_a", Json(5)), TEXT("pending[ps_a]: expected object"));
	Json Missing = Session("ps_a");
	Missing.as_dict().erase("ui_status");
	TestEqual(TEXT("a missing field"), Problems("ps_a", Missing), TEXT("pending[ps_a]: missing 'ui_status'"));
	TestEqual(TEXT("an unknown field"), Problems("ps_a", With("extra", 1)), TEXT("pending[ps_a]: unknown 'extra'"));
	TestEqual(TEXT("a bool is not an int"), Problems("ps_a", With("cursor", true)),
		TEXT("pending[ps_a].cursor: expected int, got True"));
	TestEqual(TEXT("an int of any size"), Problems("ps_a", With("cursor", Json(scfg::BigInt(10).pow(30)))), TEXT(""));
	TestEqual(TEXT("a number may be a float"), Problems("ps_a", With("created_at", 1.5)), TEXT(""));
	TestEqual(TEXT("but not a str"), Problems("ps_a", With("created_at", "now")),
		TEXT("pending[ps_a].created_at: expected number, got 'now'"));
	TestEqual(TEXT("|null"), Problems("ps_a", With("hint_tx", 5)), TEXT("pending[ps_a].hint_tx: expected str|null, got 5"));
	TestEqual(TEXT("an enum"), Problems("ps_a", With("ui_status", "bogus")),
		TEXT("pending[ps_a].ui_status: expected one of ['open', 'pending', 'cancelled', 'expired'], got 'bogus'"));
	TestEqual(TEXT("a match object"), Problems("ps_a", With("match", Json::dict({{"block", 1}, {"log_index", 2},
		{"payer", Json()}, {"recipient", kWallet}, {"tx_hash", kTx}, {"value", 4993706}, {"block_hash", "0xbb"}}))),
		TEXT(""));
	TestEqual(TEXT("a match is checked"), Problems("ps_a", With("match", Json::dict({{"block", 1}}))),
		TEXT("pending[ps_a].match: missing 'log_index' | pending[ps_a].match: missing 'payer' | "
			 "pending[ps_a].match: missing 'recipient' | pending[ps_a].match: missing 'tx_hash' | "
			 "pending[ps_a].match: missing 'value'"));
	TestEqual(TEXT("a match is an object or null"), Problems("ps_a", With("match", Json::list())),
		TEXT("pending[ps_a].match: expected object or null, got []"));
	TestEqual(TEXT("the id is its key"), Problems("ps_b", Session("ps_a")), TEXT("pending[ps_b]: id does not match its key"));
	return true;
}

SCFG_STORE_TEST(FScfgStoreBadRecordTest, "AMalformedPendingRecordIsDroppedAndTheFileKept")

// A malformed pending record would fail every tick (the Engine's expiry pass is not contained): it is dropped with a
// warning, after the whole file was copied aside; the cleaned stores are saved at once, so the next launch finds
// nothing to drop. Malformed receipts are left to the core: kept, then removed by the first reverify (and saved).
bool FScfgStoreBadRecordTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	Json Bogus = Session("ps_c");
	Bogus.as_dict().set("ui_status", "bogus");
	const Json Stored = Json::dict({{"version", 1},
		{"pending", Json::dict({{"ps_a", 5}, {"ps_b", Session("ps_b")}, {"ps_c", Bogus}, {"ps_d", Session("ps_x")}})},
		{"receipts", Json::dict({{"junk0", Json::dict()}, {"junk1", 5}})}});
	const std::string Original = scfg::json::dumps(Stored).take();
	WriteAll(Path, Original);
	FTSTicker Ticker;
	FReports Reports;
	TArray<FString> WorkerErrors;
	{
		FScfgStore Store(Options(T.Dir, Ticker, Reports));
		TestFalse(TEXT("not blocked"), Store.IsBlocked());
		TestEqual(TEXT("only the good session is loaded"), Text(Store.GetPending()), Text(Json::dict({{"ps_b", Session("ps_b")}})));
		TestEqual(TEXT("one warning per dropped record"), Reports.Warnings.Num(), 3);
		for (const TCHAR* Sid : {TEXT("'ps_a'"), TEXT("'ps_c'"), TEXT("'ps_d'")})
		{
			TestTrue(FString::Printf(TEXT("%s is named, with the file and its copy"), Sid),
				Reports.Warnings.ContainsByPredicate([&](const FString& W)
				{
					return W.Contains(Sid) && W.Contains(Path) && W.Contains(Path + TEXT(".corrupt")) &&
						W.Contains(TEXT("Restore"));
				}));
		}
		for (const FString& W : Reports.Warnings)
		{
			AddInfo(W);
		}
		TestEqual(TEXT("the original is kept, byte for byte"),
			ScfgString::FromUtf8(ReadAll(Path + TEXT(".corrupt")).value_or("")), ScfgString::FromUtf8(Original));
		TestEqual(TEXT("the cleaned stores were saved at once"), Store.NumWrites(), 1);
		TestEqual(TEXT("the receipts are kept as they are"), static_cast<int32>(Store.GetReceipts().as_dict().size()), 2);

		Result<TUniquePtr<FScfgWorker>> Made =
			Store.CreateWorker(BaseConfig(), MakeUnique<FInstantTransport>(), WorkerOptions(Ticker, WorkerErrors));
		if (!TestTrue(TEXT("the worker starts on the checked stores"), Made.ok()))
		{
			return false;
		}
		TUniquePtr<FScfgWorker> Worker = Made.take();
		Worker->Start();
		Frames(Ticker, 20);
		TestEqual(TEXT("the first reverify removed the junk receipts"),
			static_cast<int32>(Worker->GetSnapshot().receipts.as_dict().size()), 0);
		const FScfgStoreLoad OnDisk = FScfgStore::Load(Path);
		TestTrue(TEXT("and the store saved that"), Same(OnDisk.Receipts, Json::dict()) && OnDisk.Warnings.Num() == 0);
		TestTrue(TEXT("the session is still there"), OnDisk.Pending.as_dict().contains("ps_b"));
		TestEqual(TEXT("no store error"), Reports.Errors.Num(), 0);
		Worker.Reset();
	}
	TestFalse(TEXT("no second copy: nothing was left to drop"), FileExists(Path + TEXT(".corrupt.2")));
	for (const FString& E : WorkerErrors)
	{
		AddInfo(FString::Printf(TEXT("worker: %s"), *E));
	}
	return true;
}

SCFG_STORE_TEST(FScfgStoreWorkerRoundTripTest, "ARoundTripThroughTheWorker")

// The store feeds the worker its stores and saves after each OnStateChanged, only when pending or receipts changed;
// a new store (the next launch) reads what the last one saved. The store and the worker may go in either order.
bool FScfgStoreWorkerRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	FTSTicker Ticker;
	FReports Reports;
	TArray<FString> WorkerErrors;
	std::string Sid;
	double Clock = 1000;
	{
		TUniquePtr<FScfgStore> Store = MakeUnique<FScfgStore>(Options(T.Dir, Ticker, Reports, &Clock));
		Result<TUniquePtr<FScfgWorker>> Made =
			Store->CreateWorker(BaseConfig(), MakeUnique<FInstantTransport>(), WorkerOptions(Ticker, WorkerErrors));
		if (!TestTrue(TEXT("created"), Made.ok()))
		{
			return false;
		}
		TUniquePtr<FScfgWorker> Worker = Made.take();
		const TSharedRef<FScfgStateChanged> Event = Worker->OnStateChanged;
		TestEqual(TEXT("the store's handler"), Event->Num(), 1);
		Worker->Start();
		Frames(Ticker, 5);
		TestEqual(TEXT("ticks that change nothing write nothing"), Store->NumWrites(), 0);
		Worker->StartSession("unlock", "base", Json::dict({{"item", "chapter2"}}), [&Sid](Result<Json> R)
		{
			if (R.ok())
			{
				Sid = R.value().as_dict().get("id").as_str();
			}
		});
		Frames(Ticker, 5);
		if (!TestFalse(TEXT("a session"), Sid.empty()))
		{
			return false;
		}
		const FScfgStoreLoad Saved = FScfgStore::Load(Path);
		TestTrue(TEXT("saved at once with its session"), Saved.Pending.as_dict().contains(Sid) &&
			Saved.Warnings.Num() == 0);
		Clock += FScfgStore::PENDING_SAVE_INTERVAL;
		Ticker.Tick(FScfgStore::RETRY_INTERVAL);  // the scan progress of the ticks after it (the worker's next is later)
		TestTrue(TEXT("then as the snapshot has it"), Same(FScfgStore::Load(Path).Pending, Worker->GetSnapshot().pending));
		TestFalse(TEXT("nothing owed"), Store->IsSaveOwed());
		const int32 Written = Store->NumWrites();
		TestTrue(TEXT("written"), Written >= 1);
		Worker->Stop();
		Worker->Tick(nullptr);  // cancelled: no state change
		Frames(Ticker, 3);
		TestEqual(TEXT("nothing new to write"), Store->NumWrites(), Written);
		Store.Reset();  // the store first: its handler goes with it
		TestEqual(TEXT("its handler was removed"), Event->Num(), 0);
		Worker->Start();
		Worker->Tick(nullptr);
		Frames(Ticker, 5);
		Worker.Reset();
	}
	{
		TUniquePtr<FScfgStore> Store = MakeUnique<FScfgStore>(Options(T.Dir, Ticker, Reports));
		TestTrue(TEXT("the next launch reads the session"), Store->GetPending().as_dict().contains(Sid));
		Result<TUniquePtr<FScfgWorker>> Made =
			Store->CreateWorker(BaseConfig(), MakeUnique<FInstantTransport>(), WorkerOptions(Ticker, WorkerErrors));
		if (!TestTrue(TEXT("created again"), Made.ok()))
		{
			return false;
		}
		TUniquePtr<FScfgWorker> Worker = Made.take();
		TestTrue(TEXT("the Engine has it"), Worker->GetSnapshot().pending.as_dict().contains(Sid));
		Worker->Start();
		Worker->Discard(Json(Sid), nullptr);
		Frames(Ticker, 5);
		TestTrue(TEXT("its removal is saved"), Same(FScfgStore::Load(Path).Pending, Json::dict()));
		Worker.Reset();  // the worker first: the store's Detach() finds the list gone
		Store->Detach();
		Store.Reset();
	}
	TestEqual(TEXT("no store error or warning"), Reports.Errors.Num() + Reports.Warnings.Num(), 0);
	TestEqual(TEXT("no worker error"), FString::Join(WorkerErrors, TEXT(" | ")), TEXT(""));
	return true;
}

SCFG_STORE_TEST(FScfgStoreRetryTest, "AFailedSaveIsRetriedEveryTenSeconds")

// A save that fails keeps the old file, is reported once (not at each retry), and is retried every RETRY_INTERVAL
// seconds until it succeeds; a later failure is reported again; a state equal to the file's owes nothing.
bool FScfgStoreRetryTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	const FString Tmp = Path + TEXT(".tmp");
	FTSTicker Ticker;
	FReports Reports;
	FScfgStore Store(Options(T.Dir, Ticker, Reports));
	const Json Two = Json::dict({{"ps_1", Session("ps_1")}, {"ps_2", Session("ps_2")}});
	TestTrue(TEXT("a first save"), Store.Save(Pending(), StoredReceipts()));
	IFileManager::Get().MakeDirectory(*Tmp, true);  // the next writes fail
	TestFalse(TEXT("the save fails"), Store.Save(Two, StoredReceipts()));
	TestTrue(TEXT("one error, naming the step"), Reports.Errors.Num() == 1 && Reports.Errors[0].Contains(TEXT("cannot write")));
	TestTrue(TEXT("owed"), Store.IsSaveOwed());
	Ticker.Tick(FScfgStore::RETRY_INTERVAL - 1.0f);
	Ticker.Tick(1.0f);  // the first retry, at 10 s: fails again
	Ticker.Tick(FScfgStore::RETRY_INTERVAL);  // and the second
	TestTrue(TEXT("still owed"), Store.IsSaveOwed());
	TestEqual(TEXT("reported once, not at each retry"), Reports.Errors.Num(), 1);
	TestTrue(TEXT("the old file is kept"), Same(FScfgStore::Load(Path).Pending, Pending()));
	IFileManager::Get().DeleteDirectory(*Tmp, false, true);
	Ticker.Tick(FScfgStore::RETRY_INTERVAL - 1.0f);
	TestTrue(TEXT("not before the interval"), Store.IsSaveOwed());
	Ticker.Tick(1.0f);
	TestFalse(TEXT("the retry succeeded"), Store.IsSaveOwed());
	TestTrue(TEXT("the owed state is on disk"), Same(FScfgStore::Load(Path).Pending, Two));
	TestEqual(TEXT("two writes"), Store.NumWrites(), 2);
	Ticker.Tick(FScfgStore::RETRY_INTERVAL * 3);
	TestEqual(TEXT("no retry after a success"), Store.NumWrites(), 2);

	IFileManager::Get().MakeDirectory(*Tmp, true);
	TestFalse(TEXT("a new failure"), Store.Save(Pending(), Json::dict()));
	TestEqual(TEXT("is reported again"), Reports.Errors.Num(), 2);
	TestTrue(TEXT("back to what the file holds: nothing is owed"), Store.Save(Two, StoredReceipts()));
	TestFalse(TEXT("not owed"), Store.IsSaveOwed());
	Ticker.Tick(FScfgStore::RETRY_INTERVAL * 2);
	TestEqual(TEXT("and nothing more was written"), Store.NumWrites(), 2);
	IFileManager::Get().DeleteDirectory(*Tmp, false, true);
	return true;
}

SCFG_STORE_TEST(FScfgStoreDumpsErrTest, "ADumpsErrIsAFailedSave")

// json::dumps refuses an int of more than 4300 digits (Python's ValueError): that save fails, the old file is kept, it
// is reported once, and the next good state replaces it.
bool FScfgStoreDumpsErrTest::RunTest(const FString& Parameters)
{
	using namespace ScfgStoreTests;
	FTempDir T;
	const FString Path = T / TEXT("state.json");
	FTSTicker Ticker;
	FReports Reports;
	FScfgStore Store(Options(T.Dir, Ticker, Reports));
	TestTrue(TEXT("a first save"), Store.Save(Pending(), StoredReceipts()));
	const std::optional<std::string> Before = ReadAll(Path);
	const Json Huge = Json::dict({{"ps_h", Session("ps_h")}});
	Huge.as_dict().get("ps_h").as_dict().set("cursor", Json(scfg::BigInt(10).pow(4400)));
	const scfg::Status Direct = FScfgStoreTestAccess::SaveFile(Path, Huge, StoredReceipts());
	TestTrue(TEXT("SaveFile returns dumps' ValueError"), !Direct.ok() && Direct.err().kind == "ValueError");
	TestFalse(TEXT("the store's save fails"), Store.Save(Huge, StoredReceipts()));
	TestTrue(TEXT("reported once, as the ValueError"),
		Reports.Errors.Num() == 1 && Reports.Errors[0].StartsWith(TEXT("ValueError")));
	if (Reports.Errors.Num() > 0)
	{
		AddInfo(Reports.Errors[0].Left(200));
	}
	TestTrue(TEXT("owed"), Store.IsSaveOwed());
	Ticker.Tick(FScfgStore::RETRY_INTERVAL);
	TestEqual(TEXT("its retry fails quietly"), Reports.Errors.Num(), 1);
	TestTrue(TEXT("the old file is kept"), ReadAll(Path) == Before);
	TestFalse(TEXT("no .tmp was written"), FileExists(Path + TEXT(".tmp")));
	TestTrue(TEXT("the next good state is saved"), Store.Save(Json::dict(), StoredReceipts()));
	TestFalse(TEXT("nothing owed"), Store.IsSaveOwed());
	TestTrue(TEXT("on disk"), Same(FScfgStore::Load(Path).Pending, Json::dict()));
	return true;
}

#undef SCFG_STORE_TEST

#endif  // WITH_DEV_AUTOMATION_TESTS
