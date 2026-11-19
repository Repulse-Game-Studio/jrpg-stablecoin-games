// Copyright 2026 Repulse
// Twin of scfg_core/rpc.py's UrllibTransport (see ScfgHttpTransport.h).
#include "ScfgHttpTransport.h"

#include "ScfgLog.h"
#include "ScfgString.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ConfigCacheIni.h"
#if WITH_SSL
#include "Interfaces/ISslCertificateManager.h"
#include "SslModule.h"
#endif

#include "Scfg/Err.h"
#include "Scfg/Result.h"

#include <optional>
#include <string_view>
#include <utility>

namespace ScfgHttpTransportDetail
{

// The TransportError of a request that got no HTTP answer. urllib's own texts where the engine tells as much
// (socket.timeout's "timed out"; URLError's "<urlopen error ...>", whose reason, an errno text in Python, the engine
// does not give), and the core's "response too large".
// EHttpFailureReason::ResponseTooLarge is newer than the enum's first five values: an engine without it has no such
// failure to report, and the plugin still compiles there.
template <typename E>
constexpr bool IsResponseTooLarge(E Reason)
{
	if constexpr (requires { E::ResponseTooLarge; })
	{
		return Reason == E::ResponseTooLarge;
	}
	else
	{
		return false;
	}
}

std::string FailureMessage(EHttpFailureReason Reason)
{
	if (IsResponseTooLarge(Reason))
	{
		return std::string(scfg::coio::RESPONSE_TOO_LARGE);
	}
	switch (Reason)
	{
	case EHttpFailureReason::TimedOut:
		return "timed out";
	case EHttpFailureReason::ConnectionError:
		return "<urlopen error connection failed>";
	case EHttpFailureReason::Cancelled:
		return "<urlopen error the request was cancelled>";
	case EHttpFailureReason::None:
	case EHttpFailureReason::Other:
	default:
		// a connection that closed before any status line: http.client's RemoteDisconnected inside URLError
		return "<urlopen error Remote end closed connection without response>";
	}
}

scfg::Err NoAnswer(EHttpFailureReason Reason)
{
	return scfg::Err::transport_error(FailureMessage(Reason), std::nullopt);
}

// A request the engine would not start (godot's http_transport.gd says "cannot start the request to <url>").
scfg::Err CannotStart(const std::string& Url)
{
	return scfg::Err::transport_error("<urlopen error cannot start the request to '" + Url + "'>", std::nullopt);
}

#if !UE_BUILD_SHIPPING
int32& ProcessPosts()
{
	static int32 Count = 0;
	return Count;
}

TFunction<FString()>& TlsProbe()
{
	static TFunction<FString()> Probe;
	return Probe;
}
#endif

bool IsHttps(const std::string& Url)
{
	constexpr std::string_view Scheme = "https://";
	if (Url.size() < Scheme.size())
	{
		return false;
	}
	for (size_t I = 0; I < Scheme.size(); ++I)
	{
		const char C = Url[I];
		if ((C >= 'A' && C <= 'Z' ? static_cast<char>(C - 'A' + 'a') : C) != Scheme[I])
		{
			return false;
		}
	}
	return true;
}

}  // namespace ScfgHttpTransportDetail

FScfgHttpTransport::~FScfgHttpTransport()
{
	check(IsInGameThread());
	Shutdown();
}

scfg::Future<scfg::Json> FScfgHttpTransport::post_json(std::string Url, scfg::Json Payload, double Timeout)
{
	check(IsInGameThread());
	++NumPosts;
#if !UE_BUILD_SHIPPING
	++ScfgHttpTransportDetail::ProcessPosts();
#endif
	if (bShutDown)
	{
		return scfg::Future<scfg::Json>::completed(
			scfg::Err::make("CancelledError", "the HTTP transport was shut down"));
	}
	if (ScfgHttpTransportDetail::IsHttps(Url))
	{
		const FString Problem = TlsVerificationProblem();
		if (!Problem.IsEmpty())
		{
			if (!bLoggedTls)
			{
				bLoggedTls = true;
				UE_LOG(LogScfg, Error, TEXT("scfg: refusing https:// RPC requests: %hs: %s"), TlsUnavailable, *Problem);
			}
			return scfg::Future<scfg::Json>::completed(scfg::Err::transport_error(TlsUnavailable, std::nullopt));
		}
	}
	scfg::Result<std::string> Body = scfg::json::dumps(Payload);
	if (!Body.ok())
	{
		return scfg::Future<scfg::Json>::completed(std::move(Body).err());
	}
	const std::string& Text = Body.value();

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
	Request->SetVerb(TEXT("POST"));
	Request->SetURL(ScfgString::FromUtf8(Url));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("User-Agent"), UserAgent);
	Request->SetContent(TArray<uint8>(reinterpret_cast<const uint8*>(Text.data()), static_cast<int32>(Text.size())));
	if (Timeout > 0)
	{
		Request->SetTimeout(static_cast<float>(Timeout));
	}
	Request->OnProcessRequestComplete().BindRaw(this, &FScfgHttpTransport::OnComplete);

	TUniquePtr<FWaiting> Entry = MakeUnique<FWaiting>();
	Entry->Request = Request;
	scfg::Future<scfg::Json> Answer = Entry->Promise.get_future();
	Waiting.Add(&Request.Get(), MoveTemp(Entry));
	// A request the engine refuses (an empty or disallowed URL, a disabled HTTP module) is finished inside
	// ProcessRequest, which then calls OnComplete at once on the game thread (HttpRequestCommon.cpp:180-201, :224-231):
	// Starting tells OnComplete so.
	Starting = &Request.Get();
	++NumStarted;
	const bool bStarted = Request->ProcessRequest();
	Starting = nullptr;
	if (!bStarted)
	{
		if (TUniquePtr<FWaiting> Refused = Take(&Request.Get()))  // not finished by the engine: ours to fail
		{
			Request->OnProcessRequestComplete().Unbind();
			Refused->Promise.complete(ScfgHttpTransportDetail::CannotStart(Url));
		}
	}
	return Answer;
}

#if !UE_BUILD_SHIPPING
int32 FScfgHttpTransport::ProcessPosts()
{
	return ScfgHttpTransportDetail::ProcessPosts();
}

void FScfgHttpTransport::SetTlsProbeForTests(TFunction<FString()> Probe)
{
	ScfgHttpTransportDetail::TlsProbe() = MoveTemp(Probe);
}
#endif

FString FScfgHttpTransport::TlsVerificationProblem()
{
#if !UE_BUILD_SHIPPING
	if (const TFunction<FString()>& Probe = ScfgHttpTransportDetail::TlsProbe())
	{
		return Probe();
	}
#endif
#if PLATFORM_APPLE
	return FString();  // FAppleHttpRequest: NSURLSession verifies with the system's trust store
#elif WITH_SSL
	bool bVerifyPeer = true;
	if (GConfig && GConfig->GetBool(TEXT("/Script/Engine.NetworkSettings"), TEXT("n.VerifyPeer"), bVerifyPeer, GEngineIni) &&
		!bVerifyPeer)
	{
		return TEXT("[/Script/Engine.NetworkSettings] n.VerifyPeer is False in the engine ini");
	}
	if (!FSslModule::Get().GetCertificateManager().HasCertificatesAvailable())
	{
		return TEXT("the engine found no root certificates (no certificate bundle), so libcurl would not verify the peer");
	}
	return FString();
#else
	return TEXT("the engine has no SSL module on this platform");
#endif
}

void FScfgHttpTransport::OnComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected)
{
	check(IsInGameThread());
	TUniquePtr<FWaiting> Entry = Request.IsValid() ? Take(Request.Get()) : nullptr;
	if (!Entry)
	{
		return;  // not one of ours any more (Shutdown unbinds first, so this is only a safety net)
	}
	Request->OnProcessRequestComplete().Unbind();
	const bool bAnswered = bConnected && Response.IsValid() && Response->GetResponseCode() > 0;
	// Entry is out of the map: the resumed coroutine may post again, or shut this transport down.
	if (bAnswered)
	{
		const TArray<uint8>& Body = Response->GetContent();
		Entry->Promise.complete(scfg::coio::parse_rpc_body(Response->GetResponseCode(),
			std::string_view(reinterpret_cast<const char*>(Body.GetData()), static_cast<size_t>(Body.Num()))));
	}
	else if (Request.Get() == Starting)
	{
		Entry->Promise.complete(ScfgHttpTransportDetail::CannotStart(ScfgString::ToUtf8(Request->GetURL())));
	}
	else
	{
		Entry->Promise.complete(ScfgHttpTransportDetail::NoAnswer(Request->GetFailureReason()));
	}
}

TUniquePtr<FScfgHttpTransport::FWaiting> FScfgHttpTransport::Take(const IHttpRequest* Request)
{
	TUniquePtr<FWaiting> Entry;
	if (TUniquePtr<FWaiting>* Found = Waiting.Find(Request))
	{
		Entry = MoveTemp(*Found);
		Waiting.Remove(Request);
	}
	return Entry;
}

void FScfgHttpTransport::cancel_orphaned()
{
	check(IsInGameThread());
	TArray<TUniquePtr<FWaiting>> Orphans;
	for (auto It = Waiting.CreateIterator(); It; ++It)
	{
		if (It.Value()->Promise.orphaned())
		{
			Orphans.Add(MoveTemp(It.Value()));
			It.RemoveCurrent();
		}
	}
	for (TUniquePtr<FWaiting>& Each : Orphans)
	{
		// Unbound first, as in Shutdown(): CancelRequest may finish the request (and call the delegate) at once.
		Each->Request->OnProcessRequestComplete().Unbind();
		Each->Request->CancelRequest();
		++NumCancelledOrphans;
	}
	// Nobody awaits these promises: dropping them resumes nothing.
	Orphans.Reset();
}

void FScfgHttpTransport::Shutdown()
{
	check(IsInGameThread());  // it may resume waiting coroutines
	bShutDown = true;
	TMap<const IHttpRequest*, TUniquePtr<FWaiting>> Dropped = MoveTemp(Waiting);
	Waiting.Reset();
	for (TPair<const IHttpRequest*, TUniquePtr<FWaiting>>& Each : Dropped)
	{
		// Unbound first: CancelRequest may finish the request (and call the delegate) at once, or on a later tick.
		Each.Value->Request->OnProcessRequestComplete().Unbind();
		Each.Value->Request->CancelRequest();
	}
	// Dropping the promises completes their futures with CancelledError; a waiter still there resumes now.
	Dropped.Reset();
}
