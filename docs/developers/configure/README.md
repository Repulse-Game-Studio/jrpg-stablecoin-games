# Configure

```renpy
init python:
    scfg.configure(
        wallet="0xYourWalletAddressHere",   # where payments go (EIP-55 checked)
        testnet=True,                       # remove (or False) for real money
    )

    # Paid items. `code` (1-99) is written into players' payments forever:
    # NEVER change an item's code after release.
    scfg.add_item("chapter2", code=1, price="4.99", token="USDC", title="Chapter 2: The Fall")
    scfg.add_item("ost",      code=2, price="2.00", token="USDT", title="Original Soundtrack")
```

## All `configure()` options

| Option | Default | Meaning |
|---|---|---|
| `wallet` | required unless `solana_wallet` is given | Your EVM payout address. A wrong-case checksum raises an error, which catches typos |
| `solana_wallet` | none | Your Solana payout address (base58) to accept USDC/USDT on Solana. See [Solana](solana.md) |
| `testnet` | `False` | `True` uses Sepolia, Arbitrum Sepolia, Polygon Amoy, Base Sepolia, Arc Testnet and Solana Devnet (USDC only) |
| `chains` | all built-ins + yours | Which networks players may pay on, in display order: `["ethereum", "solana", "arc", "base", "arbitrum", "polygon"]`. Chains you added with `add_chain()` can be listed too. By default every built-in is offered in that order, followed by yours: the EVM chains if `wallet` is set and `"solana"` if `solana_wallet` is set. The network step shows two networks as tiles, Ethereum and Solana, and the others under **More networks**. When fewer of those two are offered (no `solana_wallet`, or a list without them), the next networks in order fill the two tiles: an EVM-only game shows Ethereum and Arc |
| `tokens` | both | `["USDC"]`, `["USDT"]` or both |
| `methods` | all on | Per-method options, see below |
| `rpc_urls` | none | Your own RPC endpoints per chain, tried before the built-in public ones, e.g. `{"arbitrum": ["https://arb-mainnet.g.alchemy.com/v2/KEY"]}`. See below |
| `confirmations` | per chain | Override blocks to wait on EVM chains, e.g. `{"ethereum": 12}`. Arc's default is 1: its blocks are final. Refused for `"solana"`, which has no block count (see [Solana](solana.md)) |
| `previous_wallets` | none | Wallets used by earlier versions of your game, EVM (`0x…`) and Solana addresses can be mixed. Keeps old purchases valid after you change wallets |
| `logo` | none | An image in your game folder, e.g. `"images/logo.png"` (`.png`, `.jpg` or `.webp`). Shown at the top of the payment screens and the browser checkout page, fitted into 240 × 64 (at 1280 × 720). A wide wordmark works best |
| `accent` | `"#0e7c74"` | Color of selected options and the main browser button, e.g. `accent=gui.accent_color`. Text on it switches between white and dark automatically |
| `onramp` | on, built-in EVM chains on mainnet | The **Pay with card** button under each network, see [below](#pay-with-card) |

## Payment methods

| Method | Options | What the player does |
|---|---|---|
| `browser` | `enabled`, `timeout_min` (30) | Desktop: clicks "Open checkout in browser"; a page served by the game on `127.0.0.1` connects MetaMask, Rabby, Coinbase Wallet or another extension (on Solana: Phantom, Solflare or Backpack), and sends the payment. Web build: clicks "Open wallet"; the same checkout opens over the game (offered only when the page has a wallet for the network; see [Web builds](../security/web-builds.md)) |
| `qr` | `enabled`, `wallet_button` (Android "Open wallet app"), `chains` (restrict) | Scans a QR code with a phone wallet (EIP-681 payment link; on Solana, a Solana Pay link) |
| `manual` | `enabled`, `allow_tx_paste` | Sends the exact amount from any wallet or exchange; can paste the transaction hash |

```renpy
scfg.configure(wallet="0x...", methods={"browser": {"enabled": False},
                                              "qr": {"chains": ["base", "polygon"]}})
```

## Pay with card

On the network step, each network that takes cards has a small **Pay with card** button
under its tile. It skips the payment-method step and shows a card page with the exact
amount; its **Open card checkout** button opens a card checkout page for that network in the
player's browser (a new tab in a web build), and the sheet moves on to waiting for the
payment. The player pays by card there, and the provider sends the token straight to your
wallet on that network. The game finds that payment the way it finds any other: by its
exact amount (the price plus the item code and nonce in the last four decimals), so nothing
about the card payment changes how an unlock is confirmed or restored.

The default page is [thirdweb's hosted Checkout widget](https://portal.thirdweb.com/bridge/checkout-widget/iframe).
It needs no account or key on your side. The link carries the network, the token's contract,
the payment's exact amount and your wallet for that network (as the `seller`), and offers
card payment only. thirdweb's card partners handle the purchase and the identity checks,
with their own regions, minimum amounts and fees (the player pays the fees). A web build opens
it with `noopener,noreferrer`, so the page is not told the game's address, and a desktop game
hands it to the player's browser, which sends no referrer for a link opened from outside.

> **Not yet checked with a real card:** the game only recognises the payment if thirdweb
> delivers exactly the amount in the link, all six decimals. Make one small real card
> payment on mainnet before you rely on it.

**On by default.** A mainnet game shows the button without changing its configuration. If
linking to an outside purchase flow matters for your store or region, turn it off with
`onramp={"enabled": False}`.

```renpy
scfg.configure(wallet="0x...", onramp={"enabled": False})            # hide it
scfg.configure(wallet="0x...", onramp={"chains": ["base", "arc"]})   # only on these networks
scfg.configure(wallet="0x...", onramp={"url": "https://pay.example/?net={chain_id}&a={amount}&to={recipient}"})
```

| Option | Default | Meaning |
|---|---|---|
| `enabled` | `True` | `False` hides the button |
| `chains` | built-in EVM chains | The networks it is offered on, from `chains=` (EVM only). Left out: the built-in EVM chains of a mainnet game, and none on testnet, where cards buy no test tokens; list chains to see the button while testing. Chains you added with `add_chain()` are only offered when listed |
| `url` | thirdweb | Your own `https://` page with a host, without spaces, at most 2048 characters. `{chain}` (the chain key), `{chain_id}`, `{token}`, `{token_address}`, `{amount}` (the exact amount, with 6 decimals: `4.993742`) and `{recipient}` (your wallet on that network) are filled in; any other brace is refused. The page must pay `{recipient}` exactly `{amount}`: the game waits for that payment |

A mistake raises a `ConfigError` at startup with a code such as `onramp.unknown_option`,
`onramp.bad_url` (also for a url without a host after `https://`), `onramp.unknown_placeholder`,
`onramp.disabled_chain` or `onramp.not_evm`. The other engines take the same option in their own
spelling: `"onramp"` in Godot's `configure()`, the **Buy Link** parameter in RPG Maker MZ,
`ScfgOptions.Onramp` (and the SCFG Settings asset's onramp fields) in Unity, and `FScfgOptions.Onramp`
(and the project settings) in Unreal; see each engine's API page.
Solana has no card button: thirdweb's checkout takes payments on EVM networks only.

A network the game offers alone, without a card button, is still chosen for the player and
the network step skipped; with a card button the step stays, so the player can choose it.

## Your own RPC endpoints and chains

**Your own RPC endpoint for a built-in chain.** The plugin talks to each chain through
free public RPC endpoints. They work, but they rate-limit and sometimes go down. If you
have an endpoint from Alchemy, Infura, QuickNode, Ankr, or your own node, pass it in
`rpc_urls`. Your endpoints are tried first, and the built-in public ones remain as a fallback:

```renpy
init python:
    scfg.configure(
        wallet="0xYourWallet",
        rpc_urls={
            "arbitrum": "https://arb-mainnet.g.alchemy.com/v2/YOUR_KEY",
            "base": ["https://base-mainnet.g.alchemy.com/v2/YOUR_KEY",
                     "https://my-backup-node.example.com"],
        },
    )
```

**Your own chain.** For an EVM chain that isn't built in (Avalanche, Optimism, Linea,
your own appchain…), describe it with `scfg.add_chain()` **before** `configure()`.
Its key then works everywhere a built-in key does: `chains=`, `rpc_urls=`, `confirmations=`
and `methods={"qr": {"chains": [...]}}`.

```renpy
init python:
    # Avalanche C-Chain, real money (used when testnet=False).
    scfg.add_chain(
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
    scfg.add_chain(
        "avalanche", testnet=True,
        name="Avalanche Fuji", chain_id=43113,
        rpc=["https://api.avax-test.network/ext/bc/C/rpc"],
        tokens={"USDC": "0x5425890298aed601595a70AB815c96711a31Bc65"},
        confirmations=3,
        explorer_tx="https://testnet.snowtrace.io/tx/{}",
        native_symbol="AVAX",
    )

    scfg.configure(
        wallet="0xYourWallet",
        testnet=True,
        chains=["base", "avalanche"],      # leave out to offer all built-ins + yours
    )
```

| `add_chain()` argument | Meaning |
|---|---|
| `key` | Your name for the chain: lowercase letters, digits, `_`, `-`. It is stored in players' receipts, so **never change it after release**. It can't reuse a built-in key (`"arc"` is built in now: if your game added Arc itself, delete that `add_chain()` call; purchases it recorded stay valid, see [Arc](arc.md#upgrading-to-the-version-with-arc-built-in)) |
| `name` | Shown to players on the network button (at most 32 characters) |
| `chain_id` | The EVM chain id (see [chainlist.org](https://chainlist.org)). Wallets use it to switch networks, and QR codes carry it |
| `rpc` | One URL or a list, tried in order. `https://` only, except `http://127.0.0.1` / `http://localhost`, with or without a port (e.g. `http://127.0.0.1:8545`), for a local test node. The same goes for `rpc_urls`. The **last** one is also handed to the player's browser wallet if it needs to add the network, so put a public endpoint last |
| `tokens` | `{"USDC": "0x…"}` and/or `"USDT"`. Copy addresses from Circle's / Tether's official lists, never from a block explorer search |
| `confirmations` | Blocks to wait before a payment counts. Use the chain's finality: a few for fast-finality chains, more for chains that reorg |
| `explorer_tx` | Transaction page with `{}` where the hash goes. `https://` only, except `http://127.0.0.1` / `http://localhost` (with any port, e.g. `http://127.0.0.1:4000/tx/{}`) for a local test chain's explorer |
| `native_symbol` | The gas token (`"AVAX"`, `"ETH"`, …), named in "you need gas" warnings. Default `"ETH"` |
| `fee_hint` | `"lowest"`, `"low"` (default) or `"high"`, shown under the network name |
| `testnet` | `False` (default) adds the chain for real-money games; `True` for `configure(testnet=True)` games. Add it twice with the same key to have both |
| `max_log_range` | Blocks per `eth_getLogs` call to start with (default 1000). The plugin shrinks it by itself when an endpoint complains |

Rules your chain must follow (checked when the game starts where possible):

- **EVM only.** The plugin finds payments through ERC-20 `Transfer` events over Ethereum
  JSON-RPC (`eth_getLogs`, `eth_getTransactionReceipt`, …). Tron, Bitcoin and other non-EVM
  chains can't be added this way. Solana is built in: see [Solana](solana.md).
- **The token must not be the chain's gas coin.** On chains like Arc, where USDC *is* the
  gas coin, a plain wallet send logs no ERC-20 event at the token contract, so `add_chain()`
  would miss it. Arc is built in for that reason (see [Arc](arc.md)).
- **6-decimal tokens only.** The item code lives in the last four of six decimals. This
  rules out BNB Chain's USDC/USDT (18 decimals). The plugin can't check this offline, so
  confirm `decimals()` on the explorer before you ship.
- **The endpoint must serve `eth_getLogs`** for recent blocks. Most do; check it with a test
  payment on the chain's testnet.
- **Test it first.** Make one payment on the testnet version of your chain, then one small
  real payment, before you release.

## Item options

`scfg.add_item(item_id, code, price, token, title, min_price=None)`

- `item_id`: lowercase letters, digits, `_` and `-`.
- `price`: a **string** like `"4.99"` (at most 2 decimals). Floats are refused.
- `min_price`: the lowest amount that still unlocks the item (defaults to `price`).
  - **Sales:** you can pass a lower `amount` to `pay()` as long as it's at least `min_price`.
  - **Raising a price later:** keep `min_price` at the old price, so earlier buyers stay unlocked.
