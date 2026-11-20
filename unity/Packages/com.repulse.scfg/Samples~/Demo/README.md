# SCFG Unity demo

A small, complete game that uses the Stablecoin for Games package: a title, a locked Chapter 2 with an Unlock button,
a Tip button and a Restore button. It runs on testnets, so no real money moves. It uses only the public API
(`SCFG.Configure`, `AddItem`, `Pay`, `Owns`, `Restore` and the `PaymentResolved` event). While a payment or restore
screen is open it pauses the game (`Time.timeScale = 0`, then back to the time scale it had), because the sheet does
not stop your own code that reads input. The twin of the Godot demo.

It configures SCFG in its `Start`, which works only before payments start (the frame after the first `Configure`),
so play this scene first. With an SCFG Settings asset in the project the demo's `Configure` replaces it, starting
from the asset's options (accent, logo, buy link; networks and tokens only from an asset set to testnet, and only the
networks it has a wallet for) and adding only testnet and its own item; loaded from another scene after payments
started there, the demo says so and stays idle. Loaded a second time in the same run, it carries on with the
configuration it made the first time.

## Run it

1. In the Package Manager, select **Stablecoin for Games (SCFG)**, open **Samples** and press **Import** next to
   **Demo**.
2. Open `Demo.unity` there and press **Play**.

The payments go to your wallet: set it on the **SCFG Demo** component in the scene (**Wallet**, an EVM `0x` address,
and **Solana Wallet** to take Solana too), or in your project's SCFG Settings asset, which the demo uses when both
component fields are blank. With neither, the demo configures nothing and says so on screen: it never pays a
placeholder address. It always runs on testnets (`Testnet = true` in `ScfgDemo.cs`); your own game turns that off for
the release.

Test payments need test money in a second wallet (the player's): testnet USDC from
[Circle's faucet](https://faucet.circle.com/) (Sepolia, Arbitrum Sepolia, Base Sepolia, Polygon Amoy, Arc Testnet and
Solana Devnet), and a little of the network's coin for the fee: Sepolia ETH (for example
[Google's faucet](https://cloud.google.com/application/web3/faucet/ethereum/sepolia), which also sends to Arbitrum
Sepolia and Base Sepolia), POL on Amoy from [Polygon's faucet](https://faucet.polygon.technology/), and Devnet SOL
from [Solana's faucet](https://faucet.solana.com/). Arc Testnet pays its fee in USDC. On Solana, the receiving wallet
must have received each token once (it needs a token account), on Devnet as on mainnet: send it a little Devnet
USDC first, or the sheet does not offer USDC on Solana.
