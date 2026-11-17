// No Python twin: godot/demo/e2e/e2e_driver.gd's Unreal form (see ScfgE2EDriver.h).
#include "ScfgE2EDriver.h"

#if !UE_BUILD_SHIPPING

#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#include "ScfgDefaultSheetHost.h"
#include "ScfgDevLog.h"
#include "ScfgFacade.h"
#include "ScfgShowcase.h"
#include "ScfgSubsystem.h"
#include "UI/SScfgPaymentSheet.h"
#include "UI/SScfgScreen.h"

#include "Scfg/Chains.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace ScfgE2EDriverDetail
{
// How far a click scrolls the sheet's body to bring its control into view (godot's MAX_WHEEL_NOTCHES).
constexpr int32 MAX_WHEEL_NOTCHES = 40;
// Frames a screenshot or a window activation may take.
constexpr int32 MAX_WAIT_FRAMES = 600;

FString Utf8(const std::string& S)
{
	return FString(UTF8_TO_TCHAR(S.c_str()));
}

std::string Std(const FString& S)
{
	return std::string(TCHAR_TO_UTF8(*S));
}

TSharedPtr<FJsonValue> Str(const FString& S)
{
	return MakeShared<FJsonValueString>(S);
}

TSharedPtr<FJsonValue> Strings(const TArray<FString>& Items)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& S : Items)
	{
		Out.Add(Str(S));
	}
	return MakeShared<FJsonValueArray>(Out);
}

// [x, y, w, h]; a widget not laid out yet (a control of a build not painted) has no finite geometry: [0, 0, 0, 0]
// (JSON has no NaN).
TSharedPtr<FJsonValue> JsonRect(const FSlateRect& R)
{
	const bool bFinite = FMath::IsFinite(R.Left) && FMath::IsFinite(R.Top) && FMath::IsFinite(R.Right) &&
		FMath::IsFinite(R.Bottom);
	auto N = [bFinite](float V) { return MakeShared<FJsonValueNumber>(bFinite ? V : 0.f); };
	return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{N(R.Left), N(R.Top), N(R.Right - R.Left),
		N(R.Bottom - R.Top)});
}

// A widget's rect on the desktop (Slate's absolute space: the window's position plus its layout), from its last paint.
FSlateRect DesktopRect(const SWidget& Widget)
{
	return Widget.GetTickSpaceGeometry().GetLayoutBoundingRect();
}

bool IsWithin(const TSharedPtr<SWidget>& Widget, const SWidget& Ancestor)
{
	for (TSharedPtr<SWidget> W = Widget; W.IsValid(); W = W->GetParentWidget())
	{
		if (W.Get() == &Ancestor)
		{
			return true;
		}
	}
	return false;
}

// The scroll box around Widget (the sheet's scrolling body), or null.
TSharedPtr<SWidget> ScrollParent(const TSharedPtr<SWidget>& Widget)
{
	for (TSharedPtr<SWidget> W = Widget.IsValid() ? Widget->GetParentWidget() : nullptr; W.IsValid(); W = W->GetParentWidget())
	{
		if (W->GetType() == FName(TEXT("SScrollBox")))
		{
			return W;
		}
	}
	return nullptr;
}

// The platform cursor moves to At (FLinuxCursor: SDL warps the X pointer and caches the position), then Slate hears of
// the move as it hears of the OS's (OnMouseMove compares with the last platform position, which this leaves alone,
// unlike FSlateApplication::SetCursorPos): it routes the move, so the widget under the cursor is hovered.
void MoveCursor(const FVector2D& At)
{
	FSlateApplication& App = FSlateApplication::Get();
	if (const TSharedPtr<GenericApplication> Platform = App.GetPlatformApplication(); Platform.IsValid() && Platform->Cursor.IsValid())
	{
		Platform->Cursor->SetPosition(FMath::RoundToInt32(At.X), FMath::RoundToInt32(At.Y));
	}
	App.OnMouseMove();
}

FString Kind(const SScfgScreen& Screen)
{
	const FName Type = Screen.GetType();
	return Type == FName(TEXT("SScfgPaymentSheet")) ? TEXT("sheet") : Type == FName(TEXT("SScfgRestoreScreen")) ? TEXT("restore") : TEXT("other");
}

// The config's "configure" object as FScfgOptions; "" or why not.
FString ReadOptions(const FJsonObject& O, FScfgOptions& Out)
{
	for (const auto& Field : O.Values)
	{
		const FString K(Field.Key.ToView());
		const TSharedPtr<FJsonValue>& V = Field.Value;
		if (K == TEXT("wallet") || K == TEXT("solana_wallet"))
		{
			FString S;
			if (!V->TryGetString(S))
			{
				return FString::Printf(TEXT("configure.%s must be a string"), *K);
			}
			(K == TEXT("wallet") ? Out.Wallet : Out.SolanaWallet) = S;
		}
		else if (K == TEXT("testnet"))
		{
			if (!V->TryGetBool(Out.bTestnet))
			{
				return TEXT("configure.testnet must be a boolean");
			}
		}
		else if (K == TEXT("chains") || K == TEXT("tokens"))
		{
			TArray<FString>& List = K == TEXT("chains") ? Out.Chains : Out.Tokens;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!V->TryGetArray(Values))
			{
				return FString::Printf(TEXT("configure.%s must be a list of strings"), *K);
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString S;
				if (!Value->TryGetString(S))
				{
					return FString::Printf(TEXT("configure.%s must be a list of strings"), *K);
				}
				List.Add(S);
			}
		}
		else
		{
			return FString::Printf(TEXT("configure.%s is not an option the e2e config takes"), *K);
		}
	}
	return FString();
}

FString ReadItem(const FString& Id, const FJsonObject& O, FScfgItem& Out)
{
	for (const auto& Field : O.Values)
	{
		const FString K(Field.Key.ToView());
		if (K == TEXT("code"))
		{
			double Code = 0;
			if (!Field.Value->TryGetNumber(Code) || Code != FMath::FloorToDouble(Code))
			{
				return FString::Printf(TEXT("items.%s.code must be an integer"), *Id);
			}
			Out.Code = int32(Code);
		}
		else if (K == TEXT("price") || K == TEXT("token") || K == TEXT("title") || K == TEXT("min_price"))
		{
			FString S;
			if (!Field.Value->TryGetString(S))
			{
				return FString::Printf(TEXT("items.%s.%s must be a string"), *Id, *K);
			}
			(K == TEXT("price") ? Out.Price : K == TEXT("token") ? Out.Token : K == TEXT("title") ? Out.Title : Out.MinPrice) = S;
		}
		else
		{
			return FString::Printf(TEXT("items.%s.%s is not an item field"), *Id, *K);
		}
	}
	return FString();
}

// Points the built-in chain entry at the local node (unreal/core-driver/Main.cpp's apply_override); "" or why not.
FString ApplyChain(const FJsonObject& O)
{
	FString Network;
	O.TryGetStringField(TEXT("network"), Network);
	scfg::OrderedMap<scfg::chains::Chain>& Registry =
		Network == TEXT("testnet") ? scfg::chains::TESTNET() : scfg::chains::MAINNET();
	FString Key;
	if (!O.TryGetStringField(TEXT("key"), Key) || Registry.find(Std(Key)) == nullptr)
	{
		return FString::Printf(TEXT("chain.key: unknown chain \"%s\""), *Key);
	}
	TArray<FString> Rpc;
	if (!O.TryGetStringArrayField(TEXT("rpc"), Rpc) || Rpc.IsEmpty())
	{
		return TEXT("chain.rpc must be a non-empty list of strings");
	}
	std::vector<std::string> Urls;
	for (const FString& U : Rpc)
	{
		Urls.push_back(Std(U));
	}
	std::optional<scfg::BigInt> Confirmations;
	if (O.HasField(TEXT("confirmations")))
	{
		int64 N = 0;
		if (!O.TryGetNumberField(TEXT("confirmations"), N))
		{
			return TEXT("chain.confirmations must be an integer");
		}
		Confirmations = scfg::BigInt(N);
	}
	std::optional<scfg::OrderedMap<scfg::chains::Token>> Tokens;
	const TSharedPtr<FJsonObject>* TokensObject = nullptr;
	if (O.TryGetObjectField(TEXT("tokens"), TokensObject) && !(*TokensObject)->Values.IsEmpty())
	{
		Tokens.emplace();
		for (const auto& T : (*TokensObject)->Values)
		{
			const FString Name(T.Key.ToView());
			FString Address;
			if (!T.Value->TryGetString(Address))
			{
				return TEXT("chain.tokens values must be strings");
			}
			scfg::Result<scfg::chains::Token> Token = scfg::chains::Token::make(Std(Address));
			if (!Token.ok())
			{
				return FString::Printf(TEXT("chain.tokens.%s: %s"), *Name, *Utf8(Token.err().str()));
			}
			Tokens->set(Std(Name), std::move(Token).value());
		}
	}
	scfg::chains::Chain Patched = Registry.find(Std(Key))->with(std::move(Urls), std::move(Confirmations), std::move(Tokens));
	Registry.set(Std(Key), std::move(Patched));
	return FString();
}

#if PLATFORM_LINUX
// SDL's mouse rect of the game window (FLinuxCursor::Lock sets it with SDL_SetWindowMouseRect): whether the cursor is
// locked to the window. The engine exports SDL's symbols (ApplicationCore's in the editor, the game binary's when
// packaged), so dlsym finds the SDL the window belongs to; unset when it cannot be asked.
TOptional<bool> CursorLocked(void* SdlWindow)
{
	using FGetRect = const void* (*)(void*);
	static FGetRect GetRect = reinterpret_cast<FGetRect>(dlsym(RTLD_DEFAULT, "SDL_GetWindowMouseRect"));
	if (!GetRect || !SdlWindow)
	{
		return {};
	}
	return GetRect(SdlWindow) != nullptr;
}
#else
TOptional<bool> CursorLocked(void*)
{
	return {};
}
#endif
}  // namespace ScfgE2EDriverDetail

using namespace ScfgE2EDriverDetail;

bool FScfgE2EDriver::Requested(FString& OutPath)
{
	return FParse::Value(FCommandLine::Get(), TEXT("scfg-e2e="), OutPath) && !OutPath.IsEmpty();
}

TSharedPtr<FScfgE2EDriver> FScfgE2EDriver::Create(const FString& Path, UScfgShowcaseGameInstance* InGame)
{
	const TSharedRef<FScfgE2EDriver> D = MakeShared<FScfgE2EDriver>();
	D->Game = InGame;
	auto Bad = [&Path](const FString& Why) -> TSharedPtr<FScfgE2EDriver>
	{
		UE_LOG(LogScfgDev, Error, TEXT("e2e config %s: %s"), *Path, *Why);
		return nullptr;
	};
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return Bad(TEXT("file not found or unreadable"));
	}
	TSharedPtr<FJsonObject> Config;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Config) || !Config.IsValid())
	{
		return Bad(TEXT("not a JSON object"));
	}
	const TSharedPtr<FJsonObject>* Configure = nullptr;
	if (!Config->TryGetObjectField(TEXT("configure"), Configure))
	{
		return Bad(TEXT("\"configure\" must be an object"));
	}
	if (const FString Why = ReadOptions(**Configure, D->ConfigOptions); !Why.IsEmpty())
	{
		return Bad(Why);
	}
	const TSharedPtr<FJsonObject>* Items = nullptr;
	if (!Config->TryGetObjectField(TEXT("items"), Items))
	{
		return Bad(TEXT("\"items\" must be an object"));
	}
	for (const auto& Item : (*Items)->Values)
	{
		const FString Id(Item.Key.ToView());
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		if (!Item.Value->TryGetObject(Fields))
		{
			return Bad(FString::Printf(TEXT("items[%s] must be an object"), *Id));
		}
		FScfgItem Out;
		if (const FString Why = ReadItem(Id, **Fields, Out); !Why.IsEmpty())
		{
			return Bad(Why);
		}
		D->ConfigItems.Add(Id, Out);
	}
	const TSharedPtr<FJsonObject>* Control = nullptr;
	int32 Port = 0;
	if (!Config->TryGetObjectField(TEXT("control"), Control) || !(*Control)->TryGetNumberField(TEXT("port"), Port) ||
		Port <= 0 || Port > 65535 || !(*Control)->TryGetStringField(TEXT("token"), D->ControlToken))
	{
		return Bad(TEXT("\"control\" must be {port, token}"));
	}
	D->ControlPort = Port;
	if (Config->HasField(TEXT("draw_world")) && !Config->TryGetBoolField(TEXT("draw_world"), D->bDrawWorld))
	{
		return Bad(TEXT("\"draw_world\" must be a boolean"));
	}
	const TSharedPtr<FJsonObject>* Chain = nullptr;
	if (Config->HasField(TEXT("chain")))
	{
		if (!Config->TryGetObjectField(TEXT("chain"), Chain))
		{
			return Bad(TEXT("\"chain\" must be an object"));
		}
		if (const FString Why = ApplyChain(**Chain); !Why.IsEmpty())
		{
			return Bad(Why);
		}
	}
	UE_LOG(LogScfgDev, Log, TEXT("e2e driver: config %s read"), *Path);
	return D;
}

FScfgE2EDriver::~FScfgE2EDriver()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickHandle);
	}
	FCoreDelegates::ShouldLaunchUrl.Unbind();
	if (FocusHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnFocusChanging().Remove(FocusHandle);
	}
	if (Socket)
	{
		Socket->Close();
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			Sockets->DestroySocket(Socket);
		}
		Socket = nullptr;
	}
}

bool FScfgE2EDriver::Start()
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets)
	{
		UE_LOG(LogScfgDev, Error, TEXT("e2e driver: no socket subsystem"));
		return false;
	}
	const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
	bool bValid = false;
	Address->SetIp(TEXT("127.0.0.1"), bValid);  // the runner's control socket: loopback only
	Address->SetPort(ControlPort);
	Socket = Sockets->CreateSocket(NAME_Stream, TEXT("scfg-e2e-control"), FNetworkProtocolTypes::IPv4);
	// A connect to a listening loopback port completes at once (the runner listens before it starts the game).
	if (!bValid || !Socket || !Socket->Connect(*Address) || !Socket->SetNonBlocking(true))
	{
		UE_LOG(LogScfgDev, Error, TEXT("e2e driver: cannot connect to the runner at 127.0.0.1:%d"), ControlPort);
		return false;
	}
	TWeakPtr<FScfgE2EDriver> Weak = AsShared();
	FCoreDelegates::ShouldLaunchUrl.BindLambda([Weak](const TCHAR* Url)
	{
		if (const TSharedPtr<FScfgE2EDriver> D = Weak.Pin())
		{
			D->Opened.Add(Url);
		}
		UE_LOG(LogScfgDev, Log, TEXT("e2e driver: the game opens %s (recorded, not launched)"), Url);
		return false;
	});
	if (FSlateApplication::IsInitialized())
	{
		FocusHandle = FSlateApplication::Get().OnFocusChanging().AddSP(this, &FScfgE2EDriver::OnFocusChanging);
	}
	const TSharedRef<FJsonObject> Hello = MakeShared<FJsonObject>();
	Hello->SetStringField(TEXT("hello"), ControlToken);
	Hello->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Send(Hello);
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("ScfgE2EDriver"), 0.f,
		[Weak](float Dt)
		{
			const TSharedPtr<FScfgE2EDriver> D = Weak.Pin();
			return D.IsValid() && D->Tick(Dt);
		});
	UE_LOG(LogScfgDev, Log, TEXT("e2e driver: connected to the runner at 127.0.0.1:%d"), ControlPort);
	return true;
}

bool FScfgE2EDriver::Tick(float DeltaTime)
{
	if (!bWorldSet)
	{
		UScfgShowcaseGameInstance* G = Game.Get();
		if (UGameViewportClient* Client = G ? G->GetGameViewportClient() : nullptr)
		{
			Client->bDisableWorldRendering = !bDrawWorld;
			bWorldSet = true;
		}
	}
	Flush();
	if (bQuitting)
	{
		return true;
	}
	ReadCommands();
	while (!Steps.IsEmpty())
	{
		FStep Step = MoveTemp(Steps[0]);
		Steps.RemoveAt(0);
		bAbortSteps = false;
		const bool bDone = Step();
		if (bAbortSteps)
		{
			Steps.Reset();  // the step replied with an error: the command's other steps are dropped
			continue;
		}
		if (!bDone)
		{
			Steps.Insert(MoveTemp(Step), 0);
			break;  // this step waits for a later frame
		}
	}
	while (Steps.IsEmpty() && !Commands.IsEmpty() && !bQuitting)
	{
		const TSharedRef<FJsonObject> Command = Commands[0];
		Commands.RemoveAt(0);
		int64 Id = 0;
		Command->TryGetNumberField(TEXT("id"), Id);
		Run(Id, Command);
	}
	Flush();
	return true;
}

void FScfgE2EDriver::ReadCommands()
{
	if (!Socket)
	{
		return;
	}
	uint8 Buffer[4096];
	while (true)
	{
		int32 Read = 0;
		if (!Socket->Recv(Buffer, sizeof(Buffer), Read))
		{
			// the runner closed the connection (Recv is false when the peer closed: engine-facts.md, task 6)
			UE_LOG(LogScfgDev, Log, TEXT("e2e driver: the runner closed the control connection, quitting"));
			bQuitting = true;
			FPlatformMisc::RequestExit(false, TEXT("ScfgE2EDriver"));
			return;
		}
		if (Read <= 0)
		{
			break;
		}
		Inbox.Append(Buffer, Read);
	}
	int32 Newline = INDEX_NONE;
	while (Inbox.Find(uint8('\n'), Newline))
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Inbox.GetData()), Newline);
		const FString Line(Converted.Length(), Converted.Get());
		Inbox.RemoveAt(0, Newline + 1);
		TSharedPtr<FJsonObject> Command;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Command) || !Command.IsValid())
		{
			ReplyError(0, FString::Printf(TEXT("not a command object: %s"), *Line));
			continue;
		}
		Commands.Add(Command.ToSharedRef());
	}
}

void FScfgE2EDriver::Send(const TSharedRef<FJsonObject>& Message)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Message, Writer);
	Text += TEXT("\n");
	const FTCHARToUTF8 Utf8Text(*Text);
	Outbox.Append(reinterpret_cast<const uint8*>(Utf8Text.Get()), Utf8Text.Length());
	Flush();
}

void FScfgE2EDriver::Flush()
{
	while (Socket && !Outbox.IsEmpty())
	{
		int32 Sent = 0;
		if (!Socket->Send(Outbox.GetData(), Outbox.Num(), Sent) || Sent <= 0)
		{
			return;  // full (a later frame sends the rest) or gone (the read notices)
		}
		Outbox.RemoveAt(0, Sent);
	}
}

void FScfgE2EDriver::Reply(int64 Id, const TSharedPtr<FJsonValue>& Ok)
{
	const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetNumberField(TEXT("id"), double(Id));
	Message->SetField(TEXT("ok"), Ok.IsValid() ? Ok : MakeShared<FJsonValueNull>());
	Send(Message);
}

void FScfgE2EDriver::ReplyError(int64 Id, const FString& Why)
{
	const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetNumberField(TEXT("id"), double(Id));
	Message->SetStringField(TEXT("error"), Why);
	Send(Message);
}

// --- what the player sees ----------------------------------------------------------------------------------------------

TSharedPtr<SWindow> FScfgE2EDriver::GameWindow() const
{
	UScfgShowcaseGameInstance* G = Game.Get();
	UGameViewportClient* Client = G ? G->GetGameViewportClient() : nullptr;
	return Client ? Client->GetWindow() : nullptr;
}

TSharedPtr<SWidget> FScfgE2EDriver::ViewportWidget() const
{
	UScfgShowcaseGameInstance* G = Game.Get();
	UGameViewportClient* Client = G ? G->GetGameViewportClient() : nullptr;
	return Client ? Client->GetGameViewportWidget() : nullptr;
}

FString FScfgE2EDriver::Owner(const TSharedPtr<SWidget>& Widget) const
{
	if (!Widget.IsValid())
	{
		return TEXT("none");
	}
	if (Widget == ViewportWidget())
	{
		return TEXT("viewport");
	}
	for (const TSharedRef<SScfgScreen>& Screen : SScfgScreen::OpenScreens())
	{
		if (IsWithin(Widget, *Screen))
		{
			return Kind(*Screen);
		}
	}
	UScfgShowcaseGameInstance* G = Game.Get();
	if (G && G->GetHud().IsValid() && IsWithin(Widget, *G->GetHud()))
	{
		return TEXT("hud");
	}
	return Widget->GetTypeAsString();
}

TSharedRef<FJsonObject> FScfgE2EDriver::InputState()
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!FSlateApplication::IsInitialized())
	{
		return Out;
	}
	FSlateApplication& App = FSlateApplication::Get();
	Out->SetStringField(TEXT("focus"), Owner(App.GetUserFocusedWidget(0)));
	const TSharedPtr<FSlateUser> Mouse = App.GetUser(App.GetUserIndexForMouse());
	Out->SetStringField(TEXT("captor"), Mouse.IsValid() && Mouse->HasCursorCapture() ? Owner(Mouse->GetCursorCaptor()) : TEXT("none"));
	const TSharedPtr<GenericApplication> Platform = App.GetPlatformApplication();
	Out->SetBoolField(TEXT("high_precision"), Platform.IsValid() && Platform->IsUsingHighPrecisionMouseMode());
	const TSharedPtr<SWindow> Window = GameWindow();
	const TSharedPtr<FGenericWindow> Native = Window.IsValid() ? Window->GetNativeWindow() : nullptr;
	const TOptional<bool> Locked = CursorLocked(Native.IsValid() ? Native->GetOSWindowHandle() : nullptr);
	if (Locked.IsSet())
	{
		Out->SetBoolField(TEXT("cursor_locked"), *Locked);
	}
	else
	{
		Out->SetField(TEXT("cursor_locked"), MakeShared<FJsonValueNull>());
	}
	Out->SetBoolField(TEXT("app_active"), App.IsActive());
	Out->SetBoolField(TEXT("window_active"), Window.IsValid() && Window->IsActive());
	return Out;
}

TSharedRef<FJsonObject> FScfgE2EDriver::Snapshot()
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Screens;
	for (const TSharedRef<SScfgScreen>& Screen : SScfgScreen::OpenScreens())
	{
		const TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
		const FString K = Kind(*Screen);
		View->SetStringField(TEXT("kind"), K);
		View->SetStringField(TEXT("step"), K == TEXT("sheet") ? StaticCastSharedRef<SScfgPaymentSheet>(Screen)->ShownStep() : FString());
		View->SetBoolField(TEXT("ended"), Screen->IsEnded());
		View->SetField(TEXT("texts"), Strings(Screen->VisibleTexts()));
		TArray<TSharedPtr<FJsonValue>> Controls;
		for (const FString& Key : Screen->FocusOrder())
		{
			const TSharedPtr<SWidget> W = Screen->Control(Key);
			if (!W.IsValid())
			{
				continue;
			}
			const TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("name"), Key);
			C->SetStringField(TEXT("text"), Screen->TextOf(Key));
			C->SetBoolField(TEXT("disabled"), !W->IsEnabled());
			C->SetBoolField(TEXT("field"), W->GetType() == FName(TEXT("SEditableTextBox")));
			C->SetField(TEXT("rect"), JsonRect(DesktopRect(*W)));
			Controls.Add(MakeShared<FJsonValueObject>(C));
		}
		View->SetArrayField(TEXT("buttons"), Controls);
		const FString Focused = Screen->FocusedKey();
		View->SetField(TEXT("focus"), Focused.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()) : Str(Focused));
		Screens.Add(MakeShared<FJsonValueObject>(View));
	}
	Out->SetArrayField(TEXT("screens"), Screens);
	const TSharedRef<FJsonObject> GameView = MakeShared<FJsonObject>();
	UScfgShowcaseGameInstance* G = Game.Get();
	if (G && G->GetHud().IsValid())
	{
		GameView->SetField(TEXT("texts"), Strings(G->GetHud()->Texts()));
		TArray<TSharedPtr<FJsonValue>> Buttons;
		for (const SScfgShowcaseHud::FButtonView& B : G->GetHud()->Buttons())
		{
			const TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("name"), B.Name);
			C->SetStringField(TEXT("text"), B.Text);
			C->SetBoolField(TEXT("disabled"), !B.Widget->IsEnabled());
			C->SetField(TEXT("rect"), JsonRect(DesktopRect(*B.Widget)));
			Buttons.Add(MakeShared<FJsonValueObject>(C));
		}
		GameView->SetArrayField(TEXT("buttons"), Buttons);
	}
	else
	{
		GameView->SetArrayField(TEXT("texts"), {});
		GameView->SetArrayField(TEXT("buttons"), {});
	}
	Out->SetObjectField(TEXT("game"), GameView);
	TArray<FString> Toasts;
	if (UScfgSubsystem* S = G ? G->Scfg() : nullptr; S && S->GetFacade())
	{
		const TSharedPtr<IScfgSheetHost> Host = S->GetFacade()->DefaultSheetHost;
		if (Host.IsValid() && S->GetFacade()->ActiveSheetHost() == Host)
		{
			Toasts = StaticCastSharedPtr<FScfgDefaultSheetHost>(Host)->ToastTexts();
		}
	}
	Out->SetField(TEXT("toasts"), Strings(Toasts));
	Out->SetField(TEXT("opened"), Strings(Opened));
	FString Clipboard;
	FPlatformApplicationMisc::ClipboardPaste(Clipboard);
	Out->SetStringField(TEXT("clipboard"), Clipboard);
	Out->SetBoolField(TEXT("paused"), G && G->GetWorld() && G->GetWorld()->IsPaused());
	const AScfgShowcasePlayerController* PC = G ? G->PlayerController() : nullptr;
	Out->SetNumberField(TEXT("world_clicks"), PC ? PC->WorldClicks : -1);
	const TSharedPtr<SWindow> Window = GameWindow();
	const FVector2f Size = Window.IsValid() ? FVector2f(Window->GetClientSizeInScreen()) : FVector2f::ZeroVector;
	Out->SetArrayField(TEXT("window"), {MakeShared<FJsonValueNumber>(Size.X), MakeShared<FJsonValueNumber>(Size.Y)});
	Out->SetObjectField(TEXT("input"), InputState());
	return Out;
}

TSharedRef<FJsonObject> FScfgE2EDriver::Wish()
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	UScfgShowcaseGameInstance* G = Game.Get();
	UGameViewportClient* Client = G ? G->GetGameViewportClient() : nullptr;
	if (!Client)
	{
		return Out;
	}
	const FScfgMouseWish W = ScfgDefaultSheetHostDetail::WishOf(Client, G)();
	Out->SetStringField(TEXT("viewport_type"), W.Viewport.IsValid() ? W.Viewport->GetTypeAsString() : FString());
	Out->SetBoolField(TEXT("is_game_viewport_widget"), W.Viewport.IsValid() && W.Viewport == Client->GetGameViewportWidget());
	// the widget is in the game's native window, and it is the one the window's own viewport interface points at
	bool bInWindow = false;
	if (W.Viewport.IsValid() && FSlateApplication::IsInitialized())
	{
		FWidgetPath Path;
		bInWindow = FSlateApplication::Get().FindPathToWidget(W.Viewport.ToSharedRef(), Path) && Path.IsValid() &&
			Path.GetWindow() == Client->GetWindow() && Client->GetWindow()->GetNativeWindow().IsValid();
	}
	Out->SetBoolField(TEXT("in_game_window"), bInWindow);
	const TSharedPtr<SWindow> Window = Client->GetWindow();
	const TSharedPtr<ISlateViewport> WindowViewport = Window.IsValid() ? Window->GetViewport() : nullptr;
	const TSharedPtr<SViewport> Viewport = Client->GetGameViewportWidget();
	Out->SetBoolField(TEXT("window_viewport_is_its_viewport"), WindowViewport.IsValid() && Viewport.IsValid() &&
		Viewport->GetViewportInterface().Pin() == WindowViewport);
	Out->SetBoolField(TEXT("capture"), W.bCapture);
	Out->SetBoolField(TEXT("high_precision"), W.bHighPrecision);
	Out->SetBoolField(TEXT("lock"), W.bLock);
	Out->SetBoolField(TEXT("always_lock"), W.bAlwaysLock);
	// what the newest open screen holds of the viewport (the captor it recorded when it took the mouse)
	const TArray<TSharedRef<SScfgScreen>> Open = SScfgScreen::OpenScreens();
	const TSharedPtr<SWidget> Held = Open.IsEmpty() ? nullptr : Open.Last()->HeldMouseCaptor();
	Out->SetStringField(TEXT("screen_holds"), Held.IsValid() ? Owner(Held) : TEXT("none"));
	return Out;
}

// --- input -----------------------------------------------------------------------------------------------------------

bool FScfgE2EDriver::Click(int64 Id, const FString& Name, const FString& Where)
{
	TSharedPtr<SWidget> Target;
	UScfgShowcaseGameInstance* G = Game.Get();
	if (Where == TEXT("game"))
	{
		if (G && G->GetHud().IsValid())
		{
			for (const SScfgShowcaseHud::FButtonView& B : G->GetHud()->Buttons())
			{
				if (B.Name == Name)
				{
					Target = B.Widget;
				}
			}
		}
	}
	else
	{
		const TArray<TSharedRef<SScfgScreen>> Open = SScfgScreen::OpenScreens();
		if (!Open.IsEmpty() && Open.Last()->FocusOrder().Contains(Name))
		{
			Target = Open.Last()->Control(Name);
		}
	}
	if (!Target.IsValid())
	{
		ReplyError(Id, FString::Printf(TEXT("no control %s shown in the %s"), *Name, *Where));
		return false;
	}
	if (!Target->IsEnabled())
	{
		ReplyError(Id, FString::Printf(TEXT("control %s is disabled"), *Name));
		return false;
	}
	const TSharedPtr<SWindow> Window = GameWindow();
	if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
	{
		ReplyError(Id, TEXT("no game window"));
		return false;
	}
	TWeakPtr<SWidget> WeakTarget = Target;
	TSharedRef<int32> Notches = MakeShared<int32>(0);
	TSharedRef<FVector2D> At = MakeShared<FVector2D>(FVector2D::ZeroVector);
	// 1. a control below the fold: the wheel over the scrolling body, one notch a frame, until it is in view
	Steps.Add([this, Id, WeakTarget, Notches]()
	{
		const TSharedPtr<SWidget> T = WeakTarget.Pin();
		if (!T.IsValid())
		{
			ReplyError(Id, TEXT("the control went away while scrolling to it"));
			bAbortSteps = true;
			return true;
		}
		const TSharedPtr<SWidget> Scroll = ScrollParent(T);
		if (!Scroll.IsValid())
		{
			return true;
		}
		const FSlateRect View = DesktopRect(*Scroll);
		const FSlateRect R = DesktopRect(*T);
		const FVector2D Centre = FVector2D(R.GetCenter());
		if (Centre.Y >= View.Top && Centre.Y <= View.Bottom)
		{
			return true;
		}
		if (*Notches == MAX_WHEEL_NOTCHES)
		{
			ReplyError(Id, FString::Printf(TEXT("control stays out of view after %d wheel notches"), *Notches));
			bAbortSteps = true;
			return true;
		}
		const FVector2D Over = FVector2D(View.GetCenter());
		MoveCursor(Over);
		FSlateApplication::Get().OnMouseWheel(Centre.Y > Over.Y ? -1.f : 1.f, Over);
		++*Notches;
		return false;
	});
	// 2. the cursor moves there (warped on the X server, then the move as SDL reports one)
	TSharedRef<FString> Under = MakeShared<FString>();
	Steps.Add([this, WeakTarget, At, Under]()
	{
		if (const TSharedPtr<SWidget> T = WeakTarget.Pin())
		{
			*At = FVector2D(DesktopRect(*T).GetCenter());
		}
		MoveCursor(*At);
		FSlateApplication& App = FSlateApplication::Get();
		// what Slate found under the cursor (the click's target, or what covers it)
		const TSharedPtr<FSlateUser> User = App.GetCursorUser();
		const TSharedPtr<SWidget> Last = User.IsValid() ? User->GetLastWidgetsUnderCursor().GetLastWidget().Pin() : nullptr;
		*Under = Last.IsValid() ? FString::Printf(TEXT("%s (%s)"), *Owner(Last), *Last->GetTypeAsString()) : TEXT("none");
		const TSharedPtr<SWindow> W = GameWindow();
		const FVector2D Cursor = App.GetCursorPos();
		const TSharedPtr<SWidget> T = WeakTarget.Pin();
		*Under += FString::Printf(TEXT(" [window at %s, cursor %s, paint rect %s, desktop rect %s]"),
			W.IsValid() ? *FVector2D(W->GetPositionInScreen()).ToString() : TEXT("-"), *Cursor.ToString(),
			T.IsValid() ? *T->GetPaintSpaceGeometry().GetLayoutBoundingRect().ToString() : TEXT("-"),
			T.IsValid() ? *DesktopRect(*T).ToString() : TEXT("-"));
		return true;
	});
	// 3. the button goes down, 4. and up, a frame apart
	TWeakPtr<SWindow> WeakWindow = Window;
	Steps.Add([WeakWindow, At]()
	{
		const TSharedPtr<SWindow> W = WeakWindow.Pin();
		FSlateApplication::Get().OnMouseDown(W.IsValid() ? W->GetNativeWindow() : nullptr, EMouseButtons::Left, *At);
		return true;
	});
	Steps.Add([this, Id, At, Notches, Under]()
	{
		FSlateApplication::Get().OnMouseUp(EMouseButtons::Left, *At);
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("under"), *Under);
		Out->SetArrayField(TEXT("at"), {MakeShared<FJsonValueNumber>(At->X), MakeShared<FJsonValueNumber>(At->Y)});
		Out->SetNumberField(TEXT("wheel_notches"), *Notches);
		Reply(Id, MakeShared<FJsonValueObject>(Out));
		return true;
	});
	return true;
}

void FScfgE2EDriver::ClickAt(int64 Id, const FVector2D& At)
{
	const TSharedPtr<SWindow> Window = GameWindow();
	if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
	{
		ReplyError(Id, TEXT("no game window"));
		return;
	}
	TWeakPtr<SWindow> WeakWindow = Window;
	TSharedRef<FString> Under = MakeShared<FString>();
	Steps.Add([this, At, Under]()
	{
		MoveCursor(At);
		const TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetCursorUser();
		const TSharedPtr<SWidget> Last = User.IsValid() ? User->GetLastWidgetsUnderCursor().GetLastWidget().Pin() : nullptr;
		*Under = Last.IsValid() ? FString::Printf(TEXT("%s (%s)"), *Owner(Last), *Last->GetTypeAsString()) : TEXT("none");
		return true;
	});
	Steps.Add([WeakWindow, At]()
	{
		const TSharedPtr<SWindow> W = WeakWindow.Pin();
		FSlateApplication::Get().OnMouseDown(W.IsValid() ? W->GetNativeWindow() : nullptr, EMouseButtons::Left, At);
		return true;
	});
	Steps.Add([this, Id, At, Under]()
	{
		FSlateApplication::Get().OnMouseUp(EMouseButtons::Left, At);
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("under"), *Under);
		Reply(Id, MakeShared<FJsonValueObject>(Out));
		return true;
	});
}

void FScfgE2EDriver::Key(const FString& Name, bool bRepeat)
{
	const FKey K(*Name);
	const uint32* KeyCode = nullptr;
	const uint32* CharCode = nullptr;
	FInputKeyManager::Get().GetCodesFromKey(K, KeyCode, CharCode);
	FSlateApplication& App = FSlateApplication::Get();
	App.OnKeyDown(KeyCode ? int32(*KeyCode) : 0, CharCode ? *CharCode : 0, bRepeat);
	App.OnKeyUp(KeyCode ? int32(*KeyCode) : 0, CharCode ? *CharCode : 0, bRepeat);
}

void FScfgE2EDriver::SetInputMode(const FString& Mode)
{
	UScfgShowcaseGameInstance* G = Game.Get();
	APlayerController* PC = G ? G->GetFirstLocalPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	if (Mode == TEXT("game"))
	{
		PC->SetShowMouseCursor(false);
		PC->SetInputMode(FInputModeGameOnly());
	}
	else
	{
		PC->SetShowMouseCursor(true);
		FInputModeGameAndUI M;
		M.SetLockMouseToViewportBehavior(Mode == TEXT("lock_always") ? EMouseLockMode::LockAlways : EMouseLockMode::DoNotLock);
		M.SetHideCursorDuringCapture(false);
		PC->SetInputMode(M);
	}
}

void FScfgE2EDriver::OnFocusChanging(const FFocusEvent& Event, const FWeakWidgetPath& OldPath,
	const TSharedPtr<SWidget>& OldWidget, const FWidgetPath& NewPath, const TSharedPtr<SWidget>& NewWidget)
{
	if (!bRecordFocus || Event.GetUser() != 0)
	{
		return;
	}
	const TCHAR* Cause = TEXT("other");
	switch (Event.GetCause())
	{
	case EFocusCause::Mouse: Cause = TEXT("mouse"); break;
	case EFocusCause::Navigation: Cause = TEXT("navigation"); break;
	case EFocusCause::SetDirectly: Cause = TEXT("set_directly"); break;
	case EFocusCause::Cleared: Cause = TEXT("cleared"); break;
	case EFocusCause::OtherWidgetLostFocus: Cause = TEXT("other_widget_lost_focus"); break;
	case EFocusCause::WindowActivate: Cause = TEXT("window_activate"); break;
	default: break;
	}
	FocusEvents.Add(Str(FString::Printf(TEXT("%s->%s (%s)"), *Owner(OldWidget), *Owner(NewWidget), Cause)));
}

void FScfgE2EDriver::ActivationCycle(int64 Id)
{
	const TSharedPtr<SWindow> Window = GameWindow();
	if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
	{
		ReplyError(Id, TEXT("no game window"));
		return;
	}
	struct FCycle
	{
		TSharedPtr<SWindow> Other;
		int32 Frames = 0;
		bool bDeactivated = false;
		bool bReactivated = false;
		TSharedPtr<FJsonObject> Before;
	};
	const TSharedRef<FCycle> C = MakeShared<FCycle>();
	C->Before = InputState();
	FocusEvents.Reset();
	bRecordFocus = true;
	TWeakPtr<SWindow> WeakGame = Window;
	// 1. another native window (an X window of its own under Xvfb) opens and takes the activation
	Steps.Add([C]()
	{
		C->Other = SNew(SWindow)
			.Title(FText::FromString(TEXT("scfg e2e: another window")))
			.ClientSize(FVector2D(320, 200))
			.ScreenPosition(FVector2D(40, 40))
			.AutoCenter(EAutoCenter::None)
			.FocusWhenFirstShown(true)
			.ActivationPolicy(EWindowActivationPolicy::Always)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Another window")))
			];
		FSlateApplication::Get().AddWindow(C->Other.ToSharedRef(), true);
		return true;
	});
	// 2. wait until the game window is no longer active
	Steps.Add([C, WeakGame]()
	{
		const TSharedPtr<SWindow> G = WeakGame.Pin();
		C->bDeactivated = G.IsValid() && !G->IsActive();
		return C->bDeactivated || ++C->Frames > MAX_WAIT_FRAMES;
	});
	// 3. it closes: the engine activates the game window again
	Steps.Add([C]()
	{
		C->Frames = 0;
		if (C->Other.IsValid())
		{
			FSlateApplication::Get().RequestDestroyWindow(C->Other.ToSharedRef());
			C->Other.Reset();
		}
		return true;
	});
	Steps.Add([C, WeakGame]()
	{
		const TSharedPtr<SWindow> G = WeakGame.Pin();
		C->bReactivated = G.IsValid() && G->IsActive();
		return C->bReactivated || ++C->Frames > MAX_WAIT_FRAMES;
	});
	// 4. five frames later (the sheet's Tick runs on every painted frame)
	Steps.Add([C]() { return ++C->Frames > 5; });
	Steps.Add([this, Id, C]()
	{
		bRecordFocus = false;
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("deactivated"), C->bDeactivated);
		Out->SetBoolField(TEXT("reactivated"), C->bReactivated);
		Out->SetArrayField(TEXT("focus_events"), FocusEvents);
		Out->SetObjectField(TEXT("before"), C->Before);
		Out->SetObjectField(TEXT("after"), InputState());
		Reply(Id, MakeShared<FJsonValueObject>(Out));
		return true;
	});
}

// --- commands --------------------------------------------------------------------------------------------------------

void FScfgE2EDriver::Run(int64 Id, const TSharedRef<FJsonObject>& Command)
{
	FString Op;
	Command->TryGetStringField(TEXT("op"), Op);
	UScfgShowcaseGameInstance* G = Game.Get();
	if (!G)
	{
		ReplyError(Id, TEXT("the game instance is gone"));
		return;
	}
	if (Op == TEXT("snapshot"))
	{
		Reply(Id, MakeShared<FJsonValueObject>(Snapshot()));
	}
	else if (Op == TEXT("click"))
	{
		FString Name, Where = TEXT("screen");
		Command->TryGetStringField(TEXT("name"), Name);
		Command->TryGetStringField(TEXT("in"), Where);
		Click(Id, Name, Where);
	}
	else if (Op == TEXT("click_at"))
	{
		double X = 0, Y = 0;
		if (!Command->TryGetNumberField(TEXT("x"), X) || !Command->TryGetNumberField(TEXT("y"), Y))
		{
			ReplyError(Id, TEXT("click_at needs x and y"));
			return;
		}
		ClickAt(Id, FVector2D(X, Y));
	}
	else if (Op == TEXT("key"))
	{
		FString Name;
		bool bRepeat = false;
		Command->TryGetStringField(TEXT("key"), Name);
		Command->TryGetBoolField(TEXT("repeat"), bRepeat);
		if (!FKey(*Name).IsValid())
		{
			ReplyError(Id, FString::Printf(TEXT("unknown key %s"), *Name));
			return;
		}
		Key(Name, bRepeat);
		Reply(Id, nullptr);
	}
	else if (Op == TEXT("type"))
	{
		FString Text;
		Command->TryGetStringField(TEXT("text"), Text);
		for (const TCHAR Ch : Text)
		{
			FSlateApplication::Get().OnKeyChar(Ch, false);
		}
		Reply(Id, nullptr);
	}
	else if (Op == TEXT("owns"))
	{
		FString Item;
		Command->TryGetStringField(TEXT("item"), Item);
		Reply(Id, MakeShared<FJsonValueBoolean>(G->Scfg()->Owns(Item)));
	}
	else if (Op == TEXT("saved_dir"))
	{
		Reply(Id, Str(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())));
	}
	else if (Op == TEXT("input"))
	{
		Reply(Id, MakeShared<FJsonValueObject>(InputState()));
	}
	else if (Op == TEXT("input_mode"))
	{
		FString Mode;
		Command->TryGetStringField(TEXT("mode"), Mode);
		if (Mode != TEXT("ui") && Mode != TEXT("game") && Mode != TEXT("lock_always"))
		{
			ReplyError(Id, FString::Printf(TEXT("unknown input mode %s"), *Mode));
			return;
		}
		SetInputMode(Mode);
		// the input mode's Slate operations (focus, capture, lock) apply at once; the state is read a frame later
		Steps.Add([this, Id]()
		{
			Reply(Id, MakeShared<FJsonValueObject>(InputState()));
			return true;
		});
	}
	else if (Op == TEXT("open"))
	{
		FString What;
		Command->TryGetStringField(TEXT("what"), What);
		if (What == TEXT("unlock"))
		{
			G->Unlock();
		}
		else if (What == TEXT("tip"))
		{
			G->Tip();
		}
		else if (What == TEXT("restore"))
		{
			G->Restore();
		}
		else
		{
			ReplyError(Id, FString::Printf(TEXT("unknown action %s"), *What));
			return;
		}
		Reply(Id, nullptr);
	}
	else if (Op == TEXT("activation_cycle"))
	{
		ActivationCycle(Id);
	}
	else if (Op == TEXT("wish"))
	{
		Reply(Id, MakeShared<FJsonValueObject>(Wish()));
	}
	else if (Op == TEXT("screenshot"))
	{
		FString Path;
		Command->TryGetStringField(TEXT("path"), Path);
		if (Path.IsEmpty() || FPaths::IsRelative(Path))
		{
			ReplyError(Id, TEXT("screenshot needs an absolute path"));
			return;
		}
		IFileManager::Get().Delete(*Path, false, true, true);
		FScreenshotRequest::RequestScreenshot(Path, /*bInShowUI*/ true, /*bAddFilenameSuffix*/ false);
		TSharedRef<int32> Frames = MakeShared<int32>(0);
		Steps.Add([this, Id, Path, Frames]()
		{
			if (IFileManager::Get().FileSize(*Path) > 0)
			{
				Reply(Id, Str(Path));
				return true;
			}
			if (++*Frames > MAX_WAIT_FRAMES)
			{
				ReplyError(Id, TEXT("the screenshot was not written"));
				return true;
			}
			return false;
		});
	}
	else if (Op == TEXT("quit"))
	{
		Reply(Id, nullptr);
		Flush();
		bQuitting = true;
		FPlatformMisc::RequestExit(false, TEXT("ScfgE2EDriver"));
	}
	else
	{
		ReplyError(Id, FString::Printf(TEXT("unknown op %s"), *Op));
	}
}

#endif  // !UE_BUILD_SHIPPING
