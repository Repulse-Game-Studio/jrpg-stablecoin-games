// No Python twin: a raw HTTP client for the localhost checkout server tests (Scfg.Glue.LocalServer.*,
// Scfg.Glue.Facade.Flows.*): an FSocket connected to 127.0.0.1:<port> that sends exactly the bytes given and reads
// until the server closes, pumping the server (Pump) in between, so a test drives both ends on the game thread without
// a latent command (Godot's _http helper in test_scfg_flows.gd). Bounded: it gives up after Patience seconds.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

#include <map>
#include <optional>
#include <string>

namespace ScfgTestHttp
{

struct FAnswer
{
	int32 Status = 0;
	std::map<std::string, std::string> Headers;  // lowercase names, first value
	std::string Body;
	std::string Raw;

	// The header's value, or "" when there is none (Name in lowercase).
	std::string Header(const std::string& Name) const
	{
		const auto It = Headers.find(Name);
		return It == Headers.end() ? std::string() : It->second;
	}
};

// Connects to 127.0.0.1:Port, sends Request, then pumps and reads until the server closes the connection or Patience
// seconds pass. The answer, or none when the connection failed (refused) or nothing complete came back.
std::optional<FAnswer> Exchange(int32 Port, const std::string& Request, const TFunction<void()>& Pump,
	double Patience = 5.0);

// True when nothing accepts a connection on 127.0.0.1:Port.
bool Refused(int32 Port);

// "<Method> <Path> HTTP/1.1\r\nHost: 127.0.0.1:<Port>\r\n[Content-Type: application/json\r\nContent-Length: n\r\n]\r\n
// <Body>"
std::string Request(const char* Method, const std::string& Path, int32 Port, const std::string& Body = std::string());

// The port of an http://127.0.0.1:<port>/... URL, or 0.
int32 PortOf(const FString& Url);

}  // namespace ScfgTestHttp
