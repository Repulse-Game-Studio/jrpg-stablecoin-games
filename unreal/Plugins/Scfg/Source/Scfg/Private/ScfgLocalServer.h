// Copyright 2026 Repulse
// No Python twin: the Unreal side of the localhost checkout server (our own server, its HTTP logic in the core's
// scfg::local_server, LocalServer.h, the twin of scfg_core/local_server.py). What is left for the engine:
// - the sockets: a TCP listener from ISocketSubsystem bound to 127.0.0.1 on a random port (never 0.0.0.0, never a
//   fixed port), non-blocking, and its accepted connections, non-blocking too; CheckoutServer::pump() reads, answers
//   and writes them without ever waiting, from the controller's poll() in every facade pump (game thread only);
// - the page's files, read from the plugin's Resources/Web/ (copies of plugin/game/scfg/web/ made by
//   tools/sync_web_page.py; Scfg.Build.cs stages them for packaged games as RuntimeDependencies) when a page server is
//   made, so the server answers from memory; without them the browser method is unavailable;
// - the developer's logo, read once when a page server is made (Godot's _server_opts), served only as png, jpeg or
//   webp;
// - FPlatformTime::Seconds() as the server's clock, and the reporter for the Err of a page that cannot start.
//
// FScfgLocalServer::Factory() is the facade's default ServerFactory (FScfgFacadeOptions::ServerFactory).
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

#include "Scfg/Checkout.h"
#include "Scfg/Err.h"
#include "Scfg/LocalServer.h"

#include <optional>
#include <string>

struct FScfgLocalServerPage
{
	// The page's files. Empty: FScfgLocalServer::WebDirectory().
	FString WebDirectory;
	// The configured logo's full path, read when a page server is made; "" for none (no /logo route). Shared with the
	// facade, which sets it in Configure().
	TSharedPtr<FString> LogoPath;
	// Told each Err a page server cannot be made or started with (the controller drops it, Checkout.h): a missing page
	// file, a listener that cannot bind.
	TFunction<void(const FString& Line)> OnError;
	// The server's clock (monotonic seconds). Default: FPlatformTime::Seconds.
	TFunction<double()> Clock;
	// The address it binds. Default: "127.0.0.1" (the core's).
	FString Host;
};

struct SCFG_API FScfgLocalServer
{
	// The plugin's Resources/Web/ (IPluginManager's base directory of the Scfg plugin), or "" when the plugin is not
	// found.
	static FString WebDirectory();
	// Listen() for the core: a non-blocking ISocketSubsystem TCP listener on Host (an IPv4 address) and port 0, with
	// local_server::LISTEN_BACKLOG; RuntimeError "cannot listen on <host>: <why>" when it cannot.
	static scfg::local_server::Listen Listen();
	// The files of local_server::ASSETS in Directory, read now: name -> bytes. A file that cannot be read is left out
	// and listed in Missing; Factory() then fails.
	static TMap<FString, std::string> ReadPage(const FString& Directory, TArray<FString>* Missing = nullptr);
	// The CheckoutController's server_factory: a local_server::CheckoutServer over Listen(), the page read from
	// WebDirectory, the logo read from *LogoPath. A page file that is missing fails the factory (RuntimeError), and a
	// server that cannot start fails start(); both are reported through OnError ("scfg: cannot start the checkout
	// page: <Err>") and make the browser method unavailable.
	static scfg::checkout::ServerFactory Factory(FScfgLocalServerPage Page);
};
