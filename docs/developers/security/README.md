# Security and limitations

What the plugin protects against:
- **Typing a fake purchase into the save data:** every receipt is re-checked on the blockchain at each launch, and invented ones are removed.
- **Look-alike "USDC" tokens:** only the official contract addresses count.
- **Fake transaction hashes sent to the browser page:** every hash is verified on-chain.

What it **cannot** prevent (no server means nobody holds a secret):
- A player reusing **someone else's real transaction hash**, or copying a `persistent` file.
- **Patched game code**, or a player-controlled fake RPC endpoint.
- A pirate re-releasing your game with their own wallet address.
- **Rare amount collisions:** two players buying the same item within minutes of each other, paying by QR or manual transfer without pasting the hash (about 1 % chance per overlapping pair).

So treat unlocks as **honour-grade**: right for supporter rewards and tips, weak for valuable content.

## Debug menus and cheats
If your game has a hidden debug mode or a level select, gate it with `owns()`. Otherwise it is an easy way around the paywall.

## Platform rules
- **Steam** forbids games that use cryptocurrencies.
- **Google Play and the App Store** require their own billing for digital unlocks.
- **itch.io and direct distribution** are the natural home for this plugin.

Check each store's current terms yourself.
