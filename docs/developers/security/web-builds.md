# Web builds
Payments work in Ren'Py's web (HTML5) build, on your own site or embedded on itch.io:

- **Browser wallet**: the player's wallet extension (MetaMask, Rabby, Phantom, ...) is
  already in the page, so the checkout opens right over the game. The option only shows
  when the page has a wallet for the chosen network, which includes a wallet app's own
  built-in browser on phones.
- A broken wallet extension in the page (one that announces itself badly or throws odd
  values) can't stop the game: the checkout leaves it out or shows a wallet error, and the
  player's other wallets still pay.
- **Phone wallet (QR)** and **manual transfer** work as on desktop. On a phone, "Open
  wallet app" hands the payment to the installed wallet.
- **Copy** buttons use the browser's clipboard; if the browser blocks it, the game says
  so. A **Paste** button on the payment screen and on the Restore screen fills in a
  transaction hash, if the browser allows it. It needs the browser's clipboard
  permission: when the player refuses, the game says so and they can type the hash
  instead. On the payment screen the hash is optional (the game finds the payment
  without it); on the Restore screen it is required.
- The game watches the chain from the page, between frames, and never freezes.
- The plugin's console messages (setup warnings, errors) are printed only in developer mode
  (`config.developer`). Ren'Py's web build shows printed text over the game, so a distributed
  build never shows them to players.

Your game must define `config.save_directory` (games created by the Ren'Py launcher do,
in `options.rpy`). Without it, Ren'Py's web build keeps saves and persistent data in
memory only, so receipts would be lost when the tab closes.

**Several games on one origin (unverified).** Some hosts serve many games from one origin
(the same scheme, host and port; only the path differs), and the browser's storage
belongs to the origin, not to the path. In Godot's web build, which is also built with
Emscripten and keeps its files in the browser's IndexedDB, a game saving there deletes
files that another game on the same origin saved after it loaded (seen in Chromium); the
Godot addon keeps a second copy of the receipts for that reason. Whether Ren'Py's web
build is affected the same way has not been checked yet (it needs the Ren'Py SDK). If it
is, a purchase can disappear from your game after the player plays another Ren'Py web
game on the same origin. Until it is checked: keep `config.save_directory` unique to your
game, serve the game from an origin of its own where you can, and remember that a lost
purchase comes back with **Restore purchases** and the transaction hash.

Your RPC endpoints must accept requests from browsers (CORS). All built-in endpoints do.
If you add your own with `rpc_urls=` or `add_chain(...)`, check them with
`uv run python tools/check_rpcs.py`: endpoints marked `web: BLOCKED` are skipped by web
players. Endpoints on the player's own machine or network (`127.0.0.1`, `localhost`,
private IP addresses) don't work from a hosted web build either: browsers block them
(Chrome's Local Network Access, and mixed content for `http`). Only public HTTPS
endpoints work for web players.

Before you release a web build, test it once on its real home with a real wallet. On
itch.io, upload it as an HTML game and pay a small amount with MetaMask (EVM) and with
Phantom (Solana). Wallet extensions are expected to work inside itch.io's game frame,
but only a real upload proves it for the wallets your players use. If that host serves
other games from the same origin, also run the shared-origin check in
[Test before release](../testing.md).

To test the web build automatically, `tools/run_renpy_web_e2e.py --sdk <sdk>` (a Ren'Py
SDK with the web platform installed) builds the game for the web and pays in headless
Chrome against Anvil (needs Anvil, Playwright, and Google Chrome or Playwright's
Chromium).
