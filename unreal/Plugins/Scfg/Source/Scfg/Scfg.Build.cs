// Copyright 2026 Repulse
// The Unreal glue around the payment core (unreal/README.md, "The glue module"): the worker that drives the core's
// Engine on the game thread, the FHttpModule transport, the UTF-8 bridge, the store (state.json), the facade with its
// game-instance subsystem and Blueprint API, the localhost checkout server, and the Slate screens (Private/UI).
using System.IO;
using UnrealBuildTool;

public class Scfg : ModuleRules
{
	public Scfg(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Public/ (the subsystem, the async nodes, the Blueprint library, the value types, the sheet host seam, the
		// default Slate host and its UMG wrapper) needs Core, CoreUObject, Engine, SlateCore (ISlateStyle) and UMG
		// (UScfgSheetWidget is a UUserWidget). The rest is private: the core's headers and the HTTP module stay out
		// of a dependent's include path (the glue never exposes scfg::Co or scfg::Future); ApplicationCore for the
		// clipboard.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "SlateCore", "UMG" });
		// DeveloperSettings for the project settings (UScfgSettings, Public/ScfgSettings.h, a UDeveloperSettings).
		PublicDependencyModuleNames.Add("DeveloperSettings");
		// Sockets for the localhost checkout server's listener (ScfgLocalServer.h), Projects to find the plugin's
		// Resources/Web/; Slate and InputCore for the screens (Private/UI), ImageWrapper to read the logo.
		// The same list on every platform of Scfg.uplugin, Android and iOS included: each is an engine runtime module
		// with an Android and an iOS implementation (Sockets: Private/Android and Private/IOS socket subsystems), and
		// the glue's code is platform-neutral. On a phone the server is never started (no browser method: the
		// controller's mobile flag), so only the page's files are left out there (below). Unverified: no Android or
		// iOS SDK on the development machine (unreal/README.md, "Mobile").
		PrivateDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "HTTP", "ImageWrapper", "InputCore", "Projects", "ScfgCore", "Slate", "Sockets" });
		// SSL: the HTTP transport refuses an https:// request unless libcurl will verify the peer, which it does only
		// when the SSL module found root certificates (FScfgHttpTransport::TlsVerificationProblem, ruling R5-4).
		PrivateDependencyModuleNames.Add("SSL");
		// bEnableExceptions and bUseRTTI keep their defaults (false).
		// The checkout page's files the server serves (local_server::ASSETS; copies of plugin/game/scfg/web/, kept by
		// tools/sync_web_page.py), staged with a packaged game as loose files at the plugin's own path, where
		// FScfgLocalServer::WebDirectory() finds them. bridge.js is the web exports' bridge: never served, not staged.
		// Desktop games only: a phone has no localhost page.
		if (Target.Platform.IsInGroup(UnrealPlatformGroup.Desktop))
		{
			foreach (string Asset in new string[] { "index.html", "app.js", "evm.js", "solana.js" })
			{
				RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "Web", Asset), StagedFileType.NonUFS);
			}
		}
		// The engine's root certificate bundle, staged with a Linux game (ruling R5-4). A packaged game does not stage
		// it by default: BaseGame.ini lists Content/Certificates only for the early-downloader pak, and UAT stages it
		// (as UFS, into the pak) only when the project's engine ini sets n.VerifyPeer=True
		// (CopyBuildToStagingDirectory.Automation.cs:2251-2281). Without either, the game's TLS would rest on the
		// distro's bundle alone, and with none the engine turns libcurl's peer verification off
		// (CurlHttpManager.cpp:277-289). This loose copy is the one a game without n.VerifyPeer=True gets. The SSL module reads it from EngineContentDir()/Certificates/ThirdParty/
		// (SslCertificateManager.cpp:331-337, through IFileManager, so a loose file works). Windows and Android load
		// their system stores; Apple platforms use NSURLSession. Unverified there.
		if (Target.Platform == UnrealTargetPlatform.Linux || Target.Platform == UnrealTargetPlatform.LinuxArm64)
		{
			RuntimeDependencies.Add("$(EngineDir)/Content/Certificates/ThirdParty/cacert.pem", StagedFileType.NonUFS);
		}
	}
}
