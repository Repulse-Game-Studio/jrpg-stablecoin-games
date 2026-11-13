# API reference (Godot)

Everything goes through the autoload `Scfg` (`addons/scfg/scfg.gd`). It mirrors the Ren'Py plugin's calls
in snake_case, with options in a Dictionary.

## Misuse never crashes the game

GDScript has no exceptions. A misused call reports `scfg: <message>` with `push_error` and:

- `configure`, `add_chain` and `add_item` return the `ScfgErr` they reported (and `null` on success);
- `pay()` and `restore()` resolve to a result with `status = "failed"` and `error = <code>` (for example
  `"item.unknown"`, `"configure.not_called"`);
- queries such as `owns()` report and return their neutral value (`false`).

Setup advice (a token the wallet cannot receive yet, a corrupt state file) is printed in debug builds only.

## Calls

| Call | Returns |
|---|---|
| `Scfg.add_chain(key, opts)` | `null`, or the `ScfgErr`. Adds your own EVM chain; call before `configure()`. `opts`: `name`, `chain_id`, `rpc` (Array of https URLs), `tokens` (`{"USDC": "0x..."}`, 6-decimal tokens), `confirmations`, `explorer_tx` (https URL with `{}` for the hash), optional `native_symbol`, `fee_hint`, `testnet`, `max_log_range` |
| `Scfg.configure(opts)` | `null`, or the `ScfgErr`. `opts`: `wallet` (EVM `0x` address) and/or `solana_wallet`, `testnet`, `chains`, `tokens`, `methods`, `rpc_urls`, `confirmations`, `previous_wallets`, `logo` (`res://` path to a png, jpg or webp), `accent` (`"#rrggbb"`), and the Godot-only `theme` and `sheet_scene` |
| `Scfg.add_item(id, opts)` | `null`, or the `ScfgErr`. `opts`: `code` (1-99, never change it after release), `price` (`"4.99"`), `token` (`"USDC"` or `"USDT"`), `title`, optional `min_price` |
| `await Scfg.pay(kind, opts)` | `ScfgPaymentResult`, when the payment sheet closes. `kind` is `"unlock"` (`opts.item` required) or `"tip"` (`opts.amount`, the suggested amount, and `opts.token` required). An unlock also takes an optional `opts.amount`, the amount to charge instead of the item's price (at least its `min_price`, which defaults to the price: so pay-what-you-want above the price, or a sale down to `min_price`), and an optional `opts.token`, another enabled token than the item's. `opts` also takes `callback` (a Callable taking the result). Amounts are Strings, never floats |
| `await Scfg.restore(opts)` | `ScfgRestoreResult`; shows the restore screen where the player pastes a transaction hash. `opts.callback` gets the result once a restore finishes |
| `Scfg.owns(item_id)` | `true` if a stored, verified payment unlocks the item. Instant, works offline |
| `Scfg.has_tipped()` | `true` once the player has tipped any amount |
| `Scfg.tip_total()` | The sum of tips as a decimal String (`"0"`, `"2.50"`) |
| `Scfg.receipts()` | Stored receipts, `{key: receipt}`, a deep copy. A receipt has `chain`, `tx_hash`, `log_index`, `block`, `kind`, `item`, `amount`, `token`, `payer`, `recipient`, `source` |
| `Scfg.pending()` | Payments still being watched, `{session id: record}`, a deep copy |
| `Scfg.export_receipts(path)` | Writes the receipts as readable JSON (default `user://scfg-receipts.json`). Returns the absolute path, or a `ScfgErr` |
| `Scfg.supported()` | Always `true`, web export included (kept for parity with Ren'Py) |
| `Scfg.clear_local_data()` | Testing helper: forgets receipts and pending payments on this device. The payments stay on the chain and Restore brings them back |
| `Scfg.notify = true` | Show a "Payment confirmed" toast through the sheet host. Default `false` |

`configure()` can be called again until payments have started (the frame after a successful call). After
that it is refused with `configure.after_start`.

## ScfgPaymentResult

What `await Scfg.pay(...)`, a `pay()` callback and the `payment_resolved` signal give you. Every field except
`status` is a `String` or `null`.

| Field | Meaning |
|---|---|
| `status` | `"paid"`, `"already_owned"` (no sheet is shown), `"cancelled"`, `"expired"`, `"underpaid"`, `"failed"` or `"pending"` |
| `kind` | `"unlock"` or `"tip"` |
| `item` | The item id (unlocks) |
| `amount` | The amount actually paid, as a decimal String (`"4.99"`), or `null` when nothing matched on chain |
| `token`, `chain` | What was paid with, and on which chain |
| `tx_hash`, `payer` | The transaction and the paying address |
| `session_id` | The payment session |
| `error` | `null`, a configuration error code for a misused call, or the reason a payment failed |

`"pending"` means the player chose **Continue playing**. The addon keeps watching in the background, across
restarts, and the final result arrives later through the `callback` or `payment_resolved`. `to_dict()` gives
the fields as a Dictionary.

## ScfgRestoreResult

| Field | Meaning |
|---|---|
| `status` | `"restored"`, `"nothing_found"`, `"cancelled"` (the screen was closed before a restore finished) or `"failed"` |
| `restored` | Array of the receipts that were added (Dictionaries with `chain`, `tx_hash`, `kind`, `item`, `amount`, `token`, ...) |
| `error` | `null`, a lookup failure code such as `"tx_not_found"`, or a configuration error code |

## Signals

- `payment_resolved(result: ScfgPaymentResult)`: a final result that has no `pay()` callback to go to (for
  example the payment that confirms after a restart, or a cancel without a callback). Connect it once, at
  startup. It also fires for the results of a `pay()` you **await without a callback**: a payment that
  confirms while its sheet is open is emitted when it confirms, and a cancel when the sheet closes, and
  then the same result is what `await` returns (Ren'Py's `on_result` works the same way). So the same
  result can reach your code twice. Keep the handler idempotent (refresh what is unlocked from
  `Scfg.owns()` rather than counting results), or pass a `callback` to `pay()`, which then gets those
  results instead of the signal.
- `receipts_changed`: the stored receipts changed (a payment confirmed, a restore, `clear_local_data()`).

A `callback` given to `pay()` receives this call's own `already_owned`, `failed` and `cancelled` results and
every later result of that payment, until the payment is over (its `paid` or `underpaid` result, or the session
ending unpaid: after 24 hours, or when the player switched networks); then the addon releases it. Without one,
the cancel and the later results reach `payment_resolved` (as well as the value `await` returns, see above),
while `already_owned` and `failed` are only returned.

## Pausing the game while the sheet is open

The sheet dims the game and takes focus, but it does **not** stop game nodes that read input in `_input` or
poll `Input` in `_process`. A game that keeps reading the confirm key would act on keys the player is
pressing in the sheet. Set `get_tree().paused = true` while you await `pay()` or `restore()` and unpause
afterwards. The autoload and the screens run with `PROCESS_MODE_ALWAYS`, so they keep working while paused
(the payment checks continue too). The demo does exactly this.

```gdscript
get_tree().paused = true
var result := await Scfg.pay("tip", {"amount": "2.00", "token": "USDC"})
get_tree().paused = false
```

## Customising the sheet

From least to most work:

1. `logo` and `accent` in `configure()`.
2. `Scfg.configure({"theme": my_theme})`: a `Theme` merged over the built-in look. Your definitions win; your
   Theme is not changed. Style the screens through the type variations listed at the top of
   `addons/scfg/ui/theme.gd` (`ScfgTitle`, `ScfgAccent`, `ScfgOption`, `ScfgSheetPanel`, ...) and the colors
   of the type `ScfgSheet` (`accent`, `on_accent`, `ink`, `line`, `ok`, `warn`, `error`, `muted`, `dim`), or the base types
   (`Label`, `Button`, `LineEdit`). The built-in theme is scaled for the window (designed at 1280x720); yours
   is used as given.
3. `Scfg.configure({"sheet_scene": preload("res://my_sheet.tscn")})`: your scene replaces the payment sheet.
   Its root needs `func bind(controller, scfg)` and `signal closed(how)`. `bind()` is called once, before the
   root enters the tree, with the `ScfgCheckoutController` and the `Scfg` autoload. Show the controller's
   fields, call its methods (`choose_network`, `choose_method`, `next_step`, `set_tip_amount`, `mark_sent`,
   `submit_hash`, `back`) and the autoload's actions (`open_browser`, `open_wallet_app`, `open_explorer`,
   `copy`, `paste_into`). Emit `closed("cancel")`, `closed("keep_playing")` (only once a session exists) or
   `closed("finish")` (after a final state), once. Never call the controller's `poll()`: the autoload does. A
   root without `bind` and `closed` is not shown, and `pay()` resolves `failed` with `sheet.unavailable`.
4. Replace `Scfg.sheet_host` with your own `ScfgSheetHost` (`addons/scfg/sheet_host.gd`) to show the payment
   sheet and the restore screen, and notifications, any way you like. The restore screen is only replaceable
   this way.

`theme` and `sheet_scene` are checked in `configure()`: a wrong type is reported as a `TypeError`.

## Translations

Every text the screens show goes through `tr()`, and the core's player messages are translated when shown.
`addons/scfg/translations/scfg.pot` is the template. Translate it into a `.po` file for your language and add
that under **Project Settings > Localization > Translations**. Item titles also go through `tr()`. Addresses,
amounts and chain names are never translated.

## Web exports

On a web export there is no localhost server: the checkout page opens as an overlay in the game's own page
and talks to the wallet extension there. A broken wallet extension (one that announces itself badly or throws odd
values) can't break the game's page: the checkout leaves it out or shows a wallet error, and the player's other
wallets still pay. What the export needs:

- **Export filter.** The page's files are not Godot resources. Add `addons/scfg/web/*` to the web preset's
  **Filters to export non-resource files/folders**. A debug build warns, naming each missing file.
- **Single-threaded export.** The addon needs no threads (RPC goes through `HTTPRequest`, which uses `fetch`),
  so turn **Thread Support** off. The page then needs no cross-origin isolation (COOP/COEP) headers.
  `godot/demo/export_presets.cfg` is a working preset.
- **Secure context.** Wallet extensions, the clipboard and the RPC calls expect HTTPS or `localhost`. Without
  one, Copy and Paste are refused and the sheet says so.
- **Receipts persist in IndexedDB.** `user://scfg/state.json` lives in the browser's IndexedDB, which belongs
  to the page's origin. Every Godot web export uses the same database there (`/userfs`), and `user://` is a
  folder in it named after `application/config/name`. Each save writes the whole database from the page's
  own in-memory copy, which it loaded at startup. So on an origin that serves several games:
  - two games with the same project name share `user://scfg/state.json` and can overwrite each other's
    receipts;
  - games with different names keep separate files, but a game deletes from IndexedDB whatever another game
    (or another tab) saved after it loaded. One game open in two tabs behaves the same for `state.json`: the
    last tab to save wins.

  So the addon also keeps a copy of the receipts in the page's `localStorage`, which other games do not
  rewrite. Every save merges the game's receipts into it, so a receipt another tab of the same game saved
  stays in the copy; only a receipt the game removed itself (failed re-verification, `clear_local_data()`)
  leaves it. `clear_local_data()` empties the copy, until another open tab of the same game saves: that
  tab's save merges its own receipts back into it. At startup the addon adds back any receipt the copy has
  and `state.json` lacks (when both have one, `state.json`'s is kept), saves the result at once, and
  re-verifies it on the chain like any stored receipt. So with one game open in two tabs, a receipt one tab
  saved comes back at the next start even when the other tab's `state.json` replaced it. Pending payments
  are not copied: a payment lost with `state.json` comes back with Restore and its transaction hash. If
  localStorage is missing, blocked or full, saving goes on as before and a debug build warns once.

  The copy's key is `scfg:<application/config/name>:<sha256 of the storage id>`. The storage id is the
  project setting `scfg/web/storage_id` if you set it, else the project name. Set it if another game on
  your origin might use the same project name: with the plugin enabled it is under **Project Settings >
  Scfg > Web > Storage Id**, or add it to `project.godot`:

  ```ini
  [scfg]

  web/storage_id="com.example.mygame"
  ```

  Don't change the storage id or the project name after a release: the copy would be looked for under a new
  key. Two games with the same project name still share `state.json` itself, so give the game a unique
  `application/config/name` too, and serve it from an origin of its own where you can. If the browser blocks
  IndexedDB (some private-browsing modes), `user://` is lost when the tab closes; a debug build warns once.

## Mobile (unverified)

> Mobile is best effort and has **not** been tested on a device. Desktop and web are the verified targets.

On Android and iOS exports the sheet does not start a localhost page. It offers an **Open wallet app**
button that opens the payment request link (EIP-681 for EVM chains, Solana Pay for Solana) through
`OS.shell_open`, plus the manual transfer and QR methods. `Scfg.is_mobile()` tells you which build you are
in. Whether a given wallet app handles the link, and what the wallet does when the game is sent to the
background, is untested. Test your build on real devices before shipping it, and keep the manual transfer
method enabled.
