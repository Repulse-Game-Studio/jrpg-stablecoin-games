# Install (RPG Maker MZ)

Accept **USDC** and **USDT** tips and unlock purchases in your RPG Maker MZ game, paid **directly to your own
wallet**. There is no server, no account and no middleman: the plugin watches the blockchain itself. It is one
plugin file, `js/plugins/SCFG.js`, with no dependencies. It works in desktop builds (NW.js) and in web
deployments. Mobile is best effort and unverified (see [Mobile](api.md#mobile-unverified)).

Chains: Ethereum, Arbitrum, Polygon (USDC and USDT), Base (USDC), Arc (USDC), Solana (USDC and USDT), plus
any EVM chain you add yourself.

> **Tested on a stand-in, not yet on real RPG Maker MZ.** Nobody has run this plugin inside the real MZ
> editor or runtime yet. It is tested on an in-repo stand-in for MZ's public plugin API (a "shim"), in headless
> Chrome and in Node, against local test chains. The plugin header, the demo project files and the NW.js parts
> are written from MZ's public documentation and may need small fixes in the real thing. If something does
> not work, please [open an issue](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/issues).

> Without a server, unlocks are honour-grade: a determined player can bypass them. Do not sell anything you
> cannot afford to lose to a bypass.

Only **RPG Maker MZ** is supported (the plugin needs a modern JavaScript runtime). MV is not.

## From the release zip

1. Download `scfg-rpgmaker-mz-<version>.zip` from the
   [latest release](https://github.com/Repulse-Game-Studio/jrpg-stablecoin-games/releases/latest).
2. Unzip it into your project folder, the one that contains `game.rmmzproject`. It adds `js/plugins/SCFG.js`,
   an `img/scfg/` folder (for your logo), and three files at the root: `scfg-README.md`, `scfg-LICENSE.txt`
   and `scfg-NOTICES.txt`.
3. In the editor, open **Tools > Plugin Manager**, add **SCFG** and turn it **ON**. Keep the file's name,
   `SCFG.js`: MZ finds a plugin's parameters by its file name. If SCFG sits below plugins
   that change the main menu or the scenes, move it to the bottom of the list.
4. Fill in the parameters:
   - **Wallet**: your EVM address (`0x...`). It receives the payments on Ethereum, Arbitrum, Polygon, Base
     and Arc.
   - **Solana Wallet**: optional, to take Solana payments too.
   - **Testnet**: leave it **ON** while you test. The sheet then offers test networks and no real money
     moves. Turn it **OFF** for the release.
   - **Items**: one entry per unlock, each with an `Id`, a `Code` (1-99, different for each item), a
     `Price`, a `Token` and a `Title`. Give each item its own **Owned Switch**.

Every parameter and command is described in the [API reference](api.md).

## Your first payment

1. In the Plugin Manager add an item: Id `chapter2`, Code `1`, Price `4.99`, Token `USDC`, Title
   `Chapter 2`, Owned Switch `0001`.
2. In an event, add the plugin command **SCFG > Unlock Item** with Item `chapter2` and Status Variable
   `0001`. The event waits while the player pays.
3. Gate your content on switch `0001`: it is ON for as long as the player owns Chapter 2, also after a New Game
   and after loading any save.

In events, use the plugin commands: a **Script** event command runs plain, non-async JavaScript, so `await`
there is a `SyntaxError` that stops the game, and the event does not wait for the payment. From your own
plugin, in an `async` function, the same payment is:

```js
async function startChapter2() {
    try {
        const result = await SCFG.pay("unlock", { item: "chapter2" });
        if (result.status === "paid" || result.status === "already_owned") {
            // start chapter two
        }
    } catch (e) {
        console.error(e); // a misused call, such as an unknown item
    }
}
```

Always catch: an unhandled rejection stops an MZ game (MZ shows its error screen). A Script command that calls
the script API anyway chains it, `SCFG.pay("unlock", { item: "chapter2" }).then(...).catch(...)`, and its event
goes on at once. See [Calling it from an event or a plugin](api.md#calling-it-from-an-event-or-a-plugin).

`SCFG.owns("chapter2")` is instant and works offline.

A complete example project is in `rpgmaker/demo/` in the repository: a map with three NPCs (Unlock, Tip,
Restore Purchases), a sign for Wallet Help and a door that opens once Chapter 2 is bought. It has no
artwork: you add the sprites from your own project. Its README says how to copy it into a new MZ project.

## What you get

```
MyGame/
├── game.rmmzproject
├── js/
│   └── plugins/
│       └── SCFG.js                (the whole plugin: payment core, sheet, checkout page)
├── img/
│   └── scfg/
│       └── README.txt             (put your logo here)
├── scfg-README.md
├── scfg-LICENSE.txt
└── scfg-NOTICES.txt
```

Receipts and pending payments are stored in MZ's global save area under the key `scfg` (the file
`save/scfg.rmmzsave` on desktop, the browser's storage on the web), never inside your save slots, so loading
an older save cannot lose a purchase.

## Next

Read the [API reference](api.md). Before a web release, read its [Web deployment](api.md#web-deployment)
section.
