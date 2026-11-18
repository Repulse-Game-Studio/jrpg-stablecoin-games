# Stablecoin for Games (SCFG) for Unity

Accept **USDC** and **USDT** tips and unlock purchases in your Unity game, paid **directly to your own wallet**.
There is no server, no account and no middleman in the payment: the package watches the blockchain itself. Unity 6.3
LTS or newer; desktop and WebGL builds: Windows and macOS are supported, unverified (Linux and WebGL are tested);
mobile is best effort and unverified. **Preview**: tested on Unity 6000.6.9f1 against local test chains.

## Quick start

```csharp
using Scfg;

SCFG.Configure(new ScfgOptions { Wallet = "0xYourWalletAddressHere", Testnet = true });
SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" });
ScfgPaymentResult result = await SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
```

Then gate the content with `SCFG.Owns("chapter2")`: it is instant and works offline. `Testnet = true` offers test
networks only, so no real money moves; set it to `false` for the release. To configure without code, create
**Assets > Create > SCFG Settings** instead and press **Validate** in its inspector.

## Samples

In the Package Manager, select this package, open **Samples** and click **Import** next to:

- **Showcase**: one screen that uses the whole API, with a live log of every result and event.
- **Demo**: a small game with a locked Chapter 2 and Unlock, Tip and Restore buttons.

Open the imported scene and press **Play**. Both run on testnets.

## Documentation

- [For Unity developers](https://jrpg.com/stablecoinforgames/docs/developers/unity/): what the package does and its
  status.
- [Install](https://jrpg.com/stablecoinforgames/docs/developers/unity/install): the git URL and tarball installs, the
  samples, the first payment, the SCFG Settings asset and the go-live checklist.
- [API reference](https://jrpg.com/stablecoinforgames/docs/developers/unity/api): every `SCFG` call, the results and
  events, customising the screens, input, persistence, WebGL builds and IL2CPP.

In the release tarball the whole guide is also next to this page, as `scfg-unity-guide.md`.

> Without a server, unlocks are honour-grade: a determined player can bypass them. Do not sell anything you cannot
> afford to lose to a bypass.
