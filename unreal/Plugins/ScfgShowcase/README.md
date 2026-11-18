# SCFG Unreal showcase

`ScfgShowcase` is an optional sample plugin: a one-screen showcase of the Stablecoin for Games plugin (`Scfg`). Every button calls one function of the `UScfgSubsystem` game-instance subsystem, and the right-hand column shows what comes back. It is the twin of the Godot showcase and of Unity's Showcase sample. It uses the plugin's public API only. Its screen and logic are in `Source/ScfgShowcase/Private/ScfgShowcase.cpp`, written to be read top to bottom.

| On screen | Calls |
|---|---|
| **Unlocks**: three items, the supporter pack also at a pay-what-you-want amount | `PayAsync(EScfgPayKind::Unlock, Request)` with `Request.Item = Id`, and `Request.Amount = "5.00"` |
| **Tip jar**: three suggested amounts | `PayAsync(EScfgPayKind::Tip, Request)` with `Request.Amount = "1.00"`, `Request.Token = "USDC"` |
| **Restore purchases** | `RestoreAsync()` |
| **Export receipts** | `ExportReceipts("")` (writes `Saved/scfg-receipts.json`) |
| **Clear local data**, a testing helper | `ClearLocalData()` |
| **Notify: on / off** | `SetNotify()`, `GetNotify()` |
| **What SCFG knows** | where the configuration came from, `Supported()`, `Owns()`, `TipTotal()`, `HasTipped()`, `Pending()`, `Receipts()` |
| **Event log** | every result and the events `OnPaymentResolved` and `OnReceiptsChanged` |

It runs on testnets only (Sepolia, Arbitrum Sepolia, Polygon Amoy, Base Sepolia, Arc Testnet, and Solana Devnet with a Solana wallet), so no real money moves. The payments go to your wallet, and the showcase has none of its own: with no wallet set it configures nothing and says so on its screen ("No wallet is set, so SCFG is not configured ..."). It never pays a placeholder address.

It has no level of its own. It is a game instance class, `UScfgShowcaseGameInstance`, that draws a Slate HUD on whatever map is loaded and picks its own game mode; the engine's empty `/Engine/Maps/Entry` is enough, so the plugin holds no assets.

## Install it

The showcase is the release's `scfg-unreal-showcase-<version>.zip`, next to the plugin zips on the [release page](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/releases). Try it in a fresh or spare C++ project, not in your game: it replaces the project's game instance.

1. Install the plugin first: unzip `scfg-unreal-<version>-ue<5.x>.zip` and copy its `Scfg` folder into your project's `Plugins` folder.
2. Unzip the showcase zip and copy its `ScfgShowcase` folder next to it, so that you have `<YourProject>/Plugins/Scfg/Scfg.uplugin` and `<YourProject>/Plugins/ScfgShowcase/ScfgShowcase.uplugin`.
3. Open the project and let the editor build the two plugins (both zips hold source code only). Check **Edit > Plugins**: **Stablecoin for Games Showcase (SCFG sample)**, in the **Samples** category, is enabled.
4. In **Project Settings > Maps & Modes**, set **Game Instance Class** to `ScfgShowcaseGameInstance` and **Game Default Map** to `/Engine/Maps/Entry` (under **Default Maps**: the engine's empty map; if the picker does not list it, add the two lines below to `Config/DefaultEngine.ini` instead). The project's `Config/DefaultEngine.ini` then has:

   ```ini
   [/Script/EngineSettings.GameMapsSettings]
   GameDefaultMap=/Engine/Maps/Entry
   GameInstanceClass=/Script/ScfgShowcase.ScfgShowcaseGameInstance
   ```

5. Put your own wallet address in **Project Settings > Plugins > SCFG > Wallet** (an EVM `0x` address; **Solana Wallet** too to take Solana) and keep **Testnet** on. Press **Validate** there.

## Run it

Start it as a game, or press **Play** in the editor (any map works, since the game instance picks its game mode; Play in the editor has not been run by the automated tests yet, which use `-game`):

```sh
"$UE_ROOT/Engine/Binaries/Linux/UnrealEditor" /path/to/YourProject.uproject -game -windowed -ResX=1280 -ResY=720
```

On Windows the editor is `Engine\Binaries\Win64\UnrealEditor.exe`, with the same arguments (not verified on Windows yet).

Where the configuration comes from, as the HUD's **Configured from** line says:

- **The project settings**, when they have a wallet and items: the plugin applies them by itself when the game instance starts, and the showcase only reads them.
- **Code**, when the settings have no items (a fresh project where you only set the wallet), or when the game is started with `-scfg-showcase-code`: `ConfigureInCode()` calls `Configure()` with your wallet, testnet on, an accent color and the plugin's `Resources/logo.png`, then `AddItem()` for the three items.
- **The command line**: `-scfg-showcase-wallet=0x...` (and `-scfg-showcase-solana-wallet=...`) gives the wallet instead of the settings. With settings items, they are applied with `ConfigureWithSettings(Wallet, SolanaWallet)`.

With no wallet anywhere, the shop stays closed and the Output Log has `LogScfgShowcase: Warning: showcase: No wallet is set, ...`. Settings with **Testnet** off are refused too: the showcase runs on testnets only.

## Pay on a testnet

1. Set your wallet as above.
2. Get test money into a second wallet (the player's): testnet USDC from [Circle's faucet](https://faucet.circle.com/), and a little of the network's coin for the fee (Sepolia ETH, for example from [Google's faucet](https://cloud.google.com/application/web3/faucet/ethereum/sepolia), POL on Amoy from [Polygon's faucet](https://faucet.polygon.technology/), Devnet SOL from [Solana's faucet](https://faucet.solana.com/)). Arc Testnet pays its fee in USDC. On Solana, your receiving wallet must have received each token once (it needs a token account), on Devnet as on mainnet: send it a little Devnet USDC first, or the sheet does not offer USDC on Solana.
3. Press **Unlock**, pick a network and a method: **Browser wallet** opens a checkout page in your browser, **Phone wallet** shows a QR code to scan, **Manual transfer** shows the exact amount and address.
4. The item unlocks once the payment is confirmed on the chain. Then try **Clear local data** and **Restore purchases** with the transaction hash: the purchase comes back.

## Packaging

The plugin's `Resources/logo.png`, which `ConfigureInCode()` uses, is staged with a packaged game by `ScfgShowcase.Build.cs` (a packaged game that cannot find it configures without a logo). If you configure it from your project settings with a logo of your own, list that logo's folder in **Project Settings > Packaging > Additional Non-Asset Directories to Package** (**Validate** in the SCFG settings warns when a logo would be left out). Package with **Testnet** on.

Do not ship the showcase in your game: remove the `ScfgShowcase` folder (or disable the plugin) and set your own Game Instance Class and Game Default Map again.

## How it is built

`ScfgShowcase.cpp` reads top to bottom in five sections: configure, build the shop, pay (and restore, export, clear), listen, show the state. It shows the patterns a real game needs:

- `Configure()` and `AddItem()` return `""` or the error they already logged, so check them at startup. A refused configuration is shown on the HUD and the game keeps running.
- The game is paused while a sheet is open. The sheet takes the focus, but game code that reads input keeps running, and the plugin keeps working while the game is paused.
- `OnPaymentResolved` can deliver the same result that a `PayAsync` callback gets, so its handler only refreshes from `Owns()` instead of counting results.
- The callbacks hold the game instance weakly: if the game quits while a sheet is open, the plugin drops the callback.

The repository's development project (`unreal/Project/ScfgDev`) plays this same plugin in its tests, through a test harness it hands the game instance (`UScfgShowcaseGameInstance::SetHarnessFactory`); a game never sets one. Tested on UE 5.8.4 (Linux). The showcase is MIT licensed (`LICENSE`), like the plugin.
