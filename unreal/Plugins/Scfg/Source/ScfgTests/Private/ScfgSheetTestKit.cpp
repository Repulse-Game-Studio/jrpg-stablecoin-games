// No Python twin: the Slate screen tests' rig (see ScfgSheetTestKit.h).
#include "ScfgSheetTestKit.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/PlatformMisc.h"
#include "Input/HittestGrid.h"
#include "Interfaces/IPluginManager.h"
#include "Layout/WidgetPath.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SVirtualWindow.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ScfgSheetTests
{

void SGameSink::Construct(const FArguments& InArgs)
{
}

FReply SGameSink::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	Got.Add(InKeyEvent.GetKey().ToString());
	return FReply::Handled();
}

FReply SGameSink::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent)
{
	Got.Add(FString(TEXT("char:")) + InCharacterEvent.GetCharacter());
	return FReply::Handled();
}

FReply SGameSink::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	Got.Add(TEXT("mouse down"));
	return FReply::Handled();
}

FReply SGameSink::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	Got.Add(TEXT("mouse up"));
	return FReply::Handled();
}

FReply SGameSink::OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent)
{
	Got.Add(TEXT("analog:") + InAnalogInputEvent.GetKey().ToString());
	return FReply::Handled();
}

FSheetRig::FSheetRig(FAutomationTestBase& Test, FVector2D Size)
{
	bReady = FSlateApplication::IsInitialized();
	if (!bReady)
	{
		Test.AddError(TEXT("FSlateApplication is not initialized: the screens cannot be tested in this process"));
		return;
	}
	FSlateApplication& App = FSlateApplication::Get();
	FocusBefore = App.GetUserFocusedWidget(0);
	SAssignNew(Window, SVirtualWindow).Size(Size);
	SAssignNew(Layer, SOverlay);
	SAssignNew(Game, SGameSink);
	Layer->AddSlot(0)[Game.ToSharedRef()];
	Window->SetContent(Layer.ToSharedRef());
	App.RegisterVirtualWindow(Window.ToSharedRef());
	Instance.Reset(NewObject<UGameInstance>(GEngine));
	Instance->InitializeStandalone();
	GameWorld = Instance->GetWorld();
	Viewport.Reset(NewObject<UGameViewportClient>(GEngine));  // its ClassWithin is UEngine
	// The engine gives a game's client its overlay in UGameEngine::CreateGameViewportWidget; a test has no game window.
	// 5.8 deprecates this setter for a "Bind" that UGameViewportClient does not have (engine-facts.md); 5.6 and 5.7
	// have it undeprecated.
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Viewport->SetViewportOverlayWidget(Window, Layer.ToSharedRef());
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	Instance->GetWorldContext()->GameViewport = Viewport.Get();
	Host = MakeShared<FScfgDefaultSheetHost>(Instance.Get());
	Frame();
	FWidgetPath Path;
	App.FindPathToWidget(Game.ToSharedRef(), Path);
	App.SetAllUserFocus(Path, EFocusCause::SetDirectly);
	Test.TestTrue(TEXT("the game's widget has the focus to begin with"), Focused() == Game);
}

FSheetRig::~FSheetRig()
{
	if (!bReady)
	{
		return;
	}
	W.Drop();
	Host.Reset();
	UWorld* World = GameWorld.Get();
	FWorldContext* Context = World ? GEngine->GetWorldContextFromWorld(World) : nullptr;
	if (Context)
	{
		Context->GameViewport = nullptr;
	}
	if (Instance.IsValid() && !bInstanceShutDown)
	{
		Instance->Shutdown();
	}
	if (Context)
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
	Instance.Reset();
	Viewport.Reset();
	FSlateApplication& App = FSlateApplication::Get();
	App.ReleaseAllPointerCapture(0);
	App.UnregisterVirtualWindow(Window.ToSharedRef());
	FWidgetPath Before;
	if (FocusBefore.IsValid() && App.FindPathToWidget(FocusBefore.ToSharedRef(), Before))
	{
		App.SetAllUserFocus(Before, EFocusCause::SetDirectly);
	}
	else
	{
		App.ClearAllUserFocus(EFocusCause::SetDirectly);
	}
}

FScfgFacade& FSheetRig::Started(FAutomationTestBase& Test)
{
	FScfgFacade& S = W.Started(Test);
	S.SheetHost = nullptr;
	S.DefaultSheetHost = Host;
	return S;
}

void FSheetRig::Frame(int32 N, float Dt)
{
	if (!bReady)
	{
		return;
	}
	for (int32 I = 0; I < N; ++I)
	{
		if (W.Facade.IsValid() && !W.Facade->IsShuttingDown())
		{
			W.Pump();
		}
		Now += Dt;
		const FVector2f Size = FVector2f(Window->GetSizeInScreen());
		Window->SlatePrepass(FSlateApplication::Get().GetApplicationScale());
		FHittestGrid& Grid = Window->GetHittestGrid();
		Grid.SetHittestArea(FVector2f::ZeroVector, Size);
		Grid.Clear();
		FPaintArgs Args(nullptr, Grid, FVector2f::ZeroVector, Now, Dt);
		FSlateWindowElementList Elements(Window);
		Window->Paint(Args, FGeometry::MakeRoot(Size, FSlateLayoutTransform()), FSlateRect(FVector2f::ZeroVector, Size),
			Elements, 0, FWidgetStyle(), true);
	}
}

void FSheetRig::Pass()
{
	W.Pass();
	Frame(2);
}

bool FSheetRig::Key(const FKey& InKey, bool bShift, bool bRepeat)
{
	const FModifierKeysState Mods(bShift, false, false, false, false, false, false, false, false);
	FSlateApplication& App = FSlateApplication::Get();
	const bool bHandled = App.ProcessKeyDownEvent(FKeyEvent(InKey, Mods, 0, bRepeat, 0, 0));
	App.ProcessKeyUpEvent(FKeyEvent(InKey, Mods, 0, false, 0, 0));
	Frame();
	return bHandled;
}

void FSheetRig::Type(const FString& Text)
{
	for (const TCHAR C : Text)
	{
		FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(C, FModifierKeysState(), 0, false));
	}
	Frame();
}

void FSheetRig::Stick(const FKey& Axis, float Value)
{
	FSlateApplication& App = FSlateApplication::Get();
	App.ProcessAnalogInputEvent(FAnalogInputEvent(Axis, FModifierKeysState(), 0, false, 0, 0, Value));
	App.ProcessAnalogInputEvent(FAnalogInputEvent(Axis, FModifierKeysState(), 0, false, 0, 0, 0.f));
	Frame();
}

void FSheetRig::Click(const TSharedRef<SWidget>& Widget)
{
	FSlateApplication& App = FSlateApplication::Get();
	const TSharedPtr<FSlateUser> User = App.GetUser(0);
	const FVector2D At = FVector2D(Widget->GetTickSpaceGeometry().GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
	const uint32 Pointer = FSlateApplication::CursorPointerIndex;
	const FPointerEvent Down(0, Pointer, At, At, TSet<FKey>{EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0.f,
		FModifierKeysState());
	const FPointerEvent Up(0, Pointer, At, At, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState());
	// the pointer moves there first (a button clicks only while hovered)
	const FPointerEvent Move(0, Pointer, At, At - FVector2D(1, 1), TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState());
	if (User.IsValid() && User->HasCapture(Pointer))
	{
		App.ProcessMouseMoveEvent(Move);
	}
	else
	{
		TArray<FWidgetAndPointer> Bubble = Window->GetHittestGrid().GetBubblePath(At, 0.f, false, 0);
		App.RoutePointerMoveEvent(FWidgetPath(Bubble), Move, false);
	}
	for (const bool bDown : {true, false})
	{
		const FPointerEvent& Event = bDown ? Down : Up;
		if (User.IsValid() && User->HasCapture(Pointer))
		{
			bDown ? App.ProcessMouseButtonDownEvent(nullptr, Event) : App.ProcessMouseButtonUpEvent(Event);
		}
		else
		{
			TArray<FWidgetAndPointer> Bubble = Window->GetHittestGrid().GetBubblePath(At, 0.f, false, 0);
			const FWidgetPath Path(Bubble);
			bDown ? App.RoutePointerDownEvent(Path, Event) : App.RoutePointerUpEvent(Path, Event);
		}
	}
	Frame();
}

void FSheetRig::GameCapturesTheMouse()
{
	FWidgetPath Path;
	FSlateApplication& App = FSlateApplication::Get();
	if (App.FindPathToWidget(Game.ToSharedRef(), Path))
	{
		App.ProcessReply(Path, FReply::Handled().CaptureMouse(Game.ToSharedRef()), nullptr, nullptr, 0);
	}
}

TSharedPtr<SWidget> FSheetRig::MouseCaptor() const
{
	const TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetUser(0);
	return User.IsValid() && User->HasCursorCapture() ? User->GetCursorCaptor() : nullptr;
}

TSharedPtr<SWidget> FSheetRig::Focused() const
{
	return FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetUserFocusedWidget(0) : nullptr;
}

TSharedPtr<SScfgPaymentSheet> FSheetRig::Sheet() const
{
	if (!Host.IsValid())
	{
		return nullptr;
	}
	const TArray<TSharedRef<SScfgScreen>> All = Host->Screens();
	for (int32 I = All.Num() - 1; I >= 0; --I)
	{
		if (All[I]->GetType() == FName(TEXT("SScfgPaymentSheet")))
		{
			return StaticCastSharedRef<SScfgPaymentSheet>(All[I]);
		}
	}
	return nullptr;
}

TSharedPtr<SScfgRestoreScreen> FSheetRig::RestoreScreen() const
{
	if (!Host.IsValid())
	{
		return nullptr;
	}
	const TArray<TSharedRef<SScfgScreen>> All = Host->Screens();
	for (int32 I = All.Num() - 1; I >= 0; --I)
	{
		if (All[I]->GetType() == FName(TEXT("SScfgRestoreScreen")))
		{
			return StaticCastSharedRef<SScfgRestoreScreen>(All[I]);
		}
	}
	return nullptr;
}

bool FSheetRig::HasFocus(const SScfgScreen& Screen, const FString& Key) const
{
	const TSharedPtr<SWidget> C = Screen.Control(Key);
	const TSharedPtr<SWidget> F = Focused();
	return C.IsValid() && F.IsValid() && (C == F || C->HasUserFocusedDescendants(0));
}

bool FSheetRig::FocusInside(const SScfgScreen& Screen) const
{
	const TSharedPtr<SWidget> F = Focused();
	return F.IsValid() && (F.Get() == &Screen || Screen.HasUserFocusedDescendants(0));
}

bool FSheetRig::KeyTo(const SScfgScreen& Screen, const FString& Key, const FKey& Direction, int32 Max)
{
	for (int32 I = 0; I < Max && !HasFocus(Screen, Key); ++I)
	{
		this->Key(Direction);
	}
	return HasFocus(Screen, Key);
}

int32 FSheetRig::NumLayered() const
{
	return Layer.IsValid() ? Layer->GetNumWidgets() - 1 : 0;
}

FString MessagesJson()
{
	TArray<FString> Candidates;
	const FString Env = FPlatformMisc::GetEnvironmentVariable(TEXT("SCFG_VECTORS"));
	if (!Env.IsEmpty())
	{
		Candidates.Add(Env / TEXT("messages.json"));
	}
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Scfg")))
	{
		FString Dir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		for (int32 Up = 0; Up < 8 && !Dir.IsEmpty(); ++Up)
		{
			Candidates.Add(Dir / TEXT("conformance") / TEXT("vectors") / TEXT("messages.json"));
			const FString Parent = FPaths::GetPath(Dir);
			if (Parent == Dir)
			{
				break;
			}
			Dir = Parent;
		}
	}
	for (const FString& Path : Candidates)
	{
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			return Text;
		}
	}
	return FString();
}

bool MatchesTemplate(const FString& Text, const FString& Template)
{
	// the parts between the "%s"s, the empty ones at the ends included (ParseIntoArray drops a trailing one)
	TArray<FString> Parts;
	for (int32 From = 0;;)
	{
		const int32 At = Template.Find(TEXT("%s"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (At == INDEX_NONE)
		{
			Parts.Add(Template.Mid(From));
			break;
		}
		Parts.Add(Template.Mid(From, At - From));
		From = At + 2;
	}
	if (Parts.Num() <= 1)
	{
		return Text == Template;
	}
	// FString's StartsWith / EndsWith are false for an empty part
	const FString& Head = Parts[0];
	const FString& Tail = Parts.Last();
	if (Text.Len() < Head.Len() + Tail.Len() || !Text.Left(Head.Len()).Equals(Head, ESearchCase::CaseSensitive) ||
		!Text.Right(Tail.Len()).Equals(Tail, ESearchCase::CaseSensitive))
	{
		return false;
	}
	int32 At = Parts[0].Len();
	const int32 End = Text.Len() - Parts.Last().Len();
	for (int32 I = 1; I < Parts.Num() - 1; ++I)
	{
		const int32 Found = Text.Find(Parts[I], ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
		if (Found == INDEX_NONE || Found + Parts[I].Len() > End)
		{
			return false;
		}
		At = Found + Parts[I].Len();
	}
	return At <= End;
}

}  // namespace ScfgSheetTests

#endif  // WITH_DEV_AUTOMATION_TESTS
