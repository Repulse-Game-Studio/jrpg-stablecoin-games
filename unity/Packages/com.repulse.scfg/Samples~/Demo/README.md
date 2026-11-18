# SCFG Unity demo

A small, complete game that uses the Stablecoin for Games package: a title, a locked Chapter 2 with an Unlock button,
a Tip button and a Restore button. It runs on testnets, so no real money moves. It uses only the public API
(`SCFG.Configure`, `AddItem`, `Pay`, `Owns`, `Restore` and the `PaymentResolved` event). While a payment or restore
screen is open it pauses the game (`Time.timeScale = 0`, then back to the time scale it had), because the sheet does
not stop your own code that reads input. The twin of the Godot demo.

It configures SCFG in its `Start`, which works only before payments start (the frame after the first `Configure`),
so play this scene first. With an SCFG Settings asset in the project the demo's `Configure` replaces it; loaded from
another scene after payments started there, the demo says so and stays idle. Loaded a second time in the same run,
it carries on with the configuration it made the first time.

## Run it

1. In the Package Manager, select **Stablecoin for Games (SCFG)**, open **Samples** and press **Import** next to
   **Demo**.
2. Open `Demo.unity` there and press **Play**.

Before you publish anything, replace the placeholder wallet in `ScfgDemo.cs` (the public EIP-55 example address) with
your own, and set `Testnet = false` when you go live.
