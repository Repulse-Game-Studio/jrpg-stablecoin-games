# API reference (RPG Maker MZ)

> **Tested on a stand-in, not yet on real RPG Maker MZ.** The plugin is tested on an in-repo stand-in for MZ's
> public plugin API, in headless Chrome and in Node. See [Install](install.md). Everything below is what the
> plugin does there; the NW.js and editor details are written from MZ's public documentation.

The plugin has three layers, from least to most work:

- the **plugin parameters** and **plugin commands**: no code;
- the **script API**, `window.SCFG`, for scripts and other plugins;
- **customising** the sheet.

The plugin mirrors the Ren'Py plugin's calls in camelCase. It also mirrors its rules: the Ren'Py behaviour is
the reference when this page is silent.

## Plugin parameters

Open the Plugin Manager and select SCFG. The parameter names below are what the editor shows.

| Parameter | Meaning |
|---|---|
| **Wallet** | Your EVM wallet address (`0x...`). It receives the payments on Ethereum, Arbitrum, Polygon, Base and Arc |
| **Solana Wallet** | Optional: your Solana wallet address, to take USDC/USDT on Solana too |
| **Testnet** | ON: test networks with test tokens, no real money. Turn it OFF for the release. Default OFF, so set it ON while you test |
| **Logo** | Optional: a png file from `img/scfg/`, shown on the payment sheet and the checkout page. The editor's file picker lists png files; for a jpg or webp logo, pass it from a script: `SCFG.configure({logo: "mark.webp"})`. A logo named only in a script is used by nothing MZ knows of, so a deployment with **Exclude unused files** leaves it out: deploy without that option, or copy the file into the deployed game's `img/scfg/` |
| **Accent** | Optional: a color like `#0e7c74` for selected options, the forward button and the progress dots |
| **Items** | What players can unlock: see below |
| **Notify** | Show a short notice when a payment confirms or purchases are restored. Default ON |
| **Tip Jar Menu** | Add a Tip Jar command to the main menu. It opens the tip sheet with 1.00 of the first enabled token. Default OFF |
| **Tip Jar Label** | The Tip Jar command's name in the menu. Default `Tip Jar` |
| **Texts** | Your own wording or language for the player-facing texts: see [Texts](#texts) |

With both wallets empty, the parameters configure nothing and your own script calls `SCFG.configure()` (see
[Configuring from a script](#configuring-from-a-script)).

A parameter the plugin cannot use (a bad address, a duplicate item code, a switch used twice) is reported with
`console.error("scfg: ...")`, also on screen in test play, and the plugin stays **disabled** until you fix it.

### Items

Each entry of **Items** declares one unlock.

| Field | Meaning |
|---|---|
| **Id** | The item's id for commands and scripts: a-z, 0-9, `_` and `-`, like `chapter2` |
| **Code** | 1-99, a different one for each item. It is written into players' payments: **never change it after release** |
| **Price** | The price, like `4.99` |
| **Token** | `USDC` or `USDT` |
| **Title** | The name players see, like `Chapter 2` |
| **Min Price** | Optional: the lowest amount that still unlocks the item (sales: keep the old price here when you raise it). Empty: the price |
| **Owned Switch** | The switch the plugin keeps ON while the item is owned. See [Owned switches](#owned-switches) |

### Advanced parameters

These take JSON text. Empty means the defaults.

| Parameter | Example |
|---|---|
| **Chains** | `["base", "arbitrum"]`: only these networks |
| **Tokens** | `["USDC"]`: only these tokens |
| **Methods** | `{"manual": {"allow_tx_paste": false}}`: payment method options |
| **RPC URLs** | `{"base": ["https://..."]}`: your own RPC URLs per network |
| **Confirmations** | `{"base": 12}`: blocks to wait before a payment counts, per EVM network (Solana's are fixed) |
| **Previous Wallets** | `["0x..."]`: wallets you were paid to before, so Restore Purchases still finds those payments |
| **Custom Chains** | `{"optimism": {"name": "Optimism", "chain_id": 10, "rpc": ["https://..."], "tokens": {"USDC": "0x..."}, "confirmations": 10, "explorer_tx": "https://.../tx/{}"}}`: your own EVM networks by key. USDC/USDT with 6 decimals. Optional per chain: `native_symbol`, `fee_hint`, `testnet`, `max_log_range`. With **Chains** empty they are enabled; with **Chains** set, list their keys there |

## Plugin commands

All four are under **SCFG** in the event editor. Unlock Item, Tip and Restore Purchases show the sheet and
the event **waits** until the player closes it. In a common event the common event waits, and so does the event
that called it (MZ runs a common event inside its caller).

| Command | Arguments |
|---|---|
| **Unlock Item** | **Item**: the item's Id. **Status Variable**: gets the status code |
| **Tip** | **Amount**: the suggested tip, like `2.50` (the player may change it); empty means `1.00`. **Token**: `USDC` or `USDT`; empty means the first enabled token. **Status Variable** |
| **Restore Purchases** | **Status Variable**. Shows the restore screen, where the player pastes a transaction hash |
| **Wallet Help** | None. Opens the players' guide to wallets in the browser, and does not wait |

### Status codes

The Status Variable holds one of these once the player closes the sheet. In scripts they are `SCFG.STATUS`
(`SCFG.STATUS.paid === 1`).

| Code | `SCFG.STATUS` | Unlock Item and Tip | Restore Purchases |
|---|---|---|---|
| 0 | `none` | Not done: the sheet is still open, or a save made while it was open was loaded (the purchase itself is kept and its switch is set) | same |
| 1 | `paid`, `restored` | Paid | Restored |
| 2 | `already_owned`, `nothing_found` | Already owned (no sheet was shown) | Nothing found |
| 3 | `pending` | The player chose **Continue playing** while the payment confirms. The final result arrives later: read the owned switch, or use `SCFG.onResult` | n/a |
| 4 | `underpaid` | Underpaid | n/a |
| 5 | `expired` | The payment window ran out | n/a |
| 6 | `cancelled` | Cancelled | Closed before a restore finished |
| 7 | `failed` | The command could not run (a misused call, payments off, no sheet to show). The console says why | same |

If you use the same variable for several commands, read it right after the command: `0` again means the
next command is waiting.

## Owned switches

> **The plugin owns the Owned Switches.** Each one is ON exactly while its item is owned, and OFF otherwise. Use
> it for nothing else, and do not turn it ON or OFF from events: the plugin puts it back.

The plugin sets them after New Game, after **every Load** (also a save made before the purchase), when a
payment confirms or purchases are restored (also in the middle of a map), when the stored receipts are first
read, and after each plugin command. An item added only from a script has no switch: read `SCFG.owns(id)`.

Purchases are global, not per save slot, so every slot and every New Game sees them.

When the game boots, MZ's boot scene waits until the stored purchases are read, as it waits for MZ's own
options and save list: the title screen and every script after it already see `SCFG.owns(...)` and the
switches.

## The script API: `window.SCFG`

`SCFG` is a global from the moment the plugin loads. A misused call **throws** (`configure`, `addChain`,
`addItem`, `owns`, `hasTipped`, `tipTotal`) or **rejects** (`pay`, `restore`) with an error that has a `code`
(for example `item.unknown`, `configure.not_called`) or a `TypeError`.

What is not your script's mistake does not reject: `pay()` and `restore()` resolve a result with
`status: "failed"` and an `error` code instead, also given to the `callback`:

- `payments.unavailable`: payments cannot run right now (the stored receipts cannot be read, or are still being
  read because the game has not finished booting; the payment engine could not start; the game is closing);
- `sheet.unavailable`: a replaced `SCFG.sheetHost` showed nothing.

> **An unhandled rejection stops an MZ game.** MZ listens for unhandled Promise rejections and uncaught errors
> and shows its error screen (`SceneManager.onReject` / `onError`): the game is over for the player. So always
> end a `pay()` or `restore()` chain with `.catch(...)`, or `await` it inside `try`/`catch`. The plugin commands
> catch for you.

```ts
interface SCFG {
  readonly version: string;              // the plugin's version
  readonly STATUS: { none: 0, paid: 1, already_owned: 2, pending: 3, underpaid: 4,
                     expired: 5, cancelled: 6, failed: 7, restored: 1, nothing_found: 2 };

  addChain(key: string, opts: ChainOptions): void;
  configure(opts: ConfigureOptions): void;
  addItem(id: string, opts: ItemOptions): void;

  pay(kind: "unlock" | "tip", opts?: PayOptions): Promise<PaymentResult>;
  restore(opts?: { callback?: (r: RestoreResult) => void }): Promise<RestoreResult>;

  owns(id: string): boolean;
  hasTipped(): boolean;
  tipTotal(): string;                    // a decimal string: "0", "2.50"
  receipts(): Receipt[];                 // copies
  pending(): Record<string, any>[];      // payments still being watched, copies
  exportReceipts(): string;
  supported(): true;
  clearLocalData(): void;
  onReceiptsChanged(fn: () => void): () => void;   // returns the unsubscribe function

  notify: boolean;                       // default true
  onResult: ((r: PaymentResult) => void) | null;
  texts: Record<string, string>;
  text(id: string, ...args: unknown[]): string;
  css: string;
  sheetHost: SheetHost;
}

interface ConfigureOptions {
  wallet?: string;                       // EVM, 0x...
  solana_wallet?: string;
  testnet?: boolean;
  chains?: string[];  tokens?: string[];  methods?: object;
  rpc_urls?: Record<string, string[]>;
  confirmations?: Record<string, number>;
  previous_wallets?: string[];
  logo?: string;                         // a file in img/scfg/: "logo" is img/scfg/logo.png
  accent?: string;                       // "#rrggbb"
}
interface ItemOptions { code: number; price: string; token: "USDC" | "USDT"; title: string; min_price?: string }
interface ChainOptions {                 // addChain(key, opts), before configure()
  name: string; chain_id: number; rpc: string[]; tokens: Record<string, string>;
  confirmations: number; explorer_tx: string;     // an https URL with {} where the hash goes
  native_symbol?: string; fee_hint?: string; testnet?: boolean; max_log_range?: number;
}
interface PayOptions {
  item?: string;                         // required for "unlock"
  amount?: string;                       // "tip": required, the suggested amount
  token?: string;                        // "tip": required
  callback?: (r: PaymentResult) => void;
}
interface PaymentResult {
  status: "paid" | "already_owned" | "cancelled" | "expired" | "underpaid" | "failed" | "pending";
  kind: "unlock" | "tip";
  item: string | null;
  amount: Dec | null;                    // the core's decimal; String(amount) gives "4.99"
  token: string | null;  chain: string | null;
  tx_hash: string | null;  payer: string | null;
  session_id: string | null;
  error: string | null;
}
interface RestoreResult {
  status: "restored" | "nothing_found" | "cancelled" | "failed";
  restored: Receipt[];                   // the receipts that were added
  error: string | null;
}
```

A **receipt** has `chain`, `tx_hash`, `log_index`, `block`, `kind`, `item`, `amount`, `token`, `payer`,
`recipient` and `source`. Amounts are strings (`"4.99"`), never floats.

| Call | Notes |
|---|---|
| `SCFG.addChain(key, opts)` | Adds your own EVM chain. Call it before `configure()` |
| `SCFG.configure(opts)` | Sets the wallet and options. It can be called again until payments have started (the frame after the stored receipts are read); after that it throws `configure.after_start` |
| `SCFG.addItem(id, opts)` | Declares a paid unlock |
| `SCFG.pay("unlock", {item})` | Shows the sheet; the Promise resolves when the player closes it. An owned item resolves `already_owned` at once, with no sheet |
| `SCFG.pay("tip", {amount, token})` | The same for a tip |
| `SCFG.restore()` | Shows the restore screen; resolves when it closes. `opts.callback` gets the result once a restore finishes |
| `SCFG.owns(id)` | `true` if a stored, verified payment unlocks the item. Instant, offline |
| `SCFG.hasTipped()`, `SCFG.tipTotal()` | Whether and how much the player has tipped |
| `SCFG.receipts()`, `SCFG.pending()` | Deep copies of the stored receipts and the payments still being watched |
| `SCFG.exportReceipts()` | Support helper. On desktop it writes `save/scfg-receipts.json` and returns the path; on the web it returns the JSON text |
| `SCFG.clearLocalData()` | Testing helper: forgets receipts and pending payments on this device. The payments stay on the chain and Restore brings them back. Called before the stored receipts are read (while the plugins load), it takes effect once they are |
| `SCFG.onReceiptsChanged(fn)` | Calls `fn` when the receipts change and once when they are first read. Returns the unsubscribe function |
| `SCFG.notify` | Show a notice when a payment confirms. Default `true`, or the **Notify** parameter |

`pay()`'s `status: "pending"` means the player chose **Continue playing**. The plugin keeps watching in the
background, across restarts, and the final result arrives later through the `callback` or `SCFG.onResult`.

A `callback` gets this call's own `already_owned`, `failed` and `cancelled` results and every later result of that
payment, until the payment is over (its `paid` or `underpaid` result, or the session ending unpaid); then it is released. Without one, a cancel and the later results reach `SCFG.onResult`, also when the awaited call
returns the same result (a payment that confirms while its sheet is open): keep the handler idempotent, for
example refresh what is unlocked from `SCFG.owns()` rather than counting results.

### Calling it from an event or a plugin

A **Script** event command runs its text as plain, non-async JavaScript: `await` there is a `SyntaxError`, which
stops the game, and the event does not wait for a Promise either. In events, use the plugin commands (the event
waits, and the Status Variable tells you the result). If you do call the script API from a Script command, chain
it, and know that the event goes on at once:

```js
SCFG.pay("unlock", { item: "chapter2" })
  .then((r) => { if (r.status === "paid") $gameSwitches.setValue(10, true); })
  .catch((e) => console.error(e));
```

`await` belongs in your own plugin's `async` functions:

```js
async function buyChapter2() {
  try {
    const r = await SCFG.pay("unlock", { item: "chapter2" });
    return r.status === "paid" || r.status === "already_owned";
  } catch (e) {          // a misused call (an unknown item, a bad option)
    console.error(e);
    return false;
  }
}
```

### Configuring from a script

A plugin listed **below** SCFG may call `SCFG.configure({...})` again from its own script, before the title
screen (payments start once the game has booted). It replaces the whole configuration, **Items included**, so
add them again with `SCFG.addItem(id, {...})`. The Custom Chains stay. An item re-added under the Id an Items
entry has keeps that entry's Owned Switch; an item only added from a script has no switch; an Items entry the
script does not add again is reported and its switch is left as it is.

```js
SCFG.addChain("myl2", { name: "My L2", chain_id: 4242, rpc: ["https://rpc.example"],
                        tokens: { USDC: "0x..." }, confirmations: 10,
                        explorer_tx: "https://scan.example/tx/{}" });
SCFG.configure({ wallet: "0xYourWallet", testnet: true });
SCFG.addItem("chapter2", { code: 1, price: "4.99", token: "USDC", title: "Chapter 2" });
```

## Pausing the game while the sheet is open

The sheet blocks the game's input while it is open: it reads MZ's `Input` and `TouchInput` (arrows, OK, cancel,
the gamepad) and the game does not see those keys. It does not pause the game's own processing, though, so
parallel events and other plugins keep running behind it. A plugin command makes its own event wait; a Script
command cannot (see [Calling it from an event or a plugin](#calling-it-from-an-event-or-a-plugin)); in your own
plugin, `await SCFG.pay(...)` before you go on.

## Customising the sheet

The sheet is an HTML overlay in a shadow root on top of the game canvas, in the same "Margin" look as the other
engines. From least to most work:

1. **Logo** and **Accent**, as parameters or in `SCFG.configure({logo, accent})`. The accent colors the selected
   option, the forward button, the progress and the dots; text on it is white or dark, whichever reads better.
   The logo is a file in `img/scfg/`. In a desktop test play a missing file is reported when you configure, and
   the plugin stays off until you fix it. In a released game a logo that cannot be read is a console warning
   and the screens show no logo; payments go on. A deployment made with **Encrypt image files** ships
   `logo.png_` instead of `logo.png`: the plugin reads and decrypts it the way MZ does its own images (MZ
   encrypts only png images: a jpg or webp logo ships as it is). A jpg or webp logo can only be set from a
   script, and **Exclude unused files** drops a file only a script names: see **Logo** in
   [Plugin parameters](#plugin-parameters). In a web game the logo is read when a payment starts, usually long
   before the player opens the wallet overlay; if it is still being read then, the overlay waits for it up to
   one second and otherwise opens without the logo.
2. **`SCFG.css`**: a stylesheet text that is appended to the sheet's own (inside its shadow root, so your game's
   CSS never reaches it, and its CSS never reaches the game). The colors are custom properties on the host
   (`--scfg-accent`, `--scfg-ink`, `--scfg-line`, `--scfg-ok`, `--scfg-warn`, `--scfg-error`, `--scfg-dim`,
   `--scfg-sheet`, ...) and `--scfg-k`, the size scale. The class names are listed at the top of
   `rpgmaker/src/mz/ui/margin.css` in the repository. Controls carry `data-key` attributes naming them
   (`forward`, `cancel`, `tx_input`, ...).

   ```js
   SCFG.css = ".sheet { border-radius: 0; } :host { --scfg-ink: #101820; }";
   ```
3. **`SCFG.sheetHost`**: replace it to show the payment sheet, the restore screen and the notices your own way
   (for example with MZ windows). The interface is:

   ```ts
   interface SheetHost {
     openPayment(ctl: CheckoutController): SheetHandle | null;
     openRestore(ctl: RestoreController): SheetHandle | null;
     close(handle: SheetHandle): void;      // after the controller was closed
     notify(text: string): void;            // already in the player's language
     update?(): void;                       // optional: once a frame
   }
   interface SheetHandle { closed: Promise<"cancel" | "keep_playing" | "finish" | string> }
   ```

   The handle's `closed` promise resolves once, when the player is done: `"cancel"`, `"keep_playing"` (only
   once a session exists) or `"finish"` (after a final state). Read the controller's fields and call its
   methods; never call `poll()` (the plugin does, every frame). Buttons that leave the game call the plugin's
   actions: `SCFG.openBrowser(ctl)`, `SCFG.openWalletApp(ctl)`, `SCFG.openExplorer(url)`, `SCFG.copy(text)` and
   `SCFG.pasteInto(ctl)`. A host that returns `null` shows nothing, and `pay()` resolves `failed` with the `error`
   `sheet.unavailable`.
   The controllers' fields are those of the core in `rpgmaker/src/core/checkout.ts`; `rpgmaker/src/mz/sheet_host.ts`
   holds the full contract.

## Texts

Every player-facing text has an id. A text is, strongest first: `SCFG.texts[id]` (script), the **Texts**
plugin parameter, then the built-in English. `%s` takes the arguments in order (and `%%` is a percent sign):
keep each `%s` of the original in your wording. An id nobody defines shows as itself; in test play a wrong id
**throws**, because it is a typo.

```js
SCFG.texts.payment_confirmed = "Merci pour votre soutien !";
SCFG.texts.cancel = "Annuler";
```

`SCFG.text(id, ...args)` returns the text for an id. The checkout page that opens in the player's browser
(desktop browser wallets) stays English. The ids and the English are listed [at the end of this page](#text-ids).

## Desktop (NW.js)

- **Browser wallet.** The wallet extension lives in the player's own browser, not in the game window. So the
  plugin serves the checkout page on `127.0.0.1` (a random port, a secret in the address, nothing else
  reachable) and opens it in the system browser with `nw.Shell.openExternal`. The page talks to the wallet; the
  sheet in the game shows the progress. Phone QR and manual transfer need no browser.
- **RPC** goes through Node's `https`, not `fetch`: NW.js's `fetch` runs from the game page's origin, so it
  depends on each RPC server's CORS headers, which Node's `https` never needs. NW.js's Node trusts only the
  certificate bundle built into it and ignores the system's proxy settings, so a request Node refuses for a
  certificate (a company or antivirus root certificate, a certificate newer than the bundle) or cannot get
  through (the name does not resolve, the connection is refused, reset or times out, a proxy asks for a
  password) is sent once more through `fetch`, which uses the system's certificates and proxy.
- **Minimised window.** MZ's frames stop while the window is minimised or hidden (the browser stops animation
  frames), and with them the plugin's checks: the sheet and the checkout page in the browser show no progress
  until the player comes back. Nothing is lost: the plugin catches up on the next frames.
- **NW.js version.** The sheet's layout spaces its rows with CSS flexbox `gap`, which Chromium has from version 84
  (NW.js 0.47). An MZ release that ships an older NW.js (unverified) shows the sheet with its controls touching;
  it still works. Update NW.js in your build if it does.
- **The plugin file must be named `SCFG.js`.** MZ hands a plugin its parameters by the file's name; renamed,
  the plugin finds no parameters.
- **Storage** is `save/scfg.rmmzsave` next to your slots. Deleting a player's `save/` folder deletes their
  receipts; Restore Purchases brings them back.
- **Closing the window.** The plugin saves new receipts and new payment sessions the moment they appear. It
  does **not** register an NW.js window `close` listener: a listener there takes over closing and can override
  other plugins' quit prompts. The trade-off: scan progress since the last save (at most about 10 seconds) can
  be lost when the window is closed, and is re-scanned at the next start. Nothing a player paid is lost.

## Web deployment

- **No server and no setup.** The checkout page is inside `SCFG.js`. On the web the browser wallet step opens as
  an overlay in the game's own page, and the plugin talks to the wallet extension there. You do not copy any
  extra files or change any server settings.
- **Broken wallet extensions.** An uncaught error in the page stops an MZ game, so the checkout page never
  lets one out of its own code: a wallet extension that announces itself badly or throws odd values at it is
  left out or shown as a wallet error, and the player's other wallets still pay. An extension that throws
  inside its own code (its own event listeners, its own timers) can still stop the game: the page contains
  only its own code, and that error is the extension's to fix.
- **HTTPS.** Wallet extensions, the clipboard and the RPC calls expect HTTPS or `localhost`. Without a secure
  context, Copy and Paste are refused and the sheet says so.
- **Receipts persist in the browser's storage.** MZ's web build keeps the global save data in `localforage`
  (IndexedDB, with a localStorage fallback), under the origin of the page. Receipts therefore survive
  restarts but belong to the browser profile and the origin: clearing site data deletes them (Restore
  Purchases brings them back with the transaction hash) and a private window loses them when it closes. In test play the plugin warns once if the
  browser cannot keep them.
- **Several games on one origin.** The receipts are one localforage entry, `rmmzsave.<gameId>.scfg`, next to
  MZ's own `rmmzsave.<gameId>.global` and `rmmzsave.<gameId>.config`. `<gameId>` is the game's
  `$dataSystem.advanced.gameId` (`advanced.gameId` in `data/System.json`), so games on one origin, such as a
  host that serves many games from one domain, keep their receipts apart. localforage writes one entry at a
  time, so a game never deletes another game's entry. A project copied from another one keeps that
  project's gameId: give a copy that ships as a separate game its own `advanced.gameId`, or the two share
  receipts (and saves) on one origin. This is checked on the stand-in
  (`twoGamesOnOneOriginKeepTheirOwnState` in `src/mz/store.test.ts`), not yet in real MZ.
- **If the page cannot run the overlay** (reported in test play), copy, paste and links fall back to the
  browser's own APIs, and there is no browser wallet method.
- **Embedding.** A game inside a third-party iframe usually has no storage and no wallet. Serve it from a page
  of its own.

## Mobile (unverified)

> Mobile is best effort and has **not** been tested on a device or in MZ's mobile export. Desktop and web are
> the verified targets (on the stand-in).

On a phone or tablet the sheet does not start a localhost page. It offers an **Open wallet app** button that
opens the payment request link (EIP-681 for EVM chains, Solana Pay for Solana), plus the manual transfer and QR
methods. Whether a given wallet app handles the link, and what happens when the game is sent to the
background, is untested. Test your build on real devices before shipping it, and keep the manual transfer
method enabled.

## Text ids

The built-in English of every text. `%s` is where an argument goes.

| Id | English |
|---|---|
| `choose_amount` | Choose an amount |
| `or_enter_your_own` | Or enter your own |
| `amount` | Amount |
| `tip` | Tip |
| `summary_line` | %s: %s %s on %s |
| `how_to_pay` | How do you want to pay? |
| `which_network` | Which network holds your %s? |
| `fees_lowest` | lowest fees |
| `fees_low` | low fees |
| `fees_high` | high fees |
| `phone_wallet` | Phone wallet |
| `scan_with_phone_wallet` | Scan a code with your phone wallet |
| `browser_wallet` | Browser wallet |
| `manual_transfer` | Manual transfer |
| `send_from_exchange` | Send from an exchange or any wallet |
| `evm_extension_sub` | MetaMask, Rabby or another wallet extension |
| `evm_browser_sub` | MetaMask, Rabby or another wallet in this browser |
| `solana_extension_sub` | Phantom, Solflare or another wallet extension |
| `solana_browser_sub` | Phantom, Solflare or another wallet in this browser |
| `evm_opens_browser_note` | Opens a checkout page in your browser for MetaMask, Rabby, Coinbase Wallet and other wallet extensions. |
| `solana_opens_browser_note` | Opens a checkout page in your browser for Phantom, Solflare, Backpack and other wallet extensions. |
| `evm_opens_here_note` | Opens your wallet right here, over the game: MetaMask, Rabby, Coinbase Wallet and other wallet extensions. |
| `solana_opens_here_note` | Opens your wallet right here, over the game: Phantom, Solflare, Backpack and other wallet extensions. |
| `preparing_payment` | Preparing the payment... |
| `qr_step_open` | 1. Open your wallet app |
| `qr_step_scan` | 2. Scan the code |
| `qr_step_approve` | 3. Approve %s |
| `open_wallet_app` | Open wallet app |
| `open_wallet` | Open wallet |
| `approve_in_wallet` | Approve %s on %s in your wallet. |
| `wallet_will_ask` | Your wallet will ask for %s. The last digits identify your payment and add less than 1 cent. |
| `wallet_changes_amount` | If your wallet changes the amount, the game still finds the payment. |
| `wallet_changes_amount_manual` | If your wallet changes the amount, use Manual transfer and paste the transaction hash. |
| `open_checkout_browser` | Open checkout in browser |
| `reopen_checkout_page` | Reopen checkout page |
| `reopen_wallet_window` | Reopen wallet window |
| `finish_in_browser` | Finish the payment on the checkout page in your browser. |
| `finish_in_wallet_window` | Finish the payment in the wallet window. |
| `to_address` | To address |
| `token_contract` | Token contract |
| `token_mint` | Token mint |
| `send_exact_amount` | Send exactly this amount on %s only. Sending on another network may lose your funds. |
| `send_exact_amount_solana` | Send exactly this amount on Solana (SPL) only. Withdrawing on another network may lose your funds. |
| `copy` | Copy |
| `copy_hash` | Copy hash |
| `copy_signature` | Copy signature |
| `paste` | Paste |
| `i_sent_it` | I've sent it |
| `sent_from_exchange_paste` | Sent from an exchange? Paste the %s: |
| `paste_hash_help` | Paste the transaction hash or signature of your payment. You can find it in your wallet's activity, or in your exchange's withdrawal history. |
| `your_hash` | Your %s: |
| `word_tx_hash` | transaction hash |
| `word_signature` | signature |
| `check` | Check |
| `waiting_for_payment` | Waiting for your payment on %s... |
| `tx_received_waiting` | Transaction received. Waiting for it to be included on %s... |
| `confirming` | Confirming on %s... %s/%s |
| `checking_blockchain` | Checking the blockchain... |
| `already_paid_note` | If you already paid, the game will keep checking for 24 hours and unlock it automatically. |
| `keep_playing_note` | You can keep playing: the game keeps checking in the background and finishes the purchase by itself. |
| `continue_playing` | Continue playing |
| `keep_as_tip` | Keep as a tip |
| `paid_title` | Payment confirmed. Thank you! |
| `payment_too_small` | Payment too small |
| `underpaid_note` | The amount received was less than the price. Please contact the developer. |
| `payment_window_closed` | The payment window has closed |
| `payment_screen_inactive` | This payment screen is no longer active. Any payment you made is still being checked in the background. |
| `view_on_explorer` | View on explorer |
| `restore` | Restore |
| `restore_purchases` | Restore purchases |
| `restored` | Restored: |
| `purchases_on_device` | Purchases on this device |
| `keep_this_to_restore` | Keep this %s to restore your purchase on another device: |
| `nothing_to_restore` | Nothing new to restore. These purchases are already on this device. |
| `unmatched_payment_prompt` | Payment of %s %s on %s. What was it for? |
| `screen_inactive` | This screen is no longer active. |
| `saving` | Saving... |
| `set` | Set |
| `dev_mode_paying_to` | DEV MODE: paying to %s |
| `dev_mode_paying_to_testnet` | DEV MODE: paying to %s (testnet) |
| `dev_solana_cant_receive` | Your Solana wallet can't receive %s yet: receive any amount once. |
| `qr_code` | QR code |
| `back` | Back |
| `next` | Next |
| `continue` | Continue |
| `cancel` | Cancel |
| `close` | Close |
| `payment_confirmed_item` | Payment confirmed: %s. Thank you! |
| `payment_confirmed` | Payment confirmed. Thank you for your support! |
| `purchases_restored` | Purchases restored. |
| `copy_failed` | Couldn't copy: your browser blocked the clipboard. |
| `network_unavailable` | Can't reach the network. Retrying... |
| `tx_reverted` | That transaction failed on-chain; no funds were sent. |
| `tx_not_a_payment` | That transaction isn't a payment for this purchase. |
| `tx_not_landed` | That transaction didn't reach the network. You can pay again. |
| `invalid_hash` | That doesn't look like a transaction hash or signature. |
| `invalid_amount` | Enter an amount of at least 0.50 with up to 2 decimals. |
| `browser_unavailable` | Couldn't start the browser checkout. Use another method. |
| `start_failed` | Couldn't start the payment. Check your connection and try again. |
| `solana_unavailable` | Payments on Solana aren't available right now. |
| `internal_error` | Something went wrong while checking the payment. Retrying... |
| `paste_blocked` | Your browser blocked pasting. Type it instead: the game also finds the payment without it. |
| `restore_invalid_hash` | That doesn't look like a transaction hash or signature. |
| `restore_tx_not_found` | Transaction not found. Check the hash, or try again later. |
| `restore_tx_reverted` | That transaction failed on-chain; nothing was paid. |
| `restore_tx_not_a_payment` | This transaction isn't a payment to this game. |
| `restore_network_unavailable` | Can't reach the network. Try again in a moment. |
| `restore_paste_blocked` | Your browser blocked pasting. Type the hash instead. |
| `restore_failed` | Something went wrong. Please try again. |
