// No Python twin: FScfgHttpTransport against a real HTTP endpoint, a tiny JSON-RPC node that the test serves itself
// with the engine's HTTPServer on 127.0.0.1 (a random free port; never 8000). Latent tests: FHttpModule answers on a
// later frame, when FHttpManager ticks, and each test waits for it frame by frame (bounded).
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

#include "Scfg/BigInt.h"
#include "Scfg/Co.h"
#include "Scfg/Err.h"
#include "Scfg/Json.h"
#include "Scfg/Result.h"
#include "Scfg/Rpc.h"

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
		Server.StartAllListeners();  // listeners enabled first: GetHttpRouter then binds at once, or says it cannot
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
			// eth_blockNumber's answer, with the request's id
			const Result<Json> Call = scfg::json::loads(Body);
			const Json Id = Call.ok() && Call.value().is_dict() ? Call.value().as_dict().get("id") : Json();
			const Result<std::string> Text =
				scfg::json::dumps(Json::dict({{"jsonrpc", "2.0"}, {"id", Id}, {"result", "0x2a"}}));
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

#undef SCFG_HTTP_TEST

#endif  // WITH_DEV_AUTOMATION_TESTS
