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

It runs on testnets (Sepolia, Arbitrum Sepolia, Polygon Amoy, Base Sepolia, Arc Testnet), so no real money moves. The
payout wallet is a placeholder that is not yours (the EIP-55 example address); replace `Wallet` in `ScfgShowcase.cs`
with your own testnet wallet to watch a real test payment arrive.

## Run it

1. In the Package Manager, select **Stablecoin for Games (SCFG)**, open **Samples** and press **Import** next to
   **Showcase**. Unity copies it to `Assets/Samples/Stablecoin for Games (SCFG)/<version>/Showcase/`.
2. Open `Showcase.unity` there and press **Play**.

The screen is built with UI Toolkit in code (`ScfgShowcase.cs`). If your project has an SCFG Settings asset, it is
applied first, and the showcase's own `SCFG.Configure` call replaces it. That works only before payments start, so
play this scene first: loaded from another scene after payments started there, the showcase says so and stays idle.
Loaded a second time in the same run, it carries on with the configuration it made the first time.

To pay for real on a testnet: put your own wallet in `Wallet`, get testnet USDC from
[Circle's faucet](https://faucet.circle.com/) into a second wallet (the player's), press **Unlock**, and pick a
network and a method (browser wallet, phone wallet or manual transfer).
