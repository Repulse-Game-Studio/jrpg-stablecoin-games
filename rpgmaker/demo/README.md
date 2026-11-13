# SCFG demo kit for RPG Maker MZ

A one-map demo of the SCFG plugin to copy into a new RPG Maker MZ project. It has three NPCs
that call the plugin commands (Unlock Item, Tip, Restore Purchases), a sign that calls Wallet
Help, and a door that opens once Chapter 2 is bought.

The kit has **no assets**: no images, no audio, and no RTP (KADOKAWA) data. You add the sprites
from your own project (step 6).

> **Unverified against real RPG Maker MZ.** These files are written from MZ's public data and
> plugin formats and are tested only on this repo's MZ shim (`rpgmaker/test/mz_shim/`) and by
> `rpgmaker/test/demo_kit.test.ts`. Nobody has opened them in the MZ editor yet. If something
> doesn't load, please report it. The list of what is unverified is at the end.

## Files

| File | What to do with it |
|---|---|
| `data/Map001.json` | **Replace** your project's `data/Map001.json` (the demo map, 17 x 13). |
| `data/MapInfos.json` | **Replace** it in a new project, whose only map is Map001. If your project has other maps, keep yours: it already lists Map001. |
| `js/plugins.js` | **Replace** it if your project has no plugins turned on. Otherwise add the `SCFG` entry to the `$plugins` list in yours, or add SCFG in the Plugin Manager and type the values shown below. |
| `data/System.json` | Not shipped (a full one would carry the RTP database). Make the few changes in the editor (step 4). |

## Install

1. Create a new project in RPG Maker MZ (File > New Project), then close the editor. It keeps
   the data in memory and would write over the copied files.
2. Build the plugin: `cd rpgmaker && npm ci && npm run build`. Copy
   `rpgmaker/dist/js/plugins/SCFG.js` to `<your project>/js/plugins/SCFG.js`. The file name
   must stay `SCFG.js`.
3. Copy `data/Map001.json`, `data/MapInfos.json` and `js/plugins.js` into the project, as the
   table above says.
4. Open the project. These changes go into `data/System.json`, and you make them in the editor:
   - **Player start:** on Map001, right-click the tile (8, 9) > Set Starting Position > Player.
     In `System.json` this sets `startMapId` 1, `startX` 8 and `startY` 9.
   - **Names (optional, only for you):** name switch 0001 "Chapter 2 owned" and variable 0001
     "SCFG status" in any switch or variable picker. These are `switches[1]` and
     `variables[1]` of `System.json`. Keep the other entries; a new project has 20 of each.
5. **Replace the placeholder wallet.** Open Tools > Plugin Manager > SCFG and set **Wallet**
   to your own EVM address. The demo's address is the public example address from the EIP-55
   spec. It is not yours, and a real payment to it is lost. Keep **Testnet** ON while you try
   the demo: test networks, test tokens, no real money.
6. **Sprites.** The events have no image (`characterName` ""), so they are invisible. Open each
   event on Map001 and pick an image from your project, for example `People1` for the NPCs and a
   door from `!Door1`. The floor is tile 2816, the first A2 autotile of tileset 1. If it isn't
   walkable in your project's tileset 1, fill the map with any floor tile.
7. Playtest. To pay on a testnet you need test USDC and a little test ETH. See
   <https://jrpg.com/stablecoinforgames/docs/developers/testing>.

## What's on the map

| Event | Tile | What it does |
|---|---|---|
| 1 Merchant | (4, 6) | **Unlock Item** `chapter2`, status in variable 1. Then it checks switch 1. |
| 2 Bard | (10, 4) | **Tip** 1.00 USDC, status in variable 1. |
| 3 Archivist | (12, 6) | **Restore Purchases**, status in variable 1. |
| 4 Door | (8, 1) | Page 1 says it's locked. Page 2 (condition: **switch 1** ON) lets the player through. |
| 5 Sign | (2, 10) | **Wallet Help** opens the players' guide to wallets in the browser. |

The plugin parameters in `js/plugins.js`:

| Parameter | Demo value |
|---|---|
| Wallet | `0x52908400098527886E0F7030069857D2E4169EE7` (**placeholder**, replace it) |
| Testnet | ON |
| Items | `chapter2`: Code 37, Price 4.99, Token USDC, Title "Chapter 2", Owned Switch 1 |
| Notify | ON |
| Tip Jar Menu | ON (the main menu gets "Tip Jar") |
| Tip Jar Label | Tip Jar |
| Everything else | empty (the defaults) |

## Switch 1 and variable 1

- **Switch 1** is Chapter 2's **Owned Switch**. The plugin owns it: it is ON exactly while
  Chapter 2 is owned. The plugin sets it after New Game, after every Load (also of a save made
  before the purchase), when a payment confirms or purchases are restored, and when the stored
  receipts are first read. Don't set it from events. A game event that turns it OFF is
  overruled at the next of those moments. Purchases are kept outside the save slots, so every
  slot sees them.
- **Variable 1** gets the status code when the player closes the payment sheet or the restore
  screen. The event waits until then.

| Code | Unlock Item / Tip | Restore Purchases |
|---|---|---|
| 0 | not done (the sheet is still open, or a save made while it was open was loaded) | same |
| 1 | paid | restored |
| 2 | already owned (no sheet is shown) | nothing found |
| 3 | pending: the player chose Continue playing while it confirms | - |
| 4 | underpaid | - |
| 5 | expired | - |
| 6 | cancelled | cancelled |
| 7 | failed: the command could not run (the console says why) | failed |

Scripts can read the same table as `SCFG.STATUS` (`SCFG.STATUS.paid === 1`).

## Unverified against real MZ

- the data file layout (`Map001.json` keys, event pages, `MapInfos.json`) and that MZ loads a
  map written in this compact form;
- the event command codes and parameters: 101/401 Show Text (the 5th parameter is the speaker
  name), 111/411/412 Conditional Branch (switch and variable forms), 357 Plugin Command with its
  657 summary lines;
- the plugin header's annotations as the Plugin Manager reads them (`multiline_string`, `combo`,
  `@require`, `@parent`, `struct<Item>[]`) and how it stores their values in `plugins.js`;
- that tileset 1's tile 2816 is a walkable floor in a new project, and that a new project has 20
  switches and 20 variables;
- what a save made while an event waits on a command does in real MZ after a load (in the shim the
  event goes on with the variable at 0).
