# API reference (Unreal)

Everything goes through `UScfgSubsystem` (`ScfgSubsystem.h`), a game-instance subsystem: one per game instance,
created and torn down with it, kept across level loads. Its calls mirror the Godot addon's and the Unity package's
in PascalCase, with typed option structs (`FScfgOptions`, `FScfgItem`, ...) in `ScfgTypes.h`. Blueprint has the same
calls as nodes in the **SCFG** category (see [Blueprint](#blueprint)). The headers in
`Plugins/Scfg/Source/Scfg/Public/` document every member; this page is the overview.

## Getting the subsystem

```cpp
#include "ScfgSubsystem.h"

UScfgSubsystem* Scfg = GetGameInstance()->GetSubsystem<UScfgSubsystem>();   // from an actor
if (!Scfg)
{
    return;   // a dedicated server has none
}
```

A dedicated server never gets one: `ShouldCreateSubsystem` refuses a server process and a play-in-editor server
instance, since a server can never show a payment sheet. There `GetSubsystem<UScfgSubsystem>()` returns null, so code
that also runs on a server must check it, as above. SCFG runs on the players' game clients only. In Blueprint, the
**Get Subsystem** node (category SCFG) returns the world context's subsystem or null: check it with **Is Valid**. The
other nodes on a server log once per node kind, `scfg: Owns: no SCFG subsystem on a dedicated server (SCFG runs on
the players' game clients only)`, and return their neutral value or resolve `Failed`.
`UScfgBlueprintLibrary::OnDedicatedServer(WorldContextObject)` tells you whether you are there.

## Misuse never crashes

Misuse never crashes the game. A misused call logs a line on the `LogScfg` category,
`LogScfg: Error: scfg: <message>`, and:

- `Configure`, `AddChain` and `AddItem` return the error code they logged, or `""` on success;
- `Pay`, `PayAsync`, `Restore`, `RestoreAsync` and the async nodes resolve to a result with `Status` `Failed` and
  `Error` set to the code (for example `item.unknown`, `configure.not_called`);
- queries such as `Owns` log and return their neutral value (`false`, `"0"`).

| Code | Meaning |
|---|---|
| `configure.not_called` | A call that needs `Configure` came before it |
| `configure.after_start` | `Configure` after payments started (the frame after the first successful configuration) |
| `configure.after_add_chain` | `AddChain` after `Configure`, or after payments started with the project settings |
| `wallet.missing`, `address.invalid`, `solana_address.invalid`, `solana_address.not_on_curve` | No payout wallet, or one that is not a valid address |
| `item.unknown` | `Pay` or `Owns` for an item `AddItem` did not declare |
| `item.invalid_id`, `item.invalid_code`, `item.duplicate_code`, `item.duplicate_id`, `item.invalid_price`, `item.invalid_title`, `item.min_price_above_price` | A bad `AddItem` |
| `chains.unknown`, `chains.wrong_network`, `tokens.unknown`, `token.not_enabled`, `config.no_usable_token` | A chain or token that is not built in, not on this network, not enabled, or that no wallet can receive |
| `methods.none_enabled`, `methods.bad_timeout`, `methods.qr_disabled_chain` | A bad `Methods` |
| `rpc.not_https`, `rpc_urls.unknown_chain`, `confirmations.unknown_chain`, `confirmations.invalid`, `confirmations.solana_fixed` | A bad `RpcUrls` or `Confirmations` entry |
| `chain.*` (`chain.invalid_key`, `chain.is_builtin`, `chain.duplicate_chain_id`, `chain.bad_explorer_tx`, ...) | A bad `AddChain` |
| `accent.invalid` | `Accent` is not `"#rrggbb"` |
| `logo.bad_type`, `logo.bad_path`, `logo.file_missing` | `Logo` is not a png, jpg or webp path inside the project, or the file is not there |
| `tip.needs_amount_and_token`, `tip.below_minimum`, `payment.below_min_price` | A tip without `Amount` and `Token` or below the minimum, or an unlock `Amount` below the item's `MinPrice` |
| `sheet.unavailable` | No screen could be shown: no game viewport (a commandlet, the editor outside play-in-editor), or your sheet host returned null |
| `RuntimeError` | The call could not run: from another thread, outside the subsystem's lifetime, payments could not start, or `ExportReceipts` could not write |

The message logged with each code says what to change.

## Configuration

### Configure

`FString Configure(const FScfgOptions& Options)` sets the payout wallet and options. It returns `""`, or the error
code it logged. Call it before payments start: payments start on the first frame after a successful configuration,
so call it from your game instance's `Init()` after `Super::Init()`, or from the first level's `BeginPlay`. It can be
called again until then; after that it is refused with `configure.after_start`. A new `Configure` starts a new
configuration: declare the items again after it.

| `FScfgOptions` field | Meaning |
|---|---|
| `Wallet` | Your EVM payout address (`0x...`). At least one of `Wallet` and `SolanaWallet` is required |
| `SolanaWallet` | Your Solana payout address (base58) |
| `bTestnet` | `true` uses the test networks (Sepolia, Arbitrum Sepolia, Polygon Amoy, Base Sepolia, Arc Testnet and Solana Devnet, USDC only), so no real money moves. Default `false` in the struct (the project settings start with it on) |
| `Chains` | The networks offered, in order, by key (`ethereum`, `arbitrum`, `polygon`, `base`, `arc`, `solana`, or your own). Empty: every network your wallets can receive on |
| `Tokens` | `USDC`, `USDT`. Empty: both |
| `Methods` | An `FScfgMethods`: which payment methods the sheet offers. Default: all of them |
| `Onramp` | An `FScfgOnramp`: the "Buy some with a card" link, see [The buy link](#the-buy-link-onramp). Default: on, on mainnet |
| `RpcUrls` | Your own RPC endpoints per chain key (an `FScfgRpcUrls` whose `Urls` are https URLs, or `http://127.0.0.1` / `http://localhost` with a port for a local node), tried before the built-in public ones |
| `Confirmations` | Confirmations required per chain key (1 or more; not for Solana, which waits for finalization) |
| `PreviousWallets` | Wallets you were paid to before (EVM and Solana addresses can be mixed). Their receipts stay valid |
| `Logo` | An image file shown on the sheet and the checkout page, relative to the project folder: png, jpg or webp (the in-game sheet shows only a png or jpg; a webp logo shows on the checkout page alone, with a warning in the log). To ship it, keep it in a folder under `Content/` (`Content/Scfg/logo.png`) and stage that folder: see [The logo in a packaged game](#the-logo-in-a-packaged-game) |
| `Accent` | The accent color of the sheet and the checkout page, `"#rrggbb"`. Empty: the default |

| `FScfgMethods` field | Meaning |
|---|---|
| `Browser.bEnabled` | The checkout page in the player's browser, for a wallet extension (desktop only). Default `true` |
| `Browser.TimeoutMinutes` | Minutes the checkout page keeps answering a payment the player kept playing through. Default 30 |
| `Qr.bEnabled` | A QR code for a phone wallet. Default `true` |
| `Qr.bWalletButton` | The **Open wallet app** button on phones. Default `true` |
| `Qr.Chains` | The chains the QR code is offered on. Empty: all |
| `Manual.bEnabled` | The address and exact amount to send by hand, from an exchange or any wallet. Default `true` |
| `Manual.bAllowTxPaste` | Whether the player may paste the transaction hash on the manual page. Default `true` |

### The buy link (`Onramp`)

Players who have no USDC or USDT yet see **No USDC yet? Buy some with a card** (with the payment's token) on the
method step, or on the only payment page when there is one method. It opens a buy page for the network they picked
in their browser, where they buy the token with a card into their own wallet and then pay as usual: the payment
itself still goes straight to your wallet. The default page is thirdweb's hosted Buy widget, card payment only,
locked to the network and token, suggesting the payment rounded up to cents (plus a cent for the fee on Arc). The
link carries only the network, the token's contract and that amount. On a network it isn't offered on, like Solana,
the sheet says **No USDC yet? Pick another network to buy some with a card** instead, which goes back to the network
step, when another network the player can pick has the link.

It is **on by default on mainnet**. Turn it off, or change it, with `FScfgOptions.Onramp` (in the project settings,
**Configure > Onramp**):

| `FScfgOnramp` field | Meaning |
|---|---|
| `bEnabled` | `true` (default) or `false` |
| `Chains` | The networks it is offered on (enabled EVM networks only). Empty: the built-in EVM networks on mainnet, and none on testnet (cards don't buy test tokens). Name testnet networks here (for example `base`, `arbitrum`) to see the link and the Solana hint before mainnet |
| `Url` | Your own `https://` page with a host, without spaces, at most 2048 characters. `{chain}`, `{chain_id}`, `{token}`, `{token_address}` and `{amount}` are filled in; any other brace is refused. Empty: thirdweb's page |

```cpp
Options.Onramp.bEnabled = false;                                               // hide it
Options.Onramp.Chains = { TEXT("base"), TEXT("arc") };                         // offer it on these networks only
Options.Onramp.Url = TEXT("https://buy.example/?net={chain_id}&a={amount}");   // your own buy page
```

A mistake is reported with its code: `onramp.bad_url` (also a url with no host after `https://`),
`onramp.unknown_placeholder`, `onramp.unmatched_brace`, `onramp.disabled_chain`, `onramp.not_evm` or
`onramp.duplicate_chain`. A replacement sheet shows the link while the controller's `ShowsOnramp()` is true and opens it with `OpenOnramp()`; while `ShowsOnrampHint()` is true it shows the "Pick another
network" button there instead, which calls `Back()`.

### AddItem

`FString AddItem(const FString& ItemId, const FScfgItem& Item)` declares a paid unlock, after `Configure`. It returns
`""`, or the error code. It works after payments have started too.

| `FScfgItem` field | Meaning |
|---|---|
| `Code` | 1-99, different for each item. Receipts carry it: never change it after release |
| `Price` | The price as a decimal string, `"4.99"`. Amounts are strings, never floats |
| `Token` | `"USDC"` or `"USDT"` |
| `Title` | What the player sees on the sheet |
| `MinPrice` | Optional: the least a payment may be and still unlock (a discount you may give later). Empty: the price |

### AddChain

`FString AddChain(const FString& Key, const FScfgChain& Chain)` adds an EVM chain that is not built in (Optimism,
Avalanche, your own L2). Call it before `Configure`; its key can then be used in `Chains`, `RpcUrls` and
`Confirmations`. It returns `""`, or the error code. With [project settings](#project-settings), which are applied
before your code runs, call `AddChain` and then `Configure` from your game instance's `Init()`: the `AddChain` sets
the settings aside (with a warning), items included, for the `Configure` that follows.

| `FScfgChain` field | Meaning |
|---|---|
| `Name` | The name players see |
| `ChainId` | The EVM chain id |
| `Rpc` | https RPC endpoints (`http://127.0.0.1` / `http://localhost`, with any port, for a local node) |
| `Tokens` | Token symbol to contract address, `"USDC"` to `"0x..."`. 6-decimal tokens only |
| `Confirmations` | Confirmations required, 1 or more |
| `ExplorerTx` | An https URL with `{}` where the transaction hash goes (`http://127.0.0.1` / `http://localhost`, with any port, for a local test chain) |
| `NativeSymbol` | The coin that pays the network fee. Default `"ETH"` |
| `FeeHint` | `"lowest"`, `"low"` or `"high"`. Default `"low"` |
| `MaxLogRange` | The most blocks one log query may span on its RPC endpoints. Default 1000 |
| `bTestnet` | `true` for a test network, used when `Configure` has `bTestnet = true`. The same key can be added once for each network |

### Project settings

`UScfgSettings` (`ScfgSettings.h`) is **Project Settings > Plugins > SCFG**, saved to `Config/DefaultGame.ini` under
`[/Script/Scfg.ScfgSettings]`: `Options` (an `FScfgOptions`, with `bTestnet` on by default), `Chains` (each an
`FScfgSettingsChain`: a `Key` and an `FScfgChain`) and `Items` (each an `FScfgSettingsItem`: `Id`, `Code`, `Price`,
`Token`, `Title`, `MinPrice`). The [install page](install.md#without-code-the-project-settings) walks through the
panel.

- **Applied at start, opt-in.** When the game instance starts, the subsystem applies the settings through the same
  calls a game makes: `AddChain` for each chain row, `Configure(Options)`, then `AddItem` for each item row. Settings
  without a wallet (`Wallet` and `SolanaWallet` both empty) are not applied at all, so a game configured from code is
  unaffected; if they have chain or item rows and nothing configured SCFG by the end of the first frame, one
  warning says why. **Validate** reports no problem for the
  missing wallet (it checks the rest with stand-in wallets) and says the settings are not applied.
- **A wallet supplied at runtime** (one per build, or read from a file) can use the settings' chains, options and
  items: `ConfigureWithSettings(Wallet, SolanaWallet)` (Blueprint too; either may be empty) runs their `AddChain`
  rows, `Configure` with their options and these wallets, then their `AddItem` rows, as calls from code, and returns
  the first error code or `""`. It checks the rows and the configuration first and changes nothing when they fail,
  so a call made before the wallet file was read can simply be repeated; a successful call replaces whatever was
  configured (the settings applied at start included). Leave the wallets in the settings empty, and call it where
  `Configure` goes (below).
  `ApplySettingsWith(Settings, Wallet, SolanaWallet)` does the same with another `UScfgSettings` object (C++). A failing row is logged like any misuse, then
  once more naming the settings; a failing chain or `Configure` applies nothing, a failing item keeps what came before
  it.
- **A code call made after them wins.** The settings are one unit. A `Configure` from code replaces everything they
  applied (options, logo, chains, items) and sees only the chains your code added itself; one that fails leaves the
  settings in place. An `AddChain` from code sets the settings aside, with a warning: call `Configure` next. A chain
  that the core refuses (its error code, such as `rpc.not_https`) leaves the settings in place, as a failing
  `Configure` does. Once payments have started with the settings, it is refused (`configure.after_add_chain`) and the
  settings stay. An
  `AddItem` from code adds to the settings' items; an id or code they already use is `item.duplicate_id` or
  `item.duplicate_code`.
- **Override them before the first frame**: in the game instance's `Init()` after `Super::Init()`, or the first
  level's `BeginPlay`. After it, `Configure` is `configure.after_start`, and its message says that the settings
  started payments (clear the wallet in the settings if your game always configures from code).
- **Validate.** The panel's **Validate** button shows every problem with its code, as a notification and on the
  **SCFG** page of the Message Log, then a warning for a logo that a packaged game would not have (see
  [The logo in a packaged game](#the-logo-in-a-packaged-game)). From code, `GetDefault<UScfgSettings>()->Validate()`
  returns the same `TArray<FScfgSettingsProblem>` (`Code`, `Message`; empty when valid) without touching any
  subsystem, `Warnings()` the warning lines (they are not problems: the editor still finds the file), and
  `Verdict(Problems, Warnings)` the lines the button shows. `UScfgSettings::LogoPackagingWarning(Logo, ProjectDir,
  UScfgSettings::StagedDirectories())` checks a logo you pass to `Configure` from code the same way. `IsEmpty()` is true without a wallet (not applied), `HasRows()`
  when there are chain or item rows. In editor code, `ScfgEditor::ValidateSettings(Settings, bNotify)` (module
  `ScfgEditor`) posts the result to the Message Log as the button does.

## Paying

### Pay and PayAsync

```cpp
void Pay(EScfgPayKind Kind, const FScfgPayRequest& Request, const FOnScfgPaid& OnPaid);   // also Blueprint
void Pay(EScfgPayKind Kind, const FScfgPayRequest& Request, TFunction<void(const FScfgPaymentResult&)> Callback);
void PayAsync(EScfgPayKind Kind, const FScfgPayRequest& Request,
              TFunction<void(const FScfgPaymentResult&)> OnResolved);
```

Each shows the payment sheet. `PayAsync`, and the Blueprint node **Pay (SCFG)**, call you once, with this call's
result, when the player closes the sheet (Unity's awaited `Pay`, Godot's `await Scfg.pay()`). `Pay` takes a callback
that gets every result of the payment instead, also those that come long after the sheet closed (see
[Where results go](#where-results-go)). `AlreadyOwned` and `Failed` results come at once, before the call returns.

`FOnScfgPaid` is a dynamic delegate: bind a `UFUNCTION` with `BindDynamic` (or `BindUFunction`). If its object is
destroyed, the later results of the payment go to `OnPaymentResolved` instead. Capture a `TWeakObjectPtr` in a C++
lambda rather than `this`.

```cpp
FScfgPayRequest Request;
Request.Amount = TEXT("2.00");
Request.Token = TEXT("USDC");
Scfg->PayAsync(EScfgPayKind::Tip, Request, [](const FScfgPaymentResult& Result) { /* ... */ });
```

`Kind` is `EScfgPayKind::Unlock` or `EScfgPayKind::Tip`. The `FScfgPayRequest` has three string fields:

| Field | Unlock | Tip |
|---|---|---|
| `Item` | Required: the item id | Not used |
| `Amount` | Optional: the amount to charge instead of the price, at least the item's `MinPrice` (which defaults to the price). So pay-what-you-want above the price, or a sale down to `MinPrice` | Required: the suggested amount, which the player may change |
| `Token` | Optional: another enabled token than the item's | Required |

### FScfgPaymentResult

What `PayAsync`, a `Pay` callback, the **Pay (SCFG)** node and `OnPaymentResolved` give you. A field the payment
does not have is an empty string. This struct and the others the plugin hands back (`FScfgReceipt`,
`FScfgPendingPayment`, `FScfgRestoreResult`) are read-only in Blueprint: break them, there is no Make or Set node.

| Field | Meaning |
|---|---|
| `Status` | An `EScfgPaymentStatus`, see below |
| `Kind` | `EScfgPayKind::Unlock` or `EScfgPayKind::Tip` |
| `Item` | The item id (unlocks) |
| `Amount` | The amount actually paid, as a decimal string (`"4.993742"`), or empty when nothing matched on chain |
| `Token`, `Chain` | What was paid with, and on which chain (its key) |
| `TxHash`, `Payer` | The transaction and the paying address |
| `SessionId` | The payment session |
| `Error` | Empty, an error code for a misused call (`item.unknown`), `RuntimeError`, or the reason a payment failed |

| `EScfgPaymentStatus` | Meaning |
|---|---|
| `Paid` | The payment confirmed |
| `AlreadyOwned` | An unlock the player already owns. It comes back at once and no sheet is shown |
| `Cancelled` | The player closed the sheet before paying |
| `Expired` | The payment window closed before a payment was found. The payment is still watched for 24 hours |
| `Underpaid` | A payment arrived but for less than the price |
| `Failed` | A misused call (`Error` is the code), or payments could not start |
| `Pending` | The player chose **Continue playing**. The plugin keeps watching in the background, across restarts, and the final result arrives later through the `Pay` callback or `OnPaymentResolved` |

### Where results go

A callback given to `Pay` receives this call's own `AlreadyOwned`, `Failed` and `Cancelled` results and every later
result of that payment (`Paid`, `Underpaid`, `Expired`), also one that confirms after the player chose **Continue
playing**, until the payment is over (its `Paid` or `Underpaid` result, or the session ending unpaid: after 24 hours,
or when the player switched networks); then the plugin releases it. `OnPaymentResolved` is not raised for those. The
callback never gets `Pending`: when the player keeps playing it simply gets the final result later.

Without a callback (`PayAsync`, the **Pay (SCFG)** node, or `Pay` with an unbound delegate), the cancel and the later
results reach `OnPaymentResolved`, as well as the value `PayAsync` or the node gives you: a payment that confirms
while its sheet is open is raised when it confirms, and a cancel when the sheet closes, and then the same result
comes from `PayAsync` when the sheet closes. So the same result can reach your code twice. `AlreadyOwned` and
`Failed` are only returned.

Handlers must be idempotent: refresh what is unlocked from `Owns()` rather than counting results. Or pass a callback
to `Pay`, which then gets those results instead of the event.

## Restoring

```cpp
void Restore(const FOnScfgRestored& OnRestored);   // also Blueprint
void RestoreAsync(TFunction<void(const FScfgRestoreResult&)> OnResolved,
                  TFunction<void(const FScfgRestoreResult&)> Callback = nullptr);
```

Both show the restore screen, where the player pastes a transaction hash. `Restore`'s `OnRestored` (and
`RestoreAsync`'s optional `Callback`) gets the result once a restore finishes, while the screen is still open.
`RestoreAsync`'s `OnResolved`, and the Blueprint node **Restore (SCFG)**, fire once, when the screen is closed.

| `FScfgRestoreResult` field | Meaning |
|---|---|
| `Status` | `EScfgRestoreStatus::Restored`, `NothingFound` (everything found was already on this device), `Cancelled` (the screen was closed before a restore finished) or `Failed` (a misused call, or payments could not start) |
| `Restored` | The receipts that were added, an array of `FScfgReceipt` |
| `Error` | Empty, a lookup error such as `tx_not_found`, `tx_reverted`, `tx_not_a_payment`, `invalid_hash` or `network_unavailable`, or a configuration error code |

## Queries

| Call | Returns |
|---|---|
| `Owns(ItemId)` | `true` if a stored, verified payment unlocks the item. Instant, works offline. An item `AddItem` did not declare is misuse (`false`) |
| `HasTipped()` | `true` once the player has tipped any amount |
| `TipTotal()` | The sum of tips as a decimal string (`"0"`, `"2.50"`) |
| `Receipts()` | The stored receipts, `TMap<FString, FScfgReceipt>` by receipt key, a copy. An `FScfgReceipt` has `Chain`, `TxHash`, `LogIndex`, `Block`, `Kind` (`EScfgPayKind`: `Unlock` or `Tip`), `Item`, `Amount`, `Token`, `Payer`, `Recipient`, `Source` (`"payment"` or `"restore"`) |
| `Pending()` | The payments still being watched, `TMap<FString, FScfgPendingPayment>` by session id, a copy. An `FScfgPendingPayment` has `SessionId`, `Kind` (`EScfgPayKind`), `Item`, `Title`, `Token`, `Chain`, `Price`, `State` (`"awaiting"` or `"confirming"`), `UiStatus` (`"open"`, `"pending"`, `"cancelled"` or `"expired"`), `HintTx`, `CreatedAt` (Unix seconds), `Cursor` (the last block scanned) |
| `ExportReceipts(Path)` | Writes the receipts as readable JSON (a list, keys sorted, indented). An empty `Path` writes `Saved/scfg-receipts.json`, a relative one is under `Saved/` too. Returns the file's absolute path, or `""` (logged). Ask players to send you this file for support |
| `ClearLocalData()` | Testing helper: forgets receipts and pending payments on this device, to see your paywall again after a test purchase. The payments stay on the chain and Restore brings them back |
| `Supported()` | Always `true`: every platform the plugin builds for (kept for parity with Ren'Py) |
| `SetNotify(bool)`, `GetNotify()` | When on, a short notice ("Payment confirmed: Chapter 2. Thank you!", "Purchases restored.") is shown through the sheet host when a payment confirms or a restore adds purchases. Default off |

The queries are plain C++ functions; their Blueprint forms are the library's nodes (see [Blueprint](#blueprint)).
`Owns`, `HasTipped` and `TipTotal` before `Configure` log `configure.not_called` and return `false` or `"0"`.

## Events

- `OnPaymentResolved` (`FOnScfgPaymentResolved`, one `const FScfgPaymentResult&`): a final result that has no `Pay`
  callback to go to, for example a payment that confirms after a restart, or the results of a `PayAsync` (see
  [Where results go](#where-results-go)). Bind it once, at startup.
- `OnReceiptsChanged` (`FOnScfgReceiptsChanged`, no parameters): the stored receipts changed (a payment confirmed, a
  restore, `ClearLocalData()` when there were receipts). It is raised on the frame after the change.

Both are dynamic multicast delegates, so the handlers are `UFUNCTION`s:

```cpp
Scfg->OnPaymentResolved.AddDynamic(this, &UMyGameInstance::HandlePaymentResolved);
Scfg->OnReceiptsChanged.AddDynamic(this, &UMyGameInstance::RefreshUnlocks);
```

In Blueprint, bind them from the subsystem with **Bind Event to On Payment Resolved** and **Bind Event to On Receipts
Changed**.

## Blueprint

| Node | What |
|---|---|
| **Get Subsystem** (pure) | The world context's `UScfgSubsystem`, or null (no game instance, or a dedicated server) |
| **Configure**, **Add Item**, **Add Chain** | The subsystem's calls, each returning `""` or the error code |
| **Pay (SCFG)** (async) | Shows the payment sheet; **On Resolved** fires once, when it closes. Like `PayAsync` |
| **Restore (SCFG)** (async) | Shows the restore screen; **On Resolved** fires once, when it closes. Like `RestoreAsync` |
| **Pay**, **Restore** (on the subsystem) | The callback forms, with an `FOnScfgPaid` / `FOnScfgRestored` event (**Create Event**) |
| **Owns (SCFG)**, **Has Tipped (SCFG)**, **Tip Total (SCFG)**, **Supported (SCFG)** (pure) | The queries |
| **Receipts (SCFG)**, **Pending (SCFG)** | The queries that copy every record. They are impure: call one once and keep the map |
| **Export Receipts**, **Clear Local Data**, **Set Notify**, **Get Notify** | The subsystem's calls |
| **On Payment Resolved**, **On Receipts Changed** | The subsystem's events |

A node without a game instance logs once per node kind, `scfg: Owns: no SCFG subsystem here (it needs a game
instance)`, and returns its neutral value; the async nodes then resolve `Failed` with `RuntimeError`. In C++,
`UScfgPayAsyncAction::PayWith(Subsystem, Kind, Request)` and `UScfgRestoreAsyncAction::RestoreWith(Subsystem)`
make the async actions on a given subsystem.

Blueprint cannot replace the payment sheet: `IScfgSheetHost` is a C++ interface (see
[Customising the sheet](#customising-the-sheet)).

## Threads and lifetime

- **Game thread only.** Call the subsystem from the game thread. A call from another thread logs `scfg:
  <Function>() must be called on the game thread` every time (and one `ensure` per process), touches nothing, and
  delivers its `Failed` result (`RuntimeError`) on the game thread on the next frame; `Configure`, `AddChain` and
  `AddItem` return `RuntimeError`, and a query its neutral value. The plugin itself starts no thread or task: its work runs on the engine's core ticker, which keeps ticking
  while the game is paused.
- **A game instance that shuts down with a screen open** resolves the call without delivering anything: no callback
  (`PayAsync`'s and `RestoreAsync`'s included), no node pin, no event, no notice. A payment whose session had started
  (the player chose a network) stays watched, and its result arrives at the next start through `OnPaymentResolved`.
  `IsShuttingDown()` is true from the start of that teardown.
- **After the teardown** the subsystem stays stopped: a call logs `scfg: <Function>: the SCFG subsystem is not
  running (outside its game instance)` once per call site. `Pay` and `Restore` then call their callback with `Failed`
  `RuntimeError`, while `PayAsync` and `RestoreAsync` drop `OnResolved` (it is not called).
- **Several game instances in one process** (play-in-editor with two or more clients) share one
  `Saved/Scfg/state.json`, each saving from its own copy, so a receipt one client stored can be dropped by another's
  next save until a restart or a Restore brings it back. Test payments with one client.

## The payment sheet

Unless you set a host of your own, the subsystem shows its own Slate screens through `FScfgDefaultSheetHost`
(`ScfgDefaultSheetHost.h`): each screen is a widget on the game instance's game viewport (in the editor, the
play-in-editor viewport) at Z-order 10000 (`FScfgDefaultSheetHost::ZOrder`), above your widgets, which it never
touches; notices one Z-order above. The game is dimmed and the sheet is docked on the right, 520 units wide, or across
the whole width on a portrait or narrow screen. Without a game viewport (a commandlet, the editor outside
play-in-editor) nothing is shown, and `Pay` and `Restore` resolve `Failed` with `sheet.unavailable` (logged).

The steps: the amount (tips only), the network, the method (browser wallet, phone wallet by QR code, manual
transfer), then waiting and done. In every build but Shipping the sheet also shows **DEV MODE: paying to** and the
start and end of the payout address. The restore screen takes a transaction hash and lists the purchases on this
device.

### Pausing the game while the sheet is open

The sheet takes the keyboard focus and every key, character, stick and mouse event that reaches it: the game behind
gets none, and neither do the viewport's own keys (the console key, Alt+Enter, the screenshot and debug keys do
nothing while a screen is open). But it does **not** stop your game: actors keep ticking and timers keep running, and
game code that reads input by other means keeps doing so. Pause the game while a sheet is open, and put back the
pause state you had, as the showcase does:

```cpp
const bool bWasPaused = UGameplayStatics::IsGamePaused(this);
UGameplayStatics::SetGamePaused(this, true);
TWeakObjectPtr<UMyGameInstance> Weak(this);
Scfg->PayAsync(EScfgPayKind::Tip, Request, [Weak, bWasPaused](const FScfgPaymentResult& Result)
{
    if (UMyGameInstance* Self = Weak.Get())
    {
        UGameplayStatics::SetGamePaused(Self, bWasPaused);
    }
});
```

The sheet, its notices and the payment checks run on real time, so they keep working while the game is paused and
at a time dilation of 0.

**Keyboard and gamepad** work without setup. Up and down (the arrow keys, the d-pad, the left stick) move through
the controls in order, left and right within a row, Tab and Shift+Tab move too, and the focus never leaves the
sheet. Enter, Space or the gamepad's bottom face button presses; Escape or the right
face button goes back, and on the first step closes the sheet as a cancel. In a text field, letters, Space and the
Left and Right keys stay in the field. A held key's repeats move the focus but never press or go back.

**The mouse.** While a screen is open it releases the viewport's mouse capture, cursor lock and high-precision mode
and shows the cursor, so a game that hides the cursor still gets a clickable sheet, and gives them back on close as
your viewport wants them at that moment. A click on the dimmed game does not reach the game.

## Customising the sheet

From least to most work:

1. **`Logo` and `Accent`** in `FScfgOptions` (or the project settings).
2. **A style override**: an `ISlateStyle` whose widget styles and brushes win by name over the plugin's. Make your
   own default host, give it the style and hand it to the subsystem:

   ```cpp
   TSharedRef<FSlateStyleSet> MyStyle = MakeShared<FSlateStyleSet>(TEXT("MySheetStyle"));
   MyStyle->Set(TEXT("Scfg.Title"), FTextBlockStyle(...));
   TSharedRef<FScfgDefaultSheetHost> Host = MakeShared<FScfgDefaultSheetHost>(GetGameInstance());
   Host->SetStyleOverride(MyStyle);
   Scfg->SetSheetHost(Host);
   ```

   The names: text styles (`FTextBlockStyle`) `Scfg.Text`, `.Title`, `.Heading`, `.Amount`, `.AmountToken`, `.Note`,
   `.Label`, `.Muted`, `.Warning`, `.Dev`, `.Error`, `.Ok`, `.Status`, `.Mono`, `.QrStep`, `.OptionText`,
   `.OptionSub`, `.OptionText.Selected`, `.OptionSub.Selected`, `.ToastText`, `.ButtonText`, `.ButtonText.Small`,
   `.ButtonText.Primary`, `.ButtonText.Accent`; buttons (`FButtonStyle`) `Scfg.Button`, `.Button.Small`,
   `.Button.Primary`, `.Button.Accent`, `.Button.Back`, `.Option`, `.Option.Selected`; the field
   (`FEditableTextBoxStyle`) `Scfg.Field`; the progress bar (`FProgressBarStyle`) `Scfg.Progress`; brushes
   `Scfg.Dim`, `.Sheet`, `.QrFrame`, `.Toast`, `.Rule`, `.Focus`, `.NoFocus`, `.Dot`, `.White`, `.Black`, `.Logo`.
3. **Your own payment sheet or restore screen**, in Slate or UMG: an `IScfgSheetHost` of your own (below). You can
   hand the screens you do not replace to an `FScfgDefaultSheetHost` you own: its `Close` ignores a screen it did not
   open.
4. **The plugin's own sheet inside your UMG**: `UScfgSheetWidget` ([The UMG wrapper](#the-umg-wrapper)).

`SetSheetHost(nullptr)` goes back to the default host; `GetSheetHost()` returns your host, or null while the default
one is used. A screen already open is closed by the host that opened it. The screens are in C++ only: Blueprint has
no `IScfgSheetHost` to implement.

### The logo in a packaged game

The logo is a loose file read from disk, not an asset, and `Configure` checks that it exists (`logo.file_missing`).
The cooker packages assets only, so a packaged game has the logo only when both hold:

1. The logo is in a **folder under `Content/`**: `Content/Scfg/logo.png`, not `Content/logo.png` itself and not a
   folder beside `Content/` such as `Branding/logo.png`.
2. That folder is listed in **Project Settings > Packaging > Additional Non-Asset Directories to Package** (in the
   advanced part of the Packaging section; the paths there are relative to `Content/`, so add `Scfg`), or in
   **Additional Non-Asset Directories To Copy**. In `Config/DefaultGame.ini` that is
   `+DirectoriesToAlwaysStageAsUFS=(Path="Scfg")` under `[/Script/UnrealEd.ProjectPackagingSettings]`.
   "To Copy" is expected to work the same way, but only "to Package" is tested.

Otherwise the editor and play in the editor still find the file, but the packaged game's `Configure` fails with
`logo.file_missing`: project settings with that logo are not applied, and every `Pay` then fails with
`configure.not_called`. The settings panel's **Validate** warns about a logo that will not be packaged; a logo you
pass to `Configure` from code is not checked there, so follow the same two rules (or call
`UScfgSettings::LogoPackagingWarning`).

### Your own sheet host

`IScfgSheetHost` (`IScfgSheetHost.h`) has four virtual functions; the base class shows nothing (its `OpenPayment` and
`OpenRestore` return null, so `Pay` and `Restore` fail with `sheet.unavailable`):

- `TSharedPtr<FScfgScreen> OpenPayment(const TSharedRef<FScfgCheckout>& Checkout)` and
  `OpenRestore(const TSharedRef<FScfgRestore>& Restore)` show a screen and return its handle, an `FScfgScreen` (or a
  subclass), or null when none can be shown. Keep the handle alive while the screen is shown.
- `Close(const TSharedRef<FScfgScreen>& Screen)` removes the screen, after the plugin closed its controller.
- `Notify(const FString& Text)` shows a short, already worded message (only sent while `SetNotify(true)`).

Your screen calls `Screen->Close(How)` once when the player is done with it (`IsClosed()` and `GetHow()` read it
back). A payment sheet closes with `"cancel"` (the player gave up), `"keep_playing"` (close the sheet but keep watching
the payment; only once a session exists) or `"finish"` (after a final state: paid, underpaid or expired); any other
word counts as `"finish"`. A restore screen closes with any word. The plugin picks the close up on its next frame, so
`Close` may be called from anywhere, `OpenPayment` included. A handle you release without calling `Close` counts as a
cancel and is not handed to your `Close`.

Read the payment through `FScfgCheckoutController(Checkout)` and the restore through `FScfgRestoreController(Restore)`
(`ScfgCheckoutController.h`), the views the plugin's own screens are built on. The plugin polls the controllers every
frame, so your screen only reads them (every frame, or when they change) and calls their buttons:

- `FScfgCheckoutController`: `Step()` (`amount`, `network`, `method`, `browser`, `qr`, `manual`, `waiting`, `done`),
  `State()` (`choose`, `starting`, `waiting`, `confirming`, `paid`, `underpaid`, `expired`, `closed`), `Methods()`
  (`browser`, `qr`, `manual`), `ChainOptions()`, `Title()`, `Token()`, `PriceText()`, `PayAmountText()` (the exact
  amount to send), `Recipient()`, `PaymentUri()` (the QR code's text), `ExplorerUrl()`, `Confirmations()` and
  `Required()`, `StepDots()`, `Message()` (its English) and `MessageId()`, `CanGoBack()`, `IsTestnet()`, `Accent()`,
  `LogoPath()`, `ShowsOnramp()` and `ShowsOnrampHint()` (the buy link and its hint) and more; the buttons `Back`,
  `Next`, `SetTip`, `SetCustomAmount`, `ApplyCustomAmount`, `ChooseNetwork`, `ChooseMethod`, `MarkSent`,
  `SetTxInput`, `SubmitHash`, and those that leave the game, `OpenBrowser`, `OpenWalletApp`, `OpenOnramp`,
  `OpenExplorer`, `Copy`, `Paste`.
- `FScfgRestoreController`: `State()` (`input`, `checking`, `choose`, `saving`, `done`, `closed`), `TxInput()`,
  `Message()`, `NumCandidates()`, `Candidate(Index)`, `PendingChoices()`, `CanConfirm()`, `Added()`, `Receipts()`
  and `ReceiptsRevision()`, `ItemTitle(Id)`, `ChainName(Key)`; the buttons `SetTxInput`, `Check`, `Choose`,
  `Confirm`, `Copy`, `Paste`.

`ScfgCheckoutController.h` starts with a complete host that shows a UMG widget of its own. Hold your widgets weakly
(`TWeakObjectPtr`): the host is plain C++ and must not keep a UObject alive. `Close` can run while the game instance
shuts down, inside garbage collection, so it must not create a UObject.

### The UMG wrapper

`UScfgSheetWidget` (`ScfgSheetWidget.h`) is the plugin's own payment sheet or restore screen as a `UUserWidget`, for a
host that shows its UI through UMG: create it, bind it once, and put it in your widget tree.

```cpp
virtual TSharedPtr<FScfgScreen> OpenPayment(const TSharedRef<FScfgCheckout>& Checkout) override
{
    UScfgSheetWidget* Made = CreateWidget<UScfgSheetWidget>(PlayerController);
    TSharedRef<FScfgScreen> Screen = Made->BindPayment(Checkout);   // BindRestore for the restore screen
    Made->AddToViewport(10000);                                       // or into your own widget tree
    Widget = Made;                                                    // a TWeakObjectPtr<UScfgSheetWidget>
    return Screen;
}
virtual void Close(const TSharedRef<FScfgScreen>& Screen) override
{
    if (UScfgSheetWidget* Shown = Widget.Get()) { Shown->RemoveFromParent(); }
}
```

It takes the focus and the mouse when constructed and gives them back when removed. It can be removed and added
again (moved between containers, a widget stack): the same screen comes back, its payment still waiting. `BindPayment`
and `BindRestore` take an optional style override; `GetScreen()` returns the bound handle.

## Texts

The screens are in English only; there is no translation hook in this version. Item titles and chain names are shown
as you declare them.

## Persistence

Receipts and pending payments are stored in `Saved/Scfg/state.json` (`FPaths::ProjectSavedDir()`), global to the
install and never inside your save games, so loading an older save cannot lose a purchase.

- Receipts and new or finished payments are written at once; a payment's scan progress at most every 10 seconds. A
  save writes a `.tmp` file and then replaces the old one, so a crash leaves the previous file whole. A failed save
  keeps the old file, is logged once and is retried every 10 seconds.
- A corrupt file is moved aside to `state.json.corrupt` (then `.corrupt.2`, ...) and the plugin starts empty, with a
  warning. Restore brings the purchases back from the chain.
- A file that exists but cannot be read (a permission, an I/O error) is left alone: no payments start, and it is never
  saved over.
- At each launch every stored receipt is checked again on the chain, and invented ones are removed.

## Network and privacy

What a game that uses the plugin opens:

| What | Where | When |
|---|---|---|
| JSON-RPC requests | Out, over HTTPS, to the chains' public RPC endpoints built into the plugin, or yours first (`RpcUrls`). User-Agent `scfg/0.1` | While there is something to check: the stored receipts at startup, a payment being watched (every 3 seconds while a screen is open, every 15 otherwise), a restore lookup, and with a Solana wallet a check at startup that it can receive each token |
| The checkout page | TCP on `127.0.0.1` only (IPv4), at a port the system picks, never `0.0.0.0` or a fixed port. Desktop only | From the moment the player picks the browser method until the sheet closes. After **Continue playing** it keeps answering, with no sheet on screen, until the payment is final or `Methods.Browser.TimeoutMinutes` passes (30 minutes by default) |
| Links | The checkout page (`http://127.0.0.1:<port>/c/<secret>/`), an explorer link, a wallet link (phones), opened in the system's browser or wallet app | Only when the player presses the button |

The checkout page answers only requests for its own secret path with a `127.0.0.1` host. The page talks to the
player's wallet extension in their browser and reports the transaction back to the game, which verifies it on the
chain.

**TLS is verified, or nothing is sent.** The plugin sends a JSON-RPC request over `https://` only when the engine
will verify the server's certificate. The engine's HTTP client (libcurl on Linux, Windows and Android) verifies the
peer only when it found root certificates, unless the engine ini says otherwise
(`[/Script/Engine.NetworkSettings] n.VerifyPeer`). Without root certificates, it would send HTTPS requests
unverified. So the plugin checks both sources before each request. If the engine found no root certificate, or
`n.VerifyPeer=False`, the request fails at once with `TransportError: TLS certificate verification unavailable`, and
nothing is sent. The RPC client then reports the chain as unavailable (`RpcUnavailable`), and the Output Log says
why, once per transport (see [Troubleshooting](#troubleshooting)). A payment
then stays pending rather than being trusted on an unverified answer. On Linux the plugin stages the engine's own
bundle (`Engine/Content/Certificates/ThirdParty/cacert.pem`) with a packaged game, and the engine also reads the
system's (`/etc/ssl/certs/ca-certificates.crt` and two other known paths). Windows and Android use their system
stores, and macOS and iOS use the system's trust (NSURLSession); none of these four has been tested.

Nothing is sent to us or to any server of ours, and there is no account, analytics or telemetry. The RPC providers
see the player's IP address and the addresses and transactions the game asks about, as with any wallet. The payment
itself is public on its blockchain, payer address included.

Not the plugin's: in every build but Shipping the engine itself listens on `0.0.0.0:1985` (its trace control
listener). A Shipping build has no trace by default (read from the engine's source; not measured here).

## Platforms

### Desktop

Tested on **Linux x64 with UE 5.8.4**: the editor and the Shipping game build without a warning, and a Development
game, uncooked and packaged, pays on local EVM, Arc and Solana test chains by manual transfer and by the browser
method. Its HTTPS requests to real RPC endpoints verify the certificate; without root certificates they are
refused before anything is sent.

Not tested yet: UE 5.6 and 5.7, Windows, macOS, mobile, a Shipping game run, play in the editor, Blueprint-only
projects, and packaging with the zen store as the cooked output store (the default).

**Windows** is in the plugin's platform list, and the plugin builds there with MSVC on UE 5.8, but it is not yet fully
tested on Windows. **macOS** is in the list too and is expected to work, but the plugin has not yet been built or
tested on it. **UE 5.6 and 5.7** are untested too: 5.6 is the minimum, and the release has a zip for each
of 5.6, 5.7 and 5.8 (the same source, each `Scfg.uplugin` declaring its engine version), but only 5.8.4 has been
tested.

### Mobile (unverified)

> Android and iOS are best effort: the plugin has **never been built** for them (no Android SDK or Xcode was
> available), let alone run on a device. Desktop is the verified target.

On a phone the sheet starts no localhost page and offers no browser method: only the QR code and the manual transfer.
The QR step has an **Open wallet app** button (unless `Methods.Qr.bWalletButton` is off) that hands the payment
request (EIP-681 for EVM chains, Solana Pay for Solana) to the system with `FPlatformProcess::LaunchURL`; back in the
game, the plugin finds the transfer on the chain as for a QR payment. Keep the QR code or the manual transfer enabled:
a configuration with only the browser method is valid but offers a phone nothing. Use `https://` RPC URLs, since
Android and iOS may refuse plain `http://`. Whether a given wallet app handles the link, and what happens when the
game goes to the background, is untested. Test your build on real devices before shipping it.

## Troubleshooting

Every line the plugin logs is on the `LogScfg` category and starts with `scfg:`; filter the Output Log on it. Misuse
is `scfg: <message>` with no heading (see [Misuse never crashes](#misuse-never-crashes)). Other errors come under one
of these headings:

| Line | What happened | What to do |
|---|---|---|
| `scfg: worker error: ...` | A command of the payment engine failed, most often on the network (every RPC endpoint of a chain failing or timing out) | If it persists, add your own endpoint in `RpcUrls`. Keys in a game build are visible to anyone: use a key restricted to read-only calls |
| `scfg: cannot read the stored receipts: ...` | `state.json` cannot be read (a permission, a corrupt file that cannot be moved aside): no payments start | Fix the file's permissions, or move it away and use Restore |
| `scfg: cannot save the payment state: ...` | A save failed; it is retried every 10 seconds | Check the disk and the `Saved/Scfg/` folder's permissions |
| `scfg: refusing https:// RPC requests: TLS certificate verification unavailable: ...` | The engine found no root certificate (no bundle staged, and on Linux none at the system's paths), or `n.VerifyPeer` is `False` in your engine ini. No RPC request is sent, so no payment can be checked | Package with the plugin as it is: it stages the engine's `cacert.pem` on Linux. Do not set `n.VerifyPeer=False`. On a Linux system without a bundle, install the distribution's `ca-certificates` package |
| `scfg: cannot start the checkout page: ...` | The page's files are missing, or it could not listen. The browser method says "Couldn't start the browser checkout. Use another method." and QR and manual remain | Check that `Plugins/Scfg/Resources/Web/` is staged with the game |
| `scfg: cannot start the payment engine: ...` | The stored payments could not be loaded | Move `state.json` away and use Restore |
| `scfg: cannot build the payment link: ...` | The QR code or wallet link for this payment could not be made | Report it, with the log |
| `scfg: screen: ...` | A button of the sheet got an error back | Report it, with the log |

And the common setups:

| Symptom | What to do |
|---|---|
| `Pay` resolves `Failed` with `sheet.unavailable` | There is no game viewport: in the editor, play in the editor or run with `-game`. With your own host, its `OpenPayment` returned null |
| `configure.after_start` | `Configure` came too late: call it in the game instance's `Init()` after `Super::Init()`, or in the first level's `BeginPlay` |
| `scfg: the SCFG settings (...) were not applied (<code>)` | Press **Validate** in **Project Settings > Plugins > SCFG** and fix what it lists |
| `scfg: the SCFG settings ... have chains or items but no wallet` | Set a wallet in the project settings, configure from code, or call `ConfigureWithSettings(Wallet, SolanaWallet)` with a wallet supplied at runtime |
| `scfg: your wallet can't receive USDT on solana yet ...` | Receive any amount of that token on your Solana wallet once, then restart the game |
| `logo.file_missing` in a packaged game | See [The logo in a packaged game](#the-logo-in-a-packaged-game) |
| Payment sent but nothing happens | The wallet may have changed the amount. Use **Manual transfer**, then paste the transaction hash |
| `GetSubsystem<UScfgSubsystem>()` is null | On a dedicated server there is none (see [Getting the subsystem](#getting-the-subsystem)); in a Game Instance Blueprint's **Event Init**, the subsystems do not exist yet |

`ExportReceipts()` writes a readable JSON file you can ask players to send you.
