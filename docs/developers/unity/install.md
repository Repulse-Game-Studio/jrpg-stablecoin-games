# Install (Unity)

Accept **USDC** and **USDT** tips and unlock purchases in your Unity game, paid **directly to your own wallet**.
There is no server, no account and no middleman: the package watches the blockchain itself. It is a UPM package,
`com.repulse.scfg`, needs Unity **6.3 LTS or newer** (tested on 6000.6.9f1), and works in
desktop builds (Windows, macOS, Linux) and in WebGL builds. Mobile is best effort and unverified (see
[Mobile](api.md#mobile-unverified)).

Chains: Ethereum, Arbitrum, Polygon (USDC and USDT), Base (USDC), Arc (USDC), Solana (USDC and USDT), plus
any EVM chain you add yourself.

> Without a server, unlocks are honour-grade: a determined player can bypass them. Do not sell anything you
> cannot afford to lose to a bypass.

## From a git URL

1. Open **Window > Package Manager**, click **+** and pick **Add package from git URL...**.
2. Paste this URL and click **Add**:

   ```
   https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games.git?path=unity/Packages/com.repulse.scfg#v0.3.0
   ```

   The `#v0.3.0` at the end pins a release tag. The Unity package first ships in release 0.3.0, so this tag
   exists from that release on; until then the Package Manager answers that the revision is not found, and the
   only way in is `#main` (or a commit hash) instead, at your own risk: the main branch is not a release, and you
   should not ship from it. The Package Manager needs `git` installed on your machine for this.

## From the release tarball

1. Download `scfg-unity-<version>.tgz` from the
   [latest release](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/releases/latest). Or build it
   from a clone of the repository with `uv run python tools/package.py --engine unity` (Python 3.9+); it is written
   to `dist/`.
2. Put the file where it stays, for example in a `Packages/` or `Vendor/` folder of your project that you commit:
   the project's manifest refers to it by its path.
3. Open **Window > Package Manager**, click **+**, pick **Add package from tarball...** and choose the file.

The tarball also carries this guide as one file, `Documentation~/scfg-unity-guide.md`, next to the license.

Either way the package appears as **Stablecoin for Games (SCFG)** under **In Project**. Its code is in the
namespace `Scfg`; the facade is the static class `SCFG`.

## Try the samples first

The package comes with two samples:

- **Showcase** is a one-screen tour of the whole API. It has three unlocks (one with a pay-what-you-want amount),
  a tip jar, Restore, Export and Clear, and next to them it shows what the package knows (owned items, tips,
  pending payments, receipts) and a live log of every result and event. Its `ScfgShowcase.cs` is written to be
  read top to bottom.
- **Demo** is a small game: a title, a locked Chapter 2, a Tip button and a Restore button, and it pauses the game
  while a screen is open.

To run one, select **Stablecoin for Games (SCFG)** in the Package Manager, open **Samples** and click **Import**
next to it. Unity copies it to `Assets/Samples/Stablecoin for Games (SCFG)/<version>/`. Open its scene
(`Showcase.unity` or `Demo.unity`) and press **Play**.

Both run on testnets, so no real money moves. Put your own testnet wallet in `Wallet` at the top of
`ScfgShowcase.cs`, and you can watch a real test payment arrive. Get test USDC from
[Circle's faucet](https://faucet.circle.com/).

## Your first payment

```csharp
using Scfg;
using UnityEngine;

public class Shop : MonoBehaviour
{
    void Awake()
    {
        SCFG.Configure(new ScfgOptions { Wallet = "0xYourWalletAddressHere", Testnet = true });
        SCFG.AddItem("chapter2", new ScfgItem { Code = 1, Price = "4.99", Token = "USDC", Title = "Chapter 2" });
    }

    public async void OnUnlockPressed()
    {
        PauseGame(true);   // the sheet does not stop your scripts, see the API page
        ScfgPaymentResult result = await SCFG.Pay(ScfgPayKind.Unlock, new ScfgPayRequest { Item = "chapter2" });
        PauseGame(false);
        if (this == null) return;   // the game quit with the sheet open
        if (result.Status == ScfgPaymentStatus.Paid || result.Status == ScfgPaymentStatus.AlreadyOwned)
            StartChapterTwo();
    }
}
```

`Testnet = true` makes the sheet offer test networks, so no real money moves. Set it to `false` when you go live.
Replace the placeholder wallet with your own. Check `SCFG.Owns("chapter2")` wherever the game gates the content:
it is instant and works offline. Call `Configure` before the first frame that needs payments, for example from
`Awake` in your first scene.

## Without code: the SCFG Settings asset

If you prefer the inspector to code, create **Assets > Create > SCFG Settings** anywhere under `Assets/` and fill
it in:

| Field | Meaning |
|---|---|
| **Wallet** | Your EVM payout address (`0x...`). It receives the payments on Ethereum, Arbitrum, Polygon, Base and Arc |
| **Solana Wallet** | Optional, your Solana payout address. At least one of the two wallets is required |
| **Testnet** | On by default. Leave it on while you test; turn it off for the release. The inspector warns while it is on |
| **Chains** | The networks offered, in order. Empty: every network your wallets can receive on |
| **Tokens** | `USDC`, `USDT`. Empty: both |
| **Accent** | The accent color of the sheet and the checkout page, `#rrggbb`. Empty: the default |
| **Logo** | A sprite shown on the sheet and the checkout page |
| **Web Storage Id** | WebGL builds only, see [WebGL builds](api.md#webgl-builds). Set it once and never change it |
| **Items** | One entry per unlock: **Id**, **Code** (1-99, different for each item, never changed after release), **Price** (`4.99`), **Token**, **Title** and an optional **Min Price** |

Click **Validate** at the bottom of the inspector: it runs the same checks as `SCFG.Configure` and `SCFG.AddItem`
and shows the first error's code and message, or how many items passed.

The asset is applied before the first scene loads, through `SCFG.Configure` and `SCFG.AddItem`, so
`SCFG.Owns("chapter2")` and `SCFG.Pay(...)` work from your first `Awake`. A build puts the asset in the player by
itself (it adds it to the build's preloaded assets for that build only, and takes it out again even when the build
fails); you do not need a `Resources` folder, and you should not add the asset to **Preloaded Assets** yourself.
Keep one asset per project: with several, none is applied (the Console says which ones) and a build fails, so
the editor and the player can never use different assets.

Code stays in charge: a `SCFG.Configure` call from your code made after the asset was applied replaces it, items
included, until payments have started (the first frame). For the options the asset does not have (`Methods`,
`RpcUrls`, `Confirmations`, `PreviousWallets`), configure from code. Your own chains too: call `SCFG.AddChain`
and then `SCFG.Configure` from the first scene's `Awake`; `AddChain` sets the asset's configuration aside (items
included) for the `Configure` that follows, as the asset cannot name a chain added from code. A misconfigured
asset is reported in the Console (`scfg: the SCFG Settings asset ... was not applied`), never thrown, and so is a
logo that cannot be used (the asset is then applied without it). The logo can be any sprite: a texture of its
own, a cell of a sprite sheet or a sprite in a Sprite Atlas, with or without Read/Write.

## Before you go live

1. Pay once with each method you enabled (browser wallet, phone wallet by QR, manual transfer) on a testnet, to
   **your own wallet**, from a second wallet of yours. Get test USDC from
   [Circle's faucet](https://faucet.circle.com/).
2. Quit the game in the middle of a payment, start it again and check that the purchase still completes.
3. Call `SCFG.ClearLocalData()`, then use **Restore** with the transaction hash.
4. In a development build (and in the editor) the sheet shows **DEV MODE: paying to** and the start and end of
   the payout address. Check that they match yours. The Manual transfer step shows the full address.
5. Set `Testnet = false` (or untick **Testnet** on the asset).
6. If you take Solana payments, receive any amount of each token on your mainnet Solana wallet once first. Until
   then a development build warns in the Console and players are not offered that token.
7. For a WebGL release, read [WebGL builds](api.md#webgl-builds) and pay once from the build at its real address.

## What you get

```
Packages/com.repulse.scfg/
├── package.json
├── Runtime/
│   ├── Core/          (the engine-independent payment core, plain C#)
│   ├── Desktop/       (the localhost checkout server; not built for WebGL)
│   ├── Unity/         (the SCFG facade, its options and results, the UI Toolkit screens, the WebGL bridge,
│   │                   and Resources/ with the screens' style sheet and the checkout page)
│   └── link.xml       (what IL2CPP must keep; the package hands it to the linker)
├── Editor/            (the SCFG Settings inspector and build step, the link.xml hook)
├── Samples~/          (Showcase and Demo, imported from the Package Manager)
└── Documentation~/
```

Receipts and pending payments are stored in `Application.persistentDataPath/scfg/state.json`, global to the
install and never inside your save slots, so loading an older save cannot lose a purchase.

## Next

Read the [API reference](api.md). Before a WebGL release, read its [WebGL builds](api.md#webgl-builds) section.
