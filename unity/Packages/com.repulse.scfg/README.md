# Stablecoin for Games (SCFG) for Unity

USDC and USDT tips and unlock purchases for Unity games, paid straight to your own wallet: no server, no account, no
middleman. The game watches the blockchain itself. Unity 6.3 LTS or newer; desktop (Windows, macOS, Linux) and WebGL
builds. **Preview.**

```csharp
using Scfg;

SCFG.Configure(new ScfgOptions { Wallet = "0x…", Testnet = true });
SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" });
ScfgPaymentResult r = await SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
bool owned = SCFG.Owns("chapter2");
```

- Install and first payment: https://jrpg.com/stablecoinforgames/docs/developers/unity/install
- API reference: https://jrpg.com/stablecoinforgames/docs/developers/unity/api
- Samples: Package Manager → this package → Samples → Showcase or Demo → Import.

Without a server, unlocks are honour-grade: a determined player can bypass them. Do not sell anything you cannot
afford to lose to a bypass. MIT licensed (LICENSE.md).
