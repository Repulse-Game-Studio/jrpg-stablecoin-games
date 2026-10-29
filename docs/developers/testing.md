# Test before release

1. Use `testnet=True` and **your own wallet**.
2. Get test USDC from Circle's faucet, plus a little test ETH (or POL on Amoy) for network fees.
3. Buy something with each method you enabled: browser wallet, QR and manual.
4. Restart the game mid-payment and check the purchase still completes.
5. Run `scfg.clear_local_data()` from the developer console (Shift+O), then use **Restore purchases** with the transaction hash.
6. In developer mode the payment screen shows **DEV MODE: paying to 0x5290...9EE7**. Check that the start and end match your address. The Manual transfer step shows the full address.

**Solana devnet checklist** (`testnet=True` with `solana_wallet`):

1. Switch Phantom to devnet: Settings → Developer settings → Testnet mode → Solana Devnet.
2. Get devnet SOL (`solana airdrop` or faucet.solana.com) and devnet USDC (faucet.circle.com → Solana Devnet).
3. Receive devnet USDC on your developer wallet once, so its token account exists, then restart the game. Until then the console warns and Solana isn't offered.
4. Pay by browser extension, by phone QR, and by a manual transfer from a second wallet.
5. Run `scfg.clear_local_data()`, then restore with each signature.
