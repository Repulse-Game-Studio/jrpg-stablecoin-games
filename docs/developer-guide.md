# cryptopay for Ren'Py: Developer Guide

Accept **USDC** and **USDT** tips and unlock purchases in your Ren'Py game, paid
**directly to your own wallet**. There is no server, no account, and no middleman:
the plugin watches the blockchain itself.

- Chains: Ethereum, Arbitrum, Polygon (USDC + USDT), Base (USDC), **Solana (USDC + USDT; devnet USDC)**, plus any EVM chain you add yourself (see [Your own RPC endpoints and chains](#your-own-rpc-endpoints-and-chains) and [Solana](#solana))
- Platforms: Windows, macOS, Linux, Android and the web (HTML5) build, on your own site or on itch.io
- Requires Ren'Py 8.2 or newer (tested on 8.3.7 and 8.5.3)

> **Read "Security and limitations" before you sell anything.** Without a server,
> unlocks are honour-grade: a determined player can bypass them.

---

## 1. Install

1. Unzip the release into your project so you get:
   ```
   game/cryptopay/                      (screens, API, browser checkout page)
   game/python-packages/cryptopay_core/ (the engine; plain Python)
   ```
2. Add your configuration in any `.rpy` file (see below).
3. Start with `testnet=True` and do a full test purchase before going live.

## 2. Configure

```renpy
init python:
    cryptopay.configure(
        wallet="0xYourWalletAddressHere",   # where payments go (EIP-55 checked)
        testnet=True,                       # remove (or False) for real money
    )

    # Paid items. `code` (1-99) is written into players' payments forever:
    # NEVER change an item's code after release.
    cryptopay.add_item("chapter2", code=1, price="4.99", token="USDC", title="Chapter 2: The Fall")
    cryptopay.add_item("ost",      code=2, price="2.00", token="USDT", title="Original Soundtrack")
```

### All `configure()` options

| Option | Default | Meaning |
|---|---|---|
| `wallet` | required unless `solana_wallet` is given | Your EVM payout address. A wrong-case checksum raises an error, which catches typos |
| `solana_wallet` | none | Your Solana payout address (base58) to accept USDC/USDT on Solana. See [Solana](#solana) |
| `testnet` | `False` | `True` uses Sepolia, Arbitrum Sepolia, Polygon Amoy, Base Sepolia and Solana Devnet (USDC only) |
| `chains` | all built-ins + yours | Which networks players may pay on, in display order: `["arbitrum", "base", "polygon", "ethereum", "solana"]`. Chains you added with `add_chain()` can be listed too. By default the EVM chains (built-in, then yours) are offered if `wallet` is set, followed by `"solana"` if `solana_wallet` is set |
| `tokens` | both | `["USDC"]`, `["USDT"]` or both |
| `methods` | all on | Per-method options, see below |
| `rpc_urls` | none | Your own RPC endpoints per chain, tried before the built-in public ones, e.g. `{"arbitrum": ["https://arb-mainnet.g.alchemy.com/v2/KEY"]}`. See below |
| `confirmations` | per chain | Override blocks to wait on EVM chains, e.g. `{"ethereum": 12}`. Refused for `"solana"`, which has no block count (see [Solana](#solana)) |
| `previous_wallets` | none | Wallets used by earlier versions of your game, EVM (`0x…`) and Solana addresses can be mixed. Keeps old purchases valid after you change wallets |
| `logo` | none | An image in your game folder, e.g. `"images/logo.png"` (`.png`, `.jpg` or `.webp`). Shown at the top of the payment screens and the browser checkout page, fitted into 240 × 64 (at 1280 × 720). A wide wordmark works best |
| `accent` | `"#0e7c74"` | Color of selected options and the main browser button, e.g. `accent=gui.accent_color`. Text on it switches between white and dark automatically |

### Payment methods

| Method | Options | What the player does |
|---|---|---|
| `browser` | `enabled`, `timeout_min` (30) | Desktop: clicks "Open checkout in browser"; a page served by the game on `127.0.0.1` connects MetaMask, Rabby, Coinbase Wallet or another extension (on Solana: Phantom, Solflare or Backpack), and sends the payment. Web build: clicks "Open wallet"; the same checkout opens over the game (offered only when the page has a wallet for the network; see [Web builds](#web-builds)) |
| `qr` | `enabled`, `wallet_button` (Android "Open wallet app"), `chains` (restrict) | Scans a QR code with a phone wallet (EIP-681 payment link; on Solana, a Solana Pay link) |
| `manual` | `enabled`, `allow_tx_paste` | Sends the exact amount from any wallet or exchange; can paste the transaction hash |

```renpy
cryptopay.configure(wallet="0x...", methods={"browser": {"enabled": False},
                                              "qr": {"chains": ["base", "polygon"]}})
```

### Your own RPC endpoints and chains

**Your own RPC endpoint for a built-in chain.** The plugin talks to each chain through
free public RPC endpoints. They work, but they rate-limit and sometimes go down. If you
have an endpoint from Alchemy, Infura, QuickNode, Ankr, or your own node, pass it in
`rpc_urls`. Your endpoints are tried first, and the built-in public ones remain as a fallback:

```renpy
init python:
    cryptopay.configure(
        wallet="0xYourWallet",
        rpc_urls={
            "arbitrum": "https://arb-mainnet.g.alchemy.com/v2/YOUR_KEY",
            "base": ["https://base-mainnet.g.alchemy.com/v2/YOUR_KEY",
                     "https://my-backup-node.example.com"],
        },
    )
```

**Your own chain.** For an EVM chain that isn't built in (Avalanche, Optimism, Linea,
your own appchain…), describe it with `cryptopay.add_chain()` **before** `configure()`.
Its key then works everywhere a built-in key does: `chains=`, `rpc_urls=`, `confirmations=`
and `methods={"qr": {"chains": [...]}}`.

```renpy
init python:
    # Avalanche C-Chain, real money (used when testnet=False).
    cryptopay.add_chain(
        "avalanche",
        name="Avalanche",                  # shown on the network button
        chain_id=43114,
        rpc=["https://api.avax.network/ext/bc/C/rpc"],
        tokens={"USDC": "0xB97EF9Ef8734C71904D8002F8b6Bc66Dd9c48a6E"},
        confirmations=3,
        explorer_tx="https://snowtrace.io/tx/{}",
        native_symbol="AVAX",
        fee_hint="low",
    )
    # The same key for testing (used when testnet=True): Avalanche Fuji.
    cryptopay.add_chain(
        "avalanche", testnet=True,
        name="Avalanche Fuji", chain_id=43113,
        rpc=["https://api.avax-test.network/ext/bc/C/rpc"],
        tokens={"USDC": "0x5425890298aed601595a70AB815c96711a31Bc65"},
        confirmations=3,
        explorer_tx="https://testnet.snowtrace.io/tx/{}",
        native_symbol="AVAX",
    )

    cryptopay.configure(
        wallet="0xYourWallet",
        testnet=True,
        chains=["base", "avalanche"],      # leave out to offer all built-ins + yours
    )
```

| `add_chain()` argument | Meaning |
|---|---|
| `key` | Your name for the chain: lowercase letters, digits, `_`, `-`. It is stored in players' receipts, so **never change it after release**. It can't reuse a built-in key |
| `name` | Shown to players on the network button (at most 32 characters) |
| `chain_id` | The EVM chain id (see [chainlist.org](https://chainlist.org)). Wallets use it to switch networks, and QR codes carry it |
| `rpc` | One URL or a list, tried in order. `https://` only, except `http://127.0.0.1` / `http://localhost` for a local test node. The **last** one is also handed to the player's browser wallet if it needs to add the network, so put a public endpoint last |
| `tokens` | `{"USDC": "0x…"}` and/or `"USDT"`. Copy addresses from Circle's / Tether's official lists, never from a block explorer search |
| `confirmations` | Blocks to wait before a payment counts. Use the chain's finality: a few for fast-finality chains, more for chains that reorg |
| `explorer_tx` | Transaction page with `{}` where the hash goes |
| `native_symbol` | The gas token (`"AVAX"`, `"ETH"`, …), named in "you need gas" warnings. Default `"ETH"` |
| `fee_hint` | `"lowest"`, `"low"` (default) or `"high"`, shown under the network name |
| `testnet` | `False` (default) adds the chain for real-money games; `True` for `configure(testnet=True)` games. Add it twice with the same key to have both |
| `max_log_range` | Blocks per `eth_getLogs` call to start with (default 1000). The plugin shrinks it by itself when an endpoint complains |

Rules your chain must follow (checked when the game starts where possible):

- **EVM only.** The plugin finds payments through ERC-20 `Transfer` events over Ethereum
  JSON-RPC (`eth_getLogs`, `eth_getTransactionReceipt`, …). Tron, Bitcoin and other non-EVM
  chains can't be added this way. Solana is built in: see [Solana](#solana).
- **6-decimal tokens only.** The item code lives in the last four of six decimals. This
  rules out BNB Chain's USDC/USDT (18 decimals). The plugin can't check this offline, so
  confirm `decimals()` on the explorer before you ship.
- **The endpoint must serve `eth_getLogs`** for recent blocks. Most do; check it with a test
  payment on the chain's testnet.
- **Test it first.** Make one payment on the testnet version of your chain, then one small
  real payment, before you release.

### Solana

Add `solana_wallet` to accept USDC and USDT on Solana (on devnet, USDC only). You can use it
alone, or next to `wallet` so players choose between EVM chains and Solana. Call
`configure()` once; these are two alternatives.

Solana only, on devnet:

```renpy
init python:
    cryptopay.configure(solana_wallet="YourSolanaWalletAddress", testnet=True)
```

EVM chains and Solana, with your own Solana endpoint:

```renpy
init python:
    cryptopay.configure(
        wallet="0xYourWallet",
        solana_wallet="YourSolanaWalletAddress",
        rpc_urls={"solana": "https://mainnet.helius-rpc.com/?api-key=YOUR_KEY"},
    )
```

- **Use a wallet address.** `solana_wallet` is the address you see in Phantom or Solflare. A
  token-account or program-derived address is refused with an error that says so. Multisig
  vaults (for example a Squads vault) aren't supported in v1.
- **Your wallet must already hold a token account.** Receive any amount of USDC (and of USDT,
  if you offer it) once. At the first check after the game starts, the game looks for your
  token account for each enabled token. If one is missing, the console prints (in developer
  mode only, never in a distributed game) `cryptopay: your
  wallet can't receive USDC on solana yet (no token account). Receive any amount of USDC there
  once; until then players aren't offered it.`, the DEV MODE banner says "Your Solana wallet
  can't receive USDC yet: receive any amount once.", and that token isn't offered on Solana.
  A network error never hides it. The check runs only when the game starts: once the token
  account exists, restart the game and Solana is offered again.
- **Your own endpoint.** `rpc_urls={"solana": "https://…"}` works like on other chains and is
  tried before the built-in public ones. The public mainnet endpoints rate-limit, and
  `solana-rpc.publicnode.com` keeps only about two days of history, so Restore for older
  payments uses a fallback endpoint or yours.
- **No `confirmations` for Solana** (the option is refused for `"solana"`). A payment counts
  once the network has finalized it, about 13 to 20 seconds.
- **Wallets.** In the browser checkout, Phantom, Solflare and Backpack connect through the
  Wallet Standard. The game builds the transaction and the wallet only signs it. On a phone,
  the player scans a Solana Pay QR code (a `solana:` link with a reference).
- **Exchanges and manual transfers** must withdraw on the **Solana (SPL)** network. The game
  finds the exact amount by scanning your token account. Pasting the transaction signature
  also works.
- **Restore** accepts an EVM `0x…` hash or a Solana signature (base58).

### Item options

`cryptopay.add_item(item_id, code, price, token, title, min_price=None)`

- `item_id`: lowercase letters, digits, `_` and `-`.
- `price`: a **string** like `"4.99"` (at most 2 decimals). Floats are refused.
- `min_price`: the lowest amount that still unlocks the item (defaults to `price`).
  - **Sales:** you can pass a lower `amount` to `pay()` as long as it's at least `min_price`.
  - **Raising a price later:** keep `min_price` at the old price, so earlier buyers stay unlocked.

## 3. Use it

```renpy
label end_of_chapter1:
    if not cryptopay.owns("chapter2"):
        "Chapter 2 is available for 4.99 USDC."
        $ result = cryptopay.pay("unlock", item="chapter2")

    if cryptopay.owns("chapter2"):
        jump chapter2
    "Maybe later, then."
```

Tips (the player can change the suggested amount on the payment screen):

```renpy
$ result = cryptopay.pay("tip", amount="3.00", token="USDC")
```

Buttons in screens use non-blocking actions:

```renpy
textbutton "Support the developer" action CryptoPay("tip", amount="3", token="USDC")
textbutton "Restore purchases" action CryptoRestore()
```

### API reference

| Call | Returns |
|---|---|
| `cryptopay.add_chain(key, name, chain_id, rpc, tokens, confirmations, explorer_tx, ...)` | Nothing. Adds your own EVM chain; call before `configure()` ([details](#your-own-rpc-endpoints-and-chains)) |
| `cryptopay.configure(wallet=None, ..., solana_wallet=None)` | Nothing. Needs `wallet` or `solana_wallet` (or both); see [All `configure()` options](#all-configure-options) |
| `cryptopay.pay(kind, item=None, amount=None, token=None, callback=None)` | `PaymentResult`, when the payment screen closes |
| `cryptopay.owns(item_id)` | `True` if a stored, verified payment unlocks the item. Instant, works offline |
| `cryptopay.has_tipped()` / `cryptopay.tip_total()` | Supporter status / total tipped (USD) |
| `cryptopay.restore(callback=None)` | `RestoreResult`; shows the restore screen |
| `cryptopay.receipts()` / `cryptopay.pending()` | Stored receipts / payments still being watched |
| `cryptopay.export_receipts(path=None)` | Writes receipts as JSON (for support requests) |
| `cryptopay.supported()` | Always `True` (the web build included). If you used it to keep your web build free, check `renpy.emscripten` instead |
| `cryptopay.clear_local_data()` | Testing helper: forgets receipts and pending payments on this device, so you can see your paywall again. Payments stay on the chain, and Restore brings them back |
| `cryptopay.on_result = fn` | Handler for results that arrive with no live callback (e.g. after a restart) |
| `cryptopay.notify = True` | Show a "Payment confirmed" notification |

`PaymentResult` fields:
- `status`: `"paid"`, `"already_owned"`, `"cancelled"`, `"expired"`, `"underpaid"`, `"failed"` or `"pending"`
- `kind`, `item`, `amount` (actually paid), `token`, `chain`, `tx_hash`, `payer`, `session_id`, `error`

`"pending"` means the player chose **Continue playing**. The plugin keeps watching in the background, across restarts, for up to 24 hours.

### Callbacks

```renpy
init python:
    def on_payment(result):
        if result.status == "paid":
            achievement.grant("supporter")
    cryptopay.on_result = on_payment          # also catches payments finished after a restart

    # per call:
    # $ cryptopay.pay("unlock", item="chapter2", callback=on_payment)
```

Rules:
- Callbacks run on the main thread, for **side effects only**. Never call `jump`, `call` or `renpy.call_screen` inside one; change the story using `pay()`'s return value or `owns()`.
- The receipt is saved **before** your callback runs. Ownership never depends on the callback.
- A session can report `"cancelled"` or `"expired"` and **later** `"paid"` (the player paid late). Always treat `"paid"` as final.

## 4. Test before release

1. Use `testnet=True` and **your own wallet**.
2. Get test USDC from Circle's faucet, plus a little test ETH (or POL on Amoy) for network fees.
3. Buy something with each method you enabled: browser wallet, QR and manual.
4. Restart the game mid-payment and check the purchase still completes.
5. Run `cryptopay.clear_local_data()` from the developer console (Shift+O), then use **Restore purchases** with the transaction hash.
6. In developer mode the payment screen shows **DEV MODE: paying to 0x5290...9EE7**. Check that the start and end match your address. The Manual transfer step shows the full address.

**Solana devnet checklist** (`testnet=True` with `solana_wallet`):

1. Switch Phantom to devnet: Settings → Developer settings → Testnet mode → Solana Devnet.
2. Get devnet SOL (`solana airdrop` or faucet.solana.com) and devnet USDC (faucet.circle.com → Solana Devnet).
3. Receive devnet USDC on your developer wallet once, so its token account exists, then restart the game. Until then the console warns and Solana isn't offered.
4. Pay by browser extension, by phone QR, and by a manual transfer from a second wallet.
5. Run `cryptopay.clear_local_data()`, then restore with each signature.

## 5. Customizing the look

The payment and restore screens are a light sheet on the right of the screen, so your game stays visible beside it. The browser checkout page uses the same look.

The payment sheet shows one step at a time, and each step slides in:

| Step | Shown when | Moves on when |
|---|---|---|
| Amount | Tips only | The player presses **Next** |
| Network | More than one network is enabled | The player picks one and the payment has started |
| Payment method | More than one method is available | The player picks Browser wallet, Phone wallet or Manual transfer |
| Browser / QR / Manual | Always, for the chosen method | **Open checkout in browser**, **I've sent it**, a pasted hash, or the game sees the transaction |
| Waiting | After the method page | The payment is confirmed, too small, or expires |
| Done | Final result | The player presses **Continue** |

- **Skipped steps:** a step with only one choice is skipped in both directions. A game with one network and one method goes straight to that method's page.
- **Going back:** the ← arrow (and Esc or right-click) goes back one step. It is hidden on the first step, and it disappears once the game has seen a transaction for this payment, so the player can't start a second payment by accident. Going back keeps the payment that has already started. Picking a different network or amount starts a new one.

```renpy
init python:
    cryptopay.configure(wallet="0x...", logo="images/logo.png", accent=gui.accent_color)
```

- **Logo and accent:** the `logo` and `accent` options above apply to both the in-game screens and the browser page.
- **Colors:** the sheet's palette lives in `cryptopay.SHEET`, `cryptopay.INK`, `cryptopay.MUTED`, `cryptopay.LINE`, `cryptopay.DIM` (the shade over your game) and friends. Set them in an `init python` block.
- **Styles:** all screen styles start with `cryptopay_`: `cryptopay_sheet`, `cryptopay_title`, `cryptopay_amount`, `cryptopay_option` (network, method and amount choices), `cryptopay_button`, `cryptopay_primary_button`, `cryptopay_text`, … Override them in your own `.rpy`. Text uses your game's default font.
- **Replacing a screen:** define `screen cryptopay_payment(cid)` or `screen cryptopay_restore(rid)` yourself, using `cryptopay/cryptopay_screens.rpy` as the reference. To change a single step, define its screen instead: `cryptopay_step_amount`, `cryptopay_step_network`, `cryptopay_step_method`, `cryptopay_step_browser`, `cryptopay_step_qr`, `cryptopay_step_manual`, `cryptopay_step_waiting` or `cryptopay_step_done`. Each takes `(ctl, cid)`. The header (`cryptopay_payment_header`) and footer (`cryptopay_payment_footer`) are screens too, and the slide is the `cryptopay_step_in` transform.
- **Sizing:** the screens scale with your game's resolution. In portrait or narrow games, the sheet takes the full width. A step's body scrolls when needed. Its forward button, the status line and the footer buttons stay pinned at the bottom.

## 6. Security and limitations

What the plugin protects against:
- **Typing a fake purchase into the save data:** every receipt is re-checked on the blockchain at each launch, and invented ones are removed.
- **Look-alike "USDC" tokens:** only the official contract addresses count.
- **Fake transaction hashes sent to the browser page:** every hash is verified on-chain.

What it **cannot** prevent (no server means nobody holds a secret):
- A player reusing **someone else's real transaction hash**, or copying a `persistent` file.
- **Patched game code**, or a player-controlled fake RPC endpoint.
- A pirate re-releasing your game with their own wallet address.
- **Rare amount collisions:** two players buying the same item within minutes of each other, paying by QR or manual transfer without pasting the hash (about 1 % chance per overlapping pair).

So treat unlocks as **honour-grade**: right for supporter rewards and tips, weak for valuable content.

### Web builds
Payments work in Ren'Py's web (HTML5) build, on your own site or embedded on itch.io:

- **Browser wallet**: the player's wallet extension (MetaMask, Rabby, Phantom, ...) is
  already in the page, so the checkout opens right over the game. The option only shows
  when the page has a wallet for the chosen network, which includes a wallet app's own
  built-in browser on phones.
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
but only a real upload proves it for the wallets your players use.

To test the web build automatically, `tools/run_renpy_web_e2e.py --sdk <sdk>` (a Ren'Py
SDK with the web platform installed) builds the game for the web and pays in headless
Chrome against Anvil (needs Anvil, Playwright, and Google Chrome or Playwright's
Chromium).

### Debug menus and cheats
If your game has a hidden debug mode or a level select, gate it with `owns()`. Otherwise it is an easy way around the paywall.

### Platform rules
- **Steam** forbids games that use cryptocurrencies.
- **Google Play and the App Store** require their own billing for digital unlocks.
- **itch.io and direct distribution** are the natural home for this plugin.

Check each store's current terms yourself.

## 7. Troubleshooting

| Symptom | What to do |
|---|---|
| "Can't reach the network. Retrying…" | All RPC endpoints for that chain are failing. Add your own endpoint via `rpc_urls` (Alchemy, Infura, QuickNode, …). Note that keys inside a game build are visible to anyone, so use a key restricted to read-only calls with a spending cap |
| Payment sent but nothing happens | The wallet may have changed the amount. Use **Manual transfer → paste transaction hash** |
| Paid on the wrong network | Funds sent to a normal wallet on another EVM chain are still in your wallet on that chain. Honour the purchase manually or refund |
| Paid from an exchange that rounds amounts | Pasting the transaction hash works for any amount ≥ the price |
| "Transaction not found" on restore | Some public nodes don't index old hashes. Try again later, or add your own RPC |
| Solana isn't offered | Your wallet has no token account for that token yet. The console says which one: receive any amount of it once, then restart the game |
| "Transaction not found" on restore (Solana) | A very new transaction needs about 15 seconds to finalize. Try again shortly |
| Player needs a refund or manual unlock | Look up their transaction on a block explorer; refunds are sent from your wallet |

`cryptopay.export_receipts()` writes a readable JSON file you can ask players to send you.

## 8. How it works (short version)

1. **The player picks a network**, and the plugin creates a payment session. The amount is your price plus 4 extra digits that encode the item code and a random number: `4.99 → 4.993742`.
2. **The game watches the chain** for a transfer of exactly that amount to your wallet. A transaction hash reported by the browser page, or pasted by the player, is checked directly instead.
3. **After enough confirmations** (blocks on top of the payment's block; on Solana, once the transaction is finalized), the transfer is stored as a receipt in `persistent`. `owns()` reads receipts, which are re-verified on-chain at each launch.
4. **Restore** reads any transaction hash back from the chain and decodes the item from its amount.
