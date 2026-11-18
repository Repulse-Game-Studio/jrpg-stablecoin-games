// Copyright 2026 Repulse
// No Python twin: the opt-in live check of FScfgHttpTransport over real HTTPS (final review F2), the counterpart of the
// repository's `pytest -m live`. Scfg.Live.Https exists only when the editor's environment has SCFG_LIVE=1; without
// it GetTests lists nothing, so `--filter Scfg` never runs it and never touches the network.
//
// Each test makes one read-only JSON-RPC call through an RpcClient over FScfgHttpTransport to the first RPC url of a
// built-in testnet chain (chains::TESTNET()), so the path every real payment takes (the engine's libcurl, its TLS and
// its certificate bundle) runs once:
//   Scfg.Live.Https.EvmBlockNumber  eth_blockNumber on Arbitrum Sepolia (TESTNET()["arbitrum"].rpc[0])
//   Scfg.Live.Https.SolanaSlot      getSlot on Solana Devnet (TESTNET()["solana"].rpc[0])
// and asserts a parsed number above 0, answered on the game thread. Nothing of the user's goes out: no wallet, no
// key, no item, only the call itself (and the transport's User-Agent, scfg/0.1). The url, the number and the time are
// logged (AddInfo). The packaged counterpart is the e2e driver's live_https (tools/run_unreal_e2e.py --showcase
// --live).
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

#include "ScfgHttpTransport.h"
#include "ScfgString.h"

#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Co.h"
#include "Scfg/Err.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"
#include "Scfg/SolanaRpc.h"

#include <optional>
#include <string>

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgLiveTests
{

using scfg::BigInt;
using scfg::Result;

// The longest a test waits for the answer (the client's own timeout is kTimeout).
constexpr double kPatience = 40.0;
constexpr double kTimeout = 30.0;

bool Enabled()
{
	return FPlatformMisc::GetEnvironmentVariable(TEXT("SCFG_LIVE")) == TEXT("1");
}

// One call: the transport, the client over it, and what came back. Kept alive by the latent command until it is done.
struct FLive
{
	FScfgHttpTransport Transport;
	std::optional<scfg::rpc::RpcClient> Client;
	std::optional<scfg::solana::rpc::SolanaRpc> Solana;
	scfg::Job<BigInt> Job;  // declared last: destroyed before the clients and the transport
	std::optional<Result<BigInt>> Got;
	std::string Url;
	bool bOnGameThread = false;
	double Posted = 0;
	double Seconds = 0;
};

}  // namespace ScfgLiveTests

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FScfgLiveHttpsTest, "Scfg.Live.Https",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FScfgLiveHttpsTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	if (!ScfgLiveTests::Enabled())
	{
		return;  // opt-in: absent unless SCFG_LIVE=1
	}
	OutBeautifiedNames.Add(TEXT("EvmBlockNumber"));
	OutTestCommands.Add(TEXT("evm"));
	OutBeautifiedNames.Add(TEXT("SolanaSlot"));
	OutTestCommands.Add(TEXT("solana"));
}

bool FScfgLiveHttpsTest::RunTest(const FString& Parameters)
{
	using namespace ScfgLiveTests;
	if (!Enabled())
	{
		AddInfo(TEXT("skipped: Scfg.Live.Https runs only with SCFG_LIVE=1 in the environment"));
		return true;
	}
	const bool bSolana = Parameters == TEXT("solana");
	const scfg::chains::Chain* Chain = scfg::chains::TESTNET().find(bSolana ? "solana" : "arbitrum");
	if (!TestTrue(TEXT("the built-in testnet chain"), Chain != nullptr && !Chain->rpc.empty()))
	{
		return false;
	}
	TestTrue(TEXT("its first RPC url is https"), Chain->rpc[0].rfind("https://", 0) == 0);
	TSharedRef<FLive> S = MakeShared<FLive>();
	S->Url = Chain->rpc[0];
	scfg::rpc::RpcClientOptions Options;
	Options.timeout = kTimeout;
	Result<scfg::rpc::RpcClient> Client = scfg::rpc::RpcClient::make({S->Url}, S->Transport, Options);
	if (!TestTrue(TEXT("client"), Client.ok()))
	{
		return false;
	}
	S->Client.emplace(Client.take());
	FLive* Raw = &S.Get();
	auto Done = [Raw](Result<BigInt> R)
	{
		Raw->bOnGameThread = IsInGameThread() && FPlatformTLS::GetCurrentThreadId() == GGameThreadId;
		Raw->Seconds = FPlatformTime::Seconds() - Raw->Posted;
		Raw->Got.emplace(std::move(R));
	};
	S->Posted = FPlatformTime::Seconds();
	if (bSolana)
	{
		S->Solana.emplace(*S->Client);
		S->Job.start(S->Solana->get_slot(), Done);
	}
	else
	{
		S->Job.start(S->Client->block_number(), Done);
	}
	const FString What = bSolana ? TEXT("getSlot (Solana Devnet)") : TEXT("eth_blockNumber (Arbitrum Sepolia)");
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, S, What]()
	{
		if (!S->Got && FPlatformTime::Seconds() - S->Posted < kPatience)
		{
			return false;
		}
		const FString Url = ScfgString::FromUtf8(S->Url);
		if (!TestTrue(TEXT("answered in time"), S->Got.has_value()))
		{
			AddError(FString::Printf(TEXT("%s at %s: no answer in %.0f s"), *What, *Url, kPatience));
			return true;
		}
		if (!S->Got->ok())
		{
			AddError(FString::Printf(TEXT("%s at %s failed after %.0f ms: %s"), *What, *Url, S->Seconds * 1000.0,
				*ScfgString::FromUtf8(S->Got->err().str())));
			return true;
		}
		TestTrue(TEXT("a number above 0"), S->Got->value() > BigInt(0));
		TestTrue(TEXT("completed on the game thread"), S->bOnGameThread);
		TestEqual(TEXT("one request"), S->Transport.Posts(), 1);
		AddInfo(FString::Printf(TEXT("live %s at %s: %s in %.0f ms"), *What, *Url,
			*ScfgString::FromUtf8(S->Got->value().to_decimal()), S->Seconds * 1000.0));
		return true;
	}));
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
