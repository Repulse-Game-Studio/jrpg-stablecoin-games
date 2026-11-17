// No Python twin: the localhost checkout server in the editor (Scfg.Glue.LocalServer.*): the core's
// local_server::CheckoutServer on the engine's sockets (ScfgLocalServer.h), made by the facade's default factory and
// pumped on the game thread, against a real HTTP client (FHttpModule) and raw loopback sockets. Its HTTP rules are the
// core's, tested by core-tests/LocalServerTests.cpp and the shared suite tests/server_security; here: the engine's
// listener and connections, where it binds (127.0.0.1 only, measured in /proc/net/tcp), the plugin's page files, the
// logo, and the Err of a page that cannot start.
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#include "ScfgLocalServer.h"
#include "ScfgString.h"
#include "ScfgTestHttpClient.h"

#include "Scfg/Checkout.h"
#include "Scfg/Json.h"
#include "Scfg/LocalServer.h"
#include "Scfg/Result.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgLocalServerTests
{

using scfg::Json;

constexpr double kPatience = 10.0;
const char* const kTx = "0xabababababababababababababababababababababababababababababababab";

// What the page's callbacks saw, and what was reported.
struct FSeen
{
	std::vector<std::pair<std::string, std::optional<std::string>>> Hints;
	TArray<FString> Errors;
};

FScfgLocalServerPage Page(const TSharedRef<FSeen>& Seen)
{
	FScfgLocalServerPage P;
	P.OnError = [Seen](const FString& Line) { Seen->Errors.Add(Line); };
	return P;
}

scfg::checkout::ServerArgs Args(const TSharedRef<FSeen>& Seen)
{
	scfg::checkout::ServerArgs A;
	A.session_info = Json::dict({{"chain_id", 42161}, {"title", "Chapter 2"}});
	A.on_tx = [Seen](std::string Tx, std::optional<std::string> Sender) { Seen->Hints.emplace_back(Tx, Sender); };
	A.get_status = [] { return Json::dict({{"state", "waiting"}}); };
	return A;
}

// A started page server from the default factory, or null (the test failed).
std::unique_ptr<scfg::checkout::Server> Started(FAutomationTestBase& Test, FScfgLocalServerPage P,
	scfg::checkout::ServerArgs A)
{
	scfg::Result<std::unique_ptr<scfg::checkout::Server>> Made = FScfgLocalServer::Factory(MoveTemp(P))(std::move(A));
	if (!Test.TestTrue(TEXT("the factory makes a server"), Made.ok()))
	{
		return nullptr;
	}
	std::unique_ptr<scfg::checkout::Server> Server = Made.take();
	const scfg::Status Up = Server->start();
	if (!Test.TestTrue(TEXT("it starts"), Up.ok()))
	{
		Test.AddError(ScfgString::FromUtf8(Up.err().str()));
		return nullptr;
	}
	return Server;
}

FString Url(const scfg::checkout::Server& Server)
{
	return ScfgString::FromUtf8(Server.url());
}

std::string Path(const scfg::checkout::Server& Server, const char* Sub = "")
{
	const std::string U = Server.url();  // http://127.0.0.1:<port>/c/<secret>/
	return U.substr(U.find("/c/")) + Sub;
}

std::string Utf8File(const FString& File)
{
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *File, FILEREAD_Silent);
	return std::string(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
}

#if PLATFORM_LINUX
// The listening sockets on Port in /proc/net/<Table> (Linux): their local addresses as the kernel prints them
// ("0100007F" is 127.0.0.1, "00000000" is 0.0.0.0).
TArray<FString> Listeners(const TCHAR* Table, int32 Port)
{
	TArray<FString> Out;
	// /proc files report a size of 0, which the engine's file reader takes at its word: read it with stdio
	std::string Raw;
	if (FILE* File = std::fopen((std::string("/proc/net/") + TCHAR_TO_UTF8(Table)).c_str(), "r"))
	{
		char Buf[4096];
		size_t N = 0;
		while ((N = std::fread(Buf, 1, sizeof Buf, File)) > 0)
		{
			Raw.append(Buf, N);
		}
		std::fclose(File);
	}
	const FString Text = ScfgString::FromUtf8(Raw);
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	const FString PortHex = FString::Printf(TEXT(":%04X"), Port);
	for (const FString& Line : Lines)
	{
		TArray<FString> Fields;
		Line.ParseIntoArrayWS(Fields);
		// sl local_address rem_address st ...: st 0A is LISTEN
		if (Fields.Num() > 3 && Fields[1].EndsWith(PortHex) && Fields[3] == TEXT("0A"))
		{
			Out.Add(Fields[1].LeftChop(PortHex.Len()));
		}
	}
	return Out;
}
#endif

// One FHttpModule request, answered on the game thread.
struct FHttpCall
{
	FString Name;
	bool bDone = false;
	int32 Code = 0;
	std::string Body;
	TMap<FString, FString> Headers;

	static TSharedRef<FHttpCall> Send(const FString& Name, const FString& Verb, const FString& Url,
		const std::string& Body = std::string())
	{
		TSharedRef<FHttpCall> Call = MakeShared<FHttpCall>();
		Call->Name = Name;
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
		Request->SetURL(Url);
		Request->SetVerb(Verb);
		if (!Body.empty())
		{
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetContent(TArray<uint8>(reinterpret_cast<const uint8*>(Body.data()), static_cast<int32>(Body.size())));
		}
		Request->SetTimeout(kPatience);
		Request->OnProcessRequestComplete().BindLambda(
			[Call](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				Call->bDone = true;
				if (bConnected && Response.IsValid())
				{
					Call->Code = Response->GetResponseCode();
					const TArray<uint8>& Bytes = Response->GetContent();
					Call->Body.assign(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()));
					for (const TCHAR* Name : {TEXT("Content-Type"), TEXT("Cache-Control"), TEXT("Referrer-Policy"),
							 TEXT("X-Content-Type-Options"), TEXT("Access-Control-Allow-Origin"), TEXT("Connection")})
					{
						Call->Headers.Add(Name, Response->GetHeader(Name));
					}
				}
			});
		Request->ProcessRequest();
		return Call;
	}
};

FString Header(const FHttpCall& Call, const TCHAR* Name)
{
	const FString* V = Call.Headers.Find(Name);
	return V ? *V : FString();
}

FString Str(const std::string& S)
{
	return ScfgString::FromUtf8(S);
}

}  // namespace ScfgLocalServerTests

#define SCFG_LOCAL_SERVER_TEST(Class, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "Scfg.Glue.LocalServer." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

SCFG_LOCAL_SERVER_TEST(FScfgLocalServerHttpTest, "ServesThePageToAnHttpClient")

// FHttpModule (libcurl, a real HTTP client) against the page: the page, a script, session.json, the status, a tx hint
// (202, on_tx with the normalized hash and sender) and a wrong secret (404), each with the security headers and no
// CORS header. The server stays up about 2 s, so the listener can be seen from outside (ss -tlnp) during the run.
bool FScfgLocalServerHttpTest::RunTest(const FString& Parameters)
{
	using namespace ScfgLocalServerTests;
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	struct FState
	{
		std::unique_ptr<scfg::checkout::Server> Server;
		TArray<TSharedRef<FHttpCall>> Calls;
		double Started = 0;
		int32 Port = 0;
	};
	const TSharedRef<FState> S = MakeShared<FState>();
	S->Server = Started(*this, Page(Seen), Args(Seen));
	if (!S->Server)
	{
		return false;
	}
	S->Started = FPlatformTime::Seconds();
	const FString Base = Url(*S->Server);
	S->Port = ScfgTestHttp::PortOf(Base);
	AddInfo(FString::Printf(TEXT("checkout page listening at %s"), *Base));
	TestTrue(TEXT("a random port, never 8000"), S->Port > 0 && S->Port != 8000);
	const std::string Hint = "{\"tx_hash\": \"" + std::string(kTx) + "\", \"from\": \"0x52908400098527886e0f7030069857d2e4169ee7\"}";
	S->Calls.Add(FHttpCall::Send(TEXT("page"), TEXT("GET"), Base));
	S->Calls.Add(FHttpCall::Send(TEXT("app.js"), TEXT("GET"), Base + TEXT("app.js")));
	S->Calls.Add(FHttpCall::Send(TEXT("session.json"), TEXT("GET"), Base + TEXT("session.json")));
	S->Calls.Add(FHttpCall::Send(TEXT("status"), TEXT("GET"), Base + TEXT("status")));
	S->Calls.Add(FHttpCall::Send(TEXT("tx"), TEXT("POST"), Base + TEXT("tx"), Hint));
	S->Calls.Add(FHttpCall::Send(TEXT("wrong secret"), TEXT("GET"),
		FString::Printf(TEXT("http://127.0.0.1:%d/c/wrong/"), S->Port)));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, S, Seen]()
	{
		S->Server->pump();  // the facade's pump does this every frame (CheckoutController::poll)
		bool bAll = true;
		for (const TSharedRef<FHttpCall>& Call : S->Calls)
		{
			bAll = bAll && Call->bDone;
		}
		const double Elapsed = FPlatformTime::Seconds() - S->Started;
		if ((!bAll && Elapsed < kPatience) || Elapsed < 2.0)
		{
			return false;
		}
		const TMap<FString, int32> Want = {{TEXT("page"), 200}, {TEXT("app.js"), 200}, {TEXT("session.json"), 200},
			{TEXT("status"), 200}, {TEXT("tx"), 202}, {TEXT("wrong secret"), 404}};
		for (const TSharedRef<FHttpCall>& Call : S->Calls)
		{
			TestTrue(Call->Name + TEXT(" answered"), Call->bDone);
			TestEqual(Call->Name + TEXT(" status"), Call->Code, Want.FindRef(Call->Name));
			TestEqual(Call->Name + TEXT(": Cache-Control"), Header(*Call, TEXT("Cache-Control")), TEXT("no-store"));
			TestEqual(Call->Name + TEXT(": Referrer-Policy"), Header(*Call, TEXT("Referrer-Policy")), TEXT("no-referrer"));
			TestEqual(Call->Name + TEXT(": nosniff"), Header(*Call, TEXT("X-Content-Type-Options")), TEXT("nosniff"));
			TestEqual(Call->Name + TEXT(": no CORS"), Header(*Call, TEXT("Access-Control-Allow-Origin")), FString());
			TestEqual(Call->Name + TEXT(": Connection"), Header(*Call, TEXT("Connection")), TEXT("close"));
		}
		const std::string Index = Utf8File(FScfgLocalServer::WebDirectory() / TEXT("index.html"));
		TestTrue(TEXT("the page is the plugin's Resources/Web/index.html"), !Index.empty() && S->Calls[0]->Body == Index);
		TestEqual(TEXT("as HTML"), Header(*S->Calls[0], TEXT("Content-Type")), TEXT("text/html; charset=utf-8"));
		TestEqual(TEXT("app.js as JavaScript"), Header(*S->Calls[1], TEXT("Content-Type")),
			TEXT("application/javascript; charset=utf-8"));
		TestEqual(TEXT("session.json"), Str(S->Calls[2]->Body), TEXT("{\"chain_id\": 42161, \"title\": \"Chapter 2\"}"));
		TestEqual(TEXT("status"), Str(S->Calls[3]->Body), TEXT("{\"state\": \"waiting\"}"));
		TestEqual(TEXT("the hint is taken"), Str(S->Calls[4]->Body), TEXT("{\"ok\": true}"));
		TestTrue(TEXT("on_tx got the normalized hash and sender"), Seen->Hints.size() == 1 &&
			Seen->Hints[0].first == kTx &&
			Seen->Hints[0].second == std::optional<std::string>("0x52908400098527886E0F7030069857D2E4169EE7"));
		TestEqual(TEXT("nothing reported"), Seen->Errors.Num(), 0);
		S->Server->stop();
		TestTrue(TEXT("stopped: the port is closed"), ScfgTestHttp::Refused(S->Port));
		S->Server.reset();
		return true;
	}));
	return true;
}

SCFG_LOCAL_SERVER_TEST(FScfgLocalServerLoopbackTest, "ListensOnLoopbackOnly")

// Where the listener is bound, as the kernel lists it: one IPv4 listener on the port, at 127.0.0.1, none at 0.0.0.0
// and none on IPv6.
bool FScfgLocalServerLoopbackTest::RunTest(const FString& Parameters)
{
	using namespace ScfgLocalServerTests;
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	std::unique_ptr<scfg::checkout::Server> Server = Started(*this, Page(Seen), Args(Seen));
	if (!Server)
	{
		return false;
	}
	const int32 Port = ScfgTestHttp::PortOf(Url(*Server));
	TestTrue(TEXT("the URL names 127.0.0.1 and a port"), Port > 0 && Port != 8000);
#if PLATFORM_LINUX
	const TArray<FString> V4 = Listeners(TEXT("tcp"), Port);
	TestEqual(TEXT("one IPv4 listener on the port, at 127.0.0.1"), FString::Join(V4, TEXT(",")), TEXT("0100007F"));
	TestEqual(TEXT("no IPv6 listener on the port"), Listeners(TEXT("tcp6"), Port).Num(), 0);
	AddInfo(FString::Printf(TEXT("/proc/net/tcp: port %d listens at %s (0100007F is 127.0.0.1)"), Port,
		*FString::Join(V4, TEXT(","))));
#else
	AddInfo(TEXT("the bind address is measured on Linux only (/proc/net/tcp)"));
#endif
	// and it answers there
	const std::optional<ScfgTestHttp::FAnswer> Got =
		ScfgTestHttp::Exchange(Port, ScfgTestHttp::Request("GET", Path(*Server, "status"), Port), [&] { Server->pump(); });
	TestTrue(TEXT("answers on 127.0.0.1"), Got && Got->Status == 200);
	Server->stop();
#if PLATFORM_LINUX
	TestEqual(TEXT("stopped: no listener left on the port"), Listeners(TEXT("tcp"), Port).Num(), 0);
#endif
	return true;
}

SCFG_LOCAL_SERVER_TEST(FScfgLocalServerRefusesTest, "RefusesWithoutTheHostOrTheSecret")

// Raw requests over a loopback socket: a foreign Host is 403, a wrong secret 404, HEAD 501 without a body, and an
// answer is exactly HTTP/1.1 with Connection: close.
bool FScfgLocalServerRefusesTest::RunTest(const FString& Parameters)
{
	using namespace ScfgLocalServerTests;
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	std::unique_ptr<scfg::checkout::Server> Server = Started(*this, Page(Seen), Args(Seen));
	if (!Server)
	{
		return false;
	}
	const int32 Port = ScfgTestHttp::PortOf(Url(*Server));
	const TFunction<void()> Pump = [&] { Server->pump(); };
	const std::string Secret = Path(*Server);
	std::optional<ScfgTestHttp::FAnswer> Got = ScfgTestHttp::Exchange(Port,
		"GET " + Secret + " HTTP/1.1\r\nHost: evil.example\r\n\r\n", Pump);
	TestTrue(TEXT("a foreign Host is 403 (DNS rebinding)"), Got && Got->Status == 403 && Got->Body == "forbidden");
	Got = ScfgTestHttp::Exchange(Port, ScfgTestHttp::Request("GET", "/c/wrong/", Port), Pump);
	TestTrue(TEXT("a wrong secret is 404"), Got && Got->Status == 404);
	Got = ScfgTestHttp::Exchange(Port, ScfgTestHttp::Request("HEAD", Secret, Port), Pump);
	TestTrue(TEXT("HEAD is 501, without a body"), Got && Got->Status == 501 && Got->Body.empty());
	Got = ScfgTestHttp::Exchange(Port, ScfgTestHttp::Request("POST", Secret + "tx", Port, "{\"tx_hash\": \"0x12\"}"), Pump);
	TestTrue(TEXT("a bad hash is 400, and never reaches on_tx"), Got && Got->Status == 400 && Seen->Hints.empty());
	TestTrue(TEXT("HTTP/1.1, Connection: close"), Got && Got->Raw.rfind("HTTP/1.1 400 Bad Request\r\n", 0) == 0 &&
		Got->Header("connection") == "close");
	Server->stop();
	return true;
}

SCFG_LOCAL_SERVER_TEST(FScfgLocalServerStopTest, "StopReleasesThePortAndIsIdempotent")

bool FScfgLocalServerStopTest::RunTest(const FString& Parameters)
{
	using namespace ScfgLocalServerTests;
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	std::unique_ptr<scfg::checkout::Server> Server = Started(*this, Page(Seen), Args(Seen));
	if (!Server)
	{
		return false;
	}
	const int32 Port = ScfgTestHttp::PortOf(Url(*Server));
	TestFalse(TEXT("listening"), ScfgTestHttp::Refused(Port));
	Server->pump();  // accepts (and later drops) the probe
	Server->stop();
	TestTrue(TEXT("stop() closes the port"), ScfgTestHttp::Refused(Port));
	Server->stop();  // twice is fine
	TestTrue(TEXT("a second start() listens again"), Server->start().ok());
	const int32 Again = ScfgTestHttp::PortOf(Url(*Server));
	const std::optional<ScfgTestHttp::FAnswer> Got = ScfgTestHttp::Exchange(Again,
		ScfgTestHttp::Request("GET", Path(*Server, "status"), Again), [&] { Server->pump(); });
	TestTrue(TEXT("and answers"), Got && Got->Status == 200);
	Server.reset();  // the destructor stops it
	TestTrue(TEXT("destroyed: the port is closed"), ScfgTestHttp::Refused(Again));
	TestEqual(TEXT("nothing reported"), Seen->Errors.Num(), 0);
	return true;
}

SCFG_LOCAL_SERVER_TEST(FScfgLocalServerCannotBindTest, "APageThatCannotStartIsReported")

// An address that is not this machine's (TEST-NET-1): start() fails with RuntimeError, reported once through OnError
// (the controller drops the Err and shows browser_unavailable).
bool FScfgLocalServerCannotBindTest::RunTest(const FString& Parameters)
{
	using namespace ScfgLocalServerTests;
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	FScfgLocalServerPage P = Page(Seen);
	P.Host = TEXT("192.0.2.1");
	scfg::Result<std::unique_ptr<scfg::checkout::Server>> Made = FScfgLocalServer::Factory(MoveTemp(P))(Args(Seen));
	if (!TestTrue(TEXT("made"), Made.ok()))
	{
		return false;
	}
	const scfg::Status Up = Made.value()->start();
	TestTrue(TEXT("start() fails"), !Up.ok() && Up.err().kind == "RuntimeError" &&
		Up.err().message.rfind("cannot listen on 192.0.2.1: ", 0) == 0);
	TestEqual(TEXT("reported once"), Seen->Errors.Num(), 1);
	if (Seen->Errors.Num() == 1 && !Up.ok())
	{
		TestEqual(TEXT("as the page's Err"), Seen->Errors[0],
			TEXT("scfg: cannot start the checkout page: ") + ScfgString::FromUtf8(Up.err().str()));
		AddInfo(Seen->Errors[0]);
	}
	Made.value()->pump();  // harmless
	return true;
}

SCFG_LOCAL_SERVER_TEST(FScfgLocalServerPageFilesTest, "ThePageFilesAreThePluginsCopies")

// The plugin's Resources/Web/ holds every file the server serves, byte for byte the shared page
// (plugin/game/scfg/web/, which tools/sync_web_page.py --check also compares); a directory without them fails the
// factory, reported once per attempt.
bool FScfgLocalServerPageFilesTest::RunTest(const FString& Parameters)
{
	using namespace ScfgLocalServerTests;
	const FString Web = FScfgLocalServer::WebDirectory();
	TestTrue(TEXT("Resources/Web/ exists"), !Web.IsEmpty() && IFileManager::Get().DirectoryExists(*Web));
	TArray<FString> Missing;
	const TMap<FString, std::string> Files = FScfgLocalServer::ReadPage(Web, &Missing);
	TestEqual(TEXT("every asset is there"), Files.Num(), static_cast<int32>(scfg::local_server::ASSETS.size()));
	TestEqual(TEXT("none missing"), Missing.Num(), 0);
	// unreal/Plugins/Scfg -> the repository's plugin/game/scfg/web/
	const FString Shared = FPaths::ConvertRelativePathToFull(
		IPluginManager::Get().FindPlugin(TEXT("Scfg"))->GetBaseDir() / TEXT("../../../plugin/game/scfg/web"));
	if (IFileManager::Get().DirectoryExists(*Shared))
	{
		for (const TPair<FString, std::string>& Each : Files)
		{
			TestTrue(Each.Key + TEXT(" is the shared page's copy"), Each.Value == Utf8File(Shared / Each.Key));
		}
	}
	else
	{
		AddInfo(TEXT("plugin/game/scfg/web/ not found next to the plugin: copies not compared"));
	}
	// A directory without the files
	const FString Empty = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("ScfgTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Empty, true);
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	FScfgLocalServerPage P = Page(Seen);
	P.WebDirectory = Empty;
	const scfg::checkout::ServerFactory Factory = FScfgLocalServer::Factory(MoveTemp(P));
	for (int32 Round = 0; Round < 2; ++Round)
	{
		const scfg::Result<std::unique_ptr<scfg::checkout::Server>> Made = Factory(Args(Seen));
		TestTrue(TEXT("a missing page file fails the factory (so the browser method is unavailable)"),
			!Made.ok() && Made.err().kind == "RuntimeError" &&
			Made.err().message.rfind("the checkout page files are missing (", 0) == 0);
	}
	TestEqual(TEXT("reported once per attempt, as the page's Err"), Seen->Errors.Num(), 2);
	TestTrue(TEXT("naming the files"), Seen->Errors.Num() == 2 &&
		Seen->Errors[0].StartsWith(TEXT("scfg: cannot start the checkout page: RuntimeError: the checkout page files are missing")) &&
		Seen->Errors[0].Contains(TEXT("index.html")));
	IFileManager::Get().DeleteDirectory(*Empty, false, true);
	return true;
}

SCFG_LOCAL_SERVER_TEST(FScfgLocalServerLogoTest, "TheLogoIsServedAsARasterImage")

// The configured logo, read when the page server is made: a .png is served as image/png, byte for byte; a logo the
// core refuses as a type (.svg) or that is gone gives no logo (404), as does no logo at all.
bool FScfgLocalServerLogoTest::RunTest(const FString& Parameters)
{
	using namespace ScfgLocalServerTests;
	const FString Dir = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("ScfgTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const std::string Png = std::string("\x89PNG\r\n\x1a\n", 8) + "pixels";
	FFileHelper::SaveArrayToFile(TArray<uint8>(reinterpret_cast<const uint8*>(Png.data()), static_cast<int32>(Png.size())),
		*(Dir / TEXT("logo.png")));
	FFileHelper::SaveStringToFile(TEXT("<svg/>"), *(Dir / TEXT("logo.svg")));
	struct FRow
	{
		FString Logo;
		int32 Status;
	};
	for (const FRow& Row : {FRow{Dir / TEXT("logo.png"), 200}, FRow{Dir / TEXT("logo.svg"), 404},
			 FRow{Dir / TEXT("gone.png"), 404}, FRow{FString(), 404}})
	{
		const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
		FScfgLocalServerPage P = Page(Seen);
		P.LogoPath = MakeShared<FString>(Row.Logo);
		std::unique_ptr<scfg::checkout::Server> Server = Started(*this, MoveTemp(P), Args(Seen));
		if (!Server)
		{
			break;
		}
		const int32 Port = ScfgTestHttp::PortOf(Url(*Server));
		const std::optional<ScfgTestHttp::FAnswer> Got = ScfgTestHttp::Exchange(Port,
			ScfgTestHttp::Request("GET", Path(*Server, "logo"), Port), [&] { Server->pump(); });
		const FString What = Row.Logo.IsEmpty() ? FString(TEXT("no logo")) : FPaths::GetCleanFilename(Row.Logo);
		TestTrue(What + TEXT(": status"), Got && Got->Status == Row.Status);
		if (Got && Row.Status == 200)
		{
			TestTrue(What + TEXT(": the file's bytes, as image/png"), Got->Body == Png && Got->Header("content-type") == "image/png");
		}
	}
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

#endif  // WITH_DEV_AUTOMATION_TESTS
