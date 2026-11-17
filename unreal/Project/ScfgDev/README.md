# SCFG Unreal showcase

`ScfgDev` is the development project of the Unreal plugin, and its game is a one-screen showcase of the Stablecoin for Games plugin. Every button calls one function of the `UScfgSubsystem` game-instance subsystem, and the right-hand column shows what comes back. It is the twin of the [Godot showcase](../../../godot/showcase/README.md) and of Unity's Showcase sample.

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

It runs on testnets (Sepolia, Arbitrum Sepolia, Polygon Amoy, Base Sepolia, Arc Testnet), so no real money moves. The payout wallet is a placeholder that is not yours (the EIP-55 example address). To watch a real test payment arrive, replace it with your own testnet wallet: see [Pay on a testnet](#pay-on-a-testnet).

## Configuration

The showcase shows both ways to configure the plugin:

- **The project settings** (the default). The plugin applies them by itself when the game instance starts. They are in Project Settings > Plugins > SCFG, saved to `Config/DefaultGame.ini` under `[/Script/Scfg.ScfgSettings]`, and committed: the wallet, testnet on, the accent color and the three items. The HUD says "Configured from: the project settings".
- **Code**. `UScfgShowcaseGameInstance::ConfigureInCode()` calls `Configure()` and `AddItem()` with the same values. It runs when the game is started with `-scfg-showcase-code`, or when the project settings are empty. A `Configure()` from code replaces what the settings applied, as long as it comes before the first frame (the game instance's `Init()`). The HUD then says "Configured from: code".

## Run it

The project loads the plugin from `unreal/Plugins/Scfg` (`AdditionalPluginDirectories`), so nothing is copied. It needs UE 5.8; the commands below use `$UE_ROOT` for the engine's folder. Build the editor target first (the editor also offers to build it when you open the project):

```sh
uv run python tools/run_unreal_build.py --target ScfgDevEditor --config Development
```

**In the editor.** Open `unreal/Project/ScfgDev/ScfgDev.uproject` and press **Play**. The project's game instance is the showcase's (`Config/DefaultEngine.ini`), and it picks its own game mode, so any map works. Play in the editor has not been run by the automated tests yet; they use `-game`.

**As a game**, without the editor's UI:

```sh
"$UE_ROOT/Engine/Binaries/Linux/UnrealEditor" unreal/Project/ScfgDev/ScfgDev.uproject -game -windowed -ResX=1280 -ResY=720
# configured in code instead of the project settings:
"$UE_ROOT/Engine/Binaries/Linux/UnrealEditor" unreal/Project/ScfgDev/ScfgDev.uproject -game -windowed -ResX=1280 -ResY=720 -scfg-showcase-code
```

On Windows the editor is `Engine\Binaries\Win64\UnrealEditor.exe`, with the same arguments (not verified on Windows yet).

The game runs on the engine's empty `/Engine/Maps/Entry` map, so the project has no content of its own. The screen is built in C++ with Slate (`Source/ScfgDev/ScfgShowcase.cpp`).

## Pay on a testnet

1. Put your own wallet address in Project Settings > Plugins > SCFG > Wallet (or in `Options=(Wallet=...)` in `Config/DefaultGame.ini`), and press **Validate** there. To configure in code instead, change `WALLET` in `ScfgShowcase.cpp` and start with `-scfg-showcase-code`.
2. Get testnet USDC from [Circle's faucet](https://faucet.circle.com/) into a second wallet (the player's).
3. Press **Unlock**, pick a network and a method:
   - **Browser wallet** opens a checkout page in your browser.
   - **Phone wallet** shows a QR code to scan.
   - **Manual transfer** shows the exact amount and address.
4. The item unlocks once the payment is confirmed on the chain.

Then try **Clear local data** and **Restore purchases** with the transaction hash: the purchase comes back.

The settings have no Solana wallet. With one, the plugin's start checks the wallet's token account over the network, and Solana Devnet is offered as well.

## How it is built

`ScfgShowcase.cpp` reads top to bottom in five sections: configure, build the shop, pay (and restore, export, clear), listen, show the state. It shows the patterns a real game needs:

- `Configure()` and `AddItem()` return `""` or the error they already logged, so check them at startup. Without the e2e driver, a refused configuration is shown on the HUD and the game keeps running.
- The game is paused while a sheet is open. The sheet takes the focus, but game code that reads input keeps running, and the plugin keeps working while the game is paused.
- `OnPaymentResolved` can deliver the same result that a `PayAsync` callback gets, so its handler only refreshes from `Owns()` instead of counting results.
- The callbacks hold the game instance weakly: if the game quits while a sheet is open, the plugin drops the callback.

## Tests

- **The boot test**, the twin of `tests/godot_showcase`: `uv run pytest -m unreal_showcase` (deselected by default), or `uv run python tools/run_unreal_e2e.py --showcase` directly. It starts the showcase on an X server of its own (`xvfb-run -a`, Mesa's lavapipe) with the e2e driver's control socket. It presses the buttons that need no chain with real clicks: Export, Clear, Notify off and on, Restore, a tip and an unlock, each sheet closed with Escape on its first step. After each press it checks the readouts and the event log. Then it starts the showcase again with `-scfg-showcase-code` and checks the same shop. No chain is involved: the sheets close before a network is chosen.
- **The payment e2e**: `uv run python tools/run_unreal_e2e.py --family evm|arc|solana` (or `uv run pytest -m unreal_e2e`) pays for the unlock and a tip on a local chain. See [unreal/README.md](../../README.md#the-linux-e2e).
- `ScfgDev.ShowcaseSettings` (in `run_unreal_tests.py --filter Scfg`) checks that the committed project settings pass `Validate()`.

The e2e driver (`ScfgE2EDriver.h`) is compiled only outside Shipping, and made only with `-scfg-e2e=<config>`.

The full API is in the plugin's headers (`unreal/Plugins/Scfg/Source/Scfg/Public/`) and [unreal/README.md](../../README.md). The showcase is MIT licensed, like the rest of the repository.
