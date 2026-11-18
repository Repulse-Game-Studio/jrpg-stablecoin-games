// Copyright 2026 Repulse
// No Python twin: see ScfgTestHttpClient.h.
#include "ScfgTestHttpClient.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "Scfg/Py.h"

namespace ScfgTestHttp
{

namespace ScfgTestHttpDetail
{

struct FClient
{
	ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Sock = nullptr;

	~FClient()
	{
		if (Sock)
		{
			Sock->Close();
			Sub->DestroySocket(Sock);
		}
	}

	bool Connect(int32 Port)
	{
		if (!Sub || Port <= 0)
		{
			return false;
		}
		Sock = Sub->CreateSocket(NAME_Stream, TEXT("scfg test client"), FNetworkProtocolTypes::IPv4);
		if (!Sock)
		{
			return false;
		}
		const TSharedRef<FInternetAddr> Address = Sub->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		bool bValid = false;
		Address->SetIp(TEXT("127.0.0.1"), bValid);
		Address->SetPort(Port);
		// A blocking connect to a loopback listener completes at once (the kernel takes it into the backlog)
		return bValid && Sock->Connect(*Address) && Sock->GetConnectionState() == SCS_Connected;
	}
};

std::optional<FAnswer> Parse(const std::string& Raw)
{
	const size_t End = Raw.find("\r\n\r\n");
	if (End == std::string::npos)
	{
		return std::nullopt;
	}
	FAnswer A;
	A.Raw = Raw;
	size_t At = 0;
	bool bFirst = true;
	while (At < End)
	{
		size_t Eol = Raw.find("\r\n", At);
		if (Eol == std::string::npos || Eol > End)
		{
			Eol = End;
		}
		const std::string Line = Raw.substr(At, Eol - At);
		At = Eol + 2;
		if (bFirst)
		{
			bFirst = false;
			const size_t Sp = Line.find(' ');
			if (Sp == std::string::npos)
			{
				return std::nullopt;
			}
			const scfg::Result<scfg::BigInt> Code = scfg::py::py_int(Line.substr(Sp + 1, 3));
			if (!Code.ok())
			{
				return std::nullopt;
			}
			A.Status = static_cast<int32>(Code.value().to_int64().value());
			continue;
		}
		const size_t Colon = Line.find(':');
		if (Colon != std::string::npos)
		{
			A.Headers.emplace(scfg::py::lower_ascii(Line.substr(0, Colon)), scfg::py::strip(Line.substr(Colon + 1)));
		}
	}
	A.Body = Raw.substr(End + 4);
	return A;
}

}  // namespace ScfgTestHttpDetail

using ScfgTestHttpDetail::FClient;

std::optional<FAnswer> Exchange(int32 Port, const std::string& Request, const TFunction<void()>& Pump, double Patience)
{
	FClient Client;
	if (!Client.Connect(Port))
	{
		return std::nullopt;
	}
	int32 Sent = 0;
	if (!Client.Sock->Send(reinterpret_cast<const uint8*>(Request.data()), static_cast<int32>(Request.size()), Sent) ||
		Sent != static_cast<int32>(Request.size()))
	{
		return std::nullopt;
	}
	Client.Sock->SetNonBlocking(true);
	std::string Raw;
	const double Until = FPlatformTime::Seconds() + Patience;
	while (FPlatformTime::Seconds() < Until)
	{
		Pump();
		uint8 Buf[4096];
		int32 Got = 0;
		const bool bOk = Client.Sock->Recv(Buf, sizeof Buf, Got);
		if (Got > 0)
		{
			Raw.append(reinterpret_cast<const char*>(Buf), static_cast<size_t>(Got));
			continue;
		}
		if (!bOk)
		{
			break;  // the server closed the connection: the answer is complete
		}
		FPlatformProcess::SleepNoStats(0.001f);  // a test helper only: lets the kernel move the bytes
	}
	return ScfgTestHttpDetail::Parse(Raw);
}

bool Refused(int32 Port)
{
	FClient Client;
	return !Client.Connect(Port);
}

std::string Request(const char* Method, const std::string& Path, int32 Port, const std::string& Body)
{
	std::string Out = std::string(Method) + " " + Path + " HTTP/1.1\r\nHost: 127.0.0.1:" + scfg::py::dec(Port) + "\r\n";
	if (!Body.empty())
	{
		Out += "Content-Type: application/json\r\nContent-Length: " + scfg::py::dec(Body.size()) + "\r\n";
	}
	return Out + "\r\n" + Body;
}

int32 PortOf(const FString& Url)
{
	const FString Prefix = TEXT("http://127.0.0.1:");
	if (!Url.StartsWith(Prefix))
	{
		return 0;
	}
	const FString Rest = Url.RightChop(Prefix.Len());
	int32 Slash = INDEX_NONE;
	Rest.FindChar(TEXT('/'), Slash);
	return Slash > 0 ? FCString::Atoi(*Rest.Left(Slash)) : 0;
}

}  // namespace ScfgTestHttp
