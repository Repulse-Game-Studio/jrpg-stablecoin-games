// Copyright 2026 Repulse
// No Python twin: see ScfgLocalServer.h.
#include "ScfgLocalServer.h"

#include "ScfgString.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "IPAddress.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "Scfg/Config.h"
#include "Scfg/Result.h"

#include <memory>
#include <utility>

namespace ScfgLocalServerDetail
{

using scfg::local_server::Listener;
using scfg::local_server::Socket;

ISocketSubsystem* Subsystem()
{
	return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
}

// Closes and destroys a socket; safe from any destructor, even after the socket subsystem is gone (then the FSocket is
// only closed: the process is exiting).
void Destroy(FSocket* Sock)
{
	Sock->Close();
	if (ISocketSubsystem* Sub = Subsystem())
	{
		Sub->DestroySocket(Sock);
	}
}

// One accepted connection (non-blocking). Destroying it closes and destroys the FSocket.
class FConnection final : public Socket
{
public:
	explicit FConnection(FSocket* InSocket) : Sock(InSocket) {}
	virtual ~FConnection() override { Destroy(Sock); }
	FConnection(const FConnection&) = delete;
	FConnection& operator=(const FConnection&) = delete;

	virtual Read read(size_t Max, std::string& Out) override
	{
		const size_t Old = Out.size();
		Out.resize(Old + Max);
		int32 Got = 0;
		// FSocketBSD::Recv: true with 0 bytes when nothing is there (EWOULDBLOCK), false when the peer closed its side
		// (0 bytes) or the connection failed (SocketsBSD.cpp:321-345)
		const bool bOk = Sock->Recv(reinterpret_cast<uint8*>(Out.data() + Old), static_cast<int32>(Max), Got);
		Out.resize(Old + (bOk && Got > 0 ? static_cast<size_t>(Got) : 0));
		if (!bOk)
		{
			return Read::closed;
		}
		return Got > 0 ? Read::data : Read::nothing;
	}

	virtual std::optional<size_t> write(std::string_view Data) override
	{
		int32 Sent = 0;
		const int32 Count = static_cast<int32>(FMath::Min<size_t>(Data.size(), MAX_int32));
		if (Sock->Send(reinterpret_cast<const uint8*>(Data.data()), Count, Sent))
		{
			return static_cast<size_t>(FMath::Max(Sent, 0));
		}
		// FSocketBSD::Send is false for any -1 of send(2): the socket is full (EWOULDBLOCK), or the connection failed
		ISocketSubsystem* Sub = Subsystem();
		const ESocketErrors Error = Sub ? Sub->GetLastErrorCode() : SE_NO_ERROR;
		if (Error == SE_EWOULDBLOCK || Error == SE_EINTR)
		{
			return size_t{0};
		}
		return std::nullopt;
	}

private:
	FSocket* Sock;
};

// The listening socket (non-blocking).
class FListener final : public Listener
{
public:
	FListener(FSocket* InSocket, int32 InPort) : Sock(InSocket), Port(InPort) {}
	virtual ~FListener() override { Destroy(Sock); }
	FListener(const FListener&) = delete;
	FListener& operator=(const FListener&) = delete;

	virtual int port() const override { return Port; }

	virtual std::unique_ptr<Socket> accept() override
	{
		// A non-blocking listener's accept(2) returns at once: null when no connection is waiting
		FSocket* Accepted = Sock->Accept(TEXT("scfg checkout page connection"));
		if (!Accepted)
		{
			return nullptr;
		}
		if (!Accepted->SetNonBlocking(true))
		{
			Destroy(Accepted);
			return nullptr;
		}
		return std::make_unique<FConnection>(Accepted);
	}

private:
	FSocket* Sock;
	int32 Port;
};

scfg::Err CannotListen(const std::string& Host, const FString& Why)
{
	return scfg::Err::make("RuntimeError", "cannot listen on " + Host + ": " + ScfgString::ToUtf8(Why));
}

FString LastSocketError()
{
	ISocketSubsystem* Sub = Subsystem();
	return Sub ? FString(Sub->GetSocketError(Sub->GetLastErrorCode())) : FString(TEXT("no socket subsystem"));
}

}  // namespace ScfgLocalServerDetail

FString FScfgLocalServer::WebDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scfg"));
	return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources") / TEXT("Web"))
							: FString();
}

scfg::local_server::Listen FScfgLocalServer::Listen()
{
	return [](const std::string& Host) -> scfg::Result<std::unique_ptr<scfg::local_server::Listener>>
	{
		using namespace ScfgLocalServerDetail;
		ISocketSubsystem* Sub = Subsystem();
		if (!Sub)
		{
			return CannotListen(Host, TEXT("no socket subsystem"));
		}
		bool bValid = false;
		const TSharedRef<FInternetAddr> Address = Sub->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Address->SetIp(*ScfgString::FromUtf8(Host), bValid);
		if (!bValid)
		{
			return CannotListen(Host, TEXT("not an IPv4 address"));
		}
		Address->SetPort(0);  // a random free port
		FSocket* Sock = Sub->CreateSocket(NAME_Stream, TEXT("scfg checkout page"), FNetworkProtocolTypes::IPv4);
		if (!Sock)
		{
			return CannotListen(Host, LastSocketError());
		}
		if (!Sock->SetNonBlocking(true) || !Sock->Bind(*Address) || !Sock->Listen(scfg::local_server::LISTEN_BACKLOG))
		{
			const FString Why = LastSocketError();
			Sock->Close();
			Sub->DestroySocket(Sock);
			return CannotListen(Host, Why);
		}
		const int32 Port = Sock->GetPortNo();
		return std::unique_ptr<scfg::local_server::Listener>(std::make_unique<FListener>(Sock, Port));
	};
}

TMap<FString, std::string> FScfgLocalServer::ReadPage(const FString& Directory, TArray<FString>* Missing)
{
	TMap<FString, std::string> Files;
	for (const std::string_view Name : scfg::local_server::ASSETS)
	{
		const FString File = ScfgString::FromUtf8(std::string(Name));
		const FString Path = Directory / File;
		TArray<uint8> Bytes;
		if (!Directory.IsEmpty() && IFileManager::Get().FileExists(*Path) &&
			FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
		{
			Files.Add(File, std::string(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num())));
		}
		else if (Missing)
		{
			Missing->Add(Path);
		}
	}
	return Files;
}

scfg::checkout::ServerFactory FScfgLocalServer::Factory(FScfgLocalServerPage Page)
{
	const TSharedRef<FScfgLocalServerPage> Shared = MakeShared<FScfgLocalServerPage>(MoveTemp(Page));
	return [Shared](scfg::checkout::ServerArgs Args) -> scfg::Result<std::unique_ptr<scfg::checkout::Server>>
	{
		const FScfgLocalServerPage& P = *Shared;
		// The page's files, read now: the server answers from memory. A missing one fails the factory (reported, as
		// the factory reports its own Err), so the controller shows browser_unavailable and the player pays by QR or
		// by hand instead of landing on an error page.
		TArray<FString> Missing;
		const FString Directory = P.WebDirectory.IsEmpty() ? FScfgLocalServer::WebDirectory() : P.WebDirectory;
		TSharedRef<const TMap<FString, std::string>> Files =
			MakeShared<const TMap<FString, std::string>>(ReadPage(Directory, &Missing));
		if (Missing.Num() > 0)
		{
			const scfg::Err Error = scfg::Err::make("RuntimeError", ScfgString::ToUtf8(FString::Printf(
				TEXT("the checkout page files are missing (%s): the plugin's Resources/Web/ folder must ship with the "
					 "game (Scfg.Build.cs stages it)"), *FString::Join(Missing, TEXT(", ")))));
			if (P.OnError)
			{
				P.OnError(TEXT("scfg: cannot start the checkout page: ") + ScfgString::FromUtf8(Error.str()));
			}
			return Error;
		}
		scfg::local_server::Options O;
		O.load_asset = [Files](const std::string& Name) -> std::optional<std::string>
		{
			const std::string* Bytes = Files->Find(ScfgString::FromUtf8(Name));
			return Bytes ? std::optional<std::string>(*Bytes) : std::nullopt;
		};
		// The logo, read once, here (Godot's _server_opts): no route without one
		const FString Logo = P.LogoPath.IsValid() ? *P.LogoPath : FString();
		if (!Logo.IsEmpty())
		{
			TArray<uint8> Bytes;
			const scfg::Result<std::string> Type = scfg::config::logo_content_type(ScfgString::ToUtf8(Logo));
			if (Type.ok() && IFileManager::Get().FileExists(*Logo) && FFileHelper::LoadFileToArray(Bytes, *Logo, FILEREAD_Silent))
			{
				scfg::local_server::Logo Read{
					std::string(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num())),
					Type.value()};
				O.load_logo = [Read = std::move(Read)]() -> std::optional<scfg::local_server::Logo> { return Read; };
			}
		}
		if (!P.Host.IsEmpty())
		{
			O.host = ScfgString::ToUtf8(P.Host);
		}
		if (P.Clock)
		{
			O.clock = [Clock = P.Clock]() { return Clock(); };
		}
		else
		{
			O.clock = []() { return FPlatformTime::Seconds(); };
		}
		O.listen = FScfgLocalServer::Listen();
		if (P.OnError)
		{
			O.on_error = [OnError = P.OnError](const scfg::Err& Error)
			{
				OnError(TEXT("scfg: cannot start the checkout page: ") + ScfgString::FromUtf8(Error.str()));
			};
		}
		return scfg::local_server::factory(std::move(O))(std::move(Args));
	};
}
