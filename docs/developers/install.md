# Install

## With uv or pipx

From your Ren'Py project folder, the one that contains `game/`:

```bash
uvx scfg-renpy install .
```

With [pipx](https://pipx.pypa.io/) instead: `pipx run scfg-renpy install .`. Either way
nothing stays installed: the command fetches the
[scfg-renpy](https://pypi.org/project/scfg-renpy/) package, copies the plugin into your
game and exits. It needs Python 3.9 or newer on your computer; your game doesn't.

## With npx

If you have Node 18 or newer but not Python:

```bash
npx scfg-renpy install .
```

The [scfg-renpy](https://www.npmjs.com/package/scfg-renpy) npm package carries the same plugin
files and copies them the same way.

## From the release zip

1. Download `scfg-renpy-<version>.zip` from the
   [latest release](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/releases/latest)
   or the [itch.io page](https://repulse-games.itch.io/stablecoin-for-games).
2. Unzip it into your Ren'Py project folder, the one that contains `game/`.

## What you get

Either way, your project then looks like this:

```
MyGame/
├── game/
│   ├── script.rpy                  (your own files, untouched)
│   ├── scfg/                       (screens, API, browser checkout page)
│   └── python-packages/
│       └── scfg_core/              (the engine; plain Python)
├── scfg-README.md                  (zip only: this guide as one file)
└── scfg-LICENSE.txt                (zip only)
```

Ren'Py loads every `.rpy` file under `game/` and imports packages from
`game/python-packages/`, so nothing else needs wiring up. If your game already has a
`python-packages/` folder, `scfg_core/` goes in next to what is there.

Then add your configuration in any `.rpy` file (see [Configure](configure/README.md)), and
start with `testnet=True` and a full [test purchase](testing.md) before going live.

The plugin has no dependencies beyond Ren'Py 8.2 or newer.

## Building the zip yourself

To build from source instead, for example to use unreleased changes from `main`, clone the
repository and run the packaging script. It needs Python 3.9 or newer and nothing else:

```bash
git clone https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games.git
cd jrpg-stablecoin-games
python tools/package.py
```

It writes `dist/scfg-renpy-<version>.zip`, the same zip a release contains. Install it as above.
Don't copy the whole repository into your game: only the contents of `plugin/game/` ship.

## Updating

```bash
uvx scfg-renpy update .
```

or `npx scfg-renpy@latest update .`. It replaces `game/scfg/` and `game/python-packages/scfg_core/` with the latest version and
prints the old and new version numbers. By hand: delete those two folders, then unzip the new
release the same way. Deleting first makes sure files removed in the new version don't linger. Your
configuration lives in your own `.rpy` files, and players' purchases live in their save
data, so neither is affected.
