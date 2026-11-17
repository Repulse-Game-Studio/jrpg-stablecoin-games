// No Python twin: FScfgHttpTransport against a real HTTP endpoint, a tiny JSON-RPC node that the test serves itself
// with the engine's HTTPServer on 127.0.0.1 (a random free port; never 8000). Latent tests: FHttpModule answers on a
// later frame, when FHttpManager ticks, and each test waits for it frame by frame (bounded).
//
// THE MAPPING to godot/tests/test_http_transport.gd (its twin; Python's UrllibTransport has none of its own), each test
// to the one here that has its behaviour, or why there is none:
//   test_200_json_is_the_parsed_body                     BlockNumberFromALocalNode
//   test_429_is_a_transport_error_with_its_status        AnswersAreReadByUrllibsRules (a 503: the same rule, "HTTP <status>"
//                                                        with the status; the 429 itself: the CMake CoioTests)
//   test_4xx_with_a_jsonrpc_error_body_is_the_body       AnswersAreReadByUrllibsRules (a 400)
//   test_non_json_body_is_invalid_json_response          AnswersAreReadByUrllibsRules
//   test_an_overlong_json_integer_is_invalid_json        none here: the body goes through the core's coio::parse_rpc_body,
//                                                        tested by CMake's CoioTests.AnIntegerOfMoreThan4300DigitsIsInvalidJson
//                                                        (a payload with one fails at once: WhatCannotBeSentFailsAtOnce)
//   test_a_response_over_the_size_limit_is_a_transport_error none here: coio::MAX_RESPONSE_BYTES in parse_rpc_body,
//                                                        CoioTests.ABodyOverTheCapIsResponseTooLarge
//   test_timeout_is_a_transport_error                    NoAnswerIsATransportError
//   test_connection_refused_is_a_transport_error         NoAnswerIsATransportError
//   test_host_outside_the_tree_is_host_gone              none: Godot's transport needs a node in the scene tree (its
//                                                        HTTPRequest children); FHttpModule has no host
//   test_host_freed_mid_request_is_host_gone_not_a_cooldown none: the same; the transport's end is Shutdown():
//                                                        ShutdownCancelsRequestsInFlight
//   test_host_removed_mid_request_is_host_gone           none: the same
//   test_close_ends_requests_in_flight_with_host_gone    ShutdownCancelsRequestsInFlight (CancelledError at once)
//   test_rpc_client_over_http                            BlockNumberFromALocalNode (an RpcClient over the transport),
//                                                        TheWorkerOverTheRealTransport
//   test_parse_rpc_body_refuses_an_overlong_integer      none here: the core's parse_rpc_body, CoioTests (as above)
//   test_parse_rpc_body_is_linear_on_many_long_digit_runs none: the time of GDScript's _has_overlong_int prefilter;
//                                                        the core has no prefilter (its Json parser counts the digits as it
//                                                        reads, CMake's JsonTests)
// WhatCannotBeSentFailsAtOnce (a dumps Err, a request the engine refuses) is the Unreal transport's own.
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "HttpPath.h"
#include "HttpRequestHandler.h"
#include "HttpRouteHandle.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AutomationTest.h"

#include "ScfgHttpTransport.h"
#include "ScfgString.h"
#include "ScfgWorker.h"

#include "Scfg/BackendsBase.h"
#include "Scfg/BigInt.h"
#include "Scfg/Chains.h"
#include "Scfg/Config.h"
#include "Scfg/Co.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgHttpTests
{

using scfg::Json;
using scfg::Result;

// The longest a test waits for an answer (a loopback answer takes a few frames).
constexpr double kPatience = 10.0;

// One request the node received.
struct FSeen
{
	FString Path;
	FString ContentType;
	FString UserAgent;
	std::string Body;
	bool bOnGameThread = false;
};

FString Header(const FHttpServerRequest& Request, const TCHAR* Name)
{
	const TArray<FString>* Values = Request.Headers.Find(Name);
	return Values && Values->Num() > 0 ? (*Values)[0] : FString();
}

// The JSON-RPC node: routes on one HTTPServer listener bound to 127.0.0.1, shared by the tests of this file and kept
// until the editor exits (HTTPServer keeps its listeners; it binds "localhost" as the loopback address,
// HttpListener.cpp:68-70).
class FLocalNode
{
public:
	static FLocalNode& Get()
	{
		static FLocalNode Node;
		return Node;
	}

	bool IsUp() const { return Router.IsValid(); }
	FString Url(const TCHAR* Path) const { return FString::Printf(TEXT("http://127.0.0.1:%u%s"), Port, Path); }

	TArray<FSeen> Seen;
	// Requests to /hang, never answered until Release().
	TArray<FHttpResultCallback> Hung;

	// Forgets the requests to /hang without answering them: their clients have given up (a timeout, a cancel), and
	// an answer now would only be a failed write (LogHttpConnection's socket_send_failure). The callbacks hold
	// their connections weakly (HttpConnection.cpp:166).
	void Release() { Hung.Reset(); }

private:
	FLocalNode()
	{
		FHttpServerModule& Server = FHttpServerModule::Get();
		// Listeners enabled first: GetHttpRouter then binds at once, or says it cannot. StartAllListeners starts every
		// HTTPServer listener registered in this editor process, not only ours: ScfgDev registers none, but in a
		// project with one (Web Remote Control, say) running these tests opens that listener too. Ours stays open,
		// on 127.0.0.1 only, until the editor exits.
		Server.StartAllListeners();
		for (int32 Attempt = 0; Attempt < 40 && !Router.IsValid(); ++Attempt)
		{
			const uint32 Candidate = static_cast<uint32>(FMath::RandRange(20000, 60999));
			if (Candidate != 8000)
			{
				Router = Server.GetHttpRouter(Candidate, /*bFailOnBindFailure*/ true);
				Port = Candidate;
			}
		}
		if (!Router.IsValid())
		{
			return;
		}
		Bind(TEXT("/rpc"), [](const FHttpServerRequest& Request, const std::string& Body)
		{
			// A node at block 42 that has no logs, answering with the request's id
			const Result<Json> Call = scfg::json::loads(Body);
			const bool bCall = Call.ok() && Call.value().is_dict();
			const Json Id = bCall ? Call.value().as_dict().get("id") : Json();
			const Json Method = bCall ? Call.value().as_dict().get("method") : Json();
			const bool bLogs = Method.is_str() && Method.as_str() == "eth_getLogs";
			const Result<std::string> Text = scfg::json::dumps(
				Json::dict({{"jsonrpc", "2.0"}, {"id", Id}, {"result", bLogs ? Json::list() : Json("0x2a")}}));
			return Reply(EHttpServerResponseCodes::Ok, Text.ok() ? Text.value() : std::string("{}"));
		});
		Bind(TEXT("/unavailable"), [](const FHttpServerRequest&, const std::string&)
		{
			return Reply(EHttpServerResponseCodes::ServiceUnavail, "<html>busy</html>");
		});
		Bind(TEXT("/rpc-error"), [](const FHttpServerRequest&, const std::string&)
		{
			return Reply(EHttpServerResponseCodes::BadRequest,
				R"({"jsonrpc": "2.0", "id": 1, "error": {"code": -32005, "message": "range too large"}})");
		});
		Bind(TEXT("/not-json"), [](const FHttpServerRequest&, const std::string&)
		{
			return Reply(EHttpServerResponseCodes::Ok, "nope");
		});
		Router->BindRoute(FHttpPath(TEXT("/hang")), EHttpServerRequestVerbs::VERB_POST,
			FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				Note(TEXT("/hang"), Request);
				Hung.Add(OnComplete);
				return true;
			}));
	}

	static TUniquePtr<FHttpServerResponse> Reply(EHttpServerResponseCodes Code, const std::string& Body)
	{
		TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Body.data()), static_cast<int32>(Body.size()));
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(MoveTemp(Bytes), TEXT("application/json"));
		Response->Code = Code;
		return Response;
	}

	void Note(const TCHAR* Route, const FHttpServerRequest& Request)
	{
		FSeen S;
		S.Path = Route;  // the route that took it (RelativePath is relative to the route)
		S.ContentType = Header(Request, TEXT("Content-Type"));
		S.UserAgent = Header(Request, TEXT("User-Agent"));
		S.Body = std::string(reinterpret_cast<const char*>(Request.Body.GetData()), static_cast<size_t>(Request.Body.Num()));
		S.bOnGameThread = IsInGameThread();
		Seen.Add(MoveTemp(S));
	}

	using FMake = TFunction<TUniquePtr<FHttpServerResponse>(const FHttpServerRequest&, const std::string&)>;

	void Bind(const TCHAR* Path, FMake Make)
	{
		Router->BindRoute(FHttpPath(Path), EHttpServerRequestVerbs::VERB_POST, FHttpRequestHandler::CreateLambda(
			[this, Path, Make](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				Note(Path, Request);
				OnComplete(Make(Request, Seen.Last().Body));
				return true;
			}));
	}

	TSharedPtr<IHttpRouter> Router;
	uint32 Port = 0;
};

// A named coroutine (no coroutine lambdas, ruling R8): the transport's Future as a Co, for a Job.
scfg::Co<Json> Await(scfg::Future<Json> Answer)
{
	co_return co_await std::move(Answer);
}

// One post_json, driven by a Job, with where and when it was answered.
struct FCall
{
	TUniquePtr<FScfgHttpTransport> Transport = MakeUnique<FScfgHttpTransport>();
	scfg::Job<Json> Job;
	std::optional<Result<Json>> Got;
	bool bOnGameThread = false;
	double Posted = 0;
	double Seconds = 0;
	uint64 Frames = 0;

	void Post(const FString& Url, double Timeout = 10.0)
	{
		Posted = FPlatformTime::Seconds();
		const uint64 Frame = GFrameCounter;
		Json Payload = Json::dict({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "eth_blockNumber"}, {"params", Json::list()}});
		Job.start(Await(Transport->post_json(ScfgString::ToUtf8(Url), std::move(Payload), Timeout)),
			[this, Frame](Result<Json> R)
			{
				bOnGameThread = IsInGameThread() && FPlatformTLS::GetCurrentThreadId() == GGameThreadId;
				Seconds = FPlatformTime::Seconds() - Posted;
				Frames = GFrameCounter - Frame;
				Got.emplace(std::move(R));
			});
	}
	bool Waited() const { return Got.has_value() || FPlatformTime::Seconds() - Posted > kPatience; }
};

void ExpectTransportError(FAutomationTestBase& Test, const FCall& Call, const TCHAR* Message, std::optional<int> Status)
{
	if (!Test.TestTrue(TEXT("answered in time"), Call.Got.has_value()))
	{
		return;
	}
	if (!Test.TestFalse(TEXT("an error"), Call.Got->ok()))
	{
		return;
	}
	const scfg::Err& E = Call.Got->err();
	Test.TestEqual(TEXT("kind"), ScfgString::FromUtf8(E.kind), TEXT("TransportError"));
	Test.TestEqual(TEXT("message"), ScfgString::FromUtf8(E.message), Message);
	Test.TestTrue(TEXT("status"), E.status == Status);
	Test.TestTrue(TEXT("completed on the game thread"), Call.bOnGameThread);
}

}  // namespace ScfgHttpTests

#define SCFG_HTTP_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.Http." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

SCFG_HTTP_TEST(FScfgHttpBlockNumberTest, "BlockNumberFromALocalNode")

// An RpcClient over FScfgHttpTransport asks the local node for eth_blockNumber: the POST carries urllib's headers and
// the JSON-RPC call, the answer comes back on the game thread, a few frames later.
bool FScfgHttpBlockNumberTest::RunTest(const FString& Parameters)
{
	using namespace ScfgHttpTests;
	FLocalNode& Node = FLocalNode::Get();
	if (!TestTrue(TEXT("the local node is listening on 127.0.0.1"), Node.IsUp()))
	{
		return false;
	}
	struct FState
	{
		FScfgHttpTransport Transport;
		std::optional<scfg::rpc::RpcClient> Client;
		scfg::Job<scfg::BigInt> Job;  // declared last: destroyed before the client and the transport
		std::optional<Result<scfg::BigInt>> Got;
		bool bOnGameThread = false;
		double Posted = 0;
		double Seconds = 0;
		uint64 Frames = 0;
		int32 SeenBefore = 0;
	};
	TSharedRef<FState> S = MakeShared<FState>();
	Result<scfg::rpc::RpcClient> Client =
		scfg::rpc::RpcClient::make({ScfgString::ToUtf8(Node.Url(TEXT("/rpc")))}, S->Transport);
	if (!TestTrue(TEXT("client"), Client.ok()))
	{
		return false;
	}
	S->Client.emplace(Client.take());
	S->SeenBefore = Node.Seen.Num();
	S->Posted = FPlatformTime::Seconds();
	const uint64 Frame = GFrameCounter;
	FState* Raw = &S.Get();
	S->Job.start(S->Client->block_number(), [Raw, Frame](Result<scfg::BigInt> R)
	{
		Raw->bOnGameThread = IsInGameThread() && FPlatformTLS::GetCurrentThreadId() == GGameThreadId;
		Raw->Seconds = FPlatformTime::Seconds() - Raw->Posted;
		Raw->Frames = GFrameCounter - Frame;
		Raw->Got.emplace(std::move(R));
	});
	TestEqual(TEXT("one request in flight"), S->Transport.InFlight(), 1);
	TestFalse(TEXT("not answered inside the call"), S->Got.has_value());
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, S, &Node]()
	{
		if (!S->Got && FPlatformTime::Seconds() - S->Posted < kPatience)
		{
			return false;
		}
		if (TestTrue(TEXT("answered"), S->Got.has_value()))
		{
			TestTrue(TEXT("eth_blockNumber"), S->Got->ok() && S->Got->value() == scfg::BigInt(42));
			if (!S->Got->ok())
			{
				AddError(ScfgString::FromUtf8(S->Got->err().str()));
			}
		}
		TestTrue(TEXT("completed on the game thread"), S->bOnGameThread);
		TestEqual(TEXT("posts"), S->Transport.Posts(), 1);
		TestEqual(TEXT("nothing left in flight"), S->Transport.InFlight(), 0);
		if (TestEqual(TEXT("the node saw one request"), Node.Seen.Num(), S->SeenBefore + 1))
		{
			const FSeen& Seen = Node.Seen.Last();
			TestEqual(TEXT("path"), Seen.Path, TEXT("/rpc"));
			TestEqual(TEXT("Content-Type"), Seen.ContentType, TEXT("application/json"));
			TestEqual(TEXT("User-Agent"), Seen.UserAgent, TEXT("scfg/0.1"));
			TestTrue(TEXT("the server ran on the game thread"), Seen.bOnGameThread);
			const Result<Json> Call = scfg::json::loads(Seen.Body);
			TestTrue(TEXT("the body is the JSON-RPC call"), Call.ok() && Call.value().is_dict() &&
				Call.value().as_dict().get("method").is_str() &&
				Call.value().as_dict().get("method").as_str() == "eth_blockNumber" &&
				Call.value().as_dict().get("jsonrpc").as_str() == "2.0");
		}
		AddInfo(FString::Printf(TEXT("eth_blockNumber over loopback: %.1f ms, %llu frame(s), on the game thread: %s"),
			S->Seconds * 1000.0, static_cast<unsigned long long>(S->Frames), S->bOnGameThread ? TEXT("yes") : TEXT("no")));
		return true;
	}));
	return true;
}

SCFG_HTTP_TEST(FScfgHttpStatusesTest, "AnswersAreReadByUrllibsRules")

// parse_rpc_body over real answers: a non-2xx status is TransportError("HTTP <status>"), unless its body is a JSON
// object with an "error" (passed through); a 2xx body that is not JSON is "invalid JSON response".
bool FScfgHttpStatusesTest::RunTest(const FString& Parameters)
{
	using namespace ScfgHttpTests;
	FLocalNode& Node = FLocalNode::Get();
	if (!TestTrue(TEXT("the local node is listening on 127.0.0.1"), Node.IsUp()))
	{
		return false;
	}
	TSharedRef<FCall> Unavailable = MakeShared<FCall>();
	TSharedRef<FCall> RpcError = MakeShared<FCall>();
	TSharedRef<FCall> NotJson = MakeShared<FCall>();
	Unavailable->Post(Node.Url(TEXT("/unavailable")));
	RpcError->Post(Node.Url(TEXT("/rpc-error")));
	NotJson->Post(Node.Url(TEXT("/not-json")));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Unavailable, RpcError, NotJson]()
	{
		if (!(Unavailable->Waited() && RpcError->Waited() && NotJson->Waited()))
		{
			return false;
		}
		ExpectTransportError(*this, *Unavailable, TEXT("HTTP 503"), 503);
		ExpectTransportError(*this, *NotJson, TEXT("invalid JSON response"), std::nullopt);
		if (TestTrue(TEXT("the RPC error answered"), RpcError->Got.has_value()))
		{
			TestTrue(TEXT("a 400 with a JSON-RPC error is the body"), RpcError->Got->ok() && RpcError->Got->value().is_dict() &&
				RpcError->Got->value().as_dict().get("error").is_dict());
		}
		return true;
	}));
	return true;
}

SCFG_HTTP_TEST(FScfgHttpNoAnswerTest, "NoAnswerIsATransportError")

// A request that gets no HTTP answer is a TransportError with no status: a refused connection (127.0.0.1:1, where
// nothing listens), and a node that does not answer within the timeout.
bool FScfgHttpNoAnswerTest::RunTest(const FString& Parameters)
{
	using namespace ScfgHttpTests;
	FLocalNode& Node = FLocalNode::Get();
	if (!TestTrue(TEXT("the local node is listening on 127.0.0.1"), Node.IsUp()))
	{
		return false;
	}
	// the engine logs both failures (LogHttp warnings): the refused request
	AddExpectedMessagePlain(TEXT("completed with reason 'ConnectionError'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	// and libcurl's own account of it ("request failed, libcurl error: 7", its info messages), and the timeout
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessagePlain(TEXT("HTTP request timed out after"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	TSharedRef<FCall> Refused = MakeShared<FCall>();
	TSharedRef<FCall> Slow = MakeShared<FCall>();
	Refused->Post(TEXT("http://127.0.0.1:1/"));
	Slow->Post(Node.Url(TEXT("/hang")), /*Timeout*/ 1.0);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Refused, Slow, &Node]()
	{
		if (!(Refused->Waited() && Slow->Waited()))
		{
			return false;
		}
		ExpectTransportError(*this, *Refused, TEXT("<urlopen error connection failed>"), std::nullopt);
		ExpectTransportError(*this, *Slow, TEXT("timed out"), std::nullopt);
		TestTrue(TEXT("the timeout took about a second"), Slow->Seconds >= 0.9 && Slow->Seconds < 5.0);
		AddInfo(FString::Printf(TEXT("refused after %.1f ms; timed out after %.2f s"), Refused->Seconds * 1000.0,
			Slow->Seconds));
		Node.Release();
		return true;
	}));
	return true;
}

SCFG_HTTP_TEST(FScfgHttpShutdownTest, "ShutdownCancelsRequestsInFlight")

// Shutdown() drops a request in flight at once (CancelledError, inside the call), unbinds it so the engine's late
// completion never reaches the transport, refuses later posts, and the transport can then be destroyed.
bool FScfgHttpShutdownTest::RunTest(const FString& Parameters)
{
	using namespace ScfgHttpTests;
	FLocalNode& Node = FLocalNode::Get();
	if (!TestTrue(TEXT("the local node is listening on 127.0.0.1"), Node.IsUp()))
	{
		return false;
	}
	TSharedRef<FCall> Call = MakeShared<FCall>();
	const int32 SeenBefore = Node.Seen.Num();
	Call->Post(Node.Url(TEXT("/hang")));
	struct FWait
	{
		double Since = FPlatformTime::Seconds();
		bool bShutDown = false;
		int32 FramesAfter = 0;
	};
	TSharedRef<FWait> Wait = MakeShared<FWait>();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Call, Wait, &Node, SeenBefore]()
	{
		if (!Wait->bShutDown)
		{
			// wait until the node holds the request, so it really is in flight
			if (Node.Seen.Num() == SeenBefore && FPlatformTime::Seconds() - Wait->Since < kPatience)
			{
				return false;
			}
			TestEqual(TEXT("the node holds the request"), Node.Seen.Num(), SeenBefore + 1);
			TestFalse(TEXT("not answered yet"), Call->Got.has_value());
			TestEqual(TEXT("in flight"), Call->Transport->InFlight(), 1);
			Call->Transport->Shutdown();
			Wait->bShutDown = true;
			TestTrue(TEXT("dropped inside Shutdown()"), Call->Got.has_value() && !Call->Got->ok() &&
				Call->Got->err().kind == "CancelledError");
			TestEqual(TEXT("nothing in flight"), Call->Transport->InFlight(), 0);
			scfg::Future<Json> Late =
				Call->Transport->post_json(ScfgString::ToUtf8(Node.Url(TEXT("/rpc"))), Json::dict(), 1.0);
			TestTrue(TEXT("a later post is refused at once"), Late.done());
			Call->Transport.Reset();  // the engine's cancelled request may still complete later: it is unbound
			Node.Release();
			return false;
		}
		return ++Wait->FramesAfter >= 10;  // some frames for a late completion to (not) arrive
	}));
	return true;
}

SCFG_HTTP_TEST(FScfgHttpRefusedAtOnceTest, "WhatCannotBeSentFailsAtOnce")

// Two requests that never leave: a payload json::dumps refuses (an int of more than 4300 digits, Python's
// ValueError) completes the Future inside post_json with that Err, and so does a request the engine refuses in
// ProcessRequest() (an empty URL: HttpRequestCommon.cpp:97-101, finished at once on the game thread, :224-231),
// as a TransportError.
bool FScfgHttpRefusedAtOnceTest::RunTest(const FString& Parameters)
{
	using namespace ScfgHttpTests;
	FLocalNode& Node = FLocalNode::Get();
	if (!TestTrue(TEXT("the local node is listening on 127.0.0.1"), Node.IsUp()))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("ProcessRequest failed. No URL was specified."), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("completed with reason 'Other'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	const int32 SeenBefore = Node.Seen.Num();
	FScfgHttpTransport Transport;

	Json Huge = Json::dict({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "eth_call"},
		{"params", Json::list({Json(scfg::BigInt(10).pow(4400))})}});
	scfg::Future<Json> Dumped = Transport.post_json(ScfgString::ToUtf8(Node.Url(TEXT("/rpc"))), std::move(Huge), 5.0);
	TestTrue(TEXT("a dumps Err completes at once"), Dumped.done());
	scfg::Job<Json> DumpedJob;
	std::optional<Result<Json>> DumpedGot;
	DumpedJob.start(Await(std::move(Dumped)), [&DumpedGot](Result<Json> R) { DumpedGot.emplace(std::move(R)); });
	TestTrue(TEXT("with dumps' ValueError"), DumpedGot && !DumpedGot->ok() && DumpedGot->err().kind == "ValueError");

	scfg::Future<Json> NoUrl = Transport.post_json("", Json::dict({{"method", "eth_blockNumber"}}), 5.0);
	TestTrue(TEXT("a request ProcessRequest() refuses completes inside post_json"), NoUrl.done());
	scfg::Job<Json> NoUrlJob;
	std::optional<Result<Json>> NoUrlGot;
	NoUrlJob.start(Await(std::move(NoUrl)), [&NoUrlGot](Result<Json> R) { NoUrlGot.emplace(std::move(R)); });
	if (TestTrue(TEXT("as a TransportError"), NoUrlGot && !NoUrlGot->ok() && NoUrlGot->err().kind == "TransportError"))
	{
		TestEqual(TEXT("its message"), ScfgString::FromUtf8(NoUrlGot->err().message),
			TEXT("<urlopen error cannot start the request to ''>"));
		TestFalse(TEXT("with no status"), NoUrlGot->err().status.has_value());
	}
	TestEqual(TEXT("both counted"), Transport.Posts(), 2);
	TestEqual(TEXT("nothing in flight"), Transport.InFlight(), 0);
	TestEqual(TEXT("the node saw nothing"), Node.Seen.Num(), SeenBefore);
	return true;
}

SCFG_HTTP_TEST(FScfgHttpWorkerTest, "TheWorkerOverTheRealTransport")

// The worker over FScfgHttpTransport and the core ticker, against the local node only (an RpcFactory whose client
// has no other url): start_session asks the node for the head, and the callback comes from the worker's ticker.
bool FScfgHttpWorkerTest::RunTest(const FString& Parameters)
{
	using namespace ScfgHttpTests;
	FLocalNode& Node = FLocalNode::Get();
	if (!TestTrue(TEXT("the local node is listening on 127.0.0.1"), Node.IsUp()))
	{
		return false;
	}
	struct FState
	{
		TUniquePtr<FScfgWorker> Worker;
		std::optional<Result<Json>> Session;
		TArray<FString> Errors;
		bool bOnGameThread = false;
		double Started = FPlatformTime::Seconds();
		int32 SeenBefore = 0;
	};
	TSharedRef<FState> S = MakeShared<FState>();
	S->SeenBefore = Node.Seen.Num();
	TUniquePtr<FScfgHttpTransport> Transport = MakeUnique<FScfgHttpTransport>();
	FScfgHttpTransport* const Http = Transport.Get();  // owned by the worker, which outlives its Engine's clients
	const std::string Url = ScfgString::ToUtf8(Node.Url(TEXT("/rpc")));
	FScfgWorker::FOptions Options;
	Options.RpcFactory = [Http, Url](const scfg::chains::Chain&) -> Result<scfg::backends::base::Rpc>
	{
		SCFG_TRY(scfg::rpc::RpcClient Client, scfg::rpc::RpcClient::make({Url}, *Http));
		return scfg::backends::base::Rpc(std::make_shared<scfg::rpc::RpcClient>(std::move(Client)));
	};
	FState* Raw = &S.Get();
	Options.OnError = [Raw](const scfg::Err& E) { Raw->Errors.Add(ScfgString::FromUtf8(E.str())); };
	Result<scfg::config::Config> Config = scfg::config::build_config(Json::dict(
		{{"wallet", "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed"}, {"chains", Json::list({"base"})}, {"testnet", true}}));
	if (!TestTrue(TEXT("config"), Config.ok()))
	{
		return false;
	}
	scfg::config::Config Built = Config.take();
	Result<scfg::models::Item> Item = scfg::config::add_item(
		Built, "chapter2", Json::dict({{"code", 37}, {"price", "4.99"}, {"token", "USDC"}, {"title", "Chapter 2"}}));
	if (!TestTrue(TEXT("item"), Item.ok()))
	{
		return false;
	}
	Result<TUniquePtr<FScfgWorker>> Made =
		FScfgWorker::Create(std::move(Built), MoveTemp(Transport), Json(), Json(), MoveTemp(Options));
	if (!TestTrue(TEXT("worker"), Made.ok()))
	{
		return false;
	}
	S->Worker = Made.take();
	S->Worker->StartSession("unlock", "base", Json::dict({{"item", "chapter2"}}), [Raw](Result<Json> R)
	{
		Raw->bOnGameThread = IsInGameThread();
		Raw->Session.emplace(std::move(R));
	});
	S->Worker->Start();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, S, &Node]()
	{
		// wait for the session, then for the tick it asked for, so no request is in flight when the worker goes
		// (a node answering a cancelled request logs LogHttpConnection's socket_send_failure)
		const bool bIdle = S->Session && !S->Worker->IsBusy() && !S->Worker->IsTickPending() &&
			S->Worker->NumQueued() == 0;
		if (!bIdle && FPlatformTime::Seconds() - S->Started < kPatience)
		{
			return false;
		}
		if (TestTrue(TEXT("start_session answered"), S->Session.has_value()))
		{
			if (TestTrue(TEXT("a session"), S->Session->ok()))
			{
				const Json& StartBlock = S->Session->value().as_dict().get("start_block");
				TestTrue(TEXT("its start block is below the node's head (42)"),
					StartBlock.is_int64() && StartBlock.as_int64() > 0 && StartBlock.as_int64() <= 42);
			}
			else
			{
				AddError(ScfgString::FromUtf8(S->Session->err().str()));
			}
		}
		TestTrue(TEXT("the callback ran on the game thread"), S->bOnGameThread);
		TestTrue(TEXT("the node was asked"), Node.Seen.Num() > S->SeenBefore);
		TestEqual(TEXT("no error"), FString::Join(S->Errors, TEXT(" | ")), FString());
		S->Worker.Reset();  // Jobs, Engine, then the transport's Shutdown, outside the ticker
		return true;
	}));
	return true;
}

#undef SCFG_HTTP_TEST

#endif  // WITH_DEV_AUTOMATION_TESTS
