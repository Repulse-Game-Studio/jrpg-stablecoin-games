# Install (Godot)

Accept **USDC** and **USDT** tips and unlock purchases in your Godot game, paid **directly to your own
wallet**. There is no server, no account and no middleman: the addon watches the blockchain itself. It is
pure GDScript (no GDExtension, no C#), needs Godot **4.3 or newer** (tested on 4.3 and 4.7.2), and works in
desktop builds and in web exports. Mobile is best effort and unverified (see [Mobile](api.md#mobile-unverified)).

Chains: Ethereum, Arbitrum, Polygon (USDC and USDT), Base (USDC), Arc (USDC), Solana (USDC and USDT), plus
any EVM chain you add yourself.

> Without a server, unlocks are honour-grade: a determined player can bypass them. Do not sell anything you
> cannot afford to lose to a bypass.

## From the release zip

1. Download `scfg-godot-<version>.zip` from the
   [latest release](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/releases/latest). Or build
   it from a clone of the repository with `uv run python tools/package.py --engine godot` (Python 3.9+); it is
   written to `dist/`.
2. Install it into your project, in either of these ways:
   - In the editor, open the **AssetLib** tab, click **Import...**, pick the zip and click **Install**. Keep
     **Ignore asset root** ticked.
   - Or unzip it anywhere and copy the `addons` folder from inside `scfg-godot-<version>/` into your project
     folder, the one that contains `project.godot`.

   Either way the addon ends up in `addons/scfg/`. The guide, the license and the third-party notices come
   with it, as `README.md`, `LICENSE` and `NOTICES.txt` inside that folder.
3. In the editor, open **Project > Project Settings > Plugins** and enable **Stablecoin for Games (SCFG)**.
   That registers the autoload `Scfg`. Disabling the plugin removes it.

If you open the project for the first time after adding the addon, let the editor finish importing before you
run the game, so the addon's `class_name` types (`ScfgPaymentResult`, ...) are registered.

## Your first payment

```gdscript
extends Node

func _ready() -> void:
    Scfg.configure({"wallet": "0xYourWalletAddressHere", "testnet": true})
    Scfg.add_item("chapter2", {"code": 1, "price": "4.99", "token": "USDC", "title": "Chapter 2"})

func _on_unlock_pressed() -> void:
    get_tree().paused = true   # the sheet does not stop your nodes, see the API page
    var result := await Scfg.pay("unlock", {"item": "chapter2"})
    get_tree().paused = false
    if result.status == "paid" or result.status == "already_owned":
        start_chapter_two()
```

`"testnet": true` makes the sheet offer test networks, so no real money moves. Remove it when you go live.
Replace the placeholder wallet with your own. Check `Scfg.owns("chapter2")` wherever the game gates the
content: it is instant and works offline.

## Try the showcase first

The repository has two runnable Godot projects that use the addon:

- [`godot/showcase/`](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/tree/main/godot/showcase)
  is a one-screen tour of the whole API. It has three unlocks (one with a pay-what-you-want amount), a tip
  jar, Restore, Export and Clear, and next to them it shows what the addon knows (owned items, tips, pending
  payments, receipts) and a live log of every result and signal. Its `main.gd` is written to be read top to
  bottom.
- [`godot/demo/`](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/tree/main/godot/demo) is a
  small game: a title screen, a locked Chapter 2, a Tip button and a Restore button. The end-to-end tests
  play it against local chains.

To run the showcase, clone the repository and run:

```
uv run python tools/sync_demo.py --engine godot   # copies addons/scfg into both projects
godot --path godot/showcase --import              # first run only
godot --path godot/showcase
```

Both run on testnets, so no real money moves. Put your own testnet wallet in `WALLET` at the top of the
showcase's `main.gd`, and you can watch a real test payment arrive. Get test USDC from
[Circle's faucet](https://faucet.circle.com/).

## What you get

```
MyGame/
├── project.godot
├── addons/
│   └── scfg/
│       ├── plugin.cfg, plugin.gd          (the editor plugin: registers the Scfg autoload)
│       ├── scfg.gd                        (the Scfg autoload: the API)
│       ├── payment_result.gd, restore_result.gd
│       ├── sheet_host.gd, store.gd, local_server.gd, web_page.gd, web_mirror.gd
│       ├── ui/                            (payment sheet, restore screen, theme, toast)
│       ├── web/                           (the browser checkout page)
│       ├── translations/scfg.pot          (template for your translations)
│       ├── core/                          (the engine-independent payment core, in GDScript)
│       └── README.md, LICENSE, NOTICES.txt  (zip only: this guide as one file, the license, notices)
```

Receipts and pending payments are stored in `user://scfg/state.json`, global to the install and never inside
your save slots, so loading an older save cannot lose a purchase.

## Next

Read the [API reference](api.md). Before a web release, read its
[Web exports](api.md#web-exports) section.
