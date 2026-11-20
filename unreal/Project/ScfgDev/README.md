# ScfgDev: the Unreal development project

`ScfgDev` is the development project of the Unreal plugin, and its game is the one-screen showcase of the Stablecoin for Games plugin: every button calls one function of the `UScfgSubsystem` game-instance subsystem, and the right-hand column shows what comes back. It is the twin of the [Godot showcase](../../../godot/showcase/README.md) and of Unity's Showcase sample.

The showcase is a plugin of its own, [`unreal/Plugins/ScfgShowcase`](../../Plugins/ScfgShowcase/README.md), which the project loads from `../../Plugins` as it loads `Scfg`. It uses the plugin's public API only, so it also builds in any project: it ships as the optional `scfg-unreal-showcase-<version>.zip` on the release page, and [its README](../../Plugins/ScfgShowcase/README.md) is the guide to it: what each button calls, how to pay on a testnet, and how its code is built. This project adds what only the repository needs: the showcase's project settings with a placeholder testnet wallet, and the e2e driver that its tests play it with.

It runs on testnets, so no real money moves. The payout wallet in this project's settings is the EIP-55 example address, a placeholder for the tests that is not yours: to watch a real test payment arrive, put your own testnet wallet in Project Settings > Plugins > SCFG > Wallet (or start the game with `-scfg-showcase-wallet=<your 0x address>`) and follow the showcase README's "Pay on a testnet". The showcase plugin itself never falls back to a placeholder wallet: with no wallet in the settings or on the command line, nothing is configured and the HUD says so.

## Configuration

The showcase shows both ways to configure the plugin:

- **The project settings** (the default). The plugin applies them by itself when the game instance starts. They are in Project Settings > Plugins > SCFG, saved to `Config/DefaultGame.ini` under `[/Script/Scfg.ScfgSettings]`, and committed: the wallet, testnet on, the accent color, the logo and the three items. The HUD says "Configured from: the project settings".
- **Code**. `UScfgShowcaseGameInstance::ConfigureInCode()` calls `Configure()` and `AddItem()` with the same items and accent, and the settings' wallet. It runs when the game is started with `-scfg-showcase-code`, or when the project settings have no items. A `Configure()` from code replaces what the settings applied, as long as it comes before the first frame (the game instance's `Init()`). The HUD then says "Configured from: code".
- **The command line**. `-scfg-showcase-wallet=0x...` (and `-scfg-showcase-solana-wallet=...`) replaces the settings' wallet.

Settings with Testnet off are refused on every path, the code path included: the showcase runs on testnets only.

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

On Windows the editor is `Engine\Binaries\Win64\UnrealEditor.exe`, with the same arguments (it builds on Windows; not yet fully tested there).

The showcase has no level of its own: the game runs on the engine's empty `/Engine/Maps/Entry` map (`GameDefaultMap`), so the project has no assets of its own. The screen is built in C++ with Slate (`unreal/Plugins/ScfgShowcase/Source/ScfgShowcase/Private/ScfgShowcase.cpp`).

The sheet's logo is `Content/Showcase/logo.png`, a loose file that the cooker does not package by itself (a copy of the showcase plugin's `Resources/logo.png`, which `ConfigureInCode()` uses only when the plugin is inside the project's folder: `Configure()` takes no logo outside it, so here the code uses this copy too). `Config/DefaultGame.ini` therefore lists its folder in Project Settings > Packaging > Additional Non-Asset Directories to Package (`+DirectoriesToAlwaysStageAsUFS=(Path="Showcase")`, relative to `Content/`). Without that, a packaged showcase's `Configure()` fails with `logo.file_missing`. Validate in the SCFG settings warns when a logo would be left out like this.

## Tests

- **The boot test**, the twin of `tests/godot_showcase`: `uv run pytest -m unreal_showcase` (deselected by default), or `uv run python tools/run_unreal_e2e.py --showcase` directly. It starts the showcase on an X server of its own (`xvfb-run -a`, Mesa's lavapipe) with the e2e driver's control socket. It presses the buttons that need no chain with real clicks: Export, Clear, Notify off and on, Restore, a tip and an unlock, each sheet closed with Escape on its first step. After each press it checks the readouts and the event log. Then it starts the showcase again with `-scfg-showcase-code` and checks the same shop, and a third time with the settings' wallet blanked (`-ini:Game:[/Script/Scfg.ScfgSettings]:Options=(bTestnet=True)`): the shop must stay closed and the HUD must say that no wallet is set. No chain is involved: the sheets close before a network is chosen, and each start checks that the plugin posted no JSON-RPC request (and the first two that it found the logo). `--showcase --packaged` runs the same tour on the cooked, staged game, where the settings come from the staged ini and the logo from the pak, and checks that the showcase plugin's own logo was staged and that the editor module `ScfgEditor` is not in the game.
- **The payment e2e**: `uv run python tools/run_unreal_e2e.py --family evm|arc|solana` (or `uv run pytest -m unreal_e2e`) pays for the unlock and a tip on a local chain. See [unreal/README.md](../../README.md#the-linux-e2e).
- `ScfgDev.ShowcaseSettings` (in `run_unreal_tests.py --filter Scfg`) checks that the committed project settings pass `Validate()` without a warning (their logo's folder is staged), `ScfgDev.ShowcaseWallets` where the showcase finds its wallet (the command line, else the settings, else none), and `ScfgDev.ShowcaseSetup` where its configuration comes from (refused without a wallet or with Testnet off, else code, the settings, or the settings with the command line's wallet).

The e2e driver (`Source/ScfgDev/ScfgE2EDriver.h`) is compiled only outside Shipping, and made only with `-scfg-e2e=<config>`. The showcase does not include it: the project's module hands it to the showcase at start as its harness (`UScfgShowcaseGameInstance::SetHarnessFactory`, in `Source/ScfgDev/ScfgDev.cpp`), so the plugin needs nothing of the dev project.

The full API is in the plugin's headers (`unreal/Plugins/Scfg/Source/Scfg/Public/`) and [unreal/README.md](../../README.md). The showcase is MIT licensed, like the rest of the repository. `uv run python tools/package.py --engine unreal` builds its zip, `dist/scfg-unreal-showcase-<version>.zip` (the layout is in `tools/package_unreal.py`).
