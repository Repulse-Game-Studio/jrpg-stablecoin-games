# SCFG Godot demo

A small, complete Godot 4.3 game that uses the Stablecoin for Games addon: a title screen, a locked
Chapter 2 with an Unlock button, a Tip button and a Restore button. It runs on testnets, so no real
money moves. It is built from plain Controls and the default theme, and uses only the public API
(`Scfg.configure`, `add_item`, `pay`, `owns`, `restore` and the `payment_resolved` signal). While a
payment or restore screen is open it pauses the game tree, because the sheet does not stop game nodes
that poll `Input`.

The demo is MIT licensed, like the rest of the repository.

## Run it

The addon is not stored in this folder: it is copied in from `godot/addons/scfg/` (the copy is
git-ignored and is a build artifact).

```
uv run python tools/sync_demo.py --engine godot
godot --path godot/demo --import     # first run only
godot --path godot/demo
```

Before you publish anything, replace the placeholder wallet in `main.gd` (the public EIP-55 example
address) with your own, and remove `"testnet": true` when you go live.

`godot/demo/` sits inside the addon's dev project (`godot/`). Godot ignores a folder that has its
own `project.godot` when it scans the parent, and prints one harmless line
(`Detected another project.godot at res://demo. The folder will be ignored.`), so the demo's copy of
the addon never clashes with the dev project's.

## Test-only hooks: `SCFG_E2E_CONFIG` and `window.scfgE2EConfig`

The end-to-end tests point the demo at a local chain. Both forms carry the same JSON object, and
neither does anything in a real run:

- Desktop: the environment variable `SCFG_E2E_CONFIG` names a JSON file.
- Web export: the page sets `window.scfgE2EConfig` to the JSON text (a string) before the game loads,
  for example with Playwright's `add_init_script`. If it is absent or not a string, the normal demo runs.
  The web export has no process environment, so the variable is not read there.

```json
{
  "configure": {"wallet": "0x...", "testnet": true},
  "items": {
    "chapter2": {"code": 1, "price": "1.00", "token": "USDC", "title": "Chapter 2: The Fall"}
  }
}
```

`configure` is passed to `Scfg.configure()` as is, and each entry of `items` to `Scfg.add_item(id, opts)`.
The demo's item id must stay `chapter2` for its Unlock button. JSON numbers are turned into integers
before they reach the core. If the hook is set but unusable (missing file, bad JSON, wrong shape),
the demo reports the reason with `push_error` and quits with exit code 1; it never falls back to the
built-in config. Do not use this hook in a game.

## The end-to-end driver: `e2e/e2e_driver.gd`

`tools/run_godot_e2e.py` plays this demo against a local chain (`--family evm|arc|solana`): it
pays the Unlock by manual transfer, tips through the browser checkout page in Playwright Chromium,
and restarts the game to check the purchase is kept. It drives the real payment sheet through
`e2e/e2e_driver.gd`, which it loads as an extra autoload with a temporary, git-ignored
`override.cfg` next to `project.godot`. The driver does nothing in a normal run, and it is not
part of the addon. The game's user data goes to a temporary `HOME`, so a run never touches yours.

```
uv run python tools/run_godot_e2e.py --godot /path/to/godot [--family arc|solana]
```

It runs headless; set `SCFG_E2E_VIDEODRIVER=x11` to watch it in a window.

## Web export and the web end-to-end test

`export_presets.cfg` has a "Web" preset: single-threaded (`variant/thread_support=false`), so the
page needs no cross-origin isolation (COOP/COEP) headers, with `addons/scfg/web/*` in its
non-resource filter so the checkout page ships in the export. Godot's web export templates are
needed once (about 1 GB, kept in `/tmp/godot-templates`; only the web templates are unpacked):

```
uv run python tools/fetch_godot.py --version 4.3-stable --templates [--templates-dir DIR]
godot --headless --path godot/demo --export-release Web /path/to/out/index.html
```

`tools/run_godot_web_e2e.py` exports the demo that way, serves it on localhost and plays it in
Playwright Chromium against Anvil (`--family evm|arc`). The browser wallet pays the Unlock through
the in-page checkout overlay, a tip is copied from the sheet and paid from Anvil, and after a tab
reload the receipts and an unpaid pending payment are still there. The page sets
`window.scfgE2EConfig` (with `rpc_urls` pointing the built-in chain at Anvil), and the e2e driver
answers on `window.scfgE2E`. The runner exports a temporary copy of the demo whose `project.godot`
adds the driver as an autoload, because an export ignores `override.cfg`.

```
uv run python tools/run_godot_web_e2e.py --godot /path/to/godot [--family arc] [--templates-dir DIR]
```
