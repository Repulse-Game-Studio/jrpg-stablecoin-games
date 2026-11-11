# Security and limitations

This page applies to every engine. Options are named as in Ren'Py (`testnet=False`); in Godot and RPG Maker
MZ they have the same names in their own spelling (`"testnet": false`, the **Testnet** parameter).

What the plugin protects against:
- **Typing a fake purchase into the save data:** at each launch every receipt for a chain the game still knows is re-checked on the blockchain, and invented ones are removed. An entry whose own fields are invalid (an infinite or out-of-range amount, a transaction id or block that is not one of its chain's) is removed and the reason reported in the console; it never unlocks anything, and the other receipts are still checked. Two kinds of receipt cannot be checked and are kept, so a purchase is never lost to a configuration change: one naming a chain this build does not know (a custom chain added with `add_chain()` and later removed, or a chain of the other network, such as a testnet chain in a `testnet=False` game; the built-in chains of the game's network are always known, also those left out of `chains=`), and one for a chain family the game no longer has a wallet for (for example a Solana receipt once `solana_wallet` is gone). A receipt typed into the save data naming such a chain is therefore not caught: keep every custom chain and every wallet you ever sold with (`previous_wallets`, `previous_solana_wallets`) in the configuration.
- **Look-alike "USDC" tokens:** only the official contract addresses count.
- **Fake transaction hashes sent to the browser page:** every hash is verified on-chain.

What it **cannot** prevent (no server means nobody holds a secret):
- A player reusing **someone else's real transaction hash**, or copying another player's stored receipts (Ren'Py's `persistent` file, Godot's `user://scfg/state.json`, RPG Maker MZ's `save/scfg.rmmzsave`).
- **Patched game code**, or a player-controlled fake RPC endpoint.
- A pirate re-releasing your game with their own wallet address.
- **Rare amount collisions:** two players buying the same item within minutes of each other, paying by QR or manual transfer without pasting the hash (about 1 % chance per overlapping pair).

So treat unlocks as **honour-grade**: right for supporter rewards and tips, weak for valuable content.

## Debug menus and cheats
If your game has a hidden debug mode or a level select, gate it with `owns()` (in RPG Maker MZ, the item's Owned Switch). Otherwise it is an easy way around the paywall.

## Platform rules
- **Steam** forbids games that use cryptocurrencies.
- **Google Play and the App Store** require their own billing for digital unlocks.
- **itch.io and direct distribution** are the natural home for this plugin.

Check each store's current terms yourself.
