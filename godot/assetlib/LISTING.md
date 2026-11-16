# Store listing: Stablecoin for Games (SCFG) for Godot

Copy-paste values for the two Godot stores. Update **Version**, **Download URL** and **Changelog** for each
release. Every image is in this folder; the `.gdignore` keeps Godot from importing it.

| File | Size | Use |
|---|---|---|
| `icon.png` | 256×256 | Asset Library icon |
| `thumbnail-16x9.png` | 1280×720 | Asset Store thumbnail (16:9 required) |
| `featured-1920x1080.png` | 1920×1080 | Asset Store featured image (optional) |
| `screenshots/01-sheet-method.png` … `06-showcase.png` | 1920×1080 | Screenshots for both stores |

The screenshots are real captures of `godot/showcase` on Godot 4.3, in testnet mode. That is why the sheet
shows "DEV MODE: paying to … (testnet)" and the Arc Testnet amounts.

---

## Shared text

**Name**

```
Stablecoin for Games (SCFG)
```

**Summary** (one line)

```
Accept USDC and USDT tips and unlock purchases in your Godot game, paid straight to your own wallet. No server, no accounts.
```

**Description** (plain text, so it reads the same in both stores)

```
Stablecoin for Games (SCFG) lets players tip you or buy unlocks with USDC or USDT, and the money goes straight to your own wallet. There is no server to run, no account to create and no middleman taking a cut: the addon watches the blockchain itself and unlocks the item once the payment is confirmed.

Supported networks: Ethereum, Arbitrum and Polygon (USDC and USDT), Base (USDC), Arc (USDC) and Solana (USDC and USDT). You can also add any other EVM chain yourself. A testnet mode lets you try everything without real money.

What players see
A payment sheet over your game. They pick an amount (for tips), a network, and a way to pay:
- Browser wallet: MetaMask, Rabby or another extension, through a checkout page that opens in their browser (desktop) or the wallet already in the page (web export).
- Phone wallet: scan a QR code.
- Manual transfer: the exact amount and address to send from an exchange or any wallet, plus a field to paste the transaction hash.
They can keep playing while the payment confirms. A "Restore purchases" screen brings a purchase back on a new device from its transaction hash.

What you get
- One autoload, Scfg: configure(), add_item(), await pay("unlock" or "tip"), await restore(), owns(), has_tipped(), tip_total(), receipts() and export_receipts().
- Signals payment_resolved and receipts_changed, and typed results (ScfgPaymentResult, ScfgRestoreResult).
- Receipts saved in user://, plus a copy in the browser's localStorage on web exports. owns() is instant and works offline.
- Your logo and accent color, your own Theme, or your own scene in place of the payment sheet.
- Every text goes through tr(). A .pot template is included for translations.

Requirements
Godot 4.3 or newer, tested on 4.3 and 4.7.2. Pure GDScript: no GDExtension, no C#. Works in desktop builds and web exports. Mobile is best effort and not yet verified.

Good to know
Without a server, unlocks are honour-grade: a determined player can bypass them. That suits tips, supporter rewards and pay-what-you-want unlocks. Don't sell anything you can't afford to lose to a bypass. The addon is still in preview for Godot. Please report problems on the issue tracker.

Getting started
Enable the plugin under Project > Project Settings > Plugins. It registers the Scfg autoload. Then:

    Scfg.configure({"wallet": "0xYourWallet", "testnet": true})
    Scfg.add_item("chapter2", {"code": 1, "price": "4.99", "token": "USDC", "title": "Chapter 2"})
    var result := await Scfg.pay("unlock", {"item": "chapter2"})

The guide is included as addons/scfg/README.md. Full docs: https://jrpg.com/stablecoinforgames/docs/
The repository has a showcase project (godot/showcase) that tours the whole API on testnets.

MIT licensed. Made by JRPG Crypto Games.
```

**Links**

| | |
|---|---|
| Repository / source code | `https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games` |
| Issues | `https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/issues` |
| Docs | `https://jrpg.com/stablecoinforgames/docs/` |
| Website | `https://jrpg.com/stablecoinforgames/` |

**License:** MIT. **Minimum Godot version:** 4.3. **Version:** 0.2.0

**Changelog for 0.2.0**

```
First release of the Godot addon: unlocks and tips in USDC/USDT on Ethereum, Arbitrum, Polygon, Base, Arc and Solana, with browser-wallet, QR and manual payment, restore purchases, receipts, theming and translations. Desktop and web exports, Godot 4.3+.
```

---

## Godot Asset Store (store.godotengine.org)

**Publisher** (made when you first upload): name `JRPG Crypto Games`, URL slug `jrpg-crypto-games`.
**Asset URL slug:** `stablecoin-for-games`

**Settings tab**

| Field | Value |
|---|---|
| Summary | the summary above |
| Description | the description above |
| Asset type | Addon |
| License | MIT |
| Link to source code | the repository URL |
| Tags | `payments`, `monetization`, `tips`, `in-app-purchases`, `crypto`, `stablecoin`, `usdc`, `usdt`, `blockchain`, `wallet`, `web3`, `solana`, `ethereum` |
| AI usage disclosure | see below (**required**) |

Don't tag the asset's own name ("scfg", "stablecoin for games"); the guidelines reject that.

**AI usage disclosure.** This is a draft: confirm that it is true before you submit it.

```
The addon's code, tests and documentation were written with the help of an AI coding assistant (Claude, by Anthropic), then reviewed, tested and edited by the developers. The store thumbnail and featured image were assembled by a script from our existing logo, brand fonts and real screenshots of the addon. The screenshots are unedited captures of the addon running in Godot 4.3.
```

If `icon.png` or any brand art was made with an image generator, say so here too.

**Media tab**

| Field | File |
|---|---|
| Thumbnail (16:9) | `thumbnail-16x9.png` |
| Screenshots | `screenshots/01-sheet-method.png` to `06-showcase.png`, in that order |
| Featured image (optional) | `featured-1920x1080.png` |
| YouTube (optional) | none yet |

**Versions tab**

| Field | Value |
|---|---|
| Version name | `0.2.0` |
| Changelog | the changelog above |
| Minimum Godot version | 4.3 |
| Maximum Godot version | leave empty |
| File | `scfg-godot-0.2.0.zip` from the GitHub release (see the note below) |

The release zip wraps the addon in a `scfg-godot-0.2.0/` top folder. The store guidelines ask for
`addons/asset_name/`. Uploading a zip with `addons/` at its root avoids a literal-minded rejection.

**Pricing tab:** free. A donation link is optional (for example the itch.io page,
`https://repulse-games.itch.io/stablecoin-for-games`).

---

## Godot Asset Library (godotengine.org/asset-library, the legacy one)

The Asset Library only takes image URLs, not uploads. The URLs below work once this folder is pushed to `main`.

| Field | Value |
|---|---|
| Asset Name | `Stablecoin for Games (SCFG)` |
| Category | Scripts |
| Godot version | 4.3 |
| Version | `0.2.0` |
| Repository host | **Custom** (GitHub would serve the whole monorepo, and the addon would land at `res://godot/addons/scfg`) |
| Repository URL | `https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games` |
| Issues URL | `https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/issues` |
| Download URL (the "Download Commit" field under Custom) | `https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/releases/download/v0.2.0/scfg-godot-0.2.0.zip` |
| Icon URL | `https://raw.githubusercontent.com/Repulse-Game-Studio/jrpg-stablecoin-games/main/godot/assetlib/icon.png` |
| License | MIT |
| Description | the description above |

**Previews** (type Image; the same URL works as the thumbnail):

```
https://raw.githubusercontent.com/Repulse-Game-Studio/jrpg-stablecoin-games/main/godot/assetlib/screenshots/01-sheet-method.png
https://raw.githubusercontent.com/Repulse-Game-Studio/jrpg-stablecoin-games/main/godot/assetlib/screenshots/02-sheet-network.png
https://raw.githubusercontent.com/Repulse-Game-Studio/jrpg-stablecoin-games/main/godot/assetlib/screenshots/03-sheet-qr.png
https://raw.githubusercontent.com/Repulse-Game-Studio/jrpg-stablecoin-games/main/godot/assetlib/screenshots/04-sheet-manual.png
https://raw.githubusercontent.com/Repulse-Game-Studio/jrpg-stablecoin-games/main/godot/assetlib/screenshots/05-sheet-amount.png
https://raw.githubusercontent.com/Repulse-Game-Studio/jrpg-stablecoin-games/main/godot/assetlib/screenshots/06-showcase.png
```

---

## Each new release

1. Build and release the zip as usual (`uv run python tools/package.py --engine godot`).
2. **Asset Store:** Versions tab, add a version: name, changelog, upload the zip.
3. **Asset Library:** edit the asset and change the Version and Download URL to the new tag. The edit goes
   through moderation again.
4. If the sheet's look changed, recapture the screenshots.
