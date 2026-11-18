// Copyright 2026 Repulse
// Twin of scfg_core/rpc.py's UrllibTransport (and of godot/addons/scfg/core/http_transport.gd): the core's JSON-RPC
// POSTs through Unreal's FHttpModule, on the game thread.
//
// post_json makes one request: POST, Content-Type: application/json, User-Agent: scfg/0.1 (urllib's header here),
// the body json::dumps(payload) (its Err, an int of more than 4300 digits, completes the Future at once), and
// SetTimeout(timeout) (see the timeout divergences below). The request is set to complete on the game thread
// (SetDelegateThreadPolicy(CompleteOnGameThread), 5.8's default, set explicitly so that a changed default cannot move
// it, Shipping included), when FHttpManager ticks, so the waiting coroutine resumes on the game thread inside
// OnProcessRequestComplete. Every entry point is game-thread only. The Future is completed with:
// - an HTTP answer (any status): coio::parse_rpc_body(status, body bytes), rpc.UrllibTransport's rules;
// - no answer: TransportError with no status and a message as close to urllib's as the engine tells
//   ("timed out", "<urlopen error connection failed>", ...; FailureMessage in the .cpp);
// - a request the engine will not start (an empty URL): TransportError("<urlopen error cannot start the request to
//   '<url>'>"), already complete when post_json returns;
// - an https:// request when TLS peer verification is unavailable (TlsVerificationProblem() is not empty):
//   TransportError("TLS certificate verification unavailable"), complete at once, nothing sent (ruling R5-4: a payment
//   plugin fails closed). The reason is logged once per transport ("LogScfg: Error: scfg: refusing https:// RPC
//   requests: TLS certificate verification unavailable: <why>").
//
// Shutdown() (also run by the destructor) unbinds each request's delegates first, so a late completion can never
// reach this object, cancels the request, then drops its promise (CancelledError).
//
// Port limits (unreal/README.md, "Known divergences"):
// - Timeouts. urllib's timeout bounds each socket operation; SetTimeout bounds the whole request, so an answer that
//   trickles in for longer than timeout succeeds in Python and is "timed out" here. A timeout <= 0 sets no total
//   timeout here (the engine's default, none), where Python fails at once (no caller passes one: the core's is 10 s).
//   The engine's own [HTTP] HttpConnectionTimeout and HttpActivityTimeout (30 s, BaseEngine.ini:40-41) also end a
//   request whose connect, or whose silence, lasts longer, whatever the core's timeout.
// - The engine reads the whole body before we see it (curl caps it at int32, CurlHttp.cpp:626), and only then is a
//   body over coio::MAX_RESPONSE_BYTES refused, as urllib reads without a cap. Redirects are followed as libcurl's
//   defaults in the engine follow them.
#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Templates/Function.h"

#include "ScfgTransport.h"

#include "Scfg/Co.h"
#include "Scfg/Json.h"

#include <string>

class SCFG_API FScfgHttpTransport final : public FScfgTransport
{
public:
	// urllib's User-Agent in rpc.UrllibTransport.
	static constexpr const TCHAR* UserAgent = TEXT("scfg/0.1");

	FScfgHttpTransport() = default;
	FScfgHttpTransport(const FScfgHttpTransport&) = delete;
	FScfgHttpTransport& operator=(const FScfgHttpTransport&) = delete;
	virtual ~FScfgHttpTransport() override;

	virtual scfg::Future<scfg::Json> post_json(std::string Url, scfg::Json Payload, double Timeout) override;
	virtual void Shutdown() override;

	// Requests posted so far (the tests count them), those handed to the engine (ProcessRequest), and those still
	// waiting for an answer.
	int32 Posts() const { return NumPosts; }
	int32 Started() const { return NumStarted; }
	int32 InFlight() const { return Waiting.Num(); }

	// The TransportError message of a refused https:// request.
	static constexpr const char* TlsUnavailable = "TLS certificate verification unavailable";
	// Why this process cannot verify the TLS peer of an https:// request, or "" when it can. On the engine's libcurl
	// platforms (Linux, Windows, Android) libcurl verifies the peer only when the SSL module found root certificates,
	// unless [/Script/Engine.NetworkSettings] n.VerifyPeer says otherwise (CurlHttpManager.cpp:277-289; the flag is
	// private to the HTTP module and read once, when it starts, before this module), so this asks the same two
	// sources: n.VerifyPeer=False, or no root certificate, is a problem. Apple platforms use NSURLSession, which
	// verifies with the system's trust store: "" there.
	static FString TlsVerificationProblem();
#if !UE_BUILD_SHIPPING
	// Tests only: replaces TlsVerificationProblem()'s answer while set (an empty function restores the real one).
	static void SetTlsProbeForTests(TFunction<FString()> Probe);
#endif
#if !UE_BUILD_SHIPPING
	// Requests every FScfgHttpTransport of this process posted (post_json calls), outside Shipping: the showcase tour
	// asserts that it made none (the e2e driver's http_requests, tools/run_unreal_e2e.py --showcase).
	static int32 ProcessPosts();
#endif

private:
	struct FWaiting
	{
		FHttpRequestPtr Request;
		scfg::Promise<scfg::Json> Promise;
	};

	void OnComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected);
	// The request's entry, taken out of Waiting (null when it is not there).
	TUniquePtr<FWaiting> Take(const IHttpRequest* Request);

	TMap<const IHttpRequest*, TUniquePtr<FWaiting>> Waiting;
	// The request inside ProcessRequest(), whose refusal the engine reports through OnComplete at once.
	const IHttpRequest* Starting = nullptr;
	int32 NumPosts = 0;
	int32 NumStarted = 0;
	bool bShutDown = false;
	bool bLoggedTls = false;
};
