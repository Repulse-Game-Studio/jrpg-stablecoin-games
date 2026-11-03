# Install

1. Download `scfg-renpy-<version>.zip` from the
   [latest release](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/releases/latest).
2. Unzip it into your Ren'Py project folder, the one that contains `game/`. Your project
   then looks like this:
   ```
   MyGame/
   ├── game/
   │   ├── script.rpy                  (your own files, untouched)
   │   ├── scfg/                       (screens, API, browser checkout page)
   │   └── python-packages/
   │       └── scfg_core/              (the engine; plain Python)
   ├── scfg-README.md                  (this guide as one file)
   └── scfg-LICENSE.txt
   ```
   Ren'Py loads every `.rpy` file under `game/` and imports packages from
   `game/python-packages/`, so nothing else needs wiring up. If your game already has a
   `python-packages/` folder, `scfg_core/` goes in next to what is there.
3. Add your configuration in any `.rpy` file: see [Configure](configure/README.md).
4. Start with `testnet=True` and do a full [test purchase](testing.md) before going live.

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

Delete `game/scfg/` and `game/python-packages/scfg_core/`, then unzip the new release the
same way. Deleting first makes sure files removed in the new version don't linger. Your
configuration lives in your own `.rpy` files, and players' purchases live in their save
data, so neither is affected.
