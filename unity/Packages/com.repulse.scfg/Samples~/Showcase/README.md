# SCFG Unity showcase

One screen that shows how the Stablecoin for Games package works. Every button calls one member of the `SCFG` facade,
and the right-hand column shows what comes back. The twin of the Godot showcase.

| On screen | Calls |
|---|---|
| **Unlocks**: three items, the supporter pack also at a pay-what-you-want amount | `SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = id })`, with `Amount = "5.00"` |
| **Tip jar**: three suggested amounts | `SCFG.Pay(ScfgPayKind.Tip, new ScfgPayRequest { Amount = "1.00", Token = "USDC" })` |
| **Restore purchases** | `SCFG.Restore()` |
| **Export receipts** | `SCFG.ExportReceipts()` |
| **Clear local data**, a testing helper | `SCFG.ClearLocalData()` |
| **What SCFG knows** | `SCFG.Owns()`, `TipTotal()`, `HasTipped()`, `Pending()`, `Receipts()` |
| **Event log** | every awaited result and the events `PaymentResolved` and `ReceiptsChanged` |

It runs on testnets (Sepolia, Arbitrum Sepolia, Polygon Amoy, Base Sepolia, Arc Testnet, and Solana Devnet with a
Solana wallet), so no real money moves. The payments go to your wallet: set it on the **SCFG Showcase** component in
the scene (**Wallet**, an EVM `0x` address, and **Solana Wallet** to take Solana too), or in your project's SCFG
Settings asset, which the showcase uses when both component fields are blank. With neither, it configures nothing and
says so in its log: it never pays a placeholder address.

## Run it

1. In the Package Manager, select **Stablecoin for Games (SCFG)**, open **Samples** and press **Import** next to
   **Showcase**. Unity copies it to `Assets/Samples/Stablecoin for Games (SCFG)/<version>/Showcase/`.
2. Open `Showcase.unity` there and press **Play**.

The screen is built with UI Toolkit in code (`ScfgShowcase.cs`). If your project has an SCFG Settings asset with a
wallet, it is applied first, and the showcase's own `SCFG.Configure` call replaces it. That call starts from the
asset's options (its accent, logo and Pay with card, so what you set there shows here), with the asset's wallets when
the component's are blank, and adds only testnet and the showcase's own items. The showcase always runs on the test
networks, so it takes the asset's networks and tokens only from an asset set to testnet, and of those networks only
the ones it has a wallet for. That works only before payments start, so play this scene first: loaded from another
scene after payments started there, the showcase says so and stays idle. Loaded a second time in the same run, it
carries on with the configuration it made the first time.

To pay for real on a testnet: set your wallet as above, then press **Unlock** and pick a network and a method
(browser wallet, phone wallet or manual transfer).

Test payments need test money in a second wallet (the player's): testnet USDC from
[Circle's faucet](https://faucet.circle.com/) (Sepolia, Arbitrum Sepolia, Base Sepolia, Polygon Amoy, Arc Testnet and
Solana Devnet), and a little of the network's coin for the fee: Sepolia ETH (for example
[Google's faucet](https://cloud.google.com/application/web3/faucet/ethereum/sepolia), which also sends to Arbitrum
Sepolia and Base Sepolia), POL on Amoy from [Polygon's faucet](https://faucet.polygon.technology/), and Devnet SOL
from [Solana's faucet](https://faucet.solana.com/). Arc Testnet pays its fee in USDC. On Solana, the receiving wallet
must have received each token once (it needs a token account), on Devnet as on mainnet: send it a little Devnet
USDC first, or the sheet does not offer USDC on Solana.
