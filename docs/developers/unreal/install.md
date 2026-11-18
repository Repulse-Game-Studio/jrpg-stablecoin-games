# Install (Unreal)

Accept **USDC** and **USDT** tips and unlock purchases in your Unreal game, paid **directly to your own wallet**.
There is no server, no account and no middleman in the payment: the plugin watches the blockchain itself. It is a code plugin,
`Scfg`, for **UE 5.6 or newer**, in C++ and Blueprint projects. Each release has a zip for each of UE 5.6, 5.7 and
5.8. It has been tested on UE 5.8.4 only, in Linux desktop builds; 5.6 and 5.7 are untested, and so are Windows, macOS
and mobile builds (see [Platforms](api.md#platforms)).

Chains: Ethereum, Arbitrum, Polygon (USDC and USDT), Base (USDC), Arc (USDC), Solana (USDC and USDT), plus any EVM
chain you add yourself.

> Without a server, unlocks are honour-grade: a determined player can bypass them. Do not sell anything you
> cannot afford to lose to a bypass.

## From the release zip

The Unreal plugin first ships in release 0.3.0. Until that release exists there is no zip; you can still
[try the showcase](#try-the-showcase-first) from a clone of the repository.

1. Download the zip for your engine version, `scfg-unreal-<version>-ue<5.x>.zip` (`-ue5.6`, `-ue5.7` or `-ue5.8`),
   from the [latest release](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/releases/latest). Each
   zip's `Scfg.uplugin` declares that engine version, so the editor does not warn that the plugin was made for
   another one. Only the 5.8 zip has been tested (on UE 5.8.4); the 5.6 and 5.7 zips hold the same source.
2. Unzip it and copy the `Scfg` folder from inside it into your project's `Plugins` folder (create it next to your
   `.uproject` if it does not exist), so that you have `<YourProject>/Plugins/Scfg/Scfg.uplugin`.
3. Open the project. The zip holds source code only, so the editor builds the plugin the first time: when it says
   that modules are missing or were built with a different engine version, let it rebuild them. That needs the C++
   compiler Unreal uses on your platform (Visual Studio on Windows, Xcode on macOS, the engine's clang toolchain on
   Linux). In a C++ project you can also build from your IDE as usual.
4. Check **Edit > Plugins**: **Stablecoin for Games (SCFG)**, in the **Monetization** category, is enabled (a plugin
   in the project's `Plugins` folder is enabled by default). Restart the editor if you changed it.

**C++ projects.** Add the module to your game module's `Build.cs` to call the API:

```csharp
PrivateDependencyModuleNames.Add("Scfg");
```

**Blueprint projects.** Nothing else to add: the nodes are in the **SCFG** category. A Blueprint-only project still
needs the compiler for step 3. If the editor will not build the plugin for it, add any C++ class (**Tools > New C++
Class**), which turns the project into a C++ project, and open it again.

The plugin from the 5.8 zip builds on its own (`RunUAT BuildPlugin` on Linux: the editor, and a Development and a
Shipping game). Copied into the `Plugins` folder of a fresh C++ project made from the Blank template, it packaged as a
Linux Development game (BuildCookRun), the game started with the plugin loaded and its SCFG project settings applied,
and the checkout page's files were staged with it. That project changed nothing else but one packaging setting, the
zen store turned off for the cooked output (Packaging > "Use Zen Server as cooked output store", for the test
machine's network isolation); the default, with the zen store on, has not been tried. Nor have opening such a project
in the editor, and Blueprint-only projects.

## Try the showcase first

The repository's development project, `unreal/Project/ScfgDev`, is a one-screen tour of the whole API. It has three
unlocks (one with a pay-what-you-want amount), a tip jar, Restore, Export and Clear, and next to them it shows what
the plugin knows (owned items, tips, pending payments, receipts) and a live log of every result and event. It is
configured from the project settings, and from code when started with `-scfg-showcase-code`. Its
`Source/ScfgDev/ScfgShowcase.cpp` is written to be read top to bottom.

How to build and run it, in the editor or as a game, and how to pay on a testnet, is in its
[README](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/blob/main/unreal/Project/ScfgDev/README.md).
It needs UE 5.8 and runs on testnets, so no real money moves. Put your own testnet wallet in its project settings to
watch a real test payment arrive. Get test USDC from [Circle's faucet](https://faucet.circle.com/).

## Your first payment in C++

Everything goes through `UScfgSubsystem`, a game-instance subsystem: one per game instance, kept across level loads.

```cpp
// MyGameInstance.cpp
#include "MyGameInstance.h"

#include "Kismet/GameplayStatics.h"
#include "ScfgSubsystem.h"

void UMyGameInstance::Init()
{
    Super::Init();   // the subsystems exist after this
    UScfgSubsystem* Scfg = GetSubsystem<UScfgSubsystem>();
    if (!Scfg)
    {
        return;   // a dedicated server has no SCFG subsystem
    }
    FScfgOptions Options;
    Options.Wallet = TEXT("0xYourWalletAddressHere");
    Options.bTestnet = true;
    Scfg->Configure(Options);   // "" or the error code it logged

    FScfgItem Item;
    Item.Code = 1;
    Item.Price = TEXT("4.99");
    Item.Token = TEXT("USDC");
    Item.Title = TEXT("Chapter 2");
    Scfg->AddItem(TEXT("chapter2"), Item);
}

void UMyGameInstance::UnlockChapterTwo()
{
    UScfgSubsystem* Scfg = GetSubsystem<UScfgSubsystem>();
    if (!Scfg)
    {
        return;
    }
    FScfgPayRequest Request;
    Request.Item = TEXT("chapter2");
    UGameplayStatics::SetGamePaused(this, true);   // the sheet does not stop your game, see the API page
    TWeakObjectPtr<UMyGameInstance> Weak(this);
    Scfg->PayAsync(EScfgPayKind::Unlock, Request, [Weak](const FScfgPaymentResult& Result)
    {
        UMyGameInstance* Self = Weak.Get();
        if (!Self)
        {
            return;
        }
        UGameplayStatics::SetGamePaused(Self, false);
        if (Result.Status == EScfgPaymentStatus::Paid || Result.Status == EScfgPaymentStatus::AlreadyOwned)
        {
            Self->StartChapterTwo();
        }
    });
}
```

`PayAsync` shows the payment sheet and calls you once, when the player closes it (or at once, for `AlreadyOwned` and
`Failed`). `bTestnet = true` makes the sheet offer test networks, so no real money moves. Set it to `false` when you
go live, and replace the placeholder wallet with your own. Check `Scfg->Owns(TEXT("chapter2"))` wherever the game
gates the content: it is instant and works offline.

Configure before payments start, on the first frame after a configuration: in your game instance's `Init()` after
`Super::Init()`, as above, or in the first level's `BeginPlay`. A `Configure` after that is refused with
`configure.after_start`.

## Your first payment in Blueprint

1. **Configure** (or use the project settings below and skip this step). In the first level's Level Blueprint, on
   **Event BeginPlay**: **Get Subsystem** (category SCFG) and **Is Valid** (it is not valid on a dedicated server),
   then the subsystem's **Configure** with an `FScfgOptions` (**Wallet** set, **Testnet** ticked), then **Add Item**
   with the item id `chapter2` and an `FScfgItem` (**Code** 1, **Price** `4.99`, **Token** `USDC`, **Title**
   `Chapter 2`). Not in a Game Instance Blueprint's **Event Init**: it runs before the game instance creates its
   subsystems, so **Get Subsystem** is not valid there yet.
2. **Pay.** On your unlock button: **Set Game Paused** (ticked), then the async node **Pay (SCFG)** with **Kind**
   `Unlock` and a **Request** whose **Item** is `chapter2`. Its **On Resolved** pin fires once, when the player closes
   the sheet: **Set Game Paused** (unticked), then a **Switch** on the result's **Status**; on **Paid** and
   **Already Owned**, open the chapter.
3. **Gate the content** with the pure node **Owns (SCFG)** wherever the game checks it.

Restore has its own async node, **Restore (SCFG)**. The other nodes are listed in the
[API reference](api.md#blueprint).

## Without code: the project settings

Open **Edit > Project Settings > Plugins > SCFG**. The settings are saved to your project's `Config/DefaultGame.ini`
(section `[/Script/Scfg.ScfgSettings]`), so commit that file.

The panel has three sections. **Configure** holds the options of `Configure`:

| Setting | Meaning |
|---|---|
| **Wallet** | Your EVM payout address (`0x...`). It receives the payments on Ethereum, Arbitrum, Polygon, Base and Arc |
| **Solana Wallet** | Optional, your Solana payout address. At least one of the two wallets is required: without one, the settings are not applied at all |
| **Testnet** | On by default in the settings. Leave it on while you test; turn it off for the release |
| **Chains**, **Tokens**, **Methods**, **Rpc Urls**, **Confirmations**, **Previous Wallets** | As the fields of `FScfgOptions` (see [Configure](api.md#configure)). Empty: the defaults |
| **Logo** | An image file in a folder under `Content/`, relative to the project folder (`Content/Scfg/logo.png`): png, jpg or webp. Add that folder (`Scfg`) to **Project Settings > Packaging > Additional Non-Asset Directories to Package**, or a packaged game cannot find the logo and its `Configure` fails (see [The logo in a packaged game](api.md#the-logo-in-a-packaged-game)) |
| **Accent** | The accent color of the sheet and the checkout page, `#rrggbb`. Empty: the default |

**Chains** lists your own EVM chains, each a **Key** and the fields of an `FScfgChain` (see
[AddChain](api.md#addchain)). **Items** has one row per unlock: **Id**, **Code** (1-99, different for each item,
never changed after release), **Price** (`4.99`), **Token** (`USDC` by default), **Title** and an optional **Min
Price**.

Press **Validate** at the top of the panel. It runs the same checks the game will run, without starting anything, and
shows the result as a notification and on the **SCFG** page of the **Message Log**: one line per problem with its
error code (`item.invalid_price: item "chapter2": ...`), or a line saying the settings are valid, plus a warning when
the logo will not be packaged (it is not in a folder under `Content/` listed in Additional Non-Asset Directories to
Package), when they would not be applied (no wallet) or when they use the test networks.

When the game instance starts, the plugin applies the settings through the same calls a game makes: `AddChain` for
each chain row, `Configure`, then `AddItem` for each item row. So `Owns (SCFG)` and `Pay (SCFG)` work from your first
level's **BeginPlay**. Settings that fail are reported in the Output Log (`scfg: the SCFG settings (Project Settings >
Plugins > SCFG) were not applied (<code>). Press Validate there.`) and never crash the game.

Code stays in charge: a `Configure` from your code made after the settings were applied replaces them, items and
chains included, until payments have started (the first frame). An `AddItem` from code adds to the settings'
items. An `AddChain` from code sets the settings aside (with a warning) for the `Configure` that must follow it,
since the settings cannot name a chain added from code. The [API reference](api.md#project-settings) has the exact
rules.

## Before you go live

1. Pay once with each method you enabled (browser wallet, phone wallet by QR, manual transfer) on a testnet, to
   **your own wallet**, from a second wallet of yours. Get test USDC from
   [Circle's faucet](https://faucet.circle.com/).
2. Quit the game in the middle of a payment, start it again and check that the purchase still completes.
3. Call `ClearLocalData()`, then use **Restore** with the transaction hash.
4. In every build but Shipping (the editor included) the sheet shows **DEV MODE: paying to** and the start and end
   of the payout address. Check that they match yours. The manual transfer step shows the full address.
5. Set `bTestnet = false` (or untick **Testnet** in the project settings).
6. If you take Solana payments, receive any amount of each token on your mainnet Solana wallet once first. Until
   then the Output Log warns (`scfg: your wallet can't receive USDT on solana yet ...`) and players are not offered
   that token.
7. Package the game and pay once from the packaged build. A Shipping game has not been run in this preview yet. If
   you set a logo, check that **Validate** gives no packaging warning (see
   [Logo](api.md#the-logo-in-a-packaged-game)).
8. Check that the packaged game verifies TLS. The plugin refuses its RPC requests when the engine cannot verify
   certificates (see [Network and privacy](api.md#network-and-privacy)). For a Linux game it stages the engine's
   certificate bundle itself (`Engine/Content/Certificates/ThirdParty/cacert.pem`, in the staged game's
   `Engine/Content/`): keep it there when you repackage or trim the staged files. Do not set
   `[/Script/Engine.NetworkSettings] n.VerifyPeer=False`. Setting `n.VerifyPeer=True` in your `DefaultEngine.ini`
   makes the rest of your game's HTTPS fail closed too, and makes UAT stage the engine's bundle in the pak on every
   platform (the tested setup). Windows and macOS use the system's certificate store
   (untested in this preview). If the packaged game's Output Log says
   `scfg: refusing https:// RPC requests: TLS certificate verification unavailable`, no payment can be checked
   until this is fixed.

## What you get

```
MyGame/
├── MyGame.uproject
└── Plugins/
    └── Scfg/
        ├── Scfg.uplugin
        ├── Resources/Web/       (the browser checkout page, staged with a packaged desktop game)
        └── Source/
            ├── ScfgCore/        (the engine-independent payment core, plain C++20)
            ├── Scfg/            (the runtime module: UScfgSubsystem, the Blueprint nodes, the value types,
            │                     the Slate payment sheet and restore screen, the localhost checkout page)
            └── ScfgEditor/      (the editor module: the Validate button of the project settings)
```

Receipts and pending payments are stored in `Saved/Scfg/state.json` (the engine's `FPaths::ProjectSavedDir()`),
global to the install and never inside your save games, so loading an older save cannot lose a purchase.

## Next

Read the [API reference](api.md), in particular [Pausing and input](api.md#pausing-and-input) and
[Where results go](api.md#where-results-go).
