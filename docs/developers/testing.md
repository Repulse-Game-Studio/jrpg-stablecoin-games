# Test before release

1. Use `testnet=True` and **your own wallet**.
2. Get test USDC from Circle's faucet, plus a little test ETH (or POL on Amoy) for network fees. On Arc Testnet the faucet's USDC is all you need: it also pays the fee.
3. Buy something with each method you enabled: browser wallet, QR and manual. On Arc, make
   the manual transfer with your wallet's own Send button: it arrives as a native send.
4. Restart the game mid-payment and check the purchase still completes.
5. Run `scfg.clear_local_data()` from the developer console (Shift+O), then use **Restore purchases** with the transaction hash.
6. In developer mode the payment screen shows **DEV MODE: paying to 0x5290...9EE7**. Check that the start and end match your address. The Manual transfer step shows the full address.

**Solana devnet checklist** (`testnet=True` with `solana_wallet`):

1. Switch Phantom to devnet: Settings → Developer settings → Testnet mode → Solana Devnet.
2. Get devnet SOL (`solana airdrop` or faucet.solana.com) and devnet USDC (faucet.circle.com → Solana Devnet).
3. Receive devnet USDC on your developer wallet once, so its token account exists, then restart the game. Until then the console warns and Solana isn't offered.
4. Pay by browser extension, by phone QR, and by a manual transfer from a second wallet.
5. Run `scfg.clear_local_data()`, then restore with each signature.

**Arc Testnet checklist** (`testnet=True` with `wallet`):

1. Get testnet USDC from faucet.circle.com (choose Arc Testnet). It also pays the fee: no second coin is needed.
2. Pay once through the browser checkout. If your wallet doesn't know Arc Testnet (chain id 5042002), the page adds it.
3. Pay once with your wallet's own **Send** button on the Manual transfer step. This is a native USDC send, which reaches the game differently from the checkout's ERC-20 transfer.
4. If your payout `wallet` is a smart-contract wallet (a Safe, a smart account), check it is deployed on Arc before you leave Arc on (see [Arc](configure/arc.md)).
5. Run `scfg.clear_local_data()`, then restore with both transaction hashes.

**Web build checklist** (a Ren'Py web build, see [Web builds](security/web-builds.md)):

1. Upload the build to its real home (on itch.io: an HTML game) and pay a small amount with a browser wallet extension.
2. Reload the page and check the purchase is still owned.
3. Shared origin (not yet verified with Ren'Py): if the host serves other games from the same origin, buy something, open another Ren'Py web game on that origin in a second tab, save there and close it, then reload your game and check the purchase is still owned. If it is gone, **Restore purchases** with the transaction hash brings it back; please report it.
