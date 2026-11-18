# SCFG Unreal showcase

`ScfgDev` is the development project of the Unreal plugin, and its game is a one-screen showcase of the Stablecoin for Games plugin. Every button calls one function of the `UScfgSubsystem` game-instance subsystem, and the right-hand column shows what comes back. It is the twin of the [Godot showcase](../../../godot/showcase/README.md) and of Unity's Showcase sample.

The showcase is a plugin of its own, [`unreal/Plugins/ScfgShowcase`](../../Plugins/ScfgShowcase/README.md), which the project loads from `../../Plugins` as it loads `Scfg`. It uses the plugin's public API only, so it also builds in any project: it ships as the optional `scfg-unreal-showcase-<version>.zip` on the release page, and its README is the guide for that. This project adds what only the repository needs: the showcase's project settings with a testnet wallet, and the e2e driver that its tests play it with.

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

It runs on testnets (Sepolia, Arbitrum Sepolia, Polygon Amoy, Base Sepolia, Arc Testnet), so no real money moves. The payout wallet in this project's settings is a placeholder that is not yours (the EIP-55 example address); the showcase plugin itself has no wallet, and in a project without one it configures nothing and says so on its HUD. To watch a real test payment arrive, put your own testnet wallet here: see [Pay on a testnet](#pay-on-a-testnet).

## Configuration

The showcase shows both ways to configure the plugin:

- **The project settings** (the default). The plugin applies them by itself when the game instance starts. They are in Project Settings > Plugins > SCFG, saved to `Config/DefaultGame.ini` under `[/Script/Scfg.ScfgSettings]`, and committed: the wallet, testnet on, the accent color, the logo and the three items. The HUD says "Configured from: the project settings".
- **Code**. `UScfgShowcaseGameInstance::ConfigureInCode()` calls `Configure()` and `AddItem()` with the same items and accent, and the settings' wallet. It runs when the game is started with `-scfg-showcase-code`, or when the project settings have no items. A `Configure()` from code replaces what the settings applied, as long as it comes before the first frame (the game instance's `Init()`). The HUD then says "Configured from: code".
- **The command line**. `-scfg-showcase-wallet=0x...` (and `-scfg-showcase-solana-wallet=...`) replaces the settings' wallet.

## Run it

The project loads the plugins from `unreal/Plugins/Scfg` and `unreal/Plugins/ScfgShowcase` (`AdditionalPluginDirectories`), so nothing is copied. It needs UE 5.8; the commands below use `$UE_ROOT` for the engine's folder. Build the editor target first (the editor also offers to build it when you open the project):

```sh
uv run python tools/run_unreal_build.py --target ScfgDevEditor --config Development
```

**In the editor.** Open `unreal/Project/ScfgDev/ScfgDev.uproject` and press **Play**. The project's game instance is the showcase's (`Config/DefaultEngine.ini`: `GameInstanceClass=/Script/ScfgShowcase.ScfgShowcaseGameInstance`), and it picks its own game mode, so any map works. Play in the editor has not been run by the automated tests yet; they use `-game`.

**As a game**, without the editor's UI:

```sh
"$UE_ROOT/Engine/Binaries/Linux/UnrealEditor" unreal/Project/ScfgDev/ScfgDev.uproject -game -windowed -ResX=1280 -ResY=720
# configured in code instead of the project settings:
"$UE_ROOT/Engine/Binaries/Linux/UnrealEditor" unreal/Project/ScfgDev/ScfgDev.uproject -game -windowed -ResX=1280 -ResY=720 -scfg-showcase-code
```

On Windows the editor is `Engine\Binaries\Win64\UnrealEditor.exe`, with the same arguments (not verified on Windows yet).

The showcase has no level of its own: the game runs on the engine's empty `/Engine/Maps/Entry` map (`GameDefaultMap`), so the project has no assets of its own. The screen is built in C++ with Slate (`unreal/Plugins/ScfgShowcase/Source/ScfgShowcase/Private/ScfgShowcase.cpp`).

The sheet's logo is `Content/Showcase/logo.png`, a loose file that the cooker does not package by itself (a copy of the showcase plugin's `Resources/logo.png`, which `ConfigureInCode()` uses only when the plugin is inside the project's folder: `Configure()` takes no logo outside it, so here the code uses this copy too). `Config/DefaultGame.ini` therefore lists its folder in Project Settings > Packaging > Additional Non-Asset Directories to Package (`+DirectoriesToAlwaysStageAsUFS=(Path="Showcase")`, relative to `Content/`). Without that, a packaged showcase's `Configure()` fails with `logo.file_missing`. Validate in the SCFG settings warns when a logo would be left out like this.

## Pay on a testnet

1. Put your own wallet address in Project Settings > Plugins > SCFG > Wallet (or in `Options=(Wallet=...)` in `Config/DefaultGame.ini`), and press **Validate** there. Or leave the settings alone and start the game with `-scfg-showcase-wallet=<your 0x address>`.
2. Get testnet USDC from [Circle's faucet](https://faucet.circle.com/) into a second wallet (the player's).
3. Press **Unlock**, pick a network and a method:
   - **Browser wallet** opens a checkout page in your browser.
   - **Phone wallet** shows a QR code to scan.
   - **Manual transfer** shows the exact amount and address.
4. The item unlocks once the payment is confirmed on the chain.

Then try **Clear local data** and **Restore purchases** with the transaction hash: the purchase comes back.

The settings have no Solana wallet. With one, the plugin's start checks the wallet's token account over the network, and Solana Devnet is offered as well.

## How it is built

`ScfgShowcase.cpp` (in the showcase plugin) reads top to bottom in five sections: configure, build the shop, pay (and restore, export, clear), listen, show the state. It shows the patterns a real game needs:

- `Configure()` and `AddItem()` return `""` or the error they already logged, so check them at startup. Without the e2e driver, a refused configuration is shown on the HUD and the game keeps running.
- The payout wallet is never a placeholder: with no wallet in the settings or on the command line, nothing is configured and the HUD says so.
- The game is paused while a sheet is open. The sheet takes the focus, but game code that reads input keeps running, and the plugin keeps working while the game is paused.
- `OnPaymentResolved` can deliver the same result that a `PayAsync` callback gets, so its handler only refreshes from `Owns()` instead of counting results.
- The callbacks hold the game instance weakly: if the game quits while a sheet is open, the plugin drops the callback.

## Tests

- **The boot test**, the twin of `tests/godot_showcase`: `uv run pytest -m unreal_showcase` (deselected by default), or `uv run python tools/run_unreal_e2e.py --showcase` directly. It starts the showcase on an X server of its own (`xvfb-run -a`, Mesa's lavapipe) with the e2e driver's control socket. It presses the buttons that need no chain with real clicks: Export, Clear, Notify off and on, Restore, a tip and an unlock, each sheet closed with Escape on its first step. After each press it checks the readouts and the event log. Then it starts the showcase again with `-scfg-showcase-code` and checks the same shop, and a third time with the settings' wallet blanked (`-ini:Game:[/Script/Scfg.ScfgSettings]:Options=(bTestnet=True)`): the shop must stay closed and the HUD must say that no wallet is set. No chain is involved: the sheets close before a network is chosen, and each start checks that the plugin posted no JSON-RPC request (and the first two that it found the logo). `--showcase --packaged` runs the same tour on the cooked, staged game, where the settings come from the staged ini and the logo from the pak, and checks that the showcase plugin's own logo was staged and that the editor module `ScfgEditor` is not in the game.
- **The payment e2e**: `uv run python tools/run_unreal_e2e.py --family evm|arc|solana` (or `uv run pytest -m unreal_e2e`) pays for the unlock and a tip on a local chain. See [unreal/README.md](../../README.md#the-linux-e2e).
- `ScfgDev.ShowcaseSettings` (in `run_unreal_tests.py --filter Scfg`) checks that the committed project settings pass `Validate()` without a warning (their logo's folder is staged), and `ScfgDev.ShowcaseWallets` where the showcase finds its wallet (the command line, else the settings, else none).

The e2e driver (`Source/ScfgDev/ScfgE2EDriver.h`) is compiled only outside Shipping, and made only with `-scfg-e2e=<config>`. The showcase does not include it: the project's module hands it to the showcase at start as its harness (`UScfgShowcaseGameInstance::SetHarnessFactory`, in `Source/ScfgDev/ScfgDev.cpp`), so the plugin needs nothing of the dev project.

The full API is in the plugin's headers (`unreal/Plugins/Scfg/Source/Scfg/Public/`) and [unreal/README.md](../../README.md). The showcase is MIT licensed, like the rest of the repository. `uv run python tools/package.py --engine unreal` builds its zip, `dist/scfg-unreal-showcase-<version>.zip` (the layout is in `tools/package_unreal.py`).
