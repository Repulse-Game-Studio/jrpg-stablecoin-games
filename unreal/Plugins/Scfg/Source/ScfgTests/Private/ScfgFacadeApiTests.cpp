// Copyright 2026 Repulse
// Twin of godot/tests/test_scfg_api.gd: the facade's configuration, queries, persistence and results, test for test
// (Scfg.Glue.Facade.Api.<the Godot name>), through FScfgFacade over the fake nodes (ScfgFacadeTestKit.h).
//
// Mapping (Godot test -> this file):
//   test_configure_validates_like_renpy                     -> same name. The options are a typed struct, so
//       Godot's "an unknown option" (colour) and "custom_chains is add_chain's" cases cannot be written here: Godot
//       only (a dictionary of options).
//   test_add_chain_after_configure_is_an_error              -> same name
//   test_logo_res_path                                      -> same name: the logo is a path relative to the project
//       directory (Godot's res://). Godot only: the res:// prefix and _logo_file()'s bytes (the page's logo is the page server's)
//   test_owns_after_receipt                                 -> same name
//   test_state_survives_restart                             -> same name
//   test_new_session_is_saved_at_once                       -> same name
//   test_scan_progress_is_saved_at_most_every_10_s          -> same name
//   test_session_state_change_is_saved_at_once              -> same name
//   test_a_pass_in_flight_when_the_autoload_is_freed_does_not_call_it -> same name: the worker destroys the pass
//       (Godot lets it finish after the node is gone); its contained error is reported nowhere
//   test_quitting_mid_request_ends_the_pass_before_the_node_goes      -> same name, over the fake transport (Godot:
//       its HTTP transport and a local responder): the request in flight is dropped by the transport's Shutdown
//   test_corrupt_state_file_starts_empty_and_warns          -> same name
//   test_a_failed_save_is_retried                           -> same name
//   test_a_persistently_failing_save_is_reported_once       -> same name
//   test_unreadable_state_file_is_never_saved_over          -> same name
//   test_payment_result_from_core                           -> same name
//   test_a_result_without_callback_reaches_payment_resolved -> same name
//   test_plugin_registers_the_storage_id_setting            -> Godot only: the web storage id and its editor plugin
//   test_plugin_registers_the_scfg_autoload                 -> Godot only: the editor plugin's autoload (Unreal
//       creates the subsystem with the game instance)
// C++ only:
//   ExportReceiptsWritesPythonsJson                         -> the exact text (json.dumps(indent=2, sort_keys=True)),
//       a relative path under Saved/, the default Saved/scfg-receipts.json, and the reason of a failed write
//   AReceiptsChangeThenClearIsAnnouncedOnce                 -> OnReceiptsChanged once when ClearLocalData follows a
//       change the pump has not announced yet
//   TheDefaultOptionsWireTheRealParts                       -> a facade with only its store directory and ticker
//       replaced: the real transport, clock, platform and interval wiring (no pass runs: no network)
#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "Misc/Paths.h"

#include "ScfgConvert.h"
#include "ScfgFacade.h"
#include "ScfgFacadeTestKit.h"
#include "ScfgStore.h"
#include "ScfgString.h"
#include "ScfgWorker.h"

#include "Scfg/Dec.h"
#include "Scfg/Json.h"
#include "Scfg/Models.h"
#include "Scfg/Receipts.h"

#include <string>

#if PLATFORM_LINUX || PLATFORM_MAC
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define SCFG_FACADE_TESTS_CHMOD 1
#else
#define SCFG_FACADE_TESTS_CHMOD 0
#endif

#if WITH_DEV_AUTOMATION_TESTS

#define SCFG_FACADE_API_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.Facade.Api." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ScfgFacadeApiTests
{

using namespace ScfgFacadeTests;

FString Code(const TOptional<scfg::Err>& E)
{
	return E.IsSet() ? ScfgString::FromUtf8(E->code) : FString(TEXT("<none>"));
}

FString Kind(const TOptional<scfg::Err>& E)
{
	return E.IsSet() ? ScfgString::FromUtf8(E->kind) : FString(TEXT("<none>"));
}

FScfgOptions Wallet(const FString& W)
{
	FScfgOptions O;
	O.Wallet = W;
	return O;
}

FScfgItem Chapter2(int32 Code = 37)
{
	FScfgItem I;
	I.Code = Code;
	I.Price = TEXT("4.99");
	I.Token = TEXT("USDC");
	I.Title = TEXT("Chapter 2");
	return I;
}

int32 Count(const TArray<FString>& Lines, const TCHAR* Prefix)
{
	int32 N = 0;
	for (const FString& Each : Lines)
	{
		N += Each.StartsWith(Prefix) ? 1 : 0;
	}
	return N;
}

bool Has(const Json& Dict, const std::string& Key)
{
	return Dict.is_dict() && Dict.as_dict().contains(Key);
}

Json Field(const Json& Dict, const std::string& Key, const std::string& Name)
{
	return Has(Dict, Key) ? Dict.as_dict().get(Key).as_dict().get(Name) : Json();
}

std::string Sid(const Json& Session)
{
	return Session.is_dict() && Session.as_dict().get("id").is_str() ? Session.as_dict().get("id").as_str() : "";
}

FString Keys(const Json& Dict)
{
	TArray<FString> Out;
	if (Dict.is_dict())
	{
		for (const std::string& K : Dict.as_dict().keys())
		{
			Out.Add(ScfgString::FromUtf8(K));
		}
	}
	return FString::Join(Out, TEXT(","));
}

void WriteText(const FString& Path, const std::string& Text)
{
	TArray<uint8> Data(reinterpret_cast<const uint8*>(Text.data()), static_cast<int32>(Text.size()));
	FFileHelper::SaveArrayToFile(Data, *Path);
}

bool SetMode(const FString& Path, int Mode)
{
#if SCFG_FACADE_TESTS_CHMOD
	return chmod(TCHAR_TO_UTF8(*Path), static_cast<mode_t>(Mode)) == 0;
#else
	(void)Path, (void)Mode;
	return false;
#endif
}

// Whether this process can open Path (root can, whatever its mode). POSIX, so the engine logs nothing.
bool CanRead(const FString& Path)
{
#if SCFG_FACADE_TESTS_CHMOD
	const int Fd = ::open(TCHAR_TO_UTF8(*Path), O_RDONLY);
	if (Fd >= 0)
	{
		::close(Fd);
		return true;
	}
	return false;
#else
	(void)Path;
	return true;
#endif
}

}  // namespace ScfgFacadeApiTests

// --- configuration --------------------------------------------------------------------------------------------------

SCFG_FACADE_API_TEST(FScfgFacadeConfigureTest, "test_configure_validates_like_renpy")

bool FScfgFacadeConfigureTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	FScfgFacade& S = W.Make();
	TOptional<scfg::Err> Got = S.Configure(Wallet(TEXT("0x123")));
	TestEqual(TEXT("a bad wallet"), Kind(Got), TEXT("ConfigError"));
	TestEqual(TEXT("Ren'Py's code"), Code(Got), TEXT("address.invalid"));
	TestEqual(TEXT("reported once"), W.Reports().Errors.Num(), 1);
	TestTrue(TEXT("the report is `scfg: ` + the message"),
		W.Reports().Errors.Num() == 1 && W.Reports().Errors[0].StartsWith(TEXT("scfg: wallet: ")));
	TestNull(TEXT("not configured"), S.GetConfig());
	Got = S.Configure(FScfgOptions());
	TestEqual(TEXT("no wallet"), Code(Got), TEXT("wallet.missing"));
	TestEqual(TEXT("add_item before configure"), Code(S.AddItem(TEXT("chapter2"), Chapter2())),
		TEXT("configure.not_called"));
	FScfgOptions Good = Wallet(UTF8_TO_TCHAR(kDev));
	Good.bTestnet = true;
	TestFalse(TEXT("a good config"), S.Configure(Good).IsSet());
	TestTrue(TEXT("kept"), S.GetConfig() && S.GetConfig()->wallet == std::string(kDev));
	TestEqual(TEXT("add_item validates"), Code(S.AddItem(TEXT("chapter2"), Chapter2(0))), TEXT("item.invalid_code"));
	TestTrue(TEXT("supported() is always true"), S.Supported());
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeAddChainTest, "test_add_chain_after_configure_is_an_error")

bool FScfgFacadeAddChainTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	FScfgFacade& S = W.Make();
	FScfgChain Chain;
	Chain.Name = TEXT("My L2");
	Chain.ChainId = 777001;
	Chain.Rpc = {TEXT("https://rpc.my-l2.example")};
	Chain.Tokens.Add(TEXT("USDC"), TEXT("0x52908400098527886E0F7030069857D2E4169EE7"));
	Chain.Confirmations = 3;
	Chain.ExplorerTx = TEXT("https://scan.my-l2.example/tx/{}");
	TestFalse(TEXT("add_chain before configure"), S.AddChain(TEXT("myl2"), Chain).IsSet());
	TestEqual(TEXT("add_chain validates"), Code(S.AddChain(TEXT("bad key!"), Chain)), TEXT("chain.invalid_key"));
	FScfgOptions Options = Wallet(UTF8_TO_TCHAR(kDev));
	Options.Chains = {TEXT("base"), TEXT("myl2")};
	TestFalse(TEXT("the custom chain is usable"), S.Configure(Options).IsSet());
	const TOptional<scfg::Err> Late = S.AddChain(TEXT("other"), Chain);
	TestEqual(TEXT("add_chain after configure"), Kind(Late), TEXT("ConfigError"));
	TestEqual(TEXT("Ren'Py's code"), Code(Late), TEXT("configure.after_add_chain"));
	TestTrue(TEXT("reported"), W.Reports().Errors.Num() >= 2 && W.Reports().Errors.Last().StartsWith(TEXT("scfg: ")));
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeLogoTest, "test_logo_res_path")

bool FScfgFacadeLogoTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	WriteText(W.Dir / TEXT("logo.png"), std::string("\x89PNG\r\n\x1a\n", 8));
	FScfgFacade& S = W.Make();
	FScfgOptions Options = Wallet(UTF8_TO_TCHAR(kDev));
	Options.Logo = TEXT("logo.png");
	TestFalse(TEXT("a logo in the project"), S.Configure(Options).IsSet());
	TestTrue(TEXT("the core gets the path as given"), S.GetConfig() && S.GetConfig()->logo == std::string("logo.png"));
	TestEqual(TEXT("resolved under the project directory"), S.GetLogoPath(), W.Dir / TEXT("logo.png"));
	Options.Logo = TEXT("nope.png");
	TestEqual(TEXT("a missing file"), Code(S.Configure(Options)), TEXT("logo.file_missing"));
	Options.Logo = TEXT("user://logo.png");
	TestEqual(TEXT("a scheme is not in the game"), Code(S.Configure(Options)), TEXT("logo.bad_path"));
	Options.Logo = TEXT("logo.svg");
	TestEqual(TEXT("raster images only"), Code(S.Configure(Options)), TEXT("logo.bad_type"));
	Options.Logo = TEXT("../logo.png");
	TestEqual(TEXT("no escaping the project"), Code(S.Configure(Options)), TEXT("logo.bad_path"));
	Options.Logo.Reset();
	TestFalse(TEXT("no logo"), S.Configure(Options).IsSet());
	TestTrue(TEXT("none"), S.GetLogoPath().IsEmpty() && S.GetConfig() && !S.GetConfig()->logo);
	return true;
}

// --- queries --------------------------------------------------------------------------------------------------------

SCFG_FACADE_API_TEST(FScfgFacadeOwnsTest, "test_owns_after_receipt")

bool FScfgFacadeOwnsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	FScfgFacade& S = W.Configured(*this);
	TestFalse(TEXT("not owned before the worker starts"), S.Owns(TEXT("chapter2")));
	W.Start();
	TestNotNull(TEXT("the first pump starts the worker"), S.GetWorker());
	TestFalse(TEXT("not owned yet"), S.Owns(TEXT("chapter2")));
	TSharedRef<int32> Changed = MakeShared<int32>(0);
	S.OnReceiptsChanged = [Changed]() { ++*Changed; };
	const scfg::Result<Json> Added = W.AddReceipt(Hash("ab"));
	TestTrue(TEXT("a receipt is stored"), Added.ok() && Added.value().is_bool() && Added.value().as_bool());
	TestTrue(TEXT("owned"), S.Owns(TEXT("chapter2")));
	TestEqual(TEXT("receipts_changed once"), *Changed, 1);
	TestEqual(TEXT("receipts()"), static_cast<int32>(S.Receipts().as_dict().size()), 1);
	TestFalse(TEXT("no tip"), S.HasTipped());
	TestEqual(TEXT("tip_total() is Python's str(Decimal)"), S.TipTotal(), TEXT("0"));
	TestFalse(TEXT("an unknown item"), S.Owns(TEXT("nothing")));
	SameLines(*this, TEXT("is reported"), W.Reports().Errors,
		TArray<FString>{TEXT("scfg: unknown item 'nothing'; declare it with AddItem()")});
	(void)W.Run(
		[](scfg::engine::Engine& E) -> scfg::Result<Json>
		{
			Json Tip = ReceiptFor(Hash("cd"));
			Tip.as_dict().set("kind", "tip");
			Tip.as_dict().set("item", Json());
			Tip.as_dict().set("amount", "2.50");
			Json Receipts = E.receipts();
			SCFG_TRY(bool Done, scfg::receipts::add_receipt(Receipts.as_dict(), Tip));
			return Json(Done);
		});
	TestTrue(TEXT("a tip"), S.HasTipped() && S.TipTotal() == TEXT("2.50"));
	const FString Export = W.Dir / TEXT("owns/export.json");
	const scfg::Result<FString> Exported = S.ExportReceipts(Export);
	TestTrue(TEXT("export_receipts returns the file's path"), Exported.ok() && Exported.value() == Export);
	FString Listed;
	FFileHelper::LoadFileToString(Listed, *Export);
	const scfg::Result<Json> List = scfg::json::loads(ScfgString::ToUtf8(Listed));
	TestTrue(TEXT("the receipts as a JSON list"),
		List.ok() && List.value().is_list() && List.value().as_list().size() == 2);
	const int32 Reported = W.Reports().Errors.Num();
	const scfg::Result<FString> Failed = S.ExportReceipts(W.Dir / TEXT("owns"));  // a directory
	TestTrue(TEXT("a failed export is an error"), !Failed.ok() && Failed.err().kind == "RuntimeError");
	TestEqual(TEXT("and is reported"), W.Reports().Errors.Num(), Reported + 1);
	S.ClearLocalData();
	TestTrue(TEXT("clear_local_data forgets them at once"), !S.Owns(TEXT("chapter2")) &&
		S.Receipts().as_dict().empty() && S.Pending().as_dict().empty());
	TestTrue(TEXT("and on disk"), Same(W.OnDiskReceipts(), Json::dict()));
	W.RunOnce();
	W.Pump();
	TestTrue(TEXT("the engine is wiped too"), !S.Owns(TEXT("chapter2")) && S.Receipts().as_dict().empty() &&
		W.Worker().GetSnapshot().receipts.as_dict().empty());
	TestTrue(TEXT("still empty on disk"), Same(W.OnDiskReceipts(), Json::dict()));
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeRestartTest, "test_state_survives_restart")

bool FScfgFacadeRestartTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	W.Configured(*this);
	W.Start();
	(void)W.AddReceipt(Hash("ab"));
	const std::string Session = Sid(W.StartSession());
	const Json Receipts = W.F().Receipts();
	const Json Pending = W.F().Pending();
	TestEqual(TEXT("one receipt"), static_cast<int32>(Receipts.as_dict().size()), 1);
	TestTrue(TEXT("one session"), Has(Pending, Session));
	W.Drop();
	FScfgFacade& B = W.Configured(*this);
	// (by JSON value: the reloaded state is normalized)
	TestTrue(TEXT("the same receipts after a restart"), Same(B.Receipts(), Receipts));
	TestTrue(TEXT("the same pending sessions"), Same(B.Pending(), Pending));
	TestTrue(TEXT("owned before the worker starts"), B.Owns(TEXT("chapter2")));
	W.Pump();
	TestTrue(*FString::Printf(TEXT("and from the worker's snapshot (%s)"),
				 *FString::Join(W.Reports().Errors, TEXT(" | "))),
		B.GetWorker() != nullptr && B.Owns(TEXT("chapter2")));
	TestTrue(TEXT("the Engine got them"), B.GetWorker() && Same(B.GetWorker()->GetSnapshot().receipts, Receipts));
	return true;
}

// --- persistence ----------------------------------------------------------------------------------------------------

SCFG_FACADE_API_TEST(FScfgFacadeNewSessionSavedTest, "test_new_session_is_saved_at_once")

bool FScfgFacadeNewSessionSavedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	W.Configured(*this);
	W.Start();
	const std::string First = Sid(W.StartSession());
	TestTrue(TEXT("the first session is on disk"), Has(W.OnDiskPending(), First));
	W.Advance(1.0);  // well inside the save interval
	const std::string Second = Sid(W.StartSession());
	TestEqual(TEXT("a new session is written at once"), Keys(W.OnDiskPending()),
		ScfgString::FromUtf8(First + "," + Second));
	W.Advance(1.0);
	W.Worker().ReportHint(Second, Hash("ef"), nullptr);
	W.Pass();
	TestTrue(TEXT("a tx hint is written at once"), Same(Field(W.OnDiskPending(), Second, "hint_tx"), Json(Hash("ef"))));
	W.Advance(1.0);
	W.Worker().Discard(First, nullptr);
	W.Pass();
	TestEqual(TEXT("a removed session is written at once"), Keys(W.OnDiskPending()), ScfgString::FromUtf8(Second));
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeScanThrottleTest, "test_scan_progress_is_saved_at_most_every_10_s")

bool FScfgFacadeScanThrottleTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	FScfgFacade& S = W.Configured(*this);
	W.Start();
	const std::string Session = Sid(W.StartSession());
	FFakeChain& Chain = W.Chains->Chain("arbitrum");
	const Json Saved = Field(W.OnDiskPending(), Session, "cursor");
	W.Advance(4.0);
	Chain.Head += 50;
	W.Pass();
	const Json Cursor = Field(S.Pending(), Session, "cursor");
	TestFalse(*FString::Printf(TEXT("the scan moved the cursor (%s -> %s)"), *Text(Saved), *Text(Cursor)),
		Same(Cursor, Saved));
	TestTrue(TEXT("scan progress waits for the interval"), Same(Field(W.OnDiskPending(), Session, "cursor"), Saved));
	W.Advance(5.9);
	Chain.Head += 50;
	W.Pass();
	TestTrue(TEXT("still waiting at 9.9 s"), Same(Field(W.OnDiskPending(), Session, "cursor"), Saved));
	W.Advance(0.1);
	Chain.Head += 50;
	W.Pass();
	TestTrue(*FString::Printf(TEXT("written once 10 s passed (on disk %s, pending %s, owed %d, writes %d, now %.17g)"),
				 *Text(Field(W.OnDiskPending(), Session, "cursor")), *Text(Field(S.Pending(), Session, "cursor")),
				 S.GetStore()->IsSaveOwed() ? 1 : 0, S.GetStore()->NumWrites(), *W.Now),
		Same(Field(W.OnDiskPending(), Session, "cursor"), Field(S.Pending(), Session, "cursor")));
	// progress not yet written when the game instance goes is flushed
	W.Advance(1.0);
	Chain.Head += 50;
	W.Pass();
	const Json Last = Field(S.Pending(), Session, "cursor");
	TestFalse(TEXT("unsaved progress"), Same(Field(W.OnDiskPending(), Session, "cursor"), Last));
	W.Drop();
	TestTrue(TEXT("flushed on exit"), Same(Field(W.OnDiskPending(), Session, "cursor"), Last));
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeStateChangeSavedTest, "test_session_state_change_is_saved_at_once")

bool FScfgFacadeStateChangeSavedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	W.Configured(*this);
	W.Start();
	const std::string Session = Sid(W.StartSession());
	W.Advance(1.0);
	(void)W.Run([Session](scfg::engine::Engine& E) -> scfg::Result<Json>
		{
			SCFG_TRY_VOID(E.set_ui_status(Session, "pending"));
			return Json();
		});
	TestTrue(TEXT("a ui_status change alone waits for the interval"),
		Same(Field(W.OnDiskPending(), Session, "ui_status"), Json("open")));
	W.Advance(1.0);
	(void)W.Run([Session](scfg::engine::Engine& E) -> scfg::Result<Json>
		{
			Json Pending = E.pending();
			Pending.as_dict().find(Session)->as_dict().set("state", "confirming");
			return Json();
		});
	TestTrue(TEXT("a state change is written at once"),
		Same(Field(W.OnDiskPending(), Session, "state"), Json("confirming")));
	TestTrue(TEXT("with everything else mirrored so far"),
		Same(Field(W.OnDiskPending(), Session, "ui_status"), Json("pending")));
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadePassInFlightTest, "test_a_pass_in_flight_when_the_autoload_is_freed_does_not_call_it")

bool FScfgFacadePassInFlightTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	// A shutdown with a pass in flight whose step fails a few frames later (an error the Engine contains and reports
	// to its on_error): the worker destroys the pass with the facade, so nothing reports it afterwards.
	FWorld W;
	W.Configured(*this);
	W.Start();
	W.StartSession();
	W.Chains->BoomAfter = 5;
	TSharedRef<bool> Ended = MakeShared<bool>(false);
	W.Worker().Tick([Ended](scfg::Result<FScfgWorker::FOutcomes>) { *Ended = true; });
	W.Frame();
	W.Frame();
	TestFalse(TEXT("the pass is in flight"), *Ended);
	TestTrue(TEXT("its request is held"), W.Worker().IsBusy());
	FScfgReporter* Reports = &W.Reports();
	const TArray<FString> Before = Reports->Errors;
	const TSharedRef<FFakeChains> Chains = W.Chains;
	TSharedPtr<FScfgFacade> Kept = W.Facade;  // to read its reports after the shutdown
	W.Drop();
	for (int32 I = 0; I < 10; ++I)
	{
		W.Ticker.Tick(0.0f);
	}
	TestFalse(TEXT("the pass never called back"), *Ended);
	TestEqual(TEXT("its contained error was reported nowhere"), Reports->Errors, Before);
	TestEqual(TEXT("the transport was shut down once"), Chains->Shutdowns, 1);
	TestEqual(TEXT("dropping the request it held"), Chains->DroppedAtShutdown, 1);
	TestNull(TEXT("the worker is gone"), Kept->GetWorker());
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeQuitMidRequestTest, "test_quitting_mid_request_ends_the_pass_before_the_node_goes")

bool FScfgFacadeQuitMidRequestTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	// A real quit has no later frame: the pass waiting on a request ends inside the teardown (Jobs, Engine, then the
	// transport's Shutdown, which drops the request), nothing is reported as a worker error, and nothing more is sent.
	FWorld W;
	const std::string Tx = Hash("ab");
	const Json Doc = Json::dict({{"version", 1}, {"pending", Json::dict()},
		{"receipts", Json::dict({{"arbitrum:" + Tx + ":3", ReceiptFor(Tx)}})}});
	ScfgFacadeApiTests::WriteText(W.StatePath(), scfg::json::dumps(Doc).value());
	W.Chains->bHold = true;  // a stored receipt: the first pass re-verifies it on chain
	W.Configured(*this);
	W.Pump();
	TSharedRef<bool> Ended = MakeShared<bool>(false);
	W.Worker().Tick([Ended](scfg::Result<FScfgWorker::FOutcomes>) { *Ended = true; });
	for (int32 I = 0; I < 5 && W.Chains->Posts == 0; ++I)
	{
		W.Frame();
	}
	TestEqual(TEXT("a request is in flight"), W.Chains->Posts, 1);
	TestFalse(TEXT("the pass waits on it"), *Ended);
	TSharedPtr<FScfgFacade> Kept = W.Facade;
	W.Drop();
	TestFalse(TEXT("the pass ended without calling back"), *Ended);
	int32 WorkerErrors = 0;
	for (const FString& Line : Kept->GetReporter().Errors)
	{
		WorkerErrors += Line.StartsWith(TEXT("scfg: worker error:")) ? 1 : 0;
	}
	TestEqual(TEXT("the cancelled request at quit is not reported as a worker error"), WorkerErrors, 0);
	TestEqual(TEXT("the transport dropped it"), W.Chains->DroppedAtShutdown, 1);
	W.Ticker.Tick(0.0f);
	TestEqual(TEXT("nothing more was sent (no failover to the public endpoints)"), W.Chains->Posts, 1);
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeCorruptStateTest, "test_corrupt_state_file_starts_empty_and_warns")

bool FScfgFacadeCorruptStateTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	ScfgFacadeApiTests::WriteText(W.StatePath(), "{\"version\": 1, \"pending\": {\"ps_1\": ");
	FScfgFacade& S = W.Configured(*this);
	TestTrue(TEXT("empty stores"),
		S.Receipts().as_dict().empty() && S.Pending().as_dict().empty() && !S.Owns(TEXT("chapter2")));
	TestEqual(TEXT("one warning"), W.Reports().Warnings.Num(), 1);
	TestTrue(TEXT("it names the file"),
		W.Reports().Warnings.Num() == 1 && W.Reports().Warnings[0].Contains(W.StatePath()));
	TestTrue(TEXT("the bad file is kept aside"), IFileManager::Get().FileExists(*(W.StatePath() + TEXT(".corrupt"))));
	W.Start();
	(void)W.AddReceipt(Hash("ab"));
	TestEqual(TEXT("a fresh state file"), static_cast<int32>(W.OnDiskReceipts().as_dict().size()), 1);
	return true;
}

// A save that failed (a full disk, a locked file) is not left until the next change or the quit: it is tried again
// once the interval passed (the store's retry, every 10 s).
SCFG_FACADE_API_TEST(FScfgFacadeSaveRetriedTest, "test_a_failed_save_is_retried")

bool FScfgFacadeSaveRetriedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	FScfgFacade& S = W.Configured(*this);
	W.Start();
	const FString Tmp = W.StatePath() + TEXT(".tmp");
	IFileManager::Get().MakeDirectory(*Tmp, true);  // a directory where the save writes: it fails
	const std::string Session = Sid(W.StartSession());
	TestFalse(TEXT("the save failed"), Has(W.OnDiskPending(), Session));
	TestTrue(TEXT("and was reported"), Count(W.Reports().Errors, TEXT("scfg: cannot save the payment state:")) > 0);
	IFileManager::Get().DeleteDirectory(*Tmp, false, true);
	W.Advance(9.0);
	W.Pump();
	TestFalse(TEXT("not retried inside the interval"), Has(W.OnDiskPending(), Session));
	W.Advance(1.0);
	W.Pump();
	TestTrue(TEXT("retried once the interval passed, with no new snapshot"), Has(W.OnDiskPending(), Session));
	TestFalse(TEXT("and clean"), S.GetStore()->IsSaveOwed());
	return true;
}

// A save that keeps failing (a full disk) is reported once, not at every retry; once a save succeeds, a later
// failure is reported again.
SCFG_FACADE_API_TEST(FScfgFacadeSaveFailingOnceTest, "test_a_persistently_failing_save_is_reported_once")

bool FScfgFacadeSaveFailingOnceTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	FScfgFacade& S = W.Configured(*this);
	W.Start();
	const FString Tmp = W.StatePath() + TEXT(".tmp");
	auto Failures = [&W]() { return Count(W.Reports().Errors, TEXT("scfg: cannot save the payment state:")); };
	IFileManager::Get().MakeDirectory(*Tmp, true);  // every save fails
	const std::string First = Sid(W.StartSession());
	TestEqual(TEXT("the first failure is reported"), Failures(), 1);
	for (int32 I = 0; I < 3; ++I)
	{
		W.Advance(10.0);
		W.Pump();
	}
	W.StartSession();  // a change saved at once fails again
	TestTrue(TEXT("still unsaved"), S.GetStore()->IsSaveOwed());
	TestEqual(TEXT("the retries and the next change are not reported again"), Failures(), 1);
	IFileManager::Get().DeleteDirectory(*Tmp, false, true);
	W.Advance(10.0);
	W.Pump();
	TestTrue(TEXT("a save succeeded"), Has(W.OnDiskPending(), First));
	IFileManager::Get().MakeDirectory(*Tmp, true);
	W.StartSession();
	TestEqual(TEXT("a failure after that success is reported again"), Failures(), 2);
	IFileManager::Get().DeleteDirectory(*Tmp, false, true);
	return true;
}

// A state file the game cannot read right now (permissions here) is not corrupt: it stays where it is, payments do
// not start this run (reported as the stored receipts being unreadable), and nothing is saved over it.
SCFG_FACADE_API_TEST(FScfgFacadeUnreadableTest, "test_unreadable_state_file_is_never_saved_over")

bool FScfgFacadeUnreadableTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	const std::string Tx = Hash("ab");
	const Json Doc = Json::dict({{"version", 1}, {"pending", Json::dict()},
		{"receipts", Json::dict({{"arbitrum:0xab:3", ReceiptFor(Tx)}})}});
	const std::string Before = scfg::json::dumps(Doc).value();
	ScfgFacadeApiTests::WriteText(W.StatePath(), Before);
	if (!SetMode(W.StatePath(), 0) || CanRead(W.StatePath()))
	{
		SetMode(W.StatePath(), 0600);
		AddInfo(TEXT("skipped: this process reads files without permission (root), or has no chmod"));
		return true;
	}
	FScfgFacade& S = W.Configured(*this);
	W.Pump();
	TestNull(TEXT("payments do not start"), S.GetWorker());
	TestTrue(TEXT("reported"), Count(W.Reports().Errors, TEXT("scfg: cannot read the stored receipts:")) == 1);
	const TSharedRef<TOptional<FScfgPaymentResult>> Box = W.Pay(EScfgPayKind::Unlock, Unlock());
	TestTrue(TEXT("pay() resolves failed at once"), Box->IsSet() &&
		(*Box)->Status == EScfgPaymentStatus::Failed && (*Box)->Error == TEXT("RuntimeError"));
	S.ClearLocalData();
	W.Drop();
	SetMode(W.StatePath(), 0600);
	std::string After;
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *W.StatePath());
		After.assign(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
	}
	TestTrue(TEXT("the file is untouched (not saved over, even at exit)"), After == Before);
	TestFalse(TEXT("and not moved aside"), IFileManager::Get().FileExists(*(W.StatePath() + TEXT(".corrupt"))));
	TestEqual(TEXT("the next launch reads the receipt"), static_cast<int32>(W.OnDiskReceipts().as_dict().size()), 1);
	return true;
}

// --- results --------------------------------------------------------------------------------------------------------

SCFG_FACADE_API_TEST(FScfgFacadeResultFromCoreTest, "test_payment_result_from_core")

bool FScfgFacadeResultFromCoreTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	scfg::models::PaymentResult Core;
	Core.status = "paid";
	Core.kind = "unlock";
	Core.item = "chapter2";
	Core.amount = scfg::Dec::parse("4.990037").value();
	Core.token = "USDC";
	Core.chain = "arbitrum";
	Core.tx_hash = "0xab";
	Core.payer = "0xcd";
	Core.session_id = "ps_1";
	const FScfgPaymentResult R = ScfgConvert::PaymentResult(Core);
	TestTrue(TEXT("every field"), R.Status == EScfgPaymentStatus::Paid && R.Kind == EScfgPayKind::Unlock &&
		R.Item == TEXT("chapter2") && R.Amount == TEXT("4.990037") && R.Token == TEXT("USDC") &&
		R.Chain == TEXT("arbitrum") && R.TxHash == TEXT("0xab") && R.Payer == TEXT("0xcd") &&
		R.SessionId == TEXT("ps_1") && R.Error.IsEmpty());
	scfg::models::PaymentResult BareCore;
	BareCore.status = "cancelled";
	BareCore.kind = "tip";
	const FScfgPaymentResult Bare = ScfgConvert::PaymentResult(BareCore);
	TestTrue(TEXT("defaults"), Bare.Status == EScfgPaymentStatus::Cancelled && Bare.Kind == EScfgPayKind::Tip &&
		Bare.Amount.IsEmpty() && Bare.SessionId.IsEmpty());
	scfg::models::RestoreResult RestoreCore;
	RestoreCore.status = "restored";
	RestoreCore.restored = {Json::dict({{"item", "chapter2"}, {"kind", "unlock"}}), Json::dict({{"kind", "tip"}})};
	const FScfgRestoreResult RR = ScfgConvert::RestoreResult(RestoreCore);
	TestTrue(TEXT("a restore result"), RR.Status == EScfgRestoreStatus::Restored && RR.Restored.Num() == 2 &&
		RR.Restored[0].Item == TEXT("chapter2") && RR.Error.IsEmpty());
	TestTrue(TEXT("a receipt's kind is the enum"),
		RR.Restored[0].Kind == EScfgPayKind::Unlock && RR.Restored[1].Kind == EScfgPayKind::Tip);
	const FScfgPendingPayment Pending = ScfgConvert::PendingPayment(Json::dict({{"id", "ps_2"}, {"kind", "tip"}}));
	TestTrue(TEXT("a pending payment's session id and kind"),
		Pending.SessionId == TEXT("ps_2") && Pending.Kind == EScfgPayKind::Tip);
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeResolvedTest, "test_a_result_without_callback_reaches_payment_resolved")

bool FScfgFacadeResolvedTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	FScfgFacade& S = W.Configured(*this);
	TSharedRef<TArray<FScfgPaymentResult>> Got = MakeShared<TArray<FScfgPaymentResult>>();
	S.OnPaymentResolved = [Got](const FScfgPaymentResult& R) { Got->Add(R); };
	scfg::models::PaymentResult Expired;
	Expired.status = "expired";
	Expired.kind = "unlock";
	Expired.item = "chapter2";
	Expired.session_id = "ps_1";
	S.DeliverForTests(Expired);
	TestEqual(TEXT("emitted"), Got->Num(), 1);
	TestTrue(TEXT("a typed result"), Got->Num() == 1 && (*Got)[0].Status == EScfgPaymentStatus::Expired);
	TSharedRef<TArray<FString>> Called = MakeShared<TArray<FString>>();
	FScfgPayCallback Callback;
	Callback.Fn = [Called](const FScfgPaymentResult& R)
	{ Called->Add(R.Status == EScfgPaymentStatus::Paid ? TEXT("paid") : TEXT("other")); };
	S.SetCallbackForTests(TEXT("ps_2"), Callback);
	scfg::models::PaymentResult Paid;
	Paid.status = "paid";
	Paid.kind = "unlock";
	Paid.item = "chapter2";
	Paid.session_id = "ps_2";
	S.DeliverForTests(Paid);
	TestTrue(TEXT("a session's callback gets it instead"), *Called == TArray<FString>{TEXT("paid")} && Got->Num() == 1);
	TestFalse(TEXT("a final result drops the callback"), S.CallbackSessionIds().Contains(TEXT("ps_2")));
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeExportTextTest, "ExportReceiptsWritesPythonsJson")

bool FScfgFacadeExportTextTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	FScfgFacade& S = W.Configured(*this);
	W.Start();
	(void)W.AddReceipt(Hash("ab"));
	const std::string Expected = "[\n  {\n    \"amount\": \"4.99\",\n    \"block\": 990,\n    \"chain\": \"arbitrum\",\n"
		"    \"item\": \"chapter2\",\n    \"kind\": \"unlock\",\n    \"log_index\": 3,\n"
		"    \"payer\": \"0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed\",\n"
		"    \"recipient\": \"0x52908400098527886e0f7030069857d2e4169ee7\",\n    \"source\": \"payment\",\n"
		"    \"token\": \"USDC\",\n    \"tx_hash\": \"" + Hash("ab") + "\"\n  }\n]";
	const FString Saved = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	FString Relative = W.Dir / TEXT("rel/receipts.json");
	TestTrue(TEXT("the test directory is under Saved/"), FPaths::MakePathRelativeTo(Relative, *(Saved / TEXT(""))));
	const scfg::Result<FString> Written = S.ExportReceipts(Relative);
	TestTrue(*FString::Printf(TEXT("a relative path is under Saved/ (%s)"), Written.ok() ? *Written.value() : TEXT("-")),
		Written.ok() && Written.value() == W.Dir / TEXT("rel/receipts.json"));
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *(W.Dir / TEXT("rel/receipts.json")));
	const std::string Text(reinterpret_cast<const char*>(Bytes.GetData()), Bytes.Num());
	TestEqual(TEXT("json.dumps(values, indent=2, sort_keys=True)"), ScfgString::FromUtf8(Text),
		ScfgString::FromUtf8(Expected));
	// The default path: Saved/scfg-receipts.json (put back as it was).
	const FString Default = Saved / TEXT("scfg-receipts.json");
	TArray<uint8> Before;
	const bool bExisted = FFileHelper::LoadFileToArray(Before, *Default, FILEREAD_Silent);
	const scfg::Result<FString> Plain = S.ExportReceipts();
	TestTrue(TEXT("the default file"), Plain.ok() && Plain.value() == Default && IFileManager::Get().FileExists(*Default));
	if (bExisted)
	{
		FFileHelper::SaveArrayToFile(Before, *Default);
	}
	else
	{
		IFileManager::Get().Delete(*Default);
	}
	const scfg::Result<FString> Failed = S.ExportReceipts(W.Dir / TEXT("rel"));
	TestTrue(*FString::Printf(TEXT("a failed write says why (%s)"),
				 Failed.ok() ? TEXT("-") : *ScfgString::FromUtf8(Failed.err().message)),
		!Failed.ok() && Failed.err().message == ScfgString::ToUtf8(TEXT("cannot write ") + W.Dir / TEXT("rel: it is a directory")));
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeClearAnnouncesOnceTest, "AReceiptsChangeThenClearIsAnnouncedOnce")

bool FScfgFacadeClearAnnouncesOnceTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;
	FScfgFacade& S = W.Configured(*this);
	W.Start();
	TSharedRef<int32> Changed = MakeShared<int32>(0);
	S.OnReceiptsChanged = [Changed]() { ++*Changed; };
	W.Worker().Run(
		[](scfg::engine::Engine& E) -> scfg::Result<Json>
		{
			Json Receipts = E.receipts();
			SCFG_TRY(bool Added, scfg::receipts::add_receipt(Receipts.as_dict(), ReceiptFor(Hash("ab"))));
			return Json(Added);
		},
		nullptr);
	W.RunOnce();  // mirrored, not yet announced (no pump)
	TestEqual(TEXT("not announced yet"), *Changed, 0);
	S.ClearLocalData();
	TestEqual(TEXT("the clear announces it"), *Changed, 1);
	W.Pump();
	W.Pass();
	TestEqual(TEXT("once"), *Changed, 1);
	return true;
}

SCFG_FACADE_API_TEST(FScfgFacadeDefaultOptionsTest, "TheDefaultOptionsWireTheRealParts")

bool FScfgFacadeDefaultOptionsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgFacadeApiTests;
	FWorld W;  // for its directory only
	FTSTicker Ticker;  // never ticked: the worker's first pass would go to the network
	FScfgFacadeOptions Options;
	Options.StoreDirectory = W.Dir;
	Options.Ticker = &Ticker;
	Options.bQuiet = true;
	const TSharedRef<FScfgFacade> S = MakeShared<FScfgFacade>(Options);
	S->SheetHost = W.Host;
	S->Start();
	TestFalse(TEXT("configure"), S->Configure(Wallet(UTF8_TO_TCHAR(kDev))).IsSet());
	TestFalse(TEXT("add_item"), S->AddItem(TEXT("chapter2"), Chapter2()).IsSet());
	S->Pump();
	TestNotNull(TEXT("the pump starts the worker over the real transport"), S->GetWorker());
	if (S->GetWorker())
	{
		TestEqual(TEXT("its interval is the facade's: 15 s with nothing open"), S->GetWorker()->GetInterval(), 15.0);
	}
	TestEqual(TEXT("the store is in the given directory"), S->GetStore() ? S->GetStore()->GetPath() : FString(),
		W.Dir / TEXT("state.json"));
	TestFalse(TEXT("the desktop platform"), S->IsMobile());
	S->Pay(EScfgPayKind::Unlock, Unlock(), FScfgPayCallback(), nullptr);  // several networks: no session starts
	if (TestEqual(TEXT("a sheet"), W.Host->Kinds.Num(), 1))
	{
		const scfg::checkout::CheckoutController& Core = W.Host->CheckoutOf(-1).Core();
		TestEqual(TEXT("the game's name is the project's"), ScfgString::FromUtf8(Core.game_name),
			FString(FApp::GetProjectName()));
		TestFalse(TEXT("not mobile"), Core.mobile);
		TestEqual(TEXT("3 s while it is open"), S->GetWorker()->GetInterval(), 3.0);
		TestEqual(TEXT("no session was started"), FString(UTF8_TO_TCHAR(Core.state().c_str())), TEXT("choose"));
	}
	S->Shutdown();
	TestTrue(TEXT("nothing was reported"), S->GetReporter().Errors.Num() == 0 && S->GetReporter().Warnings.Num() == 0);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
