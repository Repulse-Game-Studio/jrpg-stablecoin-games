# Stablecoin for Games

Take tips and sell unlocks in **USDC** and **USDT** inside your Ren'Py game, on Ethereum,
Arbitrum, Polygon, Base or Solana. The player's wallet pays yours directly. There is no
store, account or server in between: the game watches the blockchain itself.

This package installs the plugin into your Ren'Py project. Ren'Py only loads code from a
game's own `game/` folder, so the plugin is copied there rather than installed into Python.

## Install

From your Ren'Py project folder (the one that contains `game/`):

```bash
uvx scfg-renpy install .
```

or, with pipx: `pipx run scfg-renpy install .`

That adds:

```
game/scfg/                        (screens, API, browser checkout page)
game/python-packages/scfg_core/   (the engine; plain Python)
```

## Update

```bash
uvx scfg-renpy update .
```

It replaces those two folders with the latest version. Your own files, your configuration
and your players' purchases are not touched.

## Use it

```renpy
init python:
    scfg.configure(wallet="0xYourWallet", testnet=True)
    scfg.add_item("chapter2", code=1, price="4.99", token="USDC", title="Chapter 2")

label end_of_chapter1:
    $ result = scfg.pay("unlock", item="chapter2")
    if scfg.owns("chapter2"):
        jump chapter2
```

Requires Ren'Py 8.2 or newer. Read the
[developer guide](https://jrpg.com/stablecoinforgames/docs/developers/install) and the
[security notes](https://jrpg.com/stablecoinforgames/docs/developers/security) before you
sell anything: without a server, unlocks are honour-grade.

- Docs: https://jrpg.com/stablecoinforgames/docs/
- Source and release zips: https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games
- License: MIT
