# API reference (Unity)

Everything goes through the static class `SCFG` in the namespace `Scfg` (`using Scfg;`). It mirrors the Ren'Py
plugin's and the Godot addon's calls in PascalCase, with typed options instead of keyword arguments. The class is
`SCFG`, not `Scfg`: C# cannot tell a class `Scfg` from the namespace of the same name.

## Misuse never throws

A misused call reports `scfg: <message>` with `Debug.LogError` and:

- `Configure`, `AddChain` and `AddItem` return the error code they reported (and `null` on success);
- `Pay` and `Restore` resolve to a result with `Status = Failed` and `Error = <code>` (for example
  `"item.unknown"`, `"configure.not_called"`);
- queries such as `Owns` report and return their neutral value (`false`).

An exception thrown by your own handler (a `Pay` callback, an event) is logged as one `scfg: ... threw:` error line,
then the exception (`Debug.LogException`), and does not stop the package. Setup advice (a token the wallet cannot receive yet, a corrupt state file moved aside) is a
`Debug.LogWarning` in development builds and in the editor only.

| Code | Meaning |
|---|---|
| `configure.not_called` | A call that needs `Configure` came before it |
| `configure.after_start` | `Configure` after payments started (the frame after the first successful `Configure`) |
| `configure.after_add_chain` | `AddChain` after `Configure` |
| `wallet.missing`, `address.invalid`, `solana_address.invalid`, `solana_address.not_on_curve` | No payout wallet, or one that is not a valid address |
| `item.unknown` | `Pay` for an item `AddItem` did not declare |
| `item.invalid_id`, `item.invalid_code`, `item.duplicate_code`, `item.duplicate_id`, `item.invalid_price`, `item.invalid_title`, `item.min_price_above_price` | A bad `AddItem` |
| `chains.unknown`, `chains.wrong_network`, `tokens.unknown`, `token.not_enabled`, `config.no_usable_token` | A chain or token that is not built in, not on this network, not enabled, or that no wallet can receive |
| `methods.none_enabled`, `methods.bad_timeout`, `methods.qr_disabled_chain` | A bad `Methods` |
| `rpc.not_https`, `rpc_urls.unknown_chain`, `confirmations.unknown_chain`, `confirmations.invalid`, `confirmations.solana_fixed` | A bad `RpcUrls` or `Confirmations` entry |
| `chain.*` (`chain.invalid_key`, `chain.is_builtin`, `chain.duplicate_chain_id`, `chain.bad_explorer_tx`, ...) | A bad `AddChain` |
| `accent.invalid` | `Accent` is not `"#rrggbb"` |
| `tip.needs_amount_and_token`, `tip.below_minimum`, `payment.below_min_price` | A tip without `Amount` and `Token` or below the minimum, or an unlock `Amount` below the item's `MinPrice` |
| `sheet.unavailable` | No screen could be shown (`SCFG.SheetHost` is `null`, or it returned `null` or threw) |
| `TypeError` | A `null` argument (`Configure(null)`, a `null` request) |

The message logged with each code says what to change.

## Configuration

### SCFG.Configure

`string SCFG.Configure(ScfgOptions options)` sets the payout wallet and options. It returns `null`, or the error
code it reported. Call it before the first frame that needs payments (from `Awake` of your first scene, say). It can
be called again until payments have started, on the frame after a successful call; after that it is refused with
`configure.after_start`. A new `Configure` starts a new configuration: declare the items again after it.

| `ScfgOptions` field | Meaning |
|---|---|
| `Wallet` | Your EVM payout address (`0x...`). At least one of `Wallet` and `SolanaWallet` is required |
| `SolanaWallet` | Your Solana payout address (base58) |
| `Testnet` | `true` uses the test networks (Sepolia, Arbitrum Sepolia, Polygon Amoy, Base Sepolia, Arc Testnet, and Solana Devnet with USDC only), so no real money moves. Default `false`: set it to `true` while you develop |
| `Chains` | The networks offered, in order, by key (`"ethereum"`, `"arbitrum"`, `"polygon"`, `"base"`, `"arc"`, `"solana"`, or your own). Default: every network the wallets can receive on |
| `Tokens` | `"USDC"`, `"USDT"`. Default both |
| `Methods` | A `ScfgMethods`: which payment methods the sheet offers. Default: all of them |
| `Onramp` | A `ScfgOnramp`: the "Buy some with a card" link, see [The buy link](#the-buy-link-onramp). Default: on, on mainnet |
| `RpcUrls` | Your own RPC endpoints per chain key (`IDictionary<string, IList<string>>`, https URLs, or `http://127.0.0.1` / `http://localhost` with a port for a local node), tried before the built-in public ones |
| `Confirmations` | Confirmations required per chain key (`IDictionary<string, int>`) |
| `PreviousWallets` | Wallets you were paid to before (EVM and Solana addresses can be mixed). Their receipts stay valid |
| `Accent` | The accent color of the sheet and the checkout page, `"#rrggbb"` |
| `Logo` | A `Texture2D` shown on the sheet and the checkout page |
| `WebStorageId` | WebGL builds only: tells two games with the same product name on one web origin apart, see [WebGL builds](#webgl-builds). Set it once and never change it |

| `ScfgMethods` field | Meaning (a `null` field keeps the default) |
|---|---|
| `Browser` | The checkout page, for a wallet extension: on desktop in the player's browser, in a WebGL build over the game. Default `true` |
| `BrowserTimeoutMinutes` | Minutes the checkout page keeps answering. Default 30 |
| `Qr` | A QR code for a phone wallet. Default `true` |
| `QrWalletButton` | The **Open wallet app** button on phones. Default `true` |
| `QrChains` | The chains the QR code is offered on. Default: all |
| `Manual` | The address and exact amount to send by hand, from an exchange or any wallet. Default `true` |
| `ManualAllowTxPaste` | Whether the player may paste the transaction hash on the manual page. Default `true` |

### The buy link (`Onramp`)

Players who have no USDC or USDT yet see **No USDC yet? Buy some with a card** (with the payment's token) on the
method step, or on the only payment page when there is one method. It opens a buy page for the network they picked
in their browser (a new tab with `noopener,noreferrer` in a WebGL build), where they buy the token with a card into
their own wallet and then pay as usual: the payment itself still goes straight to your wallet. The default page is
thirdweb's hosted Buy widget, card payment only, locked to the network and token, suggesting the payment rounded up to
cents (plus a cent for the fee on Arc). The link carries only the network, the token's contract and that amount. On
a network it isn't offered on, like Solana, the sheet says **No USDC yet? Pick another network to buy some with a
card** instead, which goes back to the network step, when another network the player can pick has the link.

It is **on by default on mainnet**. Turn it off, or change it, with `ScfgOptions.Onramp` (or the SCFG Settings
asset's **Onramp Enabled**, **Onramp Url** and **Onramp Chains**):

```csharp
SCFG.Configure(new ScfgOptions { Wallet = "0x...", Onramp = new ScfgOnramp { Enabled = false } });          // hide it
SCFG.Configure(new ScfgOptions { Wallet = "0x...", Onramp = new ScfgOnramp { Chains = new[] { "base", "arc" } } });
SCFG.Configure(new ScfgOptions { Wallet = "0x...", Onramp = new ScfgOnramp { Url = "https://buy.example/?net={chain_id}&a={amount}" } });
```

| `ScfgOnramp` field | Meaning (a `null` field keeps the default) |
|---|---|
| `Enabled` | `true` (default) or `false` |
| `Chains` | The networks it is offered on (enabled EVM networks only). Default: the built-in EVM networks on mainnet, and none on testnet (cards don't buy test tokens). Name testnet networks here (for example `base`, `arbitrum`) to see the link and the Solana hint before mainnet |
| `Url` | Your own `https://` page with a host, without spaces, at most 2048 characters. `{chain}`, `{chain_id}`, `{token}`, `{token_address}` and `{amount}` are filled in; any other brace is refused |

A mistake is reported with its code: `onramp.bad_url` (also a url with no host after `https://`),
`onramp.unknown_placeholder`, `onramp.unmatched_brace`, `onramp.disabled_chain`, `onramp.not_evm` or
`onramp.duplicate_chain`. A replacement sheet shows the link while the controller's
`Core.ShowsOnramp` is true and opens it with `OpenOnramp()`; while `Core.ShowsOnrampHint` is true it shows the "Pick
another network" button there instead, which calls `Core.Back()`.

### SCFG.AddItem

`string SCFG.AddItem(string itemId, ScfgItem item)` declares a paid unlock, after `Configure`. It returns `null`,
or the error code.

| `ScfgItem` field | Meaning |
|---|---|
| `Code` | 1-99, different for each item. Receipts carry it: never change it after release |
| `Price` | The price as a decimal string, `"4.99"`. Amounts are strings, never floats |
| `Token` | `"USDC"` or `"USDT"` |
| `Title` | What the player sees on the sheet |
| `MinPrice` | Optional: the least a payment may be and still unlock. Default: the price |

### SCFG.AddChain

`string SCFG.AddChain(string key, ScfgChain chain)` adds an EVM chain that is not built in. Call it before
`Configure`; its key can then be used in `Chains`, `RpcUrls` and `Confirmations`. It returns `null`, or the error
code. With an [SCFG Settings asset](#scfgsettings), which is applied before your code runs, call it before payments
start (in the first scene's `Awake`): it sets the asset's configuration aside, items included, for your
`Configure` that follows. A chain that the core refuses (its error code, such as `rpc.not_https`) leaves the
asset's configuration in place, as a failing `Configure` does.

| `ScfgChain` field | Meaning |
|---|---|
| `Name` | The name players see |
| `ChainId` | The EVM chain id |
| `Rpc` | https RPC endpoints (`http://127.0.0.1` / `http://localhost`, with any port, for a local node) |
| `Tokens` | Token symbol to contract address, `{"USDC": "0x..."}`. 6-decimal tokens only |
| `Confirmations` | Confirmations required |
| `ExplorerTx` | An https URL with `{}` where the transaction hash goes (`http://127.0.0.1` / `http://localhost`, with any port, for a local test chain) |
| `NativeSymbol` | Optional: the coin that pays the network fee |
| `FeeHint` | Optional: `"lowest"`, `"low"` or `"high"` |
| `Testnet` | `true` for a test network, used when `Configure` has `Testnet = true`. The same key can be added once for each network |
| `MaxLogRange` | Optional: the most blocks one log query may span on its RPC endpoints |

### ScfgSettings

The **SCFG Settings** asset (**Assets > Create > SCFG Settings**) configures the package without code. Its serialized
fields are lowerCamel, as code that reads the asset names them: `wallet`, `solanaWallet`, `testnet` (on by default),
`chains`, `tokens`, `accent`, a `logo` sprite and `webStorageId` (`ScfgOptions`' `Wallet`, `SolanaWallet`, ...), the
buy link's `onrampEnabled`, `onrampUrl` and `onrampChains` (`ScfgOptions.Onramp`), plus a list of `items` (`id`,
`code`, `price`, `token`, `title`, `minPrice`). Blank fields keep the defaults. The [install page](install.md#without-code-the-scfg-settings-asset) walks through them.

- It is applied before the first scene loads, with `SCFG.Configure` and then `SCFG.AddItem` for each item. A build
  adds the asset to the player's preloaded assets for that build only (and takes it out again after a build that
  failed or was cancelled); in the editor's play mode the asset database finds it. Keep one per project: with
  several, none is applied (reported in play mode and in the player) and a build fails.
- An asset with neither wallet is **not applied** at startup: that is the opt-out for a game that configures from
  code, not an error. `settings.HasWallet` says which. If the asset has items and nothing configured SCFG by the end
  of the first frame, a warning says so (development builds and the editor).
- A wallet supplied at runtime (one per build, or read from a file) can still use the asset's options and items:
  `settings.Apply(wallet, solanaWallet)` (either may be `null`) applies the asset with those wallets instead of its
  own and returns the first error code, or `null`. Call it before payments start, as any `Configure`:
  `ScfgSettings.Find()?.Apply(walletFromMyConfig);` in the first scene's `Awake`.
- A `SCFG.Configure` from your code after it replaces it, items included (until payments have started).
- The inspector's **Validate** button runs the same checks without touching `SCFG` and shows the first error's code
  and message. From code, `settings.Validate()` returns that `ScfgSettings.Problem` (`Code`, `Message`), or `null`.
  An asset without a wallet is checked with stand-in wallets: the missing wallet is not a problem, and the verdict
  says the asset is not applied at startup.
- `settings.Apply()` applies it again by hand and returns the first error code, or `null`. `settings.ToOptions()`
  gives it as `ScfgOptions`. `ScfgSettings.Find()` returns the asset the startup uses, or `null`.
- The `Logo` sprite may be a cell of a sheet or a Sprite Atlas entry, readable or not: the asset cuts it out into a
  texture it owns (one per sprite; it stays valid until the asset makes another or is unloaded). A logo that cannot
  be used is reported and the asset is applied without it; nothing about the asset throws.
- `Methods`, `RpcUrls`, `Confirmations`, `PreviousWallets` and your own chains are not on the asset: configure from
  code for those (for your own chains, [`SCFG.AddChain`](#scfgaddchain) and then `SCFG.Configure`).

## Paying

### SCFG.Pay

```csharp
Awaitable<ScfgPaymentResult> SCFG.Pay(ScfgPayKind kind, ScfgPayRequest request)
void SCFG.Pay(ScfgPayKind kind, ScfgPayRequest request, Action<ScfgPaymentResult> callback)
```

Shows the payment sheet. The first form resolves when the sheet closes; the second gives every result to the
callback instead.

`kind` is `ScfgPayKind.Unlock` or `ScfgPayKind.Tip`. The `ScfgPayRequest` has three string fields:

| Field | Unlock | Tip |
|---|---|---|
| `Item` | Required: the item id | Not used |
| `Amount` | Optional: the amount to charge instead of the price, at least the item's `MinPrice` (which defaults to the price). So pay-what-you-want above the price, or a sale down to `MinPrice` | Required: the suggested amount, which the player may change |
| `Token` | Optional: another enabled token than the item's | Required |

```csharp
ScfgPaymentResult r = await SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "2.00", Token = "USDC" });
```

An `Awaitable` can be awaited once (as every Unity `Awaitable`). Await it from the main thread, in an `async`
method of a `MonoBehaviour`, for example a button handler `async void OnTipPressed()`. When the game quits with the
sheet open, the `await` still returns (with the result the sheet had, else `Cancelled`) but nothing is delivered to
callbacks or events; the payment stays watched and its result arrives at the next start through `PaymentResolved`.
A payment that confirmed in the moment before the quit is the exception: its receipt is saved, so `SCFG.Owns()` and
`SCFG.TipTotal()` count it at the next start, but no result is delivered for it. Read those at start, not only the event.
Check `this == null` after the `await` if your object may be gone by then.

### ScfgPaymentResult

What `await SCFG.Pay(...)`, a `Pay` callback and the `PaymentResolved` event give you. Every field except `Status`
may be `null`.

| Field | Meaning |
|---|---|
| `Status` | A `ScfgPaymentStatus`, see below |
| `Kind` | `ScfgPayKind.Unlock` or `ScfgPayKind.Tip` |
| `Item` | The item id (unlocks) |
| `Amount` | The amount actually paid, as a decimal string (`"4.99"`), or `null` when nothing matched on chain |
| `Token`, `Chain` | What was paid with, and on which chain (its key) |
| `TxHash`, `Payer` | The transaction and the paying address |
| `SessionId` | The payment session |
| `Error` | `null`, a configuration error code for a misused call, or the reason a payment failed |

`StatusText` gives the status as the other engines spell it (`"already_owned"`).

| `ScfgPaymentStatus` | Meaning |
|---|---|
| `Paid` | The payment confirmed |
| `AlreadyOwned` | An unlock the player already owns. It is returned at once and no sheet is shown |
| `Cancelled` | The player closed the sheet before paying |
| `Expired` | The payment window closed without a payment |
| `Underpaid` | A payment arrived but was less than the price |
| `Failed` | A misused call (`Error` is the code), or payments could not start |
| `Pending` | The player chose **Continue playing**. The package keeps watching in the background, across restarts, and the final result arrives later through the callback or `PaymentResolved` |

### Where results go

A `callback` given to `Pay` receives this call's own `AlreadyOwned`, `Failed` and `Cancelled` results and every
later result of that payment (`Paid`, `Underpaid`, `Expired`), also one that confirms after the player chose
Continue playing, until the payment is over (its `Paid` or `Underpaid` result, or the session ending unpaid: after
24 hours, or when the player switched networks); then the package releases it. `PaymentResolved` is not raised for
it. The callback form returns at once, and the callback never gets `Pending`: when the player chooses Continue
playing it simply gets the final result later.

Without a callback, the cancel and the later results reach `PaymentResolved` as well as the value `await` returns:
a payment that confirms while its sheet is open is raised when it confirms, and a cancel when the sheet closes, and
then the same result is what `await` returns. So the same result can reach your code twice. `AlreadyOwned` and
`Failed` are only returned.

Handlers must be idempotent: refresh what is unlocked from `SCFG.Owns()` rather than counting results. Or pass a
callback, which then gets those results instead of the event.

## Restoring

`Awaitable<ScfgRestoreResult> SCFG.Restore(Action<ScfgRestoreResult> callback = null)` shows the restore screen,
where the player pastes a transaction hash, and resolves when the screen is closed. The `callback`, if given, gets
the result once a restore finishes, while the screen is still open.

| `ScfgRestoreResult` field | Meaning |
|---|---|
| `Status` | `ScfgRestoreStatus.Restored`, `NothingFound` (everything found was already on this device), `Cancelled` (the screen was closed before a restore finished) or `Failed` (a misused call, or payments could not start) |
| `Restored` | The receipts that were added, a list of `ScfgReceipt` |
| `Error` | `null`, a lookup error such as `"tx_not_found"`, `"tx_reverted"`, `"tx_not_a_payment"` or `"invalid_hash"`, or a configuration error code |

## Queries

| Call | Returns |
|---|---|
| `SCFG.Owns(itemId)` | `true` if a stored, verified payment unlocks the item. Instant, works offline. An item `AddItem` did not declare is misuse (`false`) |
| `SCFG.HasTipped()` | `true` once the player has tipped any amount |
| `SCFG.TipTotal()` | The sum of tips as a decimal string (`"0"`, `"2.50"`) |
| `SCFG.Receipts()` | The stored receipts, `IReadOnlyDictionary<string, ScfgReceipt>` by receipt key, a copy. A `ScfgReceipt` has `Key`, `Chain`, `TxHash`, `LogIndex`, `Block`, `Kind`, `Item`, `Amount`, `Token`, `Payer`, `Recipient`, `Source` (`"payment"` or `"restore"`) |
| `SCFG.Pending()` | The payments still being watched, `IReadOnlyDictionary<string, ScfgPendingPayment>` by session id, a copy. A `ScfgPendingPayment` has `SessionId`, `Kind`, `Item`, `Title`, `Price`, `Token`, `Chain`, `State` (`"awaiting"` or `"confirming"`), `HintTx`, `CreatedAt` (Unix seconds) |
| `SCFG.ExportReceipts(path = null)` | Writes the receipts as readable JSON (default `Application.persistentDataPath/scfg-receipts.json`). Returns the full path, or `null` (logged) |
| `SCFG.ClearLocalData()` | Testing helper: forgets receipts and pending payments on this device. The payments stay on the chain and Restore brings them back |
| `SCFG.Supported()` | Always `true`, WebGL included (kept for parity with Ren'Py) |
| `SCFG.Notify` | A property. `true` shows a short notice through the sheet host when a payment confirms ("Payment confirmed") or a restore adds purchases. Default `false` |
| `SCFG.IsWeb` | `true` in a WebGL player |
| `SCFG.IsMobile` | `true` on Android and iOS (`Application.isMobilePlatform`) |

For your own screens there are also `SCFG.Accent` (the accent in use), `SCFG.LogoTexture`, `SCFG.ItemTitle(id)` (the
declared title, else the id), `SCFG.ChainName(key)` (the display name, else the key) and `SCFG.CleanPaste(text)`
(a pasted clipboard as the hash field takes it).

## Events

- `SCFG.PaymentResolved` (`Action<ScfgPaymentResult>`): a final result that has no `Pay` callback to go to, for
  example a payment that confirms after a restart, or the results of a `Pay` you await without a callback (see
  [Where results go](#where-results-go)). Subscribe once, at startup.
- `SCFG.ReceiptsChanged` (`Action`): the stored receipts changed (a payment confirmed, a restore,
  `ClearLocalData()`).

```csharp
void OnEnable()  { SCFG.PaymentResolved += OnResolved; SCFG.ReceiptsChanged += Refresh; }
void OnDisable() { SCFG.PaymentResolved -= OnResolved; SCFG.ReceiptsChanged -= Refresh; }
```

With **Enter Play Mode Options** and domain reload off, every play session starts from a fresh state: the last
session's subscriptions are dropped, so subscribe from your startup code as above.

## Pausing the game while the sheet is open

The sheet covers the game, takes focus and runs on unscaled time, but it does **not** stop your scripts. A game
whose `Update` keeps reading the confirm key (`Input.GetKeyDown`, an Input System action) would act on keys the
player is pressing in the sheet. Pause your game while you await `Pay` or `Restore`: disable your gameplay input
(say, its action map) and, if you like, set `Time.timeScale = 0`, then put back the time scale you had (the sheet
may open from a pause menu or during slow motion). The sheet, its notices and the payment checks run on real time,
so they keep working at `Time.timeScale = 0`. The samples pause with the time scale only, as they have no gameplay
input of their own.

```csharp
float timeScale = Time.timeScale;
Time.timeScale = 0;
gameplayActions.Disable();
ScfgPaymentResult result = await SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "2.00", Token = "USDC" });
gameplayActions.Enable();
Time.timeScale = timeScale;
```

### Run In Background

On desktop the checkout page, the RPC requests and the payment checks run from the package's own per-frame update,
so they stop whenever the player stops. With **Run In Background** off (Player Settings > Resolution and
Presentation), a player that loses the focus stops its frames, and the player has to switch to the browser to pay
there. So while a payment sheet, a checkout kept for its browser page (**Continue playing**) or a restore screen is
open, the package sets `Application.runInBackground = true`. When the last of them closes (a kept checkout closes
once its payment is final or the browser page times out), at quit and when play mode starts again without a
domain reload, it puts back the value it found. Your game does not need to change the setting. If your own code
changes `Application.runInBackground` while a sheet is open, the package's restore overwrites it; set it again
after the sheet closes. WebGL builds (the page is in the game's own tab) and mobile (Unity ignores the setting
there) are left alone.

Keyboard and gamepad work in the sheet without setup. Each step focuses its main control; up and down move through
the controls in visual order, left and right within a row, and the ends stay put, so the focus never leaves the
sheet for the game behind it. Tab and Shift+Tab move too. Enter or the gamepad's south button presses, and Escape or
the east button goes back (on the first step, it closes the sheet as a cancel). This is wired by hand on UI
Toolkit's navigation events, which both the Input Manager and the Input System's UI input module send, so it works
with either.

## Customising the sheet

The default screens are UI Toolkit. Each screen gets its own `UIDocument` on the package's PanelSettings, at sort
order 1000, above your own UI Toolkit panels unless they sort higher; your Canvas, TextMeshPro and prefabs are never touched. From least to most
work:

1. **`Logo` and `Accent`** in `ScfgOptions` (or on the SCFG Settings asset).
2. **A style sheet** of your own, applied after the package's: set `ScfgDefaultSheetHost.SheetStyle` before the
   screen opens. The package's colors are USS variables on `.scfg-screen`: `--scfg-accent`, `--scfg-ink` (the text),
   and `--scfg-sheet`, `--scfg-muted`, `--scfg-line`, `--scfg-rule`, `--scfg-hover`, `--scfg-ok`, `--scfg-warn`,
   `--scfg-error`. Every class is in the package's `Runtime/Unity/Resources/ScfgUI/ScfgSheet.uss`. An
   `--scfg-accent` from your style sheet wins over `Accent`. `ScfgDefaultSheetHost.PanelSettings` replaces the
   panel the screens are drawn on (the default scales with the screen from a 960x720 reference).

   ```css
   /* MySheet.uss */
   .scfg-screen { --scfg-accent: #7a5cff; --scfg-ink: #101010; }
   ```

   ```csharp
   ScfgDefaultSheetHost.SheetStyle = mySheetStyle;   // a StyleSheet asset you reference
   ```

3. **Your own payment sheet** (UI Toolkit, uGUI, anything): write a small `IScfgSheetHost` whose `OpenPayment`
   shows your screen, and hand the rest to the default host.

   ```csharp
   public sealed class MyHost : IScfgSheetHost
   {
       readonly ScfgDefaultSheetHost _default = new ScfgDefaultSheetHost();

       public IScfgScreen OpenPayment(ScfgCheckoutController controller) => MySheet.Show(controller);
       public IScfgScreen OpenRestore(ScfgRestoreController controller) => _default.OpenRestore(controller);
       public void Close(IScfgScreen screen) { if (screen is MySheet s) s.Hide(); else _default.Close(screen); }
       public void Notify(string text) => _default.Notify(text);
   }

   SCFG.SheetHost = new MyHost();
   ```

4. **Everything your own way**: an `IScfgSheetHost` of your own for the payment sheet, the restore screen and the
   notices. Setting `SCFG.SheetHost = null` shows nothing: `Pay` and `Restore` then fail with `sheet.unavailable`.

The screen contract (`Runtime/Unity/ScfgScreens.cs`):

- `OpenPayment` and `OpenRestore` show a screen and return an `IScfgScreen`, or `null` when none can be shown (the
  call then fails with `sheet.unavailable`). `Close(screen)` removes it, after the package closed its controller.
  `Notify(text)` shows a short message.
- An `IScfgScreen` raises `Closed(how)` once. A payment sheet closes with `"cancel"` (the player gave up),
  `"keep_playing"` (close the sheet but keep watching the payment; only once a session exists) or `"finish"`
  (after a final state: paid, underpaid or expired); any other word counts as `"finish"`. A restore screen closes
  with any word. The package picks `Closed` up on the next frame, so never raise it from inside `OpenPayment` or
  `OpenRestore`. A screen whose `Gone` turns `true` without `Closed` (its GameObject was destroyed) counts as a
  cancel and is not handed to `Close`.
- The package polls the controllers every frame, so a screen only reads their fields and calls their methods.
  `ScfgCheckoutController.Core` is the payment's state: show `Step` (`"amount"`, `"network"`, `"method"`,
  `"browser"`, `"qr"`, `"manual"`, `"waiting"`, `"done"`), `State`, `Message()`, `ChainOptions()`, `Methods()`,
  `PayAmountText`, `Recipient`, `PaymentUri()` (the QR code's text), `ExplorerUrl()`, `Confirmations` and
  `Required`, and call `ChooseNetwork`, `ChooseMethod`, `NextStep`, `SetTipAmount`, `MarkSent`, `SubmitHash` and
  `Back` from your buttons; show the buy link while `ShowsOnramp` (or its hint while `ShowsOnrampHint`, a button that
  calls `Back`). The wrapper's `OpenBrowser()`, `OpenWalletApp()`, `OpenOnramp()`, `OpenExplorer(url)`, `Copy(text)`
  and `Paste()` are the buttons that leave the game. Never call the core's `Poll()`, `Cancel()`, `KeepPlaying()`,
  `Finish()` or `Close()`: the package does.
- `ScfgRestoreController.Core` is the restore's state (`State`: `"input"`, `"checking"`, `"choose"`, `"saving"`,
  `"done"`; `TxInput`, `Error`, `Message()`, `Candidates`, `PendingChoices()`) with `Check`, `Choose` and `Confirm`;
  the wrapper adds `Paste()`, `Copy(text)`, `ItemTitle(id)`, `ChainName(key)`, `DeviceReceipts()` and
  `ReceiptsRevision` (it changes each time `ReceiptsChanged` is raised).

The package's own screens are public too (`ScfgSheetView`, `ScfgRestoreView`): their `Root` is a `VisualElement`
you can add to a panel of your own, ticked with `Tick()` every frame.

## Texts

The screens are in English only; there is no translation hook in this version. Item titles and chain names are
shown as you declare them. The texts are in `ScfgUiTexts` (`ScfgUiTexts.Get(id)`), for a screen of your own.

## Persistence

Receipts and pending payments are stored in `Application.persistentDataPath/scfg/state.json`, global to the install
and never inside your save slots, so loading an older save cannot lose a purchase. The file is read on first use.

- Receipts and new or finished payments are written at once; a payment's scan progress at most every 10 seconds,
  and again when the game quits or goes to the background. A save writes a `.tmp` file and then replaces the old
  one, so a crash leaves the previous file whole.
- A corrupt file is moved aside to `state.json.corrupt` (then `.corrupt.2`, ...) and the package starts empty, with a
  warning in development builds. Restore brings the purchases back from the chain.
- A file that exists but cannot be read right now (a permission, a lock) is left alone: no payments start that run
  (logged), and it is never saved over.
- At each launch every stored receipt is checked again on the chain, and invented ones are removed.

## WebGL builds

The package compiles for WebGL from any active platform: a build script that calls `BuildPipeline.BuildPlayer` for
WebGL from a project whose active platform is Linux, Windows or macOS needs no Switch Platform or `-buildTarget WebGL`
first (the package's own WebGL test builds that way).

A WebGL player has no localhost server: the checkout page is the same page the other engines use, shown as an
overlay over the game in its own web page, and it talks to the wallet extension there. **Browser wallet** is
offered only when the page has a wallet for the chosen network (an extension, or a wallet app's built-in browser).
A broken wallet extension (one that announces itself badly or throws odd values) can't break the game's page: the
checkout leaves it out or shows a wallet error, and the player's other wallets still pay. Links open in a new tab,
and Copy and Paste use the browser's clipboard (a refused paste or copy tells the player). Nothing needs adding to
the build: the page's files ship inside the package.

- **Secure context.** Wallet extensions and the clipboard expect HTTPS or `localhost`. Without one, Copy and Paste
  are refused and the sheet says so.
- **RPC endpoints.** In a browser every RPC call is a `fetch` from the game's origin. The built-in endpoints accept
  that (CORS); check your own `RpcUrls` and chains with `uv run python tools/check_rpcs.py`. Endpoints on the
  player's machine or network (`localhost`, private addresses) are blocked by browsers.
- **Exception support.** Keep **Player Settings > Publishing Settings > Enable Exceptions** at **Explicitly Thrown
  Exceptions Only** (the default) or a **Full** setting. The package reports misuse and network failures by
  catching exceptions; with **None**, any of them would stop the player.
- **Receipts persist in IndexedDB.** `persistentDataPath` lives in the browser's IndexedDB, which belongs to the
  page's origin. After each save the package asks the page to write it to IndexedDB (Unity does so by itself only
  when the page's loader sets `autoSyncPersistentDataPath`), so you need no template change. Every Unity WebGL build
  on one origin shares one IndexedDB database and writes back the whole tree it loaded at startup, so a game saved
  there can delete what another game (or another tab of the same game) saved after it loaded, `state.json`
  included.

  So the package also keeps a copy of the receipts in the page's `localStorage`, which other games do not rewrite.
  Every save merges the game's receipts into it, so a receipt another tab of the same game saved stays in the copy;
  only a receipt the game removed itself (failed re-verification, `ClearLocalData()`) leaves it. `ClearLocalData()`
  empties the copy, until another open tab of the same game saves: that tab's save merges its own receipts back into
  it. At startup (the first read of the state once `Configure` ran) the package adds back any receipt the copy has
  and `state.json` lacks (when both have one, `state.json`'s is kept), saves the result at once, and re-verifies it
  on the chain like any stored receipt. Pending payments are not copied: a payment lost with `state.json` comes back
  with Restore and its transaction hash. If `localStorage` is missing, blocked or full, saving goes on as before and
  a development build warns once.

  The copy's key is `scfg:<productName>:<sha256 of the storage id>`. The storage id is `ScfgOptions.WebStorageId`
  (**Web Storage Id** on the SCFG Settings asset) if you set it, else the product name (**Player Settings > Product
  Name**). Set it if another game on your origin might use the same product name, for example
  `WebStorageId = "com.example.mygame"`. Don't change the storage id or the product name after a release: the copy
  would be looked for under a new key, and start empty. Serve the game from an origin of its own where you can.
- **Private browsing.** If the browser blocks IndexedDB (some private-browsing modes, blocked site data, a
  restricted iframe), `persistentDataPath` is lost when the tab closes and the receipts are kept only in the
  `localStorage` copy; a development build warns once.
- **Keyboard.** While the checkout overlay is shown, the package turns `WebGLInput.captureAllKeyboardInput` off, so
  the player can Tab and press Enter in the overlay; your game's own value comes back once the overlay is closed or
  hidden.
- **Content-Security-Policy.** The package loads the checkout page's scripts into your page with `eval`. A page
  whose CSP forbids it (`script-src` without `'unsafe-eval'`) gets no browser checkout: a development build warns once
  and names the scripts, and the other methods still work.
- **A harmless console warning.** Unity's loader logs `[UnityCache] Response is served without Content-Length
  header` for RPC answers that come in chunks. That warning is Unity's own and does no harm.

## IL2CPP and stripping

The package works with Mono and IL2CPP at any managed stripping level, with nothing to set up. Its `Runtime/link.xml`
says what the linker must keep; Unity ignores a `link.xml` inside a package, so the package's editor code hands it
to the linker of every player build. The one type the package looks up by name (`UnityEngine.WebGLInput`, for the
keyboard on the WebGL checkout) is in that `link.xml`.

## Mobile (unverified)

> Mobile is best effort and has **not** been tested on a device. Desktop and WebGL are the verified targets.

On Android and iOS the sheet does not start a localhost page. It offers an **Open wallet app** button that opens the
payment request link (EIP-681 for EVM chains, Solana Pay for Solana) with `Application.OpenURL`, plus the QR and
manual transfer methods. `SCFG.IsMobile` tells you which build you are in. Whether a given wallet app handles the
link, and what the wallet does when the game is sent to the background, is untested. Test your build on real
devices before shipping it, and keep the manual transfer method enabled.
