# SCFG Unity demo

A small, complete game that uses the Stablecoin for Games package: a title, a locked Chapter 2 with an Unlock button,
a Tip button and a Restore button. It runs on testnets, so no real money moves. It uses only the public API
(`SCFG.Configure`, `AddItem`, `Pay`, `Owns`, `Restore` and the `PaymentResolved` event). While a payment or restore
screen is open it pauses the game (`Time.timeScale = 0`), because the sheet does not stop your own code that reads
input. The twin of the Godot demo.

## Run it

1. In the Package Manager, select **Stablecoin for Games (SCFG)**, open **Samples** and press **Import** next to
   **Demo**.
2. Open `Demo.unity` there and press **Play**.

Before you publish anything, replace the placeholder wallet in `ScfgDemo.cs` (the public EIP-55 example address) with
your own, and set `Testnet = false` when you go live.
