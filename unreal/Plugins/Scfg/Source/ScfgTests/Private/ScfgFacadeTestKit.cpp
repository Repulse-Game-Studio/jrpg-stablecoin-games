// No Python twin: the facade tests' doubles (see ScfgFacadeTestKit.h).
#include "ScfgFacadeTestKit.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#include "ScfgString.h"
#include "ScfgStore.h"
#include "ScfgWorker.h"

#include "Scfg/Backends.h"
#include "Scfg/BackendsBase.h"
#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Evm.h"
#include "Scfg/Py.h"
#include "Scfg/Receipts.h"
#include "Scfg/Rpc.h"

#include <memory>
#include <string_view>
#include <utility>

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgFacadeTests
{

const char* const kDev = "0x52908400098527886E0F7030069857D2E4169EE7";
const char* const kPayer = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";
const std::string kTx = Hash("ab");
const char* const kServerUrl = "http://127.0.0.1:1234/c/secret/";

std::string Hash(const char* Pair)
{
	std::string Out = "0x";
	for (int I = 0; I < 32; ++I)
	{
		Out += Pair;
	}
	return Out;
}

std::string UsdcArbitrum()
{
	const scfg::chains::Chain* Arbitrum = scfg::chains::MAINNET().find("arbitrum");
	const scfg::chains::Token* Usdc = Arbitrum ? Arbitrum->tokens.find("USDC") : nullptr;
	return Usdc ? Usdc->address : std::string();
}

Json ReceiptFor(const std::string& Tx)
{
	return Json::dict({{"chain", "arbitrum"}, {"tx_hash", Tx}, {"log_index", 3}, {"block", 990}, {"kind", "unlock"},
		{"item", "chapter2"}, {"amount", "4.99"}, {"token", "USDC"},
		{"payer", "0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed"},
		{"recipient", scfg::py::lower_ascii(kDev)}, {"source", "payment"}});
}

namespace KitDetail
{

// format(n, "064x").
std::string Hex64(const scfg::BigInt& N)
{
	std::string Digits = scfg::py::hex(N).substr(2);
	return std::string(Digits.size() < 64 ? 64 - Digits.size() : 0, '0') + Digits;
}

Json Envelope(const Json& Payload, Json Result)
{
	return Json::dict({{"jsonrpc", "2.0"}, {"id", Payload.as_dict().get("id")}, {"result", std::move(Result)}});
}

Json Param(const Json& Params, size_t I)
{
	if (!Params.is_list() || Params.as_list().size() <= I)
	{
		return Json();
	}
	return Params.as_list()[I];
}

std::optional<int64_t> Quantity(const Json& V)
{
	scfg::Result<std::optional<int64_t>> N = scfg::py::py_int_hex_int64(V);
	return N.ok() ? N.value() : std::nullopt;
}

}  // namespace KitDetail

Json TransferLog(const std::string& Token, const std::string& Sender, const std::string& Recipient, int64_t Value,
	int64_t Block, const std::string& TxHash)
{
	using namespace KitDetail;
	const Json Topics = Json::list({std::string(scfg::evm::TRANSFER_TOPIC), scfg::evm::address_topic(Sender).value(),
		scfg::evm::address_topic(Recipient).value()});
	return Json::dict({{"address", scfg::py::lower_ascii(Token)}, {"topics", Topics}, {"data", "0x" + Hex64(Value)},
		{"transactionHash", TxHash}, {"logIndex", scfg::py::hex(scfg::BigInt(0))},
		{"blockNumber", scfg::py::hex(scfg::BigInt(Block))}, {"blockHash", "0x" + Hex64(scfg::BigInt(Block))}});
}

bool Same(const Json& A, const Json& B)
{
	scfg::json::DumpOptions Sorted;
	Sorted.sort_keys = true;
	scfg::Result<std::string> X = scfg::json::dumps(A, Sorted);
	scfg::Result<std::string> Y = scfg::json::dumps(B, Sorted);
	return X.ok() && Y.ok() && X.value() == Y.value();
}

FString Text(const Json& V)
{
	scfg::Result<std::string> Got = scfg::json::dumps(V);
	return Got.ok() ? ScfgString::FromUtf8(Got.value()) : FString(TEXT("<dumps failed>"));
}

// --- the fake nodes ---------------------------------------------------------------------------------------------

scfg::Result<Json> FFakeNodeTransport::Answer(const std::string& Url, const Json& Payload)
{
	using namespace KitDetail;
	const std::string Prefix = "fake://evm/";
	const std::string Key = Url.rfind(Prefix, 0) == 0 ? Url.substr(Prefix.size()) : std::string();
	const std::string Method = Payload.as_dict().get("method").as_str();
	const Json Params = Payload.as_dict().get("params");
	auto Refuse = [&](const std::string& Why) -> scfg::Result<Json>
	{
		Chains->Problems.Add(ScfgString::FromUtf8(Url + " " + Method + ": " + Why));
		return Json::dict({{"jsonrpc", "2.0"}, {"id", Payload.as_dict().get("id")},
			{"error", Json::dict({{"code", -32601}, {"message", "the fake does not answer " + Method}})}});
	};
	if (Key.empty())
	{
		return Refuse("no fake chain at this url");
	}
	FFakeChain& Chain = Chains->Evm[Key];
	Chain.Calls.Add(ScfgString::FromUtf8(Method));
	if (Method == "eth_blockNumber")
	{
		return Envelope(Payload, scfg::py::hex(scfg::BigInt(Chain.Head)));
	}
	if (Method == "eth_getBlockByNumber")
	{
		const std::optional<int64_t> N = Quantity(Param(Params, 0));
		if (!N)
		{
			return Refuse("bad block number");
		}
		return Envelope(Payload, *N <= Chain.Head ? Json::dict({{"hash", "0x" + Hex64(scfg::BigInt(*N))}}) : Json());
	}
	if (Method == "eth_getTransactionReceipt")
	{
		const Json Tx = Param(Params, 0);
		auto It = Tx.is_str() ? Chain.TxReceipts.find(Tx.as_str()) : Chain.TxReceipts.end();
		return Envelope(Payload, It != Chain.TxReceipts.end() ? It->second : Json());
	}
	if (Method == "eth_getLogs")
	{
		const Json Filter = Param(Params, 0);
		if (!Filter.is_dict())
		{
			return Refuse("no filter");
		}
		const std::optional<int64_t> From = Quantity(Filter.as_dict().get("fromBlock"));
		const std::optional<int64_t> To = Quantity(Filter.as_dict().get("toBlock"));
		const Json Address = Filter.as_dict().get("address");
		if (!From || !To || !Address.is_str())
		{
			return Refuse("bad filter");
		}
		Json::List Out;
		for (const Json& Log : Chain.Logs)
		{
			const std::optional<int64_t> Block = Quantity(Log.as_dict().get("blockNumber"));
			if (Block && *From <= *Block && *Block <= *To &&
				scfg::py::lower_ascii(Log.as_dict().get("address").as_str()) == scfg::py::lower_ascii(Address.as_str()))
			{
				Out.push_back(Log);
			}
		}
		return Envelope(Payload, Json(std::move(Out)));
	}
	// A Solana node, as far as the readiness check, a session start and a scan that finds nothing need it (the mobile
	// tests' Solana Pay link): its slot is Head, every account exists, and no address has a signature.
	if (Method == "getSlot")
	{
		return Envelope(Payload, Json(Chain.Head));
	}
	if (Method == "getAccountInfo")
	{
		return Envelope(Payload, Json::dict({{"context", Json::dict({{"slot", Json(Chain.Head)}})},
			{"value", Json::dict({{"lamports", Json(int64_t{2039280})}})}}));
	}
	if (Method == "getSignaturesForAddress")
	{
		return Envelope(Payload, Json(Json::List()));
	}
	return Refuse("unknown method");
}

scfg::Future<Json> FFakeNodeTransport::post_json(std::string Url, Json Payload, double Timeout)
{
	(void)Timeout;
	++Chains->Posts;
	if (bShutDown)
	{
		return scfg::Future<Json>::completed(scfg::Err::make("CancelledError", "shut down"));
	}
	if (Chains->BoomAfter > 0)
	{
		Held.push_back(FHeld{scfg::Promise<Json>(), scfg::Err::make("RuntimeError", "boom after a few frames"),
			Chains->BoomAfter});
		return Held.back().Promise.get_future();
	}
	scfg::Result<Json> Got = Answer(Url, Payload);
	if (!Chains->bHold)
	{
		return scfg::Future<Json>::completed(std::move(Got));
	}
	Held.push_back(FHeld{scfg::Promise<Json>(), std::move(Got), 1});
	return Held.back().Promise.get_future();
}

void FFakeNodeTransport::Shutdown()
{
	++Chains->Shutdowns;
	bShutDown = true;
	std::deque<FHeld> Dropped = std::move(Held);
	Held.clear();
	Chains->DroppedAtShutdown += static_cast<int32>(Dropped.size());
}

void FFakeNodeTransport::Frame()
{
	std::deque<FHeld> Due;
	std::deque<FHeld> Later;
	for (FHeld& Each : Held)
	{
		const bool bBoom = !Each.Answer.ok();
		if (!bBoom && Chains->bHold)
		{
			Later.push_back(std::move(Each));
			continue;
		}
		(--Each.FramesLeft <= 0 ? Due : Later).push_back(std::move(Each));
	}
	Held = std::move(Later);
	for (FHeld& Each : Due)
	{
		Each.Promise.complete(std::move(Each.Answer));
	}
}

// --- the host ---------------------------------------------------------------------------------------------------

TSharedPtr<FScfgScreen> FFakeHost::OpenPayment(const TSharedRef<FScfgCheckout>& Checkout)
{
	TSharedRef<FFakeSheet> Sheet = MakeShared<FFakeSheet>();
	Sheet->Checkout = Checkout;
	Sheets.Add(Sheet);
	Shown.Add(Sheet);
	Kinds.Add(TEXT("payment"));
	return Sheet;
}

TSharedPtr<FScfgScreen> FFakeHost::OpenRestore(const TSharedRef<FScfgRestore>& Restore)
{
	TSharedRef<FFakeSheet> Sheet = MakeShared<FFakeSheet>();
	Sheet->Restore = Restore;
	Sheets.Add(Sheet);
	Shown.Add(Sheet);
	Kinds.Add(TEXT("restore"));
	return Sheet;
}

void FFakeHost::Close(const TSharedRef<FScfgScreen>& Screen)
{
	++Closes;
	Journal->Add(TEXT("sheet closed"));
	Shown.RemoveAll([&Screen](const TSharedPtr<FFakeSheet>& Each) { return Each.Get() == &*Screen; });
}

TSharedPtr<FFakeSheet> FFakeHost::Sheet(int32 I) const
{
	const int32 At = I < 0 ? Sheets.Num() + I : I;
	return Sheets.IsValidIndex(At) ? Sheets[At].Pin() : nullptr;
}

void FFakeHost::CloseSheet(int32 I, const FString& How)
{
	if (const TSharedPtr<FFakeSheet> S = Sheet(I))
	{
		S->Close(How);
	}
}

void FFakeHost::Release(int32 I)
{
	const TSharedPtr<FFakeSheet> S = Sheet(I);
	Shown.RemoveAll([&S](const TSharedPtr<FFakeSheet>& Each) { return Each == S; });
}

FScfgCheckout& FFakeHost::CheckoutOf(int32 I) const
{
	return *Sheet(I)->Checkout;
}

FScfgRestore& FFakeHost::RestoreOf(int32 I) const
{
	return *Sheet(I)->Restore;
}

// --- the recording page -------------------------------------------------------------------------------------------

namespace KitDetail
{

class FRecordingServer final : public scfg::checkout::Server
{
public:
	FRecordingServer(scfg::checkout::ServerArgs InArgs, TSharedRef<TArray<FString>> InJournal,
		TSharedRef<FServerLog> InLog)
		: Args(std::move(InArgs)), Journal(InJournal), Log(InLog)
	{
		Log->bHasBuildTx = static_cast<bool>(Args.build_tx);
	}
	virtual ~FRecordingServer() override { Log->bDestroyed = true; }
	virtual scfg::Status start() override
	{
		Log->bStarted = true;
		bRunning = true;
		return scfg::ok();
	}
	virtual void stop() override
	{
		if (bRunning)
		{
			// The controller still answers: its get_status runs on a live controller.
			const Json Status = Args.get_status ? Args.get_status() : Json();
			Journal->Add(Status.is_dict() && Status.as_dict().get("state").is_str()
					? FString(TEXT("page stopped"))
					: FString(TEXT("page stopped (the controller did not answer)")));
		}
		bRunning = false;
		Log->bStopped = true;
	}
	virtual std::string url() const override { return kServerUrl; }

private:
	scfg::checkout::ServerArgs Args;
	TSharedRef<TArray<FString>> Journal;
	TSharedRef<FServerLog> Log;
	bool bRunning = false;
};

}  // namespace KitDetail

scfg::checkout::ServerFactory RecordingFactory(
	TSharedRef<TArray<FString>> Journal, TSharedRef<TArray<TSharedRef<FServerLog>>> Logs)
{
	return [Journal, Logs](scfg::checkout::ServerArgs Args) -> scfg::Result<std::unique_ptr<scfg::checkout::Server>>
	{
		TSharedRef<FServerLog> Log = MakeShared<FServerLog>();
		Logs->Add(Log);
		return std::unique_ptr<scfg::checkout::Server>(
			std::make_unique<KitDetail::FRecordingServer>(std::move(Args), Journal, Log));
	};
}

// --- the world ----------------------------------------------------------------------------------------------------

FWorld::FWorld()
{
	Dir = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("ScfgTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Dir, true);
}

FWorld::~FWorld()
{
	Drop();
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Dir), false, false);  // ScfgTests/, once it is empty
}

FScfgFacadeOptions FWorld::Options()
{
	FScfgFacadeOptions Out;
	Out.StoreDirectory = Dir;
	Out.Clock = [Clock = Now]() { return *Clock; };
	Out.Ticker = &Ticker;
	TSharedRef<FFakeChains> Shared = Chains;
	FFakeNodeTransport** Slot = &Transport;
	// The worker's transport is the fake nodes; every chain's client posts to it under its key.
	TSharedRef<FScfgTransport*> Made = MakeShared<FScfgTransport*>(nullptr);
	Out.MakeTransport = [Shared, Slot, Made]() -> TUniquePtr<FScfgTransport>
	{
		TUniquePtr<FFakeNodeTransport> T = MakeUnique<FFakeNodeTransport>(Shared);
		*Slot = T.Get();
		*Made = T.Get();
		return T;
	};
	TSharedRef<double> Ticks = MakeShared<double>(0.0);
	Out.RpcFactory = [Made, Ticks](const scfg::chains::Chain& Chain) -> scfg::Result<scfg::backends::base::Rpc>
	{
		SCFG_TRY(const scfg::backends::base::Backend* Backend, scfg::backends::backend_for(Chain));
		std::vector<std::string> Urls{"fake://evm/" + Chain.key};
		SCFG_TRY(scfg::backends::base::Rpc Rpc, Backend->make_rpc(Chain.with(std::move(Urls)), **Made));
		// A Python fake has no failover cooldown: the client's clock jumps a day per read.
		scfg::backends::base::client_of(Rpc).clock = [Ticks]() { return *Ticks += 86400.0; };
		return Rpc;
	};
	if (bManualWorker)
	{
		Out.WorkerInterval = []() { return 1e9; };  // Godot's ManualTimer: no pass unless RunOnce()
	}
	Out.bQuiet = true;
	Out.Platform = Platform;
	if (bRecordingServer)
	{
		Out.ServerFactory = RecordingFactory(Journal, Servers);
	}
	else if (ServerFactory)
	{
		Out.ServerFactory = ServerFactory;
	}
	Out.ProjectDir = Dir;
	Out.GameName = TEXT("ScfgTests");
	Out.bPumpOnTicker = false;
	return Out;
}

FScfgFacade& FWorld::Make()
{
	Drop();
	Facade = MakeShared<FScfgFacade>(Options());
	Facade->SheetHost = Host;
	Facade->Start();
	return *Facade;
}

FScfgFacade& FWorld::Configured(FAutomationTestBase& Test)
{
	FScfgFacade& S = Make();
	FScfgOptions Opts;
	Opts.Wallet = UTF8_TO_TCHAR(kDev);
	Test.TestFalse(TEXT("configure"), S.Configure(Opts).IsSet());
	FScfgItem Item;
	Item.Code = 37;
	Item.Price = TEXT("4.99");
	Item.Token = TEXT("USDC");
	Item.Title = TEXT("Chapter 2");
	Test.TestFalse(TEXT("add_item"), S.AddItem(TEXT("chapter2"), Item).IsSet());
	return S;
}

FScfgFacade& FWorld::Started(FAutomationTestBase& Test)
{
	FScfgFacade& S = Configured(Test);
	Pump();
	Test.TestTrue(TEXT("the first pump starts the worker"), S.GetWorker() != nullptr);
	RunOnce();
	Pump();
	return S;
}

void FWorld::Drop()
{
	if (Facade.IsValid())
	{
		const TSharedPtr<FScfgFacade> Going = MoveTemp(Facade);
		Facade.Reset();
		Going->Shutdown();
		Transport = nullptr;
	}
}

void FWorld::Frame()
{
	if (Transport && Facade.IsValid() && Facade->GetWorker())
	{
		Transport->Frame();
	}
	Ticker.Tick(0.0f);
}

void FWorld::Advance(double Seconds)
{
	*Now += Seconds;
	TickerOwed += Seconds;
}

void FWorld::Pump()
{
	if (Facade.IsValid())
	{
		Facade->Pump();
	}
	// Only when the clock moved: a tick of the ticker also runs the worker's queued commands, which Godot's manual
	// scheduler runs only in run_once().
	if (TickerOwed > 0)
	{
		const double Owed = TickerOwed;
		TickerOwed = 0;
		Ticker.Tick(static_cast<float>(Owed));
	}
}

bool FWorld::RunOnce()
{
	FScfgWorker* W = Facade.IsValid() ? Facade->GetWorker() : nullptr;
	if (!W)
	{
		return false;
	}
	TSharedRef<bool> Ended = MakeShared<bool>(false);
	W->Tick([Ended](scfg::Result<FScfgWorker::FOutcomes>) { *Ended = true; });
	for (int32 I = 0; I < 200 && !*Ended; ++I)
	{
		Frame();
	}
	return *Ended;
}

void FWorld::Pass()
{
	RunOnce();
	Pump();
}

void FWorld::Start()
{
	Pump();
	RunOnce();
	Pump();
}

scfg::Result<Json> FWorld::Run(TUniqueFunction<scfg::Result<Json>(scfg::engine::Engine&)> Fn)
{
	TSharedRef<TOptional<scfg::Result<Json>>> Got = MakeShared<TOptional<scfg::Result<Json>>>();
	Worker().Run(MoveTemp(Fn), [Got](scfg::Result<Json> R) { Got->Emplace(std::move(R)); });
	RunOnce();
	Pump();
	if (!Got->IsSet())
	{
		return scfg::Err::make("RuntimeError", "the command did not finish");
	}
	return MoveTemp(Got->GetValue());
}

Json FWorld::StartSession()
{
	TSharedRef<TOptional<scfg::Result<Json>>> Got = MakeShared<TOptional<scfg::Result<Json>>>();
	Worker().StartSession("unlock", "arbitrum", Json::dict({{"item", "chapter2"}}),
		[Got](scfg::Result<Json> R) { Got->Emplace(std::move(R)); });
	RunOnce();
	Pump();
	return Got->IsSet() && Got->GetValue().ok() ? Got->GetValue().value() : Json();
}

scfg::Result<Json> FWorld::AddReceipt(const std::string& Tx)
{
	return Run(
		[Tx](scfg::engine::Engine& E) -> scfg::Result<Json>
		{
			Json Receipts = E.receipts();  // a shared handle: the Engine's own store
			SCFG_TRY(bool Added, scfg::receipts::add_receipt(Receipts.as_dict(), ReceiptFor(Tx)));
			return Json(Added);
		});
}

Json FWorld::OnDiskPending() const
{
	return FScfgStore::Load(StatePath()).Pending;
}

Json FWorld::OnDiskReceipts() const
{
	return FScfgStore::Load(StatePath()).Receipts;
}

TSharedRef<TOptional<FScfgPaymentResult>> FWorld::Pay(
	EScfgPayKind Kind, const FScfgPayRequest& Request, TSharedPtr<TArray<FScfgPaymentResult>> Called)
{
	TSharedRef<TOptional<FScfgPaymentResult>> Box = MakeShared<TOptional<FScfgPaymentResult>>();
	FScfgPayCallback Callback;
	if (Called.IsValid())
	{
		Callback.Fn = [Called](const FScfgPaymentResult& R) { Called->Add(R); };
	}
	Facade->Pay(Kind, Request, MoveTemp(Callback), [Box](const FScfgPaymentResult& R) { *Box = R; });
	return Box;
}

TSharedRef<TOptional<FScfgRestoreResult>> FWorld::Restore(TSharedPtr<TArray<FScfgRestoreResult>> Called)
{
	TSharedRef<TOptional<FScfgRestoreResult>> Box = MakeShared<TOptional<FScfgRestoreResult>>();
	FScfgRestoreCallback Callback;
	if (Called.IsValid())
	{
		Callback.Fn = [Called](const FScfgRestoreResult& R) { Called->Add(R); };
	}
	Facade->Restore(MoveTemp(Callback), [Box](const FScfgRestoreResult& R) { *Box = R; });
	return Box;
}

FScfgCheckout& FWorld::Session(FAutomationTestBase& Test)
{
	FScfgCheckout& Ctl = Host->CheckoutOf(-1);
	Test.TestTrue(TEXT("choose a network"), Ctl.Core().choose_network("arbitrum").ok());
	Pass();
	Test.TestFalse(TEXT("a session started"), SessionIdOf(Ctl).IsEmpty());
	return Ctl;
}

void FWorld::PayOnChain(FScfgCheckout& Checkout)
{
	const Json& Session = Checkout.Core().session();
	if (!Session.is_dict() || !Session.as_dict().get("units").is_int64())
	{
		return;  // no session: the caller's assertions fail, the editor does not abort
	}
	FFakeChain& Arbitrum = Chains->Chain("arbitrum");
	const int64_t Block = Arbitrum.Head + 1;
	Arbitrum.Logs.push_back(TransferLog(UsdcArbitrum(), kPayer, kDev, Session.as_dict().get("units").as_int64(), Block,
		kTx));
	Arbitrum.Head = Block;
	Pass();
	Arbitrum.Head = Block + 19;
	Pass();
}

bool SameLines(FAutomationTestBase& Test, const TCHAR* What, const TArray<FString>& Actual,
	const TArray<FString>& Expected)
{
	return Test.TestEqual(What, FString::Join(Actual, TEXT(" | ")), FString::Join(Expected, TEXT(" | ")));
}

FScfgPayRequest Unlock(const FString& Item)
{
	FScfgPayRequest R;
	R.Item = Item;
	return R;
}

FScfgPayRequest Tip(const FString& Amount, const FString& Token)
{
	FScfgPayRequest R;
	R.Amount = Amount;
	R.Token = Token;
	return R;
}

FString SessionIdOf(const FScfgCheckout& Checkout)
{
	const std::optional<std::string> Sid = Checkout.Core().session_id();
	return Sid ? ScfgString::FromUtf8(*Sid) : FString();
}

}  // namespace ScfgFacadeTests

#endif  // WITH_DEV_AUTOMATION_TESTS
